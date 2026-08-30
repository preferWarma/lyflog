#include "lyflog.h"

#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void demonstrate_levels_and_formatting() {
  LYF_DEBUG("debug details: request_id={} payload_bytes={}", "req-001", 512);
  LYF_INFO("service started on {}:{}", "127.0.0.1", 8080);
  LYF_WARN("cache hit rate is {:.1f}%", 73.48);
  LYF_ERROR("request {} failed with status={}", "req-002", 503);

  // lyflog uses fmt syntax, so width, precision, bases, and alignment work too.
  LYF_INFO("fmt examples: pi={:.3f}, hex=0x{:08x}, aligned='{:>8}'", 3.1415926,
           0x2a, "lyflog");
}

void demonstrate_runtime_level() {
  auto &logger = lyflog::Logger::instance();

  logger.set_level(lyflog::Level::Warn);
  LYF_INFO("this message is filtered out");
  LYF_WARN("the minimum level was changed to WARN at runtime");

  logger.set_level(lyflog::Level::Debug);
  LYF_DEBUG("DEBUG logging is enabled again");
}

void demonstrate_interval_logging() {
  // This loop runs five times, but the call site emits at most once per 200 ms.
  for (int heartbeat = 1; heartbeat <= 5; ++heartbeat) {
    LYF_INTERVAL_INFO(0.2, "rate-limited heartbeat, iteration={}", heartbeat);
    std::this_thread::sleep_for(100ms);
  }
}

void demonstrate_multithreaded_logging() {
  constexpr int kThreadCount = 4;
  constexpr int kMessagesPerThread = 20;

  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);
  for (int worker_id = 0; worker_id < kThreadCount; ++worker_id) {
    workers.emplace_back([worker_id] {
      for (int job_id = 0; job_id < kMessagesPerThread; ++job_id) {
        LYF_INFO("worker={} completed job={}", worker_id, job_id);
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }
}

void print_stats(const lyflog::Logger::Stats &stats) {
  const char *policy =
      stats.overflow_policy == lyflog::OverflowPolicy::Block ? "Block" : "Drop";

  std::cout << "lyflog stats\n"
            << "  running: " << std::boolalpha << stats.running << '\n'
            << "  level: " << lyflog::level_name(stats.level) << '\n'
            << "  queue: " << stats.queue_size << '/' << stats.queue_capacity
            << '\n'
            << "  overflow policy: " << policy << '\n'
            << "  dropped (overflow/no file/shutdown): "
            << stats.dropped_overflow << '/' << stats.dropped_no_file << '/'
            << stats.shutdown_drop << '\n'
            << "  file open failures: " << stats.open_fail_count << '\n'
            << "  buffer-pool fallback allocations: " << stats.pool_fallback_new
            << '\n';
}

} // namespace

int main() {
  constexpr const char *kLogPath = "logs/lyflog_example.log";

  // Calling a LYF_* macro without init() also works and uses LogConfig
  // defaults. Explicit initialization is shown here so an application can tune
  // behavior.
  lyflog::LogConfig config;
  config.set_file_path(kLogPath)
      .set_level(lyflog::Level::Debug)
      .set_with_thread_id(true)
      .set_daily_rotate(false)
      .set_max_file_size(1024 * 1024) // Rotate after approximately 1 MiB.
      .set_retain_count(3)
      .set_queue_capacity(8192)
      .set_overflow_policy(lyflog::OverflowPolicy::Block);

  auto &logger = lyflog::Logger::instance();
  logger.init(config);

  demonstrate_levels_and_formatting();
  demonstrate_runtime_level();
  demonstrate_interval_logging();
  demonstrate_multithreaded_logging();

  // sync() is useful before inspecting the file or at an application
  // checkpoint. shutdown() below also drains and flushes, but calling both
  // demonstrates the API.
  logger.sync();
  print_stats(logger.stats());
  std::cout << "log written to " << kLogPath
            << " (size rotation adds a numeric suffix)\n";

  logger.shutdown();
  return 0;
}
