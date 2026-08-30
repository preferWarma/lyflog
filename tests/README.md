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
./build/tests/lyflog_benchmark
./build/tests/lyflog_benchmark --messages 1000000 --threads 8
```

指标说明：

- `avg ns`：单条日志生产延时的算术平均值。
- `min ns` / `max ns`：单条日志生产延时的最小值和最大值。
- `p99 ns`：按 nearest-rank 计算的 P99，99% 的生产延时不超过该值。
- `MiB/s`：从生产开始到 `shutdown()` 排空队列并刷新文件的写入带宽。

生产延时包含格式化、取缓冲区、异步入队和 Block 反压。测试会逐条读取
`steady_clock`，因此结果也包含两次时钟读取的开销。

性能测试使用 Block 策略，出现日志丢弃时直接报错，因此不输出 `dropped` 指标。
运行结束后会自动删除临时日志。
