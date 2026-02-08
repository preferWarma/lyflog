#include "LogConfig.h"
#include "LogMessage.h"
#include "Logger_impl.h"
#include "sinks/ConsoleSink.h"
#include "sinks/FileSink.h"
#include "sinks/HttpSink.h"

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

using namespace lyf;

// ==================== Sink 管理接口测试 ====================

class SinkManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "sink_manager_test";
    std::filesystem::create_directories(test_dir_);
    log_path_ = test_dir_ / "test.log";

    LogConfig config;
    logger_ = std::make_unique<AsyncLogger>(config);
    logger_->ClearSinks();
    buffer_pool_ = std::make_unique<BufferPool>(100);
  }

  void TearDown() override {
    logger_.reset();
    buffer_pool_.reset();
    if (std::filesystem::exists(test_dir_)) {
      std::filesystem::remove_all(test_dir_);
    }
  }

  LogMessage CreateLogMessage(std::string_view content,
                              LogLevel level = LogLevel::INFO) {
    LogBuffer *buf = buffer_pool_->Alloc();
    size_t copy_len = std::min(content.size(), LogBuffer::SIZE - 1);
    std::memcpy(buf->data, content.data(), copy_len);
    buf->length = copy_len;
    buf->data[copy_len] = '\0';
    return LogMessage(level, "test.cpp", 42, std::this_thread::get_id(), buf,
                      buffer_pool_.get());
  }

  std::filesystem::path test_dir_;
  std::filesystem::path log_path_;
  std::unique_ptr<AsyncLogger> logger_;
  std::unique_ptr<BufferPool> buffer_pool_;
};

// 测试：添加单个 Sink
TEST_F(SinkManagerTest, AddSingleSink) {
  auto console = std::make_shared<ConsoleSink>();
  EXPECT_TRUE(logger_->AddSink(console));
  EXPECT_EQ(logger_->GetSinkCount(), 1);
  EXPECT_TRUE(logger_->HasSink<ConsoleSink>());
}

// 测试：添加多个不同类型的 Sink
TEST_F(SinkManagerTest, AddMultipleDifferentSinks) {
  auto console = std::make_shared<ConsoleSink>();
  auto file = std::make_shared<FileSink>(log_path_.string());
  EXPECT_TRUE(logger_->AddSink(console));
  EXPECT_TRUE(logger_->AddSink(file));
  EXPECT_EQ(logger_->GetSinkCount(), 2);
  EXPECT_TRUE(logger_->HasSink<ConsoleSink>());
  EXPECT_TRUE(logger_->HasSink<FileSink>());
}

// 测试：重复添加同类型 Sink 应该失败
TEST_F(SinkManagerTest, AddDuplicateSinkTypeFails) {
  auto console1 = std::make_shared<ConsoleSink>();
  auto console2 = std::make_shared<ConsoleSink>();
  EXPECT_TRUE(logger_->AddSink(console1));
  EXPECT_FALSE(logger_->AddSink(console2)); // 同类型已存在
  EXPECT_EQ(logger_->GetSinkCount(), 1);
  EXPECT_TRUE(logger_->HasSink<ConsoleSink>());
}

// 测试：HasSink 检查
TEST_F(SinkManagerTest, HasSinkCheck) {
  EXPECT_FALSE(logger_->HasSink<ConsoleSink>());
  EXPECT_FALSE(logger_->HasSink<FileSink>());
  auto console = std::make_shared<ConsoleSink>();
  logger_->AddSink(console);
  EXPECT_TRUE(logger_->HasSink<ConsoleSink>());
  EXPECT_FALSE(logger_->HasSink<FileSink>());
}

// 测试：RemoveSink 移除 Sink
TEST_F(SinkManagerTest, RemoveSink) {
  auto console = std::make_shared<ConsoleSink>();
  logger_->AddSink(console);
  EXPECT_TRUE(logger_->RemoveSink<ConsoleSink>());
  EXPECT_EQ(logger_->GetSinkCount(), 0);
  EXPECT_FALSE(logger_->HasSink<ConsoleSink>());
}

// 测试：移除不存在的 Sink 应该返回 false
TEST_F(SinkManagerTest, RemoveNonExistentSink) {
  EXPECT_FALSE(logger_->RemoveSink<ConsoleSink>());
  EXPECT_FALSE(logger_->RemoveSink<FileSink>());
}

// 测试：GetSink 获取 Sink
TEST_F(SinkManagerTest, GetSink) {
  auto console = std::make_shared<ConsoleSink>();
  logger_->AddSink(console);
  auto retrieved = logger_->GetSink<ConsoleSink>();
  EXPECT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved.get(), console.get());
  auto file_sink = logger_->GetSink<FileSink>();
  EXPECT_EQ(file_sink, nullptr);
}

// 测试：ClearSinks 清空所有 Sink
TEST_F(SinkManagerTest, ClearSinks) {
  auto console = std::make_shared<ConsoleSink>();
  auto file = std::make_shared<FileSink>(log_path_.string());

  logger_->AddSink(console);
  logger_->AddSink(file);
  EXPECT_EQ(logger_->GetSinkCount(), 2);

  logger_->ClearSinks();
  EXPECT_EQ(logger_->GetSinkCount(), 0);
  EXPECT_FALSE(logger_->HasSink<ConsoleSink>());
  EXPECT_FALSE(logger_->HasSink<FileSink>());
}

// 测试：添加 HttpSink
TEST_F(SinkManagerTest, AddHttpSink) {
  HttpSinkConfig http_config;
  http_config.host = "127.0.0.1";
  http_config.port = 18080;
  http_config.endpoint = "/api/logs";
  auto http_sink = std::make_shared<HttpSink>(http_config);
  EXPECT_TRUE(logger_->AddSink(http_sink));
  EXPECT_EQ(logger_->GetSinkCount(), 1);
  EXPECT_TRUE(logger_->HasSink<HttpSink>());
}

// 测试：混合添加多种 Sink
TEST_F(SinkManagerTest, AddMixedSinks) {
  auto console = std::make_shared<ConsoleSink>();
  auto file = std::make_shared<FileSink>(log_path_.string());
  HttpSinkConfig http_config;
  http_config.host = "127.0.0.1";
  http_config.port = 18080;
  http_config.endpoint = "/api/logs";
  auto http = std::make_shared<HttpSink>(http_config);

  EXPECT_TRUE(logger_->AddSink(console));
  EXPECT_TRUE(logger_->AddSink(file));
  EXPECT_TRUE(logger_->AddSink(http));
  EXPECT_EQ(logger_->GetSinkCount(), 3);
  EXPECT_TRUE(logger_->HasSink<ConsoleSink>());
  EXPECT_TRUE(logger_->HasSink<FileSink>());
  EXPECT_TRUE(logger_->HasSink<HttpSink>());
}

// 测试：通过 GetSink 修改 Sink 配置
TEST_F(SinkManagerTest, ModifySinkViaGetSink) {
  auto console = std::make_shared<ConsoleSink>();
  logger_->AddSink(console);
  auto retrieved = logger_->GetSink<ConsoleSink>();
  ASSERT_NE(retrieved, nullptr);
  // 修改配置
  LogConfig new_config;
  retrieved->ApplyConfig(new_config);
  // 验证是同一个对象
  EXPECT_EQ(retrieved.get(), console.get());
}

// 测试：添加后 Sink 能正常接收日志
TEST_F(SinkManagerTest, SinkReceivesLogs) {
  // 打开并清空文件内容
  std::fstream log_file(log_path_,
                        std::ios::trunc | std::ios::in | std::ios::out);
  EXPECT_TRUE(log_file.is_open());
  EXPECT_EQ(std::filesystem::file_size(log_path_), 0);

  auto file = std::make_shared<FileSink>(log_path_.string());
  logger_->AddSink(file);
  // 发送日志
  for (int i = 0; i < 5; ++i) {
    auto msg = CreateLogMessage("Test message " + std::to_string(i));
    logger_->Commit(std::move(msg));
  }
  logger_->Sync();

  // 验证文件中有内容
  EXPECT_TRUE(std::filesystem::exists(log_path_));
  EXPECT_GT(std::filesystem::file_size(log_path_), 0);

  std::string line;
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(std::getline(log_file, line));
    EXPECT_TRUE(line.find("Test message " + std::to_string(i)) !=
                std::string::npos);
  }
}

// 测试：移除 Sink 后不再接收日志
TEST_F(SinkManagerTest, RemovedSinkStopsReceiving) {
  std::fstream log_file(log_path_,
                        std::ios::trunc | std::ios::in | std::ios::out);
  EXPECT_TRUE(log_file.is_open());
  EXPECT_EQ(std::filesystem::file_size(log_path_), 0);

  auto file = std::make_shared<FileSink>(log_path_.string());
  logger_->AddSink(file);

  // 发送第一条日志
  auto msg1 = CreateLogMessage("First message");
  logger_->Commit(std::move(msg1));
  logger_->Sync();

  auto size_after_first = std::filesystem::file_size(log_path_);
  EXPECT_GT(size_after_first, 0);

  // 移除 Sink
  logger_->RemoveSink<FileSink>();

  // 发送第二条日志（应该不会写入文件）
  auto msg2 = CreateLogMessage("Second message");
  logger_->Commit(std::move(msg2));
  logger_->Sync();

  // 文件大小应该不变
  EXPECT_EQ(std::filesystem::file_size(log_path_), size_after_first);
}

// 测试：并发添加 Sink（线程安全）
TEST_F(SinkManagerTest, ConcurrentAddSink) {
  const int thread_count = 10;
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int i = 0; i < thread_count; ++i) {
    threads.emplace_back([this, &success_count]() {
      auto console = std::make_shared<ConsoleSink>();
      if (logger_->AddSink(console)) {
        success_count++;
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  // 只有一个应该成功
  EXPECT_EQ(success_count.load(), 1);
  EXPECT_EQ(logger_->GetSinkCount(), 1);
}

// 测试：并发读写 Sink（线程安全）
TEST_F(SinkManagerTest, ConcurrentReadWrite) {
  auto file = std::make_shared<FileSink>(log_path_.string());
  logger_->AddSink(file);

  std::atomic<bool> stop{false};
  std::atomic<int> read_count{0};

  // 读线程：不断检查 HasSink
  std::thread reader([this, &stop, &read_count]() {
    while (!stop) {
      logger_->HasSink<FileSink>();
      read_count++;
    }
  });

  // 写线程：添加和移除 ConsoleSink
  std::thread writer([this]() {
    for (int i = 0; i < 100; ++i) {
      auto console = std::make_shared<ConsoleSink>();
      logger_->AddSink(console);
      logger_->RemoveSink<ConsoleSink>();
    }
  });

  writer.join();
  stop = true;
  reader.join();

  // 应该没有崩溃，且读操作执行了很多次
  EXPECT_GT(read_count.load(), 0);
  EXPECT_EQ(logger_->GetSinkCount(), 1); // 只有 FileSink
}

// 测试：添加 nullptr 应该失败
TEST_F(SinkManagerTest, AddNullSink) {
  std::shared_ptr<ConsoleSink> null_sink = nullptr;
  EXPECT_FALSE(logger_->AddSink(null_sink));
  EXPECT_EQ(logger_->GetSinkCount(), 0);
}

// 测试：多次添加移除同一类型
TEST_F(SinkManagerTest, MultipleAddRemoveCycles) {
  for (int i = 0; i < 10; ++i) {
    auto console = std::make_shared<ConsoleSink>();
    EXPECT_TRUE(logger_->AddSink(console));
    EXPECT_TRUE(logger_->HasSink<ConsoleSink>());
    EXPECT_EQ(logger_->GetSinkCount(), 1);

    EXPECT_TRUE(logger_->RemoveSink<ConsoleSink>());
    EXPECT_FALSE(logger_->HasSink<ConsoleSink>());
    EXPECT_EQ(logger_->GetSinkCount(), 0);
  }
}

// 测试：移除后重新添加同类型
TEST_F(SinkManagerTest, RemoveThenReAdd) {
  auto console1 = std::make_shared<ConsoleSink>();
  logger_->AddSink(console1);

  EXPECT_TRUE(logger_->RemoveSink<ConsoleSink>());

  auto console2 = std::make_shared<ConsoleSink>();
  EXPECT_TRUE(logger_->AddSink(console2)); // 应该可以添加
  EXPECT_EQ(logger_->GetSinkCount(), 1);
}
