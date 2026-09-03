#include "lyflog.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// 默认生产线程数：给后台 worker 留一核余量；Apple 上只用性能核，
// 避免 P/E 核混跑与超售把调度噪声混进策略对比（策略差异集中在中尾部
// 延迟，对调度噪声最敏感）。
std::size_t default_producer_threads() {
#if defined(__APPLE__)
  int perf_cores = 0;
  std::size_t size = sizeof(perf_cores);
  if (sysctlbyname("hw.perflevel0.logicalcpu", &perf_cores, &size, nullptr,
                   0) == 0 &&
      perf_cores > 1) {
    return static_cast<std::size_t>(perf_cores) - 1;
  }
#endif
  const std::size_t hardware = std::thread::hardware_concurrency();
  return hardware > 1 ? hardware - 1 : 1;
}

struct Options {
  std::size_t messages = 200000;
  std::size_t threads = default_producer_threads();
  std::size_t queue_capacity = 32768;
  lyflog::OverflowPolicy policy = lyflog::OverflowPolicy::Block;
  bool sweep = false;
};

const char *policy_name(lyflog::OverflowPolicy policy) {
  return policy == lyflog::OverflowPolicy::Block ? "block" : "drop";
}

std::size_t parse_positive(const char *text, const char *option) {
  std::size_t value = 0;
  const char *end = text + std::char_traits<char>::length(text);
  const auto result = std::from_chars(text, end, value);
  if (result.ec != std::errc{} || result.ptr != end || value == 0) {
    throw std::runtime_error(std::string(option) +
                             " expects a positive integer");
  }
  return value;
}

Options parse_options(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--messages" && i + 1 < argc) {
      options.messages = parse_positive(argv[++i], "--messages");
    } else if (argument == "--threads" && i + 1 < argc) {
      options.threads = parse_positive(argv[++i], "--threads");
    } else if (argument == "--queue-capacity" && i + 1 < argc) {
      options.queue_capacity = parse_positive(argv[++i], "--queue-capacity");
    } else if (argument == "--policy" && i + 1 < argc) {
      const std::string policy = argv[++i];
      if (policy == "block") {
        options.policy = lyflog::OverflowPolicy::Block;
      } else if (policy == "drop") {
        options.policy = lyflog::OverflowPolicy::Drop;
      } else {
        throw std::runtime_error("--policy expects block or drop");
      }
    } else if (argument == "--sweep") {
      options.sweep = true;
    } else if (argument == "--help") {
      std::cout << "Usage: lyflog_benchmark [--messages N] [--threads N]\n"
                << "                        [--policy block|drop] "
                   "[--queue-capacity N]\n"
                << "                        [--sweep]\n"
                << "\n"
                << "  --sweep   两种策略 × 多档容量（512/4096/--queue-capacity）"
                   "的矩阵对比\n"
                << "  默认线程数留一核给后台 worker（Apple 上取性能核）\n";
      std::exit(0);
    } else {
      throw std::runtime_error("unknown or incomplete option: " + argument);
    }
  }
  return options;
}

class BenchmarkDirectory {
public:
  BenchmarkDirectory() {
    const auto stamp = Clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("lyflog-benchmark-" + std::to_string(stamp));
    fs::create_directories(path_);
  }

  ~BenchmarkDirectory() {
    lyflog::Logger::instance().shutdown();
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] fs::path file(const std::string &name) const {
    return path_ / name;
  }

private:
  fs::path path_;
};

struct Result {
  std::string name;
  std::size_t threads = 0;
  std::size_t messages = 0;
  double avg_ns = 0.0;
  double p50_ns = 0.0;
  double p99_ns = 0.0;
  double p999_ns = 0.0;
  double max_ns = 0.0;
  double end_to_end_seconds = 0.0;
  std::uintmax_t bytes = 0;
  std::size_t dropped = 0;
};

Result run_benchmark(const std::string &name, std::size_t thread_count,
                     std::size_t message_count, std::size_t queue_capacity,
                     lyflog::OverflowPolicy policy, const fs::path &path) {
  lyflog::LogConfig config;
  config.set_file_path(path.string())
      .set_level(lyflog::Level::Info)
      .set_with_thread_id(true)
      .set_daily_rotate(false)
      .set_file_buffer_size(1024 * 1024)
      .set_batch_size(4096)
      .set_queue_capacity(queue_capacity)
      .set_buffer_pool_size(32768)
      .set_tls_buffer_count(64)
      .set_overflow_policy(policy);

  auto &logger = lyflog::Logger::instance();
  logger.init(config);
  const auto baseline = logger.stats();

  std::vector<std::thread> producers;
  producers.reserve(thread_count);
  std::vector<double> latencies_ns(message_count, 0.0);
  std::latch ready(static_cast<std::ptrdiff_t>(thread_count));
  std::latch start(1);
  std::latch done(static_cast<std::ptrdiff_t>(thread_count));
  const std::size_t base_count = message_count / thread_count;
  const std::size_t remainder = message_count % thread_count;

  for (std::size_t thread_id = 0; thread_id < thread_count; ++thread_id) {
    const std::size_t count = base_count + (thread_id < remainder ? 1 : 0);
    const std::size_t offset =
        thread_id * base_count + std::min(thread_id, remainder);
    producers.emplace_back([&, thread_id, count, offset] {
      ready.count_down();
      start.wait();
      for (std::size_t sequence = 0; sequence < count; ++sequence) {
        const auto submit_begin = Clock::now();
        LYF_INFO("benchmark thread={} sequence={} value={:.3f}", thread_id,
                 sequence, static_cast<double>(sequence) / 10.0);
        latencies_ns[offset + sequence] =
            std::chrono::duration<double, std::nano>(Clock::now() -
                                                     submit_begin)
                .count();
      }
      done.count_down();
    });
  }

  ready.wait();
  const auto begin = Clock::now();
  start.count_down();
  done.wait();
  for (auto &producer : producers) {
    producer.join();
  }

  const auto stats = logger.stats();
  const std::size_t dropped =
      (stats.dropped_overflow - baseline.dropped_overflow) +
      (stats.dropped_no_file - baseline.dropped_no_file) +
      (stats.shutdown_drop - baseline.shutdown_drop);
  if (policy == lyflog::OverflowPolicy::Block && dropped != 0) {
    throw std::runtime_error("Block 策略下出现日志丢弃");
  }
  // 多生产者日志通过 shutdown() 全局排空。
  logger.shutdown();
  const auto flushed = Clock::now();

  std::error_code error;
  const std::uintmax_t bytes = fs::file_size(path, error);
  if (error) {
    throw std::runtime_error("cannot inspect benchmark output: " +
                             error.message());
  }

  long double total_ns = 0.0;
  double max_ns = latencies_ns.front();
  for (const double latency : latencies_ns) {
    total_ns += latency;
    max_ns = std::max(max_ns, latency);
  }
  // 分位数（nearest-rank，自顶向下数 tail_count 条的边界）：
  // p99 是 Block 反压等待是否越过 1% 阈值的观测点，而 p99.9/max 才是
  // 等待的真实聚集区——只看 p99 会误判两种策略差异不大。
  auto percentile = [&latencies_ns](std::size_t tail_count) {
    const std::size_t index =
        latencies_ns.size() > tail_count ? latencies_ns.size() - tail_count - 1
                                         : 0;
    std::nth_element(latencies_ns.begin(),
                     latencies_ns.begin() + static_cast<std::ptrdiff_t>(index),
                     latencies_ns.end());
    return latencies_ns[index];
  };
  const double p50 = percentile(latencies_ns.size() / 2);
  const double p99 = percentile(latencies_ns.size() / 100);
  const double p999 = percentile(latencies_ns.size() / 1000);

  return {name,
          thread_count,
          message_count,
          static_cast<double>(total_ns / message_count),
          p50,
          p99,
          p999,
          max_ns,
          std::chrono::duration<double>(flushed - begin).count(),
          bytes,
          dropped};
}

void print_header() {
  std::cout << std::left << std::setw(20) << "case" << std::right
            << std::setw(9) << "threads" << std::setw(12) << "messages"
            << std::setw(11) << "avg ns" << std::setw(11) << "p50 ns"
            << std::setw(11) << "p99 ns" << std::setw(12) << "p99.9 ns"
            << std::setw(13) << "max ns" << std::setw(11) << "MiB/s"
            << std::setw(18) << "dropped" << '\n';
}

void print_result(const Result &result) {
  constexpr double kMiB = 1024.0 * 1024.0;
  const double bandwidth =
      (static_cast<double>(result.bytes) / kMiB) / result.end_to_end_seconds;

  std::string dropped;
  if (result.dropped == 0) {
    dropped = "0";
  } else {
    std::ostringstream stream;
    stream << result.dropped << " (" << std::fixed << std::setprecision(1)
           << 100.0 * static_cast<double>(result.dropped) / result.messages
           << "%)";
    dropped = stream.str();
  }

  std::cout << std::left << std::setw(20) << result.name << std::right
            << std::setw(9) << result.threads << std::setw(12)
            << result.messages << std::fixed << std::setprecision(1)
            << std::setw(11) << result.avg_ns << std::setw(11) << result.p50_ns
            << std::setw(11) << result.p99_ns << std::setw(12) << result.p999_ns
            << std::setw(13) << result.max_ns << std::setw(11) << bandwidth
            << std::setw(18) << dropped << '\n';
}

// 扫描容量档位：小（必溢出）/ 中（间歇溢出）/ 配置值（常不溢出）。
// 只有 dropped > 0 的行才说明溢出策略真的被触发，对比才有意义。
std::vector<std::size_t> sweep_capacities(std::size_t configured) {
  std::vector<std::size_t> capacities{512, 4096, configured};
  std::sort(capacities.begin(), capacities.end());
  capacities.erase(std::unique(capacities.begin(), capacities.end()),
                   capacities.end());
  return capacities;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parse_options(argc, argv);
    BenchmarkDirectory directory;

    std::cout
        << "lyflog benchmark (includes formatting, queueing, and file I/O)\n"
        << "messages per case: " << options.messages
        << ", producer threads: " << options.threads << ", worker: 1\n\n";

    if (options.sweep) {
      print_header();
      for (const auto policy :
           {lyflog::OverflowPolicy::Block, lyflog::OverflowPolicy::Drop}) {
        for (const std::size_t capacity :
             sweep_capacities(options.queue_capacity)) {
          const std::string name =
              std::string(policy_name(policy)) + " cap=" +
              std::to_string(capacity);
          print_result(run_benchmark(
              name, options.threads, options.messages, capacity, policy,
              directory.file(name + ".log")));
        }
      }
      return 0;
    }

    std::cout << "policy: " << policy_name(options.policy)
              << ", queue capacity: " << options.queue_capacity << "\n\n";
    print_header();
    print_result(run_benchmark("single-thread", 1, options.messages,
                               options.queue_capacity, options.policy,
                               directory.file("single.log")));
    print_result(run_benchmark("multi-thread", options.threads,
                               options.messages, options.queue_capacity,
                               options.policy, directory.file("multi.log")));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return 1;
  }
}
