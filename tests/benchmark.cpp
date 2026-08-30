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

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Options {
  std::size_t messages = 200000;
  std::size_t threads =
      std::max<std::size_t>(1, std::thread::hardware_concurrency());
};

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
    } else if (argument == "--help") {
      std::cout << "Usage: lyflog_benchmark [--messages N] [--threads N]\n";
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
  double min_ns = 0.0;
  double max_ns = 0.0;
  double p99_ns = 0.0;
  double end_to_end_seconds = 0.0;
  std::uintmax_t bytes = 0;
};

Result run_benchmark(const std::string &name, std::size_t thread_count,
                     std::size_t message_count, const fs::path &path) {
  lyflog::LogConfig config;
  config.set_file_path(path.string())
      .set_level(lyflog::Level::Info)
      .set_with_thread_id(true)
      .set_daily_rotate(false)
      .set_file_buffer_size(1024 * 1024)
      .set_batch_size(4096)
      .set_queue_capacity(32768)
      .set_buffer_pool_size(32768)
      .set_tls_buffer_count(64)
      .set_overflow_policy(lyflog::OverflowPolicy::Block);

  auto &logger = lyflog::Logger::instance();
  logger.init(config);

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
      stats.dropped_overflow + stats.dropped_no_file + stats.shutdown_drop;
  if (dropped != 0) {
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
  double min_ns = latencies_ns.front();
  double max_ns = latencies_ns.front();
  for (const double latency : latencies_ns) {
    total_ns += latency;
    min_ns = std::min(min_ns, latency);
    max_ns = std::max(max_ns, latency);
  }
  const std::size_t p99_index = message_count - message_count / 100 - 1;
  std::nth_element(latencies_ns.begin(), latencies_ns.begin() + p99_index,
                   latencies_ns.end());

  return {name,
          thread_count,
          message_count,
          static_cast<double>(total_ns / message_count),
          min_ns,
          max_ns,
          latencies_ns[p99_index],
          std::chrono::duration<double>(flushed - begin).count(),
          bytes};
}

void print_result(const Result &result) {
  constexpr double kMiB = 1024.0 * 1024.0;
  const double bandwidth =
      (static_cast<double>(result.bytes) / kMiB) / result.end_to_end_seconds;

  std::cout << std::left << std::setw(14) << result.name << std::right
            << std::setw(9) << result.threads << std::setw(14)
            << result.messages << std::fixed << std::setprecision(1)
            << std::setw(14) << result.avg_ns << std::setw(14) << result.min_ns
            << std::setw(14) << result.max_ns << std::setw(14) << result.p99_ns
            << std::setw(12) << bandwidth << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parse_options(argc, argv);
    BenchmarkDirectory directory;

    std::cout
        << "lyflog benchmark (includes formatting, queueing, and file I/O)\n"
        << "messages per case: " << options.messages << "\n\n"
        << std::left << std::setw(14) << "case" << std::right << std::setw(9)
        << "threads" << std::setw(14) << "messages" << std::setw(14) << "avg ns"
        << std::setw(14) << "min ns" << std::setw(14) << "max ns"
        << std::setw(14) << "p99 ns" << std::setw(12) << "MiB/s" << '\n';

    print_result(run_benchmark("single-thread", 1, options.messages,
                               directory.file("single.log")));
    print_result(run_benchmark("multi-thread", options.threads,
                               options.messages, directory.file("multi.log")));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return 1;
  }
}
