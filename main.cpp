#define LYF_INNER_LOG

#include "Logger.h"
#include "sinks/ConsoleSink.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gflags/gflags.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/types.h>
#include <unistd.h>
#elif __linux__
#include <fstream>
#endif

void PrintMemoryUsage(const std::string &label) {
#ifdef __APPLE__
  task_t task = mach_task_self();
  struct task_basic_info info;
  mach_msg_type_number_t size = TASK_BASIC_INFO_COUNT;
  kern_return_t kerr =
      task_info(task, TASK_BASIC_INFO, (task_info_t)&info, &size);

  if (kerr == KERN_SUCCESS) {
    double memory_mb =
        static_cast<double>(info.resident_size) / 1024.0 / 1024.0;
    std::cout << label << " - Memory: " << memory_mb << " MB (resident)"
              << std::endl;
  } else {
    std::cout << label << " - Memory: Unable to get memory info" << std::endl;
  }
#elif __linux__
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.find("VmRSS:") == 0) {
      std::cout << label << " - " << line << std::endl;
      break;
    }
  }
#else
  std::cout << label << " - Memory: Unsupported platform" << std::endl;
#endif
}

using namespace lyf;

DEFINE_uint64(logs, 1'000'000, "总日志条数");
DEFINE_uint64(warmup_logs, 0, "预热条数");
DEFINE_uint64(threads, 4, "线程数(0表示硬件并发)");
DEFINE_uint64(capacity, 65536, "队列容量");
DEFINE_string(policy, "BLOCK", "队列满策略(默认 BLOCK)");
DEFINE_uint64(timeout_us, QueConfig::kMaxBlockTimeout_us, "BLOCK 超时");
DEFINE_uint64(buffer_pool, 65536, "BufferPool 初始数量");
DEFINE_string(sink, "file", "输出 sink");
DEFINE_string(log_file, "app.log", "日志文件路径");
DEFINE_string(level, "INFO", "Logger 级别");
DEFINE_uint64(sample_rate, 1000, "每 N 条采 1 个延迟样本(表示不采样)");

/*
// 示例命令
./main --threads 4 --logs 10000000 --warmup-logs 1000 --policy BLOCK --capacity
65536 --sink file --buffer-pool 65536 --log-file app.log --sample-rate 1000
*/

struct BenchmarkConfig {
  size_t log_count = 1'000'000;
  size_t warmup_logs = 0;
  size_t thread_count = std::max(1u, std::thread::hardware_concurrency());
  size_t capacity = 65536;
  QueueFullPolicy policy = QueueFullPolicy::BLOCK;
  size_t timeout_us = QueConfig::kMaxBlockTimeout_us;
  size_t buffer_pool_size = 65536;
  LogLevel level = LogLevel::INFO;
  std::string sink = "file";
  std::string log_file = "app.log";
  size_t sample_rate = 1000;
};

struct ThreadStats {
  uint64_t count = 0;
  uint64_t sum_ns = 0;
  uint64_t min_ns = std::numeric_limits<uint64_t>::max();
  uint64_t max_ns = 0;
  std::vector<uint64_t> samples;
};
using AggregateStats = ThreadStats;

static uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static void truncateLogFile(const std::string &logfile) {
  std::fstream ofs(logfile, std::ios::out | std::ios::trunc);
  ofs.close();
}

static size_t CountLines(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return 0;
  }
  // 统计换行符数量
  return std::count(std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>(), '\n');
}

static BenchmarkConfig BuildConfigFromFlags() {
  BenchmarkConfig cfg;
  cfg.log_count = static_cast<size_t>(FLAGS_logs);
  cfg.warmup_logs = static_cast<size_t>(FLAGS_warmup_logs);
  cfg.thread_count = static_cast<size_t>(FLAGS_threads);
  cfg.capacity = static_cast<size_t>(FLAGS_capacity);
  cfg.policy = ParsePolicy(FLAGS_policy);
  cfg.timeout_us = static_cast<size_t>(FLAGS_timeout_us);
  cfg.buffer_pool_size = static_cast<size_t>(FLAGS_buffer_pool);
  cfg.level = ParseLevel(FLAGS_level);
  cfg.sink = FLAGS_sink;
  cfg.log_file = FLAGS_log_file;
  cfg.sample_rate = static_cast<size_t>(FLAGS_sample_rate);
  if (cfg.thread_count == 0) {
    cfg.thread_count = std::max(1u, std::thread::hardware_concurrency());
  }
  return cfg;
}

static void InitLogger(const BenchmarkConfig &cfg) {
  auto &logger = Logger::Instance();
  LogConfig log_cfg;
  log_cfg.SetQueueCapacity(cfg.capacity)
      .SetQueueFullPolicy(cfg.policy)
      .SetQueueBlockTimeoutUs(cfg.timeout_us)
      .SetBufferPoolSize(cfg.buffer_pool_size)
      .SetLevel(cfg.level)
      .SetRotatePolicy(RotatePolicy::NONE)
      .SetLogPath(""); // 设置为空避免初始化时创建文件sink
  logger.Init(log_cfg);
  if (cfg.sink == "console") {
    logger.AddSink(std::make_shared<ConsoleSink>());
  } else {
    logger.AddSink(std::make_shared<FileSink>(cfg.log_file));
  }
}

static AggregateStats Aggregate(const std::vector<ThreadStats> &stats) {
  AggregateStats agg;
  size_t total_samples = 0;
  for (const auto &s : stats) {
    agg.count += s.count;
    agg.sum_ns += s.sum_ns;
    agg.min_ns = std::min(agg.min_ns, s.min_ns);
    agg.max_ns = std::max(agg.max_ns, s.max_ns);
    total_samples += s.samples.size();
  }
  agg.samples.reserve(total_samples);
  for (const auto &s : stats) {
    agg.samples.insert(agg.samples.end(), s.samples.begin(), s.samples.end());
  }
  return agg;
}

static uint64_t Percentile(std::vector<uint64_t> &values, double p) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  double idx = (values.size() - 1) * p;
  size_t i = static_cast<size_t>(idx);
  return values[i];
}

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  auto cfg = BuildConfigFromFlags();
  InitLogger(cfg);
  if (cfg.sink != "console") {
    truncateLogFile(cfg.log_file);
  }

  std::vector<std::thread> threads;
  threads.reserve(cfg.thread_count);
  std::vector<ThreadStats> thread_stats(cfg.thread_count);
  std::atomic<bool> start_flag{false};

  size_t base = cfg.log_count / cfg.thread_count;
  size_t remain = cfg.log_count % cfg.thread_count;
  size_t warm_base = cfg.warmup_logs / cfg.thread_count;
  size_t warm_remain = cfg.warmup_logs % cfg.thread_count;

  for (size_t t = 0; t < cfg.thread_count; ++t) {
    size_t count = base + (t < remain ? 1 : 0);
    size_t warm_count = warm_base + (t < warm_remain ? 1 : 0);
    threads.emplace_back([&, t, count, warm_count]() {
      for (size_t i = 0; i < warm_count; ++i) {
        INFO("warmup {}", i);
      }
      while (!start_flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      auto &stats = thread_stats[t];
      if (cfg.sample_rate > 0) {
        stats.samples.reserve(count / cfg.sample_rate + 1);
      }
      for (size_t i = 0; i < count; ++i) {
        uint64_t begin_ns = NowNs();
        INFO("Hello, LogSystem! {} {}", t, i);
        uint64_t end_ns = NowNs();
        uint64_t lat = end_ns - begin_ns;
        stats.count += 1;
        stats.sum_ns += lat;
        stats.min_ns = std::min(stats.min_ns, lat);
        stats.max_ns = std::max(stats.max_ns, lat);
        if (cfg.sample_rate > 0 && (i % cfg.sample_rate == 0)) {
          stats.samples.push_back(lat);
        }
      }
    });
  }

  PrintMemoryUsage("Before benchmark");

  uint64_t start_ns = NowNs();
  start_flag.store(true, std::memory_order_release);
  for (auto &th : threads) {
    th.join();
  }
  uint64_t submit_end_ns = NowNs();
  PrintMemoryUsage("After submit");

  Logger::Instance().Sync();
  uint64_t end_ns = NowNs();
  PrintMemoryUsage("After sync");

  auto agg = Aggregate(thread_stats);
  uint64_t total_time_ns = end_ns - start_ns;
  uint64_t submit_time_ns = submit_end_ns - start_ns;
  uint64_t sync_time_ns = end_ns - submit_end_ns;
  double avg_ns =
      agg.count > 0 ? static_cast<double>(agg.sum_ns) / agg.count : 0;

  uint64_t p50 = Percentile(agg.samples, 0.50);
  uint64_t p95 = Percentile(agg.samples, 0.95);
  uint64_t p99 = Percentile(agg.samples, 0.99);
  uint64_t p999 = Percentile(agg.samples, 0.999);

  uint64_t min_lat_ns = agg.min_ns;
  uint64_t max_lat_ns = agg.max_ns;

  uint64_t drop_count = Logger::Instance().GetDropCount();
  // 停止 Logger
  size_t shutdown_begin_ns = NowNs();
  Logger::Instance().Shutdown();
  size_t shutdown_end_ns = NowNs();

  uint64_t logfile_size_MB = 0;
  if (cfg.sink != "console") {
    logfile_size_MB = std::filesystem::file_size(cfg.log_file) / (1024 * 1024);
  }

  std::cout << "threads: " << cfg.thread_count << std::endl;
  std::cout << "logs: " << cfg.log_count << std::endl;
  std::cout << "policy: " << QueueFullPolicyToString(cfg.policy) << std::endl;
  std::cout << "capacity: " << cfg.capacity << std::endl;
  std::cout << "buffer pool size: " << cfg.buffer_pool_size << std::endl;
  std::cout << "total time: " << total_time_ns / 1e9 << " s" << std::endl;
  std::cout << "submit time: " << submit_time_ns / 1e9 << " s" << std::endl;
  std::cout << "sync time: " << sync_time_ns / 1e9 << " s" << std::endl;
  std::cout << "shutdown time: " << (shutdown_end_ns - shutdown_begin_ns) / 1e9
            << " s" << std::endl;
  std::cout << "avg submit latency: " << avg_ns << " ns" << std::endl;
  std::cout << "min/max latency: " << min_lat_ns << "/" << max_lat_ns << " ns"
            << std::endl;
  std::cout << "p50/p95/p99/p999: " << p50 << "/" << p95 << "/" << p99 << "/"
            << p999 << " ns" << std::endl;
  if (cfg.sink != "console") {
    std::cout << "logfile: " << cfg.log_file << std::endl;
    std::cout << "logfile size: " << logfile_size_MB << " MB" << std::endl;
    double throughput =
        total_time_ns > 0 ? 1.0 * logfile_size_MB / (total_time_ns / 1e9) : 0.0;
    std::cout << "avg throughput: " << throughput << " MB/s" << std::endl;
  }

  std::cout << "drop count: " << drop_count << std::endl;
  if (cfg.sink != "console") {
    auto line_count = CountLines(cfg.log_file);
    std::cout << "line count: " << line_count << std::endl;
  }

  return 0;
}
