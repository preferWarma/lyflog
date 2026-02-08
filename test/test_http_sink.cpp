#include "LogConfig.h"
#include "LogMessage.h"
#include "sinks/HttpSink.h"
#include "third/httplib.h"

#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace lyf;

// 模拟 HTTP 服务器用于测试
class MockHttpServer {
public:
  MockHttpServer(int port)
      : port_(port), server_(std::make_unique<httplib::Server>()) {
    SetupRoutes();
  }

  void Start() {
    server_thread_ =
        std::thread([this]() { server_->listen("127.0.0.1", port_); });
    // 等待服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  void Stop() {
    server_->stop();
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
  }

  // 获取接收到的请求数量
  size_t GetRequestCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return request_count_;
  }

  // 获取最后接收到的请求体
  std::string GetLastRequestBody() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_request_body_;
  }

  // 设置响应状态码
  void SetResponseStatus(int status) {
    std::lock_guard<std::mutex> lock(mutex_);
    response_status_ = status;
  }

  // 获取接收到的日志条目数量
  size_t GetReceivedLogCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_log_count_;
  }

  // 重置计数器
  void ResetCounters() {
    std::lock_guard<std::mutex> lock(mutex_);
    request_count_ = 0;
    total_log_count_ = 0;
    last_request_body_.clear();
  }

private:
  void SetupRoutes() {
    server_->Post("/api/logs",
                  [this](const httplib::Request &req, httplib::Response &res) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++request_count_;
                    last_request_body_ = req.body;

                    // 解析 JSON 并统计日志数量
                    try {
                      auto json = nlohmann::json::parse(req.body);
                      if (json.contains("logs") && json["logs"].is_array()) {
                        total_log_count_ += json["logs"].size();
                      }
                    } catch (...) {
                      // 忽略解析错误
                    }

                    res.status = response_status_;
                    res.set_content(R"({"status":"ok"})", "application/json");
                  });

    server_->Post(
        "/api/error", [](const httplib::Request &, httplib::Response &res) {
          res.status = 500;
          res.set_content(R"({"error":"internal error"})", "application/json");
        });

    server_->Post("/api/timeout",
                  [](const httplib::Request &, httplib::Response &res) {
                    // 模拟超时 - 不返回响应
                    res.status = 0;
                  });
  }

  int port_;
  std::unique_ptr<httplib::Server> server_;
  std::thread server_thread_;
  mutable std::mutex mutex_;
  size_t request_count_ = 0;
  size_t total_log_count_ = 0;
  std::string last_request_body_;
  int response_status_ = 200;
};

// HttpSink 测试夹具
class HttpSinkTest : public ::testing::Test {
protected:
  void SetUp() override {
    buffer_pool_ = std::make_unique<BufferPool>(100);
    mock_server_ = std::make_unique<MockHttpServer>(test_port_);
    mock_server_->Start();
  }

  void TearDown() override {
    if (sink_) {
      sink_.reset();
    }
    mock_server_->Stop();
    buffer_pool_.reset();
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

  void CreateSink(const HttpSinkConfig &config) {
    sink_ = std::make_unique<HttpSink>(config);
  }

  void CreateDefaultSink() {
    HttpSinkConfig config;
    config.host = "127.0.0.1";
    config.port = test_port_;
    config.endpoint = "/api/logs";
    config.batch_size = 5;           // 小批量便于测试
    config.flush_interval_ms = 1000; // 短间隔便于测试
    config.timeout_ms = 2000;
    config.max_retries = 2;
    config.retry_interval_ms = 100;
    CreateSink(config);
  }

  static constexpr int test_port_ = 18080;

  std::unique_ptr<BufferPool> buffer_pool_;
  std::unique_ptr<MockHttpServer> mock_server_;
  std::unique_ptr<HttpSink> sink_;
};

// 测试：正常发送日志
TEST_F(HttpSinkTest, NormalSend) {
  CreateDefaultSink();

  // 发送单条日志
  auto msg = CreateLogMessage("Test log message");
  sink_->Log(msg);
  sink_->Flush();

  // 等待发送完成
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_GE(mock_server_->GetRequestCount(), 1);
  EXPECT_GE(mock_server_->GetReceivedLogCount(), 1);

  // 验证请求体格式
  std::string body = mock_server_->GetLastRequestBody();
  EXPECT_FALSE(body.empty());

  auto json = nlohmann::json::parse(body);
  EXPECT_TRUE(json.contains("logs"));
  EXPECT_TRUE(json.contains("count"));
  EXPECT_TRUE(json.contains("timestamp"));
}

// 测试：批量发送
TEST_F(HttpSinkTest, BatchSend) {
  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = test_port_;
  config.endpoint = "/api/logs";
  config.batch_size = 10;
  config.flush_interval_ms = 5000; // 长间隔，确保触发批量发送
  config.timeout_ms = 2000;
  CreateSink(config);

  // 发送多条日志，达到批量阈值
  for (int i = 0; i < 10; ++i) {
    auto msg = CreateLogMessage("Batch log message " + std::to_string(i));
    sink_->Log(msg);
  }

  // 等待批量发送
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_GE(mock_server_->GetReceivedLogCount(), 10);
}

// 测试：Flush 强制发送
TEST_F(HttpSinkTest, FlushForceSend) {
  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = test_port_;
  config.endpoint = "/api/logs";
  config.batch_size = 100; // 大批量，不会自动触发
  config.flush_interval_ms = 10000;
  config.timeout_ms = 2000;
  CreateSink(config);

  // 发送少量日志
  for (int i = 0; i < 3; ++i) {
    auto msg = CreateLogMessage("Flush test message " + std::to_string(i));
    sink_->Log(msg);
  }

  // 未 Flush 前不应发送
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(mock_server_->GetReceivedLogCount(), 0);

  // Flush 后应发送
  sink_->Flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GE(mock_server_->GetReceivedLogCount(), 3);
}

// 测试：Sync 强制发送
TEST_F(HttpSinkTest, SyncForceSend) {
  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = test_port_;
  config.endpoint = "/api/logs";
  config.batch_size = 100;
  config.flush_interval_ms = 10000;
  config.timeout_ms = 2000;
  CreateSink(config);

  auto msg = CreateLogMessage("Sync test message");
  sink_->Log(msg);
  sink_->Sync();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GE(mock_server_->GetReceivedLogCount(), 1);
}

// 测试：服务器错误响应
TEST_F(HttpSinkTest, ServerErrorResponse) {
  mock_server_->SetResponseStatus(500);

  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = test_port_;
  config.endpoint = "/api/logs";
  config.batch_size = 1;
  config.flush_interval_ms = 1000;
  config.timeout_ms = 1000;
  config.max_retries = 1;
  config.retry_interval_ms = 50;
  CreateSink(config);

  auto msg = CreateLogMessage("Error test message");
  sink_->Log(msg);
  sink_->Flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // 应该有错误信息
  std::string error = sink_->GetLastError();
  EXPECT_FALSE(error.empty());
  EXPECT_NE(error.find("500"), std::string::npos);
}

// 测试：连接失败（无效端口）
TEST_F(HttpSinkTest, ConnectionFailure) {
  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = 99999; // 无效端口
  config.endpoint = "/api/logs";
  config.batch_size = 1;
  config.timeout_ms = 500;
  config.max_retries = 0;
  CreateSink(config);

  auto msg = CreateLogMessage("Connection failure test");
  sink_->Log(msg);
  sink_->Flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // 应该有错误信息
  std::string error = sink_->GetLastError();
  EXPECT_FALSE(error.empty());
}

// 测试：配置更新
TEST_F(HttpSinkTest, ConfigUpdate) {
  CreateDefaultSink();

  HttpSinkConfig new_config;
  new_config.host = "127.0.0.1";
  new_config.port = test_port_;
  new_config.endpoint = "/api/logs";
  new_config.batch_size = 20;
  new_config.flush_interval_ms = 2000;

  sink_->SetHttpConfig(new_config);
  HttpSinkConfig current = sink_->GetHttpConfig();

  EXPECT_EQ(current.batch_size, 20);
  EXPECT_EQ(current.flush_interval_ms, 2000);
}

// 测试：自定义请求头
TEST_F(HttpSinkTest, CustomHeaders) {
  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = test_port_;
  config.endpoint = "/api/logs";
  config.batch_size = 1;
  config.headers = {{"X-Custom-Header", "test-value"},
                    {"Authorization", "Bearer token123"}};
  CreateSink(config);

  auto msg = CreateLogMessage("Custom header test");
  sink_->Log(msg);
  sink_->Flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GE(mock_server_->GetRequestCount(), 1);
}

// 测试：不同日志级别
TEST_F(HttpSinkTest, DifferentLogLevels) {
  CreateDefaultSink();

  std::vector<LogMessage> msgs;
  msgs.reserve(5);
  msgs.emplace_back(CreateLogMessage("Debug message", LogLevel::DEBUG));
  msgs.emplace_back(CreateLogMessage("Info message", LogLevel::INFO));
  msgs.emplace_back(CreateLogMessage("Warn message", LogLevel::WARN));
  msgs.emplace_back(CreateLogMessage("Error message", LogLevel::ERROR));
  msgs.emplace_back(CreateLogMessage("Fatal message", LogLevel::FATAL));

  for (const auto &msg : msgs) {
    sink_->Log(msg);
  }
  sink_->Flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GE(mock_server_->GetReceivedLogCount(), 5);
}

// 测试：空日志内容
TEST_F(HttpSinkTest, EmptyLogContent) {
  CreateDefaultSink();

  auto msg = CreateLogMessage("");
  sink_->Log(msg);
  sink_->Flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GE(mock_server_->GetRequestCount(), 1);
}

// 测试：长日志内容
TEST_F(HttpSinkTest, LongLogContent) {
  CreateDefaultSink();

  std::string long_content(3000, 'A');
  auto msg = CreateLogMessage(long_content);
  sink_->Log(msg);
  sink_->Flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GE(mock_server_->GetRequestCount(), 1);
}

// 测试：构造器参数
TEST_F(HttpSinkTest, ConstructorWithParams) {
  sink_ = std::make_unique<HttpSink>("127.0.0.1", test_port_, "/api/logs");

  auto msg = CreateLogMessage("Constructor test");
  sink_->Log(msg);
  sink_->Flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GE(mock_server_->GetRequestCount(), 1);
}

// 测试：待发送计数
TEST_F(HttpSinkTest, PendingCount) {
  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = test_port_;
  config.endpoint = "/api/logs";
  config.batch_size = 100; // 大批量，不会自动触发
  config.flush_interval_ms = 10000;
  CreateSink(config);

  EXPECT_EQ(sink_->GetPendingCount(), 0);

  auto msg1 = CreateLogMessage("Message 1");
  auto msg2 = CreateLogMessage("Message 2");
  sink_->Log(msg1);
  sink_->Log(msg2);

  EXPECT_EQ(sink_->GetPendingCount(), 2);

  sink_->Flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(sink_->GetPendingCount(), 0);
  EXPECT_GE(mock_server_->GetReceivedLogCount(), 2);
}

// 测试：析构时发送剩余日志
TEST_F(HttpSinkTest, DestructorFlush) {
  {
    HttpSinkConfig config;
    config.host = "127.0.0.1";
    config.port = test_port_;
    config.endpoint = "/api/logs";
    config.batch_size = 100;
    config.flush_interval_ms = 10000; // 长间隔，避免自动 Flush
    HttpSink sink(config);

    auto msg = CreateLogMessage("Destructor flush test");
    sink.Log(msg);
    // sink 析构时应自动 Flush
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_GE(mock_server_->GetReceivedLogCount(), 1);
}

// 测试：ApplyConfig
TEST_F(HttpSinkTest, ApplyLogConfig) {
  CreateDefaultSink();

  LogConfig log_config;
  sink_->ApplyConfig(log_config);

  // 应该正常执行，不抛出异常
  auto msg = CreateLogMessage("Apply config test");
  sink_->Log(msg);
  sink_->Flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GE(mock_server_->GetRequestCount(), 1);
}

// 测试：客户端有效性检查
TEST_F(HttpSinkTest, ClientValidity) {
  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = test_port_;
  config.endpoint = "/api/logs";
  CreateSink(config);

  EXPECT_TRUE(sink_->IsClientValid());
}

// 测试：无效端口的客户端
TEST_F(HttpSinkTest, InvalidPortClient) {
  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = 99999; // 无效端口
  config.endpoint = "/api/logs";
  config.max_retries = 0;
  CreateSink(config);

  // 发送请求后应该产生错误
  auto msg = CreateLogMessage("Invalid port test");
  sink_->Log(msg);
  sink_->Flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // 应该有错误信息（连接失败）
  std::string error = sink_->GetLastError();
  EXPECT_FALSE(error.empty());
}

// 性能测试：大量日志发送
TEST_F(HttpSinkTest, HighVolumeSend) {
  HttpSinkConfig config;
  config.host = "127.0.0.1";
  config.port = test_port_;
  config.endpoint = "/api/logs";
  config.batch_size = 50;
  config.flush_interval_ms = 100;
  config.timeout_ms = 5000;
  CreateSink(config);

  const int log_count = 200;
  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < log_count; ++i) {
    auto msg = CreateLogMessage("High volume message " + std::to_string(i));
    sink_->Log(msg);
  }

  sink_->Flush();

  // 等待所有日志发送完成
  int wait_count = 0;
  while (mock_server_->GetReceivedLogCount() < static_cast<size_t>(log_count) &&
         wait_count < 50) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ++wait_count;
  }

  auto end = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  EXPECT_GE(mock_server_->GetReceivedLogCount(),
            static_cast<size_t>(log_count));
  EXPECT_LT(duration.count(), 10000); // 应该在 10 秒内完成
}
