// HttpSink 使用示例
#include "LogConfig.h"
#include "Logger.h"
#include "sinks/HttpSink.h"

using namespace lyf;

int main() {
  // 初始化日志系统
  auto log_cfg =
      LogConfig()
          .SetLevel(LogLevel::DEBUG)
          .SetLogPath("") // filesink 路径设置为空，避免自动添加 filesink
          .SetWorkerBatchSize(1024);
  Logger::Instance().Init(log_cfg);

  std::cout << "=== HttpSink使用示例 ===" << std::endl;

  // 创建 HttpSink
  HttpSinkConfig http_cfg;
  http_cfg.host = "127.0.0.1";
  http_cfg.port = 8080;
  http_cfg.endpoint = "/api/logs"; // API端点
  http_cfg.content_type = "application/json";
  http_cfg.timeout_ms = 5000;
  http_cfg.max_retries = 3;
  http_cfg.batch_size = 50;
  http_cfg.flush_interval_ms = 3000;
  auto httpSink = std::make_shared<HttpSink>(http_cfg);

  // 添加到Sink管理器
  Logger::Instance().AddSink(std::move(httpSink));

  // 记录一些测试日志
  DEBUG("这是一条DEBUG级别的日志");
  INFO("这是一条INFO级别的日志");
  WARN("这是一条WARNING级别的日志");
  ERROR("这是一条ERROR级别的日志");

  // 批量日志测试
  for (int i = 0; i < 100; ++i) {
    INFO("批量日志测试 - 消息编号: {}", i);
  }

  // 刷新所有日志
  Logger::Instance().Sync();
  std::cout << "日志已刷新" << std::endl;

  return 0;
}
