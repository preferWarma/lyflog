# 测试与性能测试

单元测试使用 [Catch2](https://github.com/catchorg/Catch2)，并由 CTest 逐项发现。
CMake 会自动下载指定版本的 Catch2。

配置并构建项目（默认启用测试）：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
```

运行单元测试：

```bash
ctest --test-dir build --output-on-failure
```

运行性能测试：

```bash
# Block 模式（默认）
./build/tests/lyflog_benchmark --messages 1000000 --threads 8

# Drop 模式；
./build/tests/lyflog_benchmark --messages 1000000 --threads 8 \
  --policy drop --queue-capacity 65536
```

指标说明：

- `avg ns`：单条日志生产延时的算术平均值。
- `min ns` / `max ns`：单条日志生产延时的最小值和最大值。
- `p99 ns`：按 nearest-rank 计算的 P99，99% 的生产延时不超过该值。
- `MiB/s`：从生产开始到 `shutdown()` 排空队列并刷新文件的写入带宽。
- `dropped`：本轮因队列溢出、文件不可用或关停竞争而丢弃的日志数。

生产延时包含格式化、取缓冲区、异步入队，以及 Block 反压或 Drop 快速返回。
Drop 模式会统计全部调用的延时，包括被丢弃的日志。测试逐条读取
`steady_clock`，结果也包含两次时钟读取的开销。

Block 模式出现丢弃时会直接报错。运行结束后自动删除临时日志。
