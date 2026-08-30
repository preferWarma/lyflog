# lyflog

`lyflog` 是一个基于 C++17 的单头文件异步日志库，使用 `fmt` 格式化，使用
`moodycamel::ConcurrentQueue` 传递日志。

## 特性

- Debug、Info、Warn、Error、Fatal 五级日志
- 异步批量写入，支持 Block 和 Drop 两种队列溢出策略
- 按天或文件大小轮转，支持保留天数和文件数限制
- 运行时调整日志级别
- 多线程写入、调用点限流和线程 ID
- 同步 Fatal、主动刷新和运行指标

## 快速开始

```cpp
#include "lyflog.h"

int main() {
  lyflog::LogConfig config;
  config.set_file_path("logs/app.log")
      .set_level(lyflog::Level::Debug)
      .set_with_thread_id(true)
      .set_daily_rotate(false);

  auto &logger = lyflog::Logger::instance();
  logger.init(config);

  LYF_DEBUG("request_id={}", 42);
  LYF_INFO("server started at {}:{}", "127.0.0.1", 8080);
  LYF_WARN("queue usage={:.1f}%", 85.2);
  LYF_ERROR("request failed: {}", "timeout");

  logger.shutdown();
}
```

未显式调用 `init()` 时，首次使用日志宏会自动加载默认配置。

## 日志宏

```cpp
LYF_DEBUG("value={}", value);
LYF_INFO("value={}", value);
LYF_WARN("value={}", value);
LYF_ERROR("value={}", value);
LYF_FATAL("value={}", value);

// 此调用点每 0.5 秒最多输出一次
LYF_INTERVAL_INFO(0.5, "heartbeat={}", heartbeat);
```

单条日志正文最多 4096 字节，超出部分会被截断。

## 常用配置

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `file_path` | `app.log` | 日志文件路径 |
| `level` | `Info` | 最低日志级别 |
| `with_thread_id` | `false` | 输出线程 ID |
| `daily_rotate` | `true` | 按天轮转 |
| `retain_days` | `7` | 保留天数，0 表示不限制 |
| `max_file_size` | `0` | 按大小轮转，0 表示关闭 |
| `retain_count` | `0` | 轮转文件上限，0 表示不限制 |
| `queue_capacity` | `65536` | 队列软上限 |
| `overflow_policy` | `Block` | 队列满时阻塞或丢弃 |
| `fatal_sync_flush` | `true` | Fatal 日志同步落盘 |

完整配置见 [`LogConfig`](lyflog.h)。

## 刷新与关闭

- `sync()`：等待调用线程此前提交的日志落盘，不排空其他生产者。
- `shutdown()`：停止后台线程并排空全局队列。

多线程程序应先停止或汇合生产线程，再调用 `shutdown()`。

## 构建

CMake 会自动下载 `fmt` 和 `concurrentqueue`。

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build -j
./build/example
```

## 测试与性能测试

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/tests/lyflog_benchmark --messages 1000000 --threads 8
```

性能测试报告生产延时的平均值、最小值、最大值、P99 和完整落盘带宽。详细说明见
[`tests/README.md`](tests/README.md)。
