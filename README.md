# HighPerformance-AsyncLogSystem

一个专注极致吞吐与低延迟的高性能异步日志系统，面向多线程场景，采用无锁队列与批量消费降低提交开销，配合可配置背压策略与多种 Sink（控制台、文件、HTTP）。支持按需初始化、从配置文件启动、以及日志级别热更新。


## 特性

- 异步日志提交与批量消费
- 可配置背压策略（DROP/BLOCK）
- 线程本地缓冲与全局缓冲池
- 多种 Sink：Console / File / HTTP
- 文件日志按日或按大小轮转
- 基于 toml 的配置与级别热更新

## 性能与压测

性能压测入口为 main.cpp，对日志提交与同步进行统计，输出吞吐、延迟与内存占用信息。建议在空闲机器上运行，以减少外部噪声。

```bash
./build/main --threads 4 --logs 10000000 --warmup_logs 0 --policy BLOCK --capacity 65536 --sink file --buffer_pool 65536 --log_file app.log --sample_rate 1000
```

关键参数说明：

- threads：线程数（0 表示硬件并发）
- logs：总日志条数
- warmup_logs：预热条数
- policy：队列满策略（BLOCK 或 DROP）
- capacity：队列容量
- sink：输出 sink（console 或 file）
- log_file：文件路径（仅 file）
- sample_rate：延迟采样间隔（每 N 条采 1 次）

4 线程压测结果（文件 sink）：

```text
Before benchmark - Memory: 336.234 MB (resident)
After submit - Memory: 347.734 MB (resident)
After sync - Memory: 347.734 MB (resident)
threads: 4
logs: 10000000
policy: BLOCK
capacity: 65536
buffer pool size: 65536
total time: 1.80699 s
submit time: 1.80593 s
sync time: 0.00106175 s
shutdown time: 0.0227422 s
avg submit latency: 691.405 ns
min/max latency: 0/2826208 ns
p50/p95/p99/p999: 291/625/958/269834 ns
logfile: app.log
logfile size: 1433 MB
avg throughput: 793.032 MB/s
drop count: 0
line count: 10000000
```

## 构建

前置要求：CMake >= 3.20，C++20 编译器。

```bash
cmake -S . -B build
cmake --build build
```

生成的可执行文件：

- build/main
- build/http_sink_example

## 快速开始

### 代码方式初始化

```cpp
#include "Logger.h"
#include "sinks/ConsoleSink.h"

using namespace lyf;

int main() {
  // 不指定file路径为空 SetLogPath("")时，
  // 默认自动提供一个FileSink指向默认日志文件logfile.log
  LogConfig cfg;
  cfg.SetLevel(LogLevel::INFO);
  Logger::Instance().Init(cfg);

  // 添加ConsoleSink，日志同时输出到控制台
  Logger::Instance().AddSink(std::make_shared<ConsoleSink>());

  INFO("hello {}", 42);
  Logger::Instance().Sync();
  return 0;
}
```

### 从配置文件启动

```cpp
#include "Logger.h"

using namespace lyf;

int main() {
  Logger::Instance().InitFromConfig("config.toml");
  INFO("log from config");
  Logger::Instance().Sync();  // 等待当前日志写入完成
  return 0;
}
```

## 配置示例

```toml
[logger]
level = "INFO"
full_policy = "BLOCK"
time_format = "%Y-%m-%d %H:%M:%S"

[logger.performance]
worker_batch_size = 2048
queue_capacity = 65536
queue_block_timeout_us = -1
buffer_pool_size = 40960
tls_buffer_count = 64

[sink.file]
log_path = "logfile.log"
file_buffer_size_kb = 128
rotate_policy = "NONE"
rotate_size_mb = 1024
max_rotate_files = 7

[sink.console]
console_buffer_size_kb = 16

[other]
reload_interval_ms = 1000
```

## HttpSink 示例

```cpp
#include "LogConfig.h"
#include "Logger.h"
#include "sinks/HttpSink.h"

using namespace lyf;

int main() {
  auto log_cfg = LogConfig()
                  .SetLevel(LogLevel::DEBUG)
                  .SetLogPath("") // 显示指定为空，不启用默认的FileSink
                  .SetWorkerBatchSize(1024);
  Logger::Instance().Init(log_cfg);

  HttpSinkConfig http_cfg;
  http_cfg.host = "127.0.0.1";
  http_cfg.port = 8080;
  http_cfg.endpoint = "/api/logs";
  http_cfg.content_type = "application/json";
  http_cfg.timeout_ms = 5000;
  http_cfg.max_retries = 3;
  http_cfg.batch_size = 50;
  http_cfg.flush_interval_ms = 3000;

  Logger::Instance().AddSink(std::make_shared<HttpSink>(http_cfg));

  INFO("http sink {}", 1);
  Logger::Instance().Sync();
  return 0;
}
```

## 运行与测试

运行示例：

```bash
./build/main
./build/http_sink_example
```

单元测试与基准测试由 CMake 选项控制：

- BUILD_TESTING=ON 构建单元测试
- BUILD_BENCHMARKS=ON 构建基准测试

运行测试：

```bash
ctest --test-dir build
# 或 ./build/test/unit_tests
```
