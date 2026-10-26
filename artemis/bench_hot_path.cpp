/**
 * Artemis benchmark: Quill hot-path latency and backend throughput for a
 * trading-style workload. Writes artemis_results.json in the working directory.
 *
 * Metrics
 *   hot_path_mean_ns, hot_path_p50_ns, hot_path_p99_ns, hot_path_p999_ns  per-call latency on the
 *       producer thread (lower is better), order-event log lines
 *   backend_msgs_per_sec  sustained messages/s through the backend to a file sink
 *       (higher is better)
 *   lines_ok  1 when every line reached the file with the expected content
 *
 * Exits non-zero when the output check fails, so a version that drops or
 * corrupts log lines cannot score.
 */

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/backend/BackendUtilities.h"
#include "quill/core/Rdtsc.h"
#include "quill/sinks/FileSink.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
  #include <emmintrin.h>
#endif

namespace
{
constexpr size_t kBatches = 20'000;
constexpr size_t kMessagesPerBatch = 20;
constexpr auto kBatchGap = std::chrono::microseconds{30};
constexpr size_t kThroughputMessages = 2'000'000;

constexpr std::string_view kSymbols[] = {"AAPL", "MSFT", "NVDA", "ESZ6", "NQZ6", "EURUSD", "BTC-PERP", "TSLA"};

inline uint64_t fenced_rdtsc() noexcept
{
#if defined(__x86_64__) || defined(_M_X64)
  _mm_lfence();
  uint64_t const t = quill::detail::rdtsc();
  _mm_lfence();
  return t;
#else
  return quill::detail::rdtsc();
#endif
}

double ns_per_tick()
{
  auto const t0 = std::chrono::steady_clock::now();
  uint64_t const c0 = fenced_rdtsc();
  std::this_thread::sleep_for(std::chrono::milliseconds{200});
  uint64_t const c1 = fenced_rdtsc();
  auto const t1 = std::chrono::steady_clock::now();
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
    static_cast<double>(c1 - c0);
}

void spin_for(std::chrono::nanoseconds d)
{
  auto const end = std::chrono::steady_clock::now() + d;
  while (std::chrono::steady_clock::now() < end)
  {
  }
}

void pin(uint16_t cpu)
{
  try
  {
    quill::detail::set_cpu_affinity(std::vector<uint16_t>{cpu});
  }
  catch (std::exception const& e)
  {
    std::cerr << "cpu affinity: " << e.what() << '\n';
  }
}

quill::Logger* make_logger(std::string const& name, std::string const& path)
{
  auto sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
    path,
    []()
    {
      quill::FileSinkConfig cfg;
      cfg.set_open_mode('w');
      return cfg;
    }(),
    quill::FileEventNotifier{});

  quill::Logger* logger = quill::Frontend::create_or_get_logger(
    name, std::move(sink),
    quill::PatternFormatterOptions{"%(time) [%(thread_id)] %(short_source_location) %(log_level) %(message)",
                                   "%H:%M:%S.%Qns", quill::Timezone::GmtTime, false});
  logger->set_log_level(quill::LogLevel::Info);
  return logger;
}

inline void log_order(quill::Logger* logger, uint64_t order_id, size_t i)
{
  std::string_view const symbol = kSymbols[i % std::size(kSymbols)];
  double const price = 100.0 + static_cast<double>(i % 1000) * 0.25;
  uint32_t const qty = static_cast<uint32_t>(100 + (i % 50) * 10);
  if ((i & 1u) == 0)
  {
    LOG_INFO(logger, "NEW order_id={} sym={} side={} px={} qty={}", order_id, symbol, "BUY", price, qty);
  }
  else
  {
    LOG_INFO(logger, "FILL order_id={} sym={} px={} qty={} venue={}", order_id, symbol, price, qty, "XNAS");
  }
}

// Counts lines and checks a few known lines in full.
bool check_file(std::string const& path, size_t expected_lines, uint64_t first_id)
{
  std::ifstream in(path);
  std::string line;
  size_t count = 0;
  size_t matched = 0;
  std::string const first_new = "NEW order_id=" + std::to_string(first_id) + " sym=AAPL side=BUY px=100 qty=100";
  std::string const first_fill = "FILL order_id=" + std::to_string(first_id + 1) + " sym=MSFT px=100.25 qty=110 venue=XNAS";
  while (std::getline(in, line))
  {
    if (line.find("order_id=") == std::string::npos)
    {
      continue;
    }
    ++count;
    if (line.find(first_new) != std::string::npos || line.find(first_fill) != std::string::npos)
    {
      ++matched;
    }
  }
  if (count != expected_lines || matched != 2)
  {
    std::cerr << path << ": expected " << expected_lines << " order lines and 2 exact matches, got " << count
              << " lines and " << matched << " matches\n";
    return false;
  }
  return true;
}

uint64_t percentile(std::vector<uint64_t>& v, double p)
{
  size_t const idx = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
  return v[idx];
}
} // namespace

int main()
{
  std::remove("artemis_results.json");

  unsigned const cores = std::max(1u, std::thread::hardware_concurrency());
  uint16_t const backend_cpu = static_cast<uint16_t>(2 % cores);
  uint16_t const producer_cpu = static_cast<uint16_t>(4 % cores);

  quill::BackendOptions backend_options;
  backend_options.cpu_affinity = std::vector<uint16_t>{backend_cpu};
  backend_options.sleep_duration = std::chrono::nanoseconds{0};
  quill::Backend::start(backend_options);

  double const tick_ns = ns_per_tick();

  // 1. Hot-path latency: short bursts with gaps, so the backend keeps up and the
  //    measurement is the producer-side cost of a log call.
  std::vector<uint64_t> latencies;
  latencies.reserve(kBatches * kMessagesPerBatch);
  {
    quill::Logger* logger = make_logger("latency", "artemis_latency.log");
    std::thread producer(
      [&]()
      {
        pin(producer_cpu);
        LOG_INFO(logger, "warmup");
        logger->flush_log();
        uint64_t order_id = 1'000'000;
        for (size_t b = 0; b < kBatches; ++b)
        {
          for (size_t m = 0; m < kMessagesPerBatch; ++m)
          {
            uint64_t const t0 = fenced_rdtsc();
            log_order(logger, order_id, m);
            uint64_t const t1 = fenced_rdtsc();
            latencies.push_back(t1 - t0);
            ++order_id;
          }
          spin_for(kBatchGap);
        }
        logger->flush_log();
      });
    producer.join();
  }

  // 2. Backend throughput: push messages as fast as the queue allows and time
  //    until the backend has written them all.
  double msgs_per_sec = 0.0;
  {
    quill::Logger* logger = make_logger("throughput", "artemis_throughput.log");
    std::thread producer(
      [&]()
      {
        pin(producer_cpu);
        LOG_INFO(logger, "warmup");
        logger->flush_log();
        auto const start = std::chrono::steady_clock::now();
        uint64_t order_id = 5'000'000;
        for (size_t i = 0; i < kThroughputMessages; ++i)
        {
          log_order(logger, order_id++, i);
        }
        logger->flush_log();
        auto const end = std::chrono::steady_clock::now();
        double const secs = std::chrono::duration<double>(end - start).count();
        msgs_per_sec = static_cast<double>(kThroughputMessages) / secs;
      });
    producer.join();
  }

  quill::Backend::stop();

  bool const ok = check_file("artemis_latency.log", kBatches * kMessagesPerBatch, 1'000'000) &&
    check_file("artemis_throughput.log", kThroughputMessages, 5'000'000);
  if (!ok)
  {
    return 1;
  }

  double sum_ticks{0.0};
  for (uint64_t const t : latencies)
  {
    sum_ticks += static_cast<double>(t);
  }
  double const mean = sum_ticks / static_cast<double>(latencies.size()) * tick_ns;
  double const p50 = static_cast<double>(percentile(latencies, 0.50)) * tick_ns;
  double const p99 = static_cast<double>(percentile(latencies, 0.99)) * tick_ns;
  double const p999 = static_cast<double>(percentile(latencies, 0.999)) * tick_ns;

  std::printf("hot_path_mean_ns=%.2f p50=%.2f p99=%.2f p999=%.2f backend_msgs_per_sec=%.0f\n", mean, p50, p99,
              p999, msgs_per_sec);

  {
    std::ofstream out("artemis_results.json.tmp");
    out.precision(6);
    out << "{\"hot_path_mean_ns\": " << mean << ", \"hot_path_p50_ns\": " << p50 << ", \"hot_path_p99_ns\": " << p99
        << ", \"hot_path_p999_ns\": " << p999 << ", \"backend_msgs_per_sec\": " << static_cast<uint64_t>(msgs_per_sec)
        << ", \"lines_ok\": 1}\n";
  }
  std::rename("artemis_results.json.tmp", "artemis_results.json");

  std::remove("artemis_latency.log");
  std::remove("artemis_throughput.log");
  return 0;
}
