#include "lyflog.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

class TestDirectory {
public:
  TestDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ =
        fs::temp_directory_path() / ("lyflog-tests-" + std::to_string(stamp));
    fs::create_directories(path_);
  }

  ~TestDirectory() {
    lyflog::Logger::instance().shutdown();
    std::error_code error;
    fs::remove_all(path_, error);
  }

  TestDirectory(const TestDirectory &) = delete;
  TestDirectory &operator=(const TestDirectory &) = delete;

  [[nodiscard]] fs::path file(const std::string &name) const {
    return path_ / name;
  }

  [[nodiscard]] const fs::path &path() const { return path_; }

private:
  fs::path path_;
};

lyflog::LogConfig test_config(const fs::path &path,
                              lyflog::Level level = lyflog::Level::Debug) {
  lyflog::LogConfig config;
  config.set_file_path(path.string())
      .set_level(level)
      .set_daily_rotate(false)
      .set_max_file_size(0)
      .set_file_buffer_size(4096)
      .set_batch_size(128)
      .set_queue_capacity(4096)
      .set_buffer_pool_size(4096)
      .set_tls_buffer_count(8)
      .set_overflow_policy(lyflog::OverflowPolicy::Block);
  return config;
}

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.is_open());
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::size_t count_occurrences(const std::string &text,
                              const std::string &needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

} // namespace

TEST_CASE("level names and config setters", "[config]") {
  CHECK(std::string(lyflog::level_name(lyflog::Level::Debug)) == "DEBUG");
  CHECK(std::string(lyflog::level_name(lyflog::Level::Info)) == "INFO");
  CHECK(std::string(lyflog::level_name(lyflog::Level::Warn)) == "WARN");
  CHECK(std::string(lyflog::level_name(lyflog::Level::Error)) == "ERROR");
  CHECK(std::string(lyflog::level_name(lyflog::Level::Fatal)) == "FATAL");

  lyflog::LogConfig config;
  config.set_file_path("custom.log")
      .set_level(lyflog::Level::Warn)
      .set_with_thread_id(true)
      .set_daily_rotate(false)
      .set_retain_days(14)
      .set_max_file_size(1024)
      .set_retain_count(5)
      .set_overflow_policy(lyflog::OverflowPolicy::Drop)
      .set_fatal_sync_flush(false);

  CHECK(config.file_path == "custom.log");
  CHECK(config.level == lyflog::Level::Warn);
  CHECK(config.with_thread_id);
  CHECK_FALSE(config.daily_rotate);
  CHECK(config.retain_days == 14);
  CHECK(config.max_file_size == 1024);
  CHECK(config.retain_count == 5);
  CHECK(config.overflow_policy == lyflog::OverflowPolicy::Drop);
  CHECK_FALSE(config.fatal_sync_flush);
}

TEST_CASE("formatting, filtering, and runtime level", "[logging][level]") {
  TestDirectory directory;
  const fs::path path = directory.file("format.log");
  auto &logger = lyflog::Logger::instance();
  logger.init(test_config(path));

  LYF_DEBUG("debug-marker value={:04d}", 7);
  LYF_INFO("format-marker pi={:.2f} hex={:x}", 3.14159, 255);
  logger.set_level(lyflog::Level::Warn);
  LYF_INFO("filtered-marker");
  LYF_WARN("warn-marker");
  logger.sync();

  const std::string output = read_file(path);
  CHECK(output.find("DEBUG") != std::string::npos);
  CHECK(output.find("debug-marker value=0007") != std::string::npos);
  CHECK(output.find("format-marker pi=3.14 hex=ff") != std::string::npos);
  CHECK(output.find("filtered-marker") == std::string::npos);
  CHECK(output.find("warn-marker") != std::string::npos);
}

TEST_CASE("fatal logs are synchronously flushed", "[logging][fatal]") {
  TestDirectory directory;
  const fs::path path = directory.file("fatal.log");
  auto &logger = lyflog::Logger::instance();
  logger.init(test_config(path));

  LYF_FATAL("fatal-sync-marker");

  // 不调用 sync()，验证 Fatal 自动落盘。
  CHECK(read_file(path).find("fatal-sync-marker") != std::string::npos);
}

TEST_CASE("interval logging limits a call site", "[logging][interval]") {
  TestDirectory directory;
  const fs::path path = directory.file("interval.log");
  auto &logger = lyflog::Logger::instance();
  logger.init(test_config(path));

  for (int i = 0; i < 20; ++i) {
    LYF_INTERVAL_INFO(60, "interval-marker iteration={}", i);
  }
  logger.sync();

  CHECK(count_occurrences(read_file(path), "interval-marker") == 1);
}

TEST_CASE("concurrent producers do not lose records", "[logging][thread]") {
  TestDirectory directory;
  const fs::path path = directory.file("threads.log");
  auto config = test_config(path);
  config.set_with_thread_id(true).set_queue_capacity(8192);
  auto &logger = lyflog::Logger::instance();
  logger.init(config);

  constexpr int kThreadCount = 4;
  constexpr int kMessagesPerThread = 100;
  std::vector<std::thread> threads;
  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([thread_id] {
      for (int sequence = 0; sequence < kMessagesPerThread; ++sequence) {
        LYF_INFO("concurrent-marker thread={} sequence={}", thread_id,
                 sequence);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  // sync() 仅排序当前生产者；shutdown() 才会排空全局队列。
  logger.shutdown();

  CHECK(count_occurrences(read_file(path), "concurrent-marker") ==
        kThreadCount * kMessagesPerThread);
}

TEST_CASE("size rotation honors retain count", "[logging][rotation]") {
  TestDirectory directory;
  const fs::path base_path = directory.file("rotate.log");
  auto config = test_config(base_path);
  config.set_max_file_size(512).set_retain_count(3).set_file_buffer_size(128);
  auto &logger = lyflog::Logger::instance();
  logger.init(config);

  const std::string payload(96, 'x');
  for (int i = 0; i < 100; ++i) {
    LYF_INFO("rotation-marker index={} payload={}", i, payload);
  }
  logger.shutdown();

  std::size_t rotated_file_count = 0;
  for (const auto &entry : fs::directory_iterator(directory.path())) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file() && name.rfind("rotate.", 0) == 0 &&
        entry.path().extension() == ".log") {
      ++rotated_file_count;
    }
  }
  CHECK(rotated_file_count >= 2);
  CHECK(rotated_file_count <= 3);
}

TEST_CASE("stats expose runtime configuration", "[stats]") {
  TestDirectory directory;
  const fs::path path = directory.file("stats.log");
  auto config = test_config(path, lyflog::Level::Warn);
  config.set_queue_capacity(1234).set_overflow_policy(
      lyflog::OverflowPolicy::Drop);
  auto &logger = lyflog::Logger::instance();
  logger.init(config);

  auto stats = logger.stats();
  CHECK(stats.running);
  CHECK(stats.level == lyflog::Level::Warn);
  CHECK(stats.queue_capacity == 1234);
  CHECK(stats.overflow_policy == lyflog::OverflowPolicy::Drop);

  logger.set_level(lyflog::Level::Debug);
  CHECK(logger.stats().level == lyflog::Level::Debug);
  logger.shutdown();
  CHECK_FALSE(logger.stats().running);
}
