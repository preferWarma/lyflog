// lyflog.h —— 极简单头文件异步日志库 (C++17)
//
// 用法：
//   #include "lyflog.h"
//   int main() {
//     // (可选) 第一次 LYF_* 会自动采取默认参数初始化
//     lyflog::LogConfig config;
//     config.set_file_path("app.log")
//           .set_level(lyflog::Level::Info)
//           .set_retain_days(7);
//     lyflog::Logger::instance().init(config);
//     LYF_INFO("hello {} {}", "world", 42);
//     // 运行时拼接的格式串需显式包裹（跳过编译期校验）：
//     LYF_INFO(fmt::runtime(dyn_fmt), value);
//     LYF_INTERVAL_INFO(5, "heartbeat alive={} qps={}", alive, qps);
//     // 等待当前线程此前提交的日志落盘；排空所有线程请用 shutdown()
//     lyflog::Logger::instance().sync();
//     lyflog::Logger::instance().shutdown();
//   }

#pragma once

#include <concurrentqueue.h>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lyflog {

// ==================== 日志级别 ====================
enum class Level : int {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
  Fatal = 4,
};

inline const char *level_name(Level lv) {
  switch (lv) {
  case Level::Debug:
    return "DEBUG";
  case Level::Info:
    return "INFO";
  case Level::Warn:
    return "WARN";
  case Level::Error:
    return "ERROR";
  case Level::Fatal:
    return "FATAL";
  }
  return "UNKNOWN";
}

// 队列溢出策略：队列水位达到 queue_capacity 软上限后的行为。
enum class OverflowPolicy {
  Block, // 自旋让出等待水位下降（默认）：保证不丢日志，代价是生产者被反压。
  Drop,  // 立即丢弃并计数：日志完整性让位于服务可用性。
         // Error/Fatal 级别不丢弃，退化为等待水位下降后入队。
};

// ==================== 配置 ====================
// setter 生成宏, 便于链式set调用
#define LYFLOG_SETTER(name, type)                                              \
  LogConfig &set_##name(type v) {                                              \
    name = std::move(v);                                                       \
    return *this;                                                              \
  }

struct LogConfig {
  std::string file_path = "app.log";             // 输出文件路径
  Level level = Level::Info;                     // 最低输出级别
  std::string time_format = "%Y-%m-%d %H:%M:%S"; // strftime 格式
  std::size_t file_buffer_size = 128 * 1024;     // 文件全缓冲大小 (字节)
  std::size_t batch_size = 2048;                 // 后台单次消费的批大小
  std::size_t queue_capacity = 65536;   // 软上限：超过后按 overflow_policy 处理
  std::size_t buffer_pool_size = 65536; // 全局 BufferPool 预分配数量
  std::size_t tls_buffer_count = 64;    // 线程本地一次批发的 Buffer 数量
  bool with_thread_id = false;          // 前缀是否包含线程 ID
  bool daily_rotate = true;             // 按天轮转（默认开）
  std::size_t retain_days = 7;          // 按天轮转保留天数（0=不清理）；
  std::size_t max_file_size = 0;        // 按大小轮转阈值（字节，0=不启用）
  std::size_t retain_count = 0; // 轮转文件保留个数上限（0=只考虑 retain_days）
  OverflowPolicy overflow_policy =
      OverflowPolicy::Block;    // 队列满策略（Drop 下 Error/Fatal 不丢）
  bool fatal_sync_flush = true; // LYF_FATAL 是否同步落盘

  // setter
  LYFLOG_SETTER(file_path, std::string)
  LYFLOG_SETTER(level, Level)
  LYFLOG_SETTER(time_format, std::string)
  LYFLOG_SETTER(file_buffer_size, std::size_t)
  LYFLOG_SETTER(batch_size, std::size_t)
  LYFLOG_SETTER(queue_capacity, std::size_t)
  LYFLOG_SETTER(buffer_pool_size, std::size_t)
  LYFLOG_SETTER(tls_buffer_count, std::size_t)
  LYFLOG_SETTER(with_thread_id, bool)
  LYFLOG_SETTER(daily_rotate, bool)
  LYFLOG_SETTER(retain_days, std::size_t)
  LYFLOG_SETTER(max_file_size, std::size_t)
  LYFLOG_SETTER(retain_count, std::size_t)
  LYFLOG_SETTER(overflow_policy, OverflowPolicy)
  LYFLOG_SETTER(fatal_sync_flush, bool)
};

#undef LYFLOG_SETTER

// ==================== 内部实现 ====================
namespace detail {

// 单条日志正文的最大长度（超出部分截断）
static const std::size_t kPerLogMaxSize = 4096;

// 懒增长池的初始预分配量（个）。后续按需翻倍增长至上限。
static const std::size_t kPoolInitialBatch = 4096;

// 定长 Buffer：承载单条日志正文，由 BufferPool 池化管理，全程无 malloc。
struct LogBuffer {
  char data[kPerLogMaxSize];
  std::size_t length = 0;
  void reset() { length = 0; }
};

// 无锁 Buffer 内存池。懒增长：初始仅预分配一小批，耗尽时抢占式批量扩容
class BufferPool {
public:
  // max_count：池增长上限（超过后纯走兜底 new）。initial：初始预分配量。
  explicit BufferPool(std::size_t max_count, std::size_t initial) {
    initial = std::min(initial, max_count);
    committed_.store(initial, std::memory_order_relaxed);
    std::vector<LogBuffer *> batch;
    batch.reserve(initial);
    for (std::size_t i = 0; i < initial; ++i) {
      batch.push_back(new LogBuffer());
    }
    if (!batch.empty()) {
      pool_.enqueue_bulk(batch.begin(), batch.size());
    }
  }

  ~BufferPool() {
    LogBuffer *buf = nullptr;
    while (pool_.try_dequeue(buf)) {
      delete buf;
    }
  }

  BufferPool(const BufferPool &) = delete;
  BufferPool &operator=(const BufferPool &) = delete;

  LogBuffer *alloc() {
    LogBuffer *buf = nullptr;
    // 先尝试从池中获取
    if (pool_.try_dequeue(buf)) {
      buf->reset();
      return buf;
    }
    // 池空时尝试批量扩容
    if (try_grow()) {
      if (pool_.try_dequeue(buf)) {
        buf->reset();
        return buf;
      }
    }
    // 到达批量扩容上限，池耗尽走 malloc 兜底
    fallback_new_.fetch_add(1, std::memory_order_relaxed);
    return new LogBuffer();
  }

  // 池耗尽兜底 new 的累计次数。
  [[nodiscard]] std::size_t fallback_new_count() const {
    return fallback_new_.load(std::memory_order_relaxed);
  }

  // 线程退出时直接销毁 TLS 剩余 buffer 后把额度还给池（committed_ 减回）
  void release_batch(std::size_t n) {
    if (n > 0) {
      std::size_t cur = committed_.load(std::memory_order_relaxed);
      while (cur > 0 &&
             !committed_.compare_exchange_weak(cur, cur - std::min(n, cur),
                                               std::memory_order_relaxed)) {
      }
    }
  }

  // 归还 buffer 入队
  void free(LogBuffer *buf) {
    if (buf) {
      pool_.enqueue(buf);
    }
  }

  // 批量分配 count 个 buffer
  std::size_t alloc_batch(std::vector<LogBuffer *> &out, std::size_t count) {
    std::size_t n = pool_.try_dequeue_bulk(std::back_inserter(out), count);
    if (n == count) {
      return n;
    }
    // 不够一批：尝试批量扩容补齐差额，再补一次批发。
    if (try_grow()) {
      n += pool_.try_dequeue_bulk(std::back_inserter(out), count - n);
    }
    return n;
  }

  // 批量归还 buffer 入队
  void free_batch(const std::vector<LogBuffer *> &bufs) {
    if (!bufs.empty()) {
      pool_.enqueue_bulk(bufs.begin(), bufs.size());
    }
  }

private:
  // 抢占式批量扩容
  bool try_grow() {
    std::size_t old = committed_.load(std::memory_order_relaxed);
    while (old < max_count_) {
      std::size_t step =
          std::min(std::max(old, kGrowStepMin), max_count_ - old);
      std::size_t next = old + step;
      if (committed_.compare_exchange_strong(old, next,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
        std::vector<LogBuffer *> batch;
        batch.reserve(step);
        for (std::size_t i = 0; i < step; ++i) {
          batch.push_back(new LogBuffer());
        }
        if (!batch.empty()) {
          pool_.enqueue_bulk(batch.begin(), batch.size());
        }
        return true;
      }
      // CAS 失败：old 已被更新为当前值，重试。
    }
    return false;
  }

  static constexpr std::size_t kGrowStepMin = 4096;

  moodycamel::ConcurrentQueue<LogBuffer *> pool_;
  std::atomic<std::size_t> fallback_new_{0};
  std::size_t max_count_ = 0; // 池增长上限
  // 已认领（已 new 或正在 new）的 buffer 总额度。初始即 initial。
  std::atomic<std::size_t> committed_{0};
};

// 退役池墓园：re-init/shutdown 时不 delete 池（moodycamel 队列在 producer
// 线程退出后析构会解引用死线程 TLS → SIGSEGV），移入此处泄漏至进程退出。
// 仅 lifecycle_mutex_ 持有者调用 retire()，故无需自身加锁。泄漏量有界：
// 每次 re-init 一份池结构 + 其时点未归还的 buffer。
struct RetiredPoolRegistry {
  static void retire(std::shared_ptr<BufferPool> pool) {
    static std::vector<std::shared_ptr<BufferPool>> graveyard;
    graveyard.push_back(std::move(pool)); // 故意保留至进程退出
  }
};

// 队列中承载的一条记录。持有池化 Buffer 的所有权：仅可移动
// 析构时自动把 Buffer 归还 BufferPool。
// buffer_ptr 为空且 sync_prom 非空时表示一个生产者局部 flush 屏障。
struct LogRecord {
  Level level = Level::Info;
  const char *file = nullptr; // __FILE__，指向静态字符串，无需拷贝
  int line = 0;
  std::size_t tid = 0;
  std::int64_t time_ns = 0;
  LogBuffer *buffer_ptr = nullptr;
  BufferPool *pool = nullptr;
  // 非 null 表示生产者局部 flush 屏障
  std::shared_ptr<std::promise<void>> sync_prom;

  LogRecord() = default;

  LogRecord(LogRecord &&o) noexcept
      : level(o.level), file(o.file), line(o.line), tid(o.tid),
        time_ns(o.time_ns), buffer_ptr(o.buffer_ptr), pool(o.pool),
        sync_prom(std::move(o.sync_prom)) {
    o.buffer_ptr = nullptr;
    o.pool = nullptr;
  }

  LogRecord &operator=(LogRecord &&o) noexcept {
    if (this != &o) {
      if (buffer_ptr && pool) {
        pool->free(buffer_ptr); // 释放旧的
      }
      // 旧 sync_prom 若未被兑现则随之释放。
      level = o.level;
      file = o.file;
      line = o.line;
      tid = o.tid;
      time_ns = o.time_ns;
      buffer_ptr = o.buffer_ptr;
      pool = o.pool;
      sync_prom = std::move(o.sync_prom);
      o.buffer_ptr = nullptr;
      o.pool = nullptr;
    }
    return *this;
  }

  LogRecord(const LogRecord &) = delete;
  LogRecord &operator=(const LogRecord &) = delete;

  ~LogRecord() {
    if (buffer_ptr && pool) {
      pool->free(buffer_ptr);
    }
  }
};

// 线程本地 Buffer 缓存：一次从全局池批发一批 Buffer，摊薄无锁队列访问；
// 线程退出时把剩余 Buffer 整批归还。
class ThreadLocalBufferCache {
public:
  ThreadLocalBufferCache(std::shared_ptr<BufferPool> pool, std::size_t batch)
      : pool_(std::move(pool)), batch_(batch == 0 ? 1 : batch) {}

  ~ThreadLocalBufferCache() {
    // 线程退出：直接 delete 剩余 buffer 并归还增长额度, cache_ 为空时 no-op。
    for (LogBuffer *buf : cache_) {
      delete buf;
    }
    if (!cache_.empty() && pool_) {
      pool_->release_batch(cache_.size());
    }
  }

  LogBuffer *get() {
    // 本地有缓存：直接返回
    if (!cache_.empty()) {
      LogBuffer *buf = cache_.back();
      cache_.pop_back();
      buf->reset();
      return buf;
    }
    // 本地无缓存：批量从全局池获取
    cache_.reserve(batch_);
    std::size_t n = pool_->alloc_batch(cache_, batch_);
    if (n > 0) {
      LogBuffer *buf = cache_.back();
      cache_.pop_back();
      buf->reset();
      return buf;
    }
    return pool_->alloc(); // 池已空，走兜底
  }

private:
  std::vector<LogBuffer *> cache_;
  std::shared_ptr<BufferPool> pool_;
  std::size_t batch_;
};

// 秒级缓存的时间格式化器（后台单线程使用，无需加锁）
class TimeFormatter {
public:
  const char *format(std::int64_t time_ns, const std::string &fmt,
                     std::size_t &out_len, std::int64_t &out_day_key) {
    std::time_t sec = static_cast<std::time_t>(time_ns / 1000000000LL);
    if (sec == last_sec_ && len_ > 0) {
      out_len = len_;
      out_day_key = day_key_;
      return buf_;
    }
    last_sec_ = sec;
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &sec);
#else
    localtime_r(&sec, &tm_buf);
#endif
    len_ = std::strftime(buf_, sizeof(buf_), fmt.c_str(), &tm_buf);
    out_len = len_;
    day_key_ = static_cast<std::int64_t>(tm_buf.tm_year + 1900) * 10000LL +
               (tm_buf.tm_mon + 1) * 100LL + tm_buf.tm_mday;
    out_day_key = day_key_;
    return buf_;
  }

private:
  char buf_[64] = {0};
  std::time_t last_sec_ = -1;
  std::size_t len_ = 0;
  std::int64_t day_key_ = 0;
};

// 仅单 worker 访问 file_，用无锁版 stdio 写省去 flockfile/funlockfile。
#if defined(__GLIBC__)
#define LYFLOG_FWRITE_UNLOCKED ::fwrite_unlocked
#define LYFLOG_FFLUSH_UNLOCKED ::fflush_unlocked
#else
#define LYFLOG_FWRITE_UNLOCKED std::fwrite
#define LYFLOG_FFLUSH_UNLOCKED std::fflush
#endif

// 无分配十进制追加（worker 前缀里拼 line/tid 用）
inline void append_u64(fmt::memory_buffer &buf, unsigned long long v) {
  char tmp[20];
  int p = 20;
  if (v == 0) {
    buf.push_back('0');
    return;
  }
  while (v > 0) {
    tmp[--p] = static_cast<char>('0' + (v % 10));
    v /= 10;
  }
  buf.append(tmp + p, tmp + 20);
}

// 文件输出目标：追加写 + 全缓冲。仅由后台线程访问。
class FileSink {
public:
  explicit FileSink(const LogConfig &cfg)
      : time_format_(cfg.time_format), base_path_(cfg.file_path),
        with_thread_id_(cfg.with_thread_id), daily_rotate_(cfg.daily_rotate),
        retain_days_(cfg.retain_days), file_buffer_size_(cfg.file_buffer_size),
        max_file_size_(cfg.max_file_size), retain_count_(cfg.retain_count),
        // staging 攒到 ≈ FILE 缓冲大小时回落：既批量 fwrite（省 flockfile），
        // 又不越过 FILE 缓冲导致 glibc 绕过缓冲直写
        staging_flush_threshold_(cfg.file_buffer_size > 0 ? cfg.file_buffer_size
                                                          : (128 * 1024)) {
    if (!daily_rotate_ && max_file_size_ == 0) {
      // 纯固定文件：立即以追加模式打开。
      ensure_parent_dir(base_path_);
      file_ = std::fopen(base_path_.c_str(), "a");
      apply_setvbuf();
      if (!file_) {
        note_open_failure(base_path_);
      }
    }
    // 轮转模式（按天/按大小）延后到首条 write() 按记录日期开文件，
    // 避免 init 时产生空文件。
  }

  ~FileSink() {
    if (file_) {
      flush(); // 排空 staging 后再关闭（防御性；正常路径 worker 退出已 flush）
      std::fclose(file_);
    }
  }

  [[nodiscard]] bool valid() const { return file_ != nullptr; }

  void write(const LogRecord &r) {
    if (!r.buffer_ptr) {
      return;
    }
    std::size_t tlen = 0;
    std::int64_t day_key = 0;
    const char *tbuf = time_.format(r.time_ns, time_format_, tlen, day_key);

    // 按天轮转：日期变更（或首条写入）时排空旧文件、开新日期文件。
    if (daily_rotate_ &&
        (day_key != current_day_key_ || (!file_ && retry_due()))) {
      rotate(day_key);
    }
    // 纯按大小轮转（daily 关）：首条写入（或失败退避到期）时开文件；
    if (!daily_rotate_ && max_file_size_ > 0 && !file_ && retry_due()) {
      open_current();
    }
    // 按大小轮转（与按天正交）：当前文件达到 max_file_size_ 时切下一序号。
    if (file_ && max_file_size_ > 0 &&
        current_file_bytes_ + staging_.size() >= max_file_size_) {
      rotate_size();
    }
    // 无日志文件可用：drop 记录
    if (!file_) {
      dropped_no_file_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    // 前缀直接拼进 staging_, "<time> <LEVEL> [tid] file:line "
    staging_.append(tbuf, tbuf + tlen);
    staging_.push_back(' ');
    {
      const char *ln = level_name(r.level);
      staging_.append(ln, ln + std::strlen(ln));
    }
    staging_.push_back(' ');
    if (with_thread_id_) {
      append_u64(staging_, r.tid);
      staging_.push_back(' ');
    }
    staging_.append(r.file, r.file + std::strlen(r.file));
    staging_.push_back(':');
    append_u64(staging_, static_cast<unsigned long long>(r.line));
    staging_.push_back(' ');
    // 正文（来自池化 Buffer） + 换行
    staging_.append(r.buffer_ptr->data,
                    r.buffer_ptr->data + r.buffer_ptr->length);
    staging_.push_back('\n');

    // 攒批：攒到 ≈ file_buffer_size 时一次性 fwrite_unlocked 落 FILE* 缓冲
    if (staging_.size() >= staging_flush_threshold_) {
      flush_to_file_unsafe();
    }
  }

  void flush() {
    flush_to_file_unsafe(); // staging -> FILE* 缓冲
    if (file_) {
      LYFLOG_FFLUSH_UNLOCKED(file_); // FILE* 缓冲 -> 内核/磁盘
    }
  }

  // ---- 诊断计数 ----
  [[nodiscard]] std::size_t open_fail_count() const {
    return open_fail_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t dropped_no_file_count() const {
    return dropped_no_file_.load(std::memory_order_relaxed);
  }

private:
  // 仅单 worker 访问，用无锁写。staging 为空时空操作。
  void flush_to_file_unsafe() {
    if (file_ && staging_.size() > 0) {
      LYFLOG_FWRITE_UNLOCKED(staging_.data(), 1, staging_.size(), file_);
      current_file_bytes_ += staging_.size(); // 按大小轮转的水位
      staging_.clear();
    }
  }

  // fopen 失败告警：30s 限流向 stderr 输出一条。
  void note_open_failure(const std::string &path) {
    open_fail_count_.fetch_add(1, std::memory_order_relaxed);
    auto now = std::chrono::steady_clock::now();
    if (now >= next_stderr_warn_tp_) {
      next_stderr_warn_tp_ = now + std::chrono::seconds(kStderrWarnSec);
      std::fprintf(stderr,
                   "[lyflog] cannot open log file '%s' (dropped=%zu so far); "
                   "logs are being LOST until open succeeds\n",
                   path.c_str(),
                   dropped_no_file_.load(std::memory_order_relaxed));
      std::fflush(stderr);
    }
  }

  // 按当前 (day_key, seq) 打开目标文件。size 轮转模式下若同日已有更高
  // 序号文件（进程重启续写），探测并跳到最大序号追加，避免覆盖/错位。
  // 失败进退避 + stderr 告警。
  void open_current() {
    if (max_file_size_ > 0 && seq_ == 0) {
      // 探测当天（或无日期模式下）已存在的最大序号。
      while (true) {
        std::string probe = current_path();
        std::error_code ec;
        if (std::filesystem::exists(probe, ec)) {
          ++seq_;
          continue;
        }
        break;
      }
    }
    std::string path = current_path();
    ensure_parent_dir(path);
    file_ = std::fopen(path.c_str(), "a");
    apply_setvbuf();
    if (file_) {
      current_file_bytes_ = probe_file_size(path);
      if (daily_rotate_) {
        prune_old_files(current_day_key_);
        if (retain_count_ > 0) {
          prune_over_count();
        }
      } else if (retain_count_ > 0) {
        prune_over_count();
      }
    } else {
      note_open_failure(path);
      next_rotate_retry_tp_ = std::chrono::steady_clock::now() +
                              std::chrono::seconds(kRotateRetrySec);
    }
  }

  // 退避是否到期（fopen 失败后是否允许重试 rotate）。
  bool retry_due() const {
    return std::chrono::steady_clock::now() >= next_rotate_retry_tp_;
  }

  // 按天轮转
  void rotate(std::int64_t day_key) {
    flush_to_file_unsafe(); // staging → 旧 FILE* 缓冲（file_ 为空则空操作）
    if (file_) {
      std::fclose(file_); // fclose 自带 fflush，旧缓冲落盘
      file_ = nullptr;
    }
    current_day_key_ = day_key;
    seq_ = 0;
    open_current();
  }

  // 按大小轮转
  void rotate_size() {
    flush_to_file_unsafe();
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
    ++seq_;
    open_current();
  }

  // 当前应打开的文件路径：
  //   daily+size : <stem>_<YYYY-MM-DD>.<NNN><ext>（size 未启用则无 .NNN 段）
  //   size only  : <stem>.<NNN><ext>
  //   neither    : <base> = <stem><ext>
  std::string current_path() const {
    std::string stem, ext;
    std::tie(stem, ext) = split_base_path(base_path_);
    char tail[24] = {0};
    if (daily_rotate_) {
      char date[16];
      date_str(current_day_key_, date);
      if (max_file_size_ > 0) {
        std::snprintf(tail, sizeof(tail), "_%s.%03zu", date, seq_);
      } else {
        std::snprintf(tail, sizeof(tail), "_%s", date);
      }
    } else if (max_file_size_ > 0) {
      std::snprintf(tail, sizeof(tail), ".%03zu", seq_);
    }
    return stem + tail + ext;
  }

  // 已存在同名文件时取其大小作为起始水位（追加模式语义，保证按大小
  // 轮转阈值对重启进程同样生效）。文件不存在返回 0。
  static std::size_t probe_file_size(const std::string &path) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::size_t>(sz);
  }

  // 按天轮转保留期清理：删除目录中匹配 <base>_YYYY-MM-DD<ext> 且日期早于
  // (today - (retain_days-1)) 的文件。
  void prune_old_files(std::int64_t today_key) {
    if (retain_days_ == 0) {
      return;
    }
    // cutoff = today - (retain_days - 1) 天
    std::tm cut_tm;
    std::memset(&cut_tm, 0, sizeof(cut_tm));
    cut_tm.tm_year = static_cast<int>(today_key / 10000) - 1900;
    cut_tm.tm_mon = static_cast<int>((today_key / 100) % 100) - 1;
    cut_tm.tm_mday = static_cast<int>(today_key % 100);
    cut_tm.tm_hour = 12; // 正午避开 DST 边界
    cut_tm.tm_mday -= static_cast<int>(retain_days_ - 1);
    std::mktime(&cut_tm);
    std::int64_t cutoff_key =
        static_cast<std::int64_t>(cut_tm.tm_year + 1900) * 10000 +
        (cut_tm.tm_mon + 1) * 100 + cut_tm.tm_mday;

    // 拆 base_path_ 为 dir + (stem, ext)，切分与 current_path() 共用同一
    // split_base_path()，保证清理模式与生成文件名严格一致
    auto slash = base_path_.find_last_of('/');
    std::string dir =
        (slash == std::string::npos) ? "." : base_path_.substr(0, slash);
    std::string stem, ext;
    std::tie(stem, ext) = split_base_path(base_path_);
    auto stem_slash = stem.find_last_of('/');
    if (stem_slash != std::string::npos) {
      stem = stem.substr(stem_slash + 1);
    }
    std::string prefix = stem + "_";

    // 判定一个文件名是否为 <prefix>YYYY-MM-DD[.NNN]<ext> 形态（.NNN 为
    // 按大小轮转的序号段，可缺席）、其日期键是否 < cutoff
    auto day_key_of = [&](const std::string &name,
                          std::int64_t &out_key) -> bool {
      std::size_t date_len = 10;
      if (name.size() < prefix.size() + date_len + ext.size()) {
        return false;
      }
      if (name.compare(0, prefix.size(), prefix) != 0) {
        return false;
      }
      // 结尾先对齐 ext，再看 ext 前是否紧跟 3 位数字序号段 ".NNN"。
      if (name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
        return false;
      }
      std::size_t date_start = prefix.size();
      std::size_t date_end = date_start + date_len;
      std::size_t seq_start = date_end; // 指向 ".NNN"（若有）
      std::size_t body_end = name.size() - ext.size();
      if (body_end - date_end == 4 && name[date_end] == '.' &&
          name[date_end + 1] >= '0' && name[date_end + 1] <= '9' &&
          name[date_end + 2] >= '0' && name[date_end + 2] <= '9' &&
          name[date_end + 3] >= '0' && name[date_end + 3] <= '9') {
        // 合法序号段，日期区不含它
      } else if (body_end == date_end) {
        // 无序号段
      } else {
        return false;
      }
      const char *p = name.c_str() + date_start;
      if (p[4] != '-' || p[7] != '-') {
        return false;
      }
      for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) {
          continue;
        }
        if (p[i] < '0' || p[i] > '9') {
          return false;
        }
      }
      int y = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 +
              (p[3] - '0');
      int m = (p[5] - '0') * 10 + (p[6] - '0');
      int dd = (p[8] - '0') * 10 + (p[9] - '0');
      out_key = static_cast<std::int64_t>(y) * 10000 + m * 100 + dd;
      (void)seq_start;
      return true;
    };

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir_path = dir;
    // directory_iterator 失败（目录不存在/无权限）传 ec 静默跳过。
    for (auto it = fs::directory_iterator(dir_path, ec);
         it != fs::directory_iterator(); ++it) {
      if (!fs::is_regular_file(it->status(ec))) {
        continue;
      }
      std::string name = it->path().filename().string();
      std::int64_t key = 0;
      if (day_key_of(name, key) && key < cutoff_key) {
        fs::remove(it->path(), ec); // 失败忽略（权限/不存在等）
      }
    }
  }

  // 打开文件前确保父目录存在(递归创建)。已存在则 no-op;
  static void ensure_parent_dir(const std::string &path) {
    std::error_code ec;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, ec); // ec 忽略
    }
  }

  // retain_count 个数上限清理
  void prune_over_count() {
    namespace fs = std::filesystem;
    auto slash = base_path_.find_last_of('/');
    std::string dir =
        (slash == std::string::npos) ? "." : base_path_.substr(0, slash);
    std::string stem, ext;
    std::tie(stem, ext) = split_base_path(base_path_);
    // 剥目录：stem 只保留最后一个 '/' 之后的部分。
    auto stem_slash = stem.find_last_of('/');
    if (stem_slash != std::string::npos) {
      stem = stem.substr(stem_slash + 1);
    }

    // 候选 = <stem>[_YYYY-MM-DD][.NNN]<ext>；解析出 (日期键, 序号) 用于排序。
    struct Entry {
      std::int64_t day;
      std::size_t seq;
      fs::path path;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec);
         it != fs::directory_iterator(); ++it) {
      std::string name = it->path().filename().string();
      std::int64_t day = 0;
      std::size_t seq = 0;
      if (!parse_rotated_name(name, stem, ext, daily_rotate_,
                              max_file_size_ > 0, day, seq)) {
        continue;
      }
      entries.push_back({day, seq, it->path()});
    }
    if (entries.size() <= retain_count_) {
      return;
    }
    // 倒序（新→旧）：日期大/序号大在前。
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) {
                if (a.day != b.day)
                  return a.day > b.day;
                return a.seq > b.seq;
              });
    for (std::size_t i = retain_count_; i < entries.size(); ++i) {
      fs::remove(entries[i].path, ec); // 失败忽略
    }
  }

  // 解析轮转文件名 <stem>[_YYYY-MM-DD][.NNN]<ext>：
  // has_date/has_seq 指定当前命名模式（与 current_path() 一致），两段均可
  // 选但顺序固定（日期在前、序号在后）。匹配返回 true 并输出日期键（无
  // 日期段时为 0）与序号（无序号段时为 0）。
  static bool parse_rotated_name(const std::string &name,
                                 const std::string &stem,
                                 const std::string &ext, bool has_date,
                                 bool has_seq, std::int64_t &out_day,
                                 std::size_t &out_seq) {
    std::size_t pos = 0;
    out_day = 0;
    out_seq = 0;
    if (name.compare(0, stem.size(), stem) != 0) {
      return false;
    }
    pos = stem.size();
    if (name.size() <= pos ||
        name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
      return false;
    }
    std::size_t body_end = name.size() - ext.size();
    if (has_date) {
      if (body_end - pos < 11 || name[pos] != '_') {
        return false;
      }
      const char *p = name.c_str() + pos + 1; // 10 字符日期区
      if (p[4] != '-' || p[7] != '-') {
        return false;
      }
      for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7)
          continue;
        if (p[i] < '0' || p[i] > '9')
          return false;
      }
      int y = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 +
              (p[3] - '0');
      int m = (p[5] - '0') * 10 + (p[6] - '0');
      int d = (p[8] - '0') * 10 + (p[9] - '0');
      if (m < 1 || m > 12 || d < 1 || d > 31)
        return false;
      out_day = static_cast<std::int64_t>(y) * 10000 + m * 100 + d;
      pos += 11;
    }
    if (has_seq) {
      if (body_end - pos < 4 || name[pos] != '.') {
        return false;
      }
      const char *p = name.c_str() + pos + 1; // 序号区
      std::size_t seq_len = body_end - pos - 1;
      std::size_t v = 0;
      for (std::size_t i = 0; i < seq_len; ++i) {
        if (p[i] < '0' || p[i] > '9')
          return false;
        v = v * 10 + static_cast<std::size_t>(p[i] - '0');
      }
      out_seq = v;
      pos = body_end;
    }
    return pos == body_end; // 中间不允许杂散字符
  }

  // 把 base 拆为 (stem, ext)，ext 含前导 '.'（无扩展名则为空串）。
  static std::pair<std::string, std::string>
  split_base_path(const std::string &base) {
    auto slash = base.find_last_of('/');
    std::size_t name_start = (slash == std::string::npos) ? 0 : slash + 1;
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > name_start) {
      return {base.substr(0, dot), base.substr(dot)};
    }
    return {base, std::string()};
  }

  // 日期键 (YYYYMMDD) -> "YYYY-MM-DD" 字符串。
  static void date_str(std::int64_t day_key, char out[16]) {
    int y = static_cast<int>(day_key / 10000);
    int m = static_cast<int>((day_key / 100) % 100);
    int d = static_cast<int>(day_key % 100);
    std::snprintf(out, 16, "%04d-%02d-%02d", y, m, d);
  }

  // 为已打开的 file_ 配置全缓冲（轮转重开文件后再次调用）。复用 iobuf_。
  void apply_setvbuf() {
    if (file_ && file_buffer_size_ > 0) {
      if (iobuf_.size() != file_buffer_size_) {
        iobuf_.resize(file_buffer_size_);
      }
      std::setvbuf(file_, iobuf_.data(), _IOFBF, iobuf_.size());
    }
  }

  std::FILE *file_ = nullptr;
  std::vector<char> iobuf_;
  std::string time_format_;
  std::string base_path_; // 轮转基准路径（非轮转模式未使用）
  bool with_thread_id_;
  bool daily_rotate_;
  std::size_t retain_days_;            // 保留天数（0=不清理），轮转后据此删旧
  std::size_t file_buffer_size_;       // setvbuf 缓冲大小，轮转重开时复用
  std::size_t max_file_size_;          // 按大小轮转阈值（0=不启用）
  std::size_t retain_count_;           // 轮转文件个数上限（0=不启用）
  std::int64_t current_day_key_ = 0;   // 当前 file_ 对应的本地日期 (YYYYMMDD)
  std::size_t seq_ = 0;                // 当前 (日期内) 序号：按大小轮转的段号
  std::size_t current_file_bytes_ = 0; // 当前文件已写字节（大小轮转水位）
  // fopen 失败后的退避：next 到期前不重试 rotate（秒级)
  static constexpr int kRotateRetrySec = 30;
  std::chrono::steady_clock::time_point next_rotate_retry_tp_{};
  // stderr 告警限流：open 失败每 kStderrWarnSec 秒至多一条。
  static constexpr int kStderrWarnSec = 30;
  std::chrono::steady_clock::time_point next_stderr_warn_tp_{};
  // 诊断计数
  std::atomic<std::size_t> open_fail_count_{0};
  std::atomic<std::size_t> dropped_no_file_{0};

  TimeFormatter time_;
  fmt::memory_buffer staging_; // worker 私有：攒批整行，每批一次落 FILE*
  std::size_t staging_flush_threshold_; // staging 回落的安全上限
};

inline std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 每线程缓存一次线程 ID 的 hash，避免每条日志都做哈希
inline std::size_t cached_tid() {
  static thread_local std::size_t tid =
      std::hash<std::thread::id>{}(std::this_thread::get_id());
  return tid;
}

} // namespace detail

// ==================== Logger 单例 ====================
class Logger {
public:
  // 故意泄漏单例：进程退出时不析构（避免静态析构序问题)
  // 日志库在退出时丢掉最后一次 flush 之外的清理工作；
  // 需要优雅落盘的调用方可以在 main 末尾显式调用 shutdown()。
  static Logger &instance() {
    static Logger *inst = new Logger();
    return *inst;
  }

  // 初始化：建池、打开文件并启动后台线程。重复调用会先关闭旧实例。
  // 线程安全：内部持 lifecycle_mutex_，可与并发 submit/sync 自动初始化竞争。
  void init(const LogConfig &cfg) {
    std::lock_guard<std::mutex> lk(lifecycle_mutex_);
    shutdown_locked();
    cfg_ = cfg;
    watermark_.store(0, std::memory_order_relaxed);
    cfg_queue_capacity_.store(cfg.queue_capacity, std::memory_order_relaxed);
    cfg_overflow_drop_.store(cfg.overflow_policy == OverflowPolicy::Drop,
                             std::memory_order_relaxed);
    fatal_sync_flush_.store(cfg.fatal_sync_flush, std::memory_order_relaxed);
    tls_batch_hint_.store(cfg.tls_buffer_count, std::memory_order_relaxed);
    level_.store(static_cast<int>(cfg.level), std::memory_order_relaxed);
    pool_ = std::make_shared<detail::BufferPool>(cfg.buffer_pool_size,
                                                 detail::kPoolInitialBatch);
    sink_.reset(new detail::FileSink(cfg_));
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&Logger::worker_loop, this);
  }

  // 关闭：排空队列、落盘、回收线程、释放池。析构时自动调用。
  void shutdown() {
    std::lock_guard<std::mutex> lk(lifecycle_mutex_);
    shutdown_locked();
  }

  ~Logger() { shutdown(); }

  [[nodiscard]] bool running() const {
    return running_.load(std::memory_order_acquire);
  }
  [[nodiscard]] Level level() const {
    return static_cast<Level>(level_.load(std::memory_order_relaxed));
  }

  // 热更新设置日志级别
  void set_level(Level lv) {
    level_.store(static_cast<int>(lv), std::memory_order_relaxed);
  }

  // BufferPool 池耗尽、走 malloc 兜底的累计次数。
  [[nodiscard]] std::size_t pool_fallback_new_count() const {
    return pool_ ? pool_->fallback_new_count() : 0;
  }

  // 自监控指标快照。
  struct Stats {
    bool running = false;           // worker 是否在跑
    Level level = Level::Info;      // 当前级别
    std::size_t queue_size = 0;     // 当前队列水位（含在途预留的近似值）
    std::size_t queue_capacity = 0; // 背压软上限（0=不设限）
    OverflowPolicy overflow_policy = OverflowPolicy::Block;
    std::size_t dropped_overflow = 0;  // 队列满按 Drop 策略丢弃的累计条数
    std::size_t dropped_no_file = 0;   // 文件打开失败丢弃的累计条数
    std::size_t open_fail_count = 0;   // fopen 累计失败次数（成功后不清零）
    std::size_t pool_fallback_new = 0; // 池耗尽走 malloc 兜底的累计次数
    std::size_t shutdown_drop = 0; // 关停窗口（running_=false）丢弃的累计条数
  };
  [[nodiscard]] Stats stats() const {
    Stats s;
    s.running = running_.load(std::memory_order_acquire);
    s.level = level();
    // 水位计数是入队预留语义，含在途未发布记录，略高于真实队列长度
    const std::int64_t level_count = watermark_.load(std::memory_order_relaxed);
    s.queue_size = level_count > 0 ? static_cast<size_t>(level_count) : 0;
    s.queue_capacity = cfg_queue_capacity_.load(std::memory_order_relaxed);
    s.overflow_policy = cfg_overflow_drop_.load(std::memory_order_relaxed)
                            ? OverflowPolicy::Drop
                            : OverflowPolicy::Block;
    s.dropped_overflow = dropped_overflow_.load(std::memory_order_relaxed);
    s.shutdown_drop = shutdown_drop_.load(std::memory_order_relaxed);
    if (sink_) {
      s.dropped_no_file = sink_->dropped_no_file_count();
      s.open_fail_count = sink_->open_fail_count();
    }
    s.pool_fallback_new = pool_ ? pool_->fallback_new_count() : 0;
    return s;
  }

  // 提交一条日志：调用线程从 TLS 缓存取 Buffer，fmt 格式化写入后入队。
  detail::ThreadLocalBufferCache *
  get_tls_cache(const std::shared_ptr<detail::BufferPool> &pool) {
    static thread_local std::shared_ptr<detail::BufferPool> tls_pool;
    static thread_local std::unique_ptr<detail::ThreadLocalBufferCache>
        tls_cache;
    if (tls_pool.get() != pool.get() || !tls_cache) {
      tls_pool = pool;
      tls_cache.reset(new detail::ThreadLocalBufferCache(
          pool, tls_batch_hint_.load(std::memory_order_relaxed)));
    }
    return tls_cache.get();
  }

  // 提交一条日志：调用线程从 TLS 缓存取 Buffer，fmt 格式化写入后入队。
  template <typename... Args>
  void submit(Level lv, const char *file, int line,
              fmt::format_string<Args...> fmt, Args &&...args) {
    // Drop 策略下，可丢弃级别（< Error）在水位超限时直接返回
    if (cfg_overflow_drop_.load(std::memory_order_relaxed) &&
        lv < Level::Error && over_watermark() &&
        running_.load(std::memory_order_relaxed)) {
      dropped_overflow_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // 热路径只读共享状态：pool_ 的拷贝与 TLS 身份比较。
    std::shared_ptr<detail::BufferPool> pool = pool_;
    if (!pool) {
      return;
    }

    detail::ThreadLocalBufferCache *cache = get_tls_cache(pool);
    detail::LogBuffer *buf = cache->get();
    fmt::format_to_n_result<char *> res = fmt::format_to_n(
        buf->data, detail::kPerLogMaxSize, fmt, std::forward<Args>(args)...);
    buf->length = std::min(res.size, detail::kPerLogMaxSize);

    detail::LogRecord r;
    r.level = lv;
    r.file = file;
    r.line = line;
    r.tid = detail::cached_tid();
    r.time_ns = detail::now_ns();
    r.buffer_ptr = buf;
    r.pool = pool.get(); // 消费后由 LogRecord 析构归还此池
    push(std::move(r));

    // Fatal 与屏障同属一个生产者，FIFO 保证 Fatal 先落盘。
    if (lv == Level::Fatal &&
        fatal_sync_flush_.load(std::memory_order_relaxed) &&
        running_.load(std::memory_order_acquire)) {
      wait_flush_barrier();
    }
  }

  // 插入调用线程的局部刷新屏障并等待；不排序其他生产者。
  void wait_flush_barrier() {
    std::promise<void> prom;
    std::future<void> fut = prom.get_future();
    detail::LogRecord barrier;
    barrier.sync_prom = std::make_shared<std::promise<void>>(std::move(prom));
    if (!running_.load(std::memory_order_acquire)) {
      return; // 复查：enqueue 前可能已被 shutdown
    }
    watermark_.fetch_add(1, std::memory_order_relaxed);
    queue_.enqueue(std::move(barrier));
    while (fut.wait_for(std::chrono::milliseconds(50)) !=
           std::future_status::ready) {
      if (!running_.load(std::memory_order_acquire)) {
        return; // worker 已退出，barrier 不会再被兑现
      }
    }
  }

  // 等待调用线程此前提交的日志落盘；不排空其他生产者。
  // 全局排空应先停止或汇合生产线程，再调用 shutdown()。
  void sync() {
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }
    wait_flush_barrier();
  }

private:
  Logger() = default;
  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

  // 调用方已持有 lifecycle_mutex_。
  // 排空队列 → join worker → 丢弃 sink → 池退役进墓园。
  void shutdown_locked() {
    if (!running_.exchange(false)) {
      return;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    sink_.reset();
    // join 后最终排空一轮：worker 退出与生产者 enqueue 之间存在窗口，
    // 此时 sink_ 已空，记录的 write 是 no-op，析构仅归还 buffer。
    std::vector<detail::LogRecord> drain;
    while (queue_.try_dequeue_bulk(std::back_inserter(drain), 4096) > 0) {
      drain.clear();
    }
    watermark_.store(0, std::memory_order_relaxed);
    if (pool_) {
      detail::RetiredPoolRegistry::retire(std::move(pool_));
    }
  }

  // 水位是否已达软上限（queue_capacity=0 视为不设限）。
  bool over_watermark() const {
    const std::size_t cap = cfg_queue_capacity_.load(std::memory_order_relaxed);
    return cap > 0 && watermark_.load(std::memory_order_relaxed) >=
                          static_cast<std::int64_t>(cap);
  }

  void push(detail::LogRecord &&r) {
    // 水位达软上限时在此等待（queue_capacity=0 不生效）：
    //   Block（默认）：自旋让出等水位下降，保证不丢、生产者被反压；
    //   Drop：可丢弃级别已在 submit() 顶部提前丢弃，不会走到这里；
    //         Error/Fatal 免删，同样在此等待水位（超限量有界，可接受）。
    const bool must_wait =
        !cfg_overflow_drop_.load(std::memory_order_relaxed) ||
        r.level >= Level::Error;
    while (must_wait && over_watermark() &&
           running_.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    // 复查：自旋期间若被 shutdown 抢先（worker 已退出），丢弃本条
    if (!running_.load(std::memory_order_relaxed)) {
      shutdown_drop_.fetch_add(1, std::memory_order_relaxed);
      return; // ~LogRecord 归还 buffer（TLS 持有的池 shared_ptr 仍存活）
    }

    // 正常入队。
    watermark_.fetch_add(1, std::memory_order_relaxed);
    queue_.enqueue(std::move(r));
  }

  void worker_loop() {
    std::vector<detail::LogRecord> batch;
    batch.reserve(cfg_.batch_size);
    // 批量归还：摘出的 buffer 集中 free_batch，避免逐条 enqueue 池队列。
    std::vector<detail::LogBuffer *> free_list;
    free_list.reserve(cfg_.batch_size);
    // 周期落盘确保日志在 flush_interval 内可见
    auto last_flush = std::chrono::steady_clock::now();
    const auto flush_interval = std::chrono::seconds(1);
    while (running_.load(std::memory_order_acquire) ||
           queue_.size_approx() > 0) {
      auto now = std::chrono::steady_clock::now();
      if (sink_ && now - last_flush >= flush_interval) {
        sink_->flush();
        last_flush = now;
      }
      std::size_t n =
          queue_.try_dequeue_bulk(std::back_inserter(batch), cfg_.batch_size);
      if (n == 0) {
        // 队列空时休眠避免忙等
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      // 出队即按批减水位（含屏障记录）
      watermark_.fetch_sub(static_cast<std::int64_t>(n),
                           std::memory_order_relaxed);
      for (auto &r : batch) {
        // 局部刷新屏障：刷新已处理内容并兑现。
        if (r.sync_prom) {
          sink_->flush();
          r.sync_prom->set_value();
          continue;
        }
        // 正常日志：写入
        sink_->write(r);
      }
      // 批量归还 buffer
      free_list.clear();
      {
        detail::BufferPool *cur_pool = nullptr;
        for (auto &r : batch) {
          if (r.buffer_ptr && r.pool) {
            if (r.pool != cur_pool) {
              if (cur_pool) {
                cur_pool->free_batch(free_list);
                free_list.clear();
              }
              cur_pool = r.pool;
            }
            free_list.push_back(r.buffer_ptr);
            r.buffer_ptr = nullptr; // 摘出所有权，析构时不再归还
            r.pool = nullptr;
          }
        }
        if (cur_pool && !free_list.empty()) {
          cur_pool->free_batch(free_list);
        }
      }
      batch.clear(); // LogRecord 析构（buffer 已摘出，仅析构空记录）
    }

    // worker 退出前最后 flush
    if (sink_) {
      sink_->flush();
    }
  }

  // cfg_ 仅在持 lifecycle_mutex_ 的 init/shutdown/worker_loop 中访问；
  // 热路径（submit/push）只读下面的原子快照，避免与 init 写 cfg_ 竞争。
  LogConfig cfg_;
  std::atomic<std::size_t> cfg_queue_capacity_{0};
  std::atomic<bool> cfg_overflow_drop_{false};
  std::atomic<bool> fatal_sync_flush_{true};
  std::atomic<std::size_t> tls_batch_hint_{64};
  // 队列水位：入队前 +1（预留，屏障同样计数），worker 每批出队后 -n。
  std::atomic<int64_t> watermark_{0};
  // 丢弃诊断计数
  std::atomic<std::size_t> dropped_overflow_{0};
  std::atomic<std::size_t> shutdown_drop_{0};

  std::mutex lifecycle_mutex_;
  std::atomic<int> level_{static_cast<int>(Level::Info)};
  std::atomic<bool> running_{false};
  std::thread worker_;
  std::shared_ptr<detail::BufferPool> pool_;
  std::unique_ptr<detail::FileSink> sink_;
  moodycamel::ConcurrentQueue<detail::LogRecord> queue_;
};

// 自动初始化经 ensure_default_init() 串行化：多线程同时首用不会并发 init。
inline void ensure_default_init() {
  static std::once_flag once;
  if (!::lyflog::Logger::instance().running()) {
    std::call_once(once, [] {
      if (!::lyflog::Logger::instance().running()) {
        ::lyflog::Logger::instance().init(::lyflog::LogConfig());
      }
    });
  }
}

} // namespace lyflog

// ==================== 用户宏 ====================
// 统一前缀 LYF_，便于与仓库内其它日志库区分。
#define LYF_BASE(lv, ...)                                                      \
  do {                                                                         \
    ::lyflog::Logger &_lg = ::lyflog::Logger::instance();                      \
    if (!_lg.running()) {                                                      \
      ::lyflog::ensure_default_init();                                         \
    }                                                                          \
    if ((lv) >= _lg.level()) {                                                 \
      _lg.submit((lv), __FILE__, __LINE__, __VA_ARGS__);                       \
    }                                                                          \
  } while (0)

#define LYF_DEBUG(...) LYF_BASE(::lyflog::Level::Debug, __VA_ARGS__)
#define LYF_INFO(...) LYF_BASE(::lyflog::Level::Info, __VA_ARGS__)
#define LYF_WARN(...) LYF_BASE(::lyflog::Level::Warn, __VA_ARGS__)
#define LYF_ERROR(...) LYF_BASE(::lyflog::Level::Error, __VA_ARGS__)
#define LYF_FATAL(...) LYF_BASE(::lyflog::Level::Fatal, __VA_ARGS__)

// 间隔限流日志宏：每 sec 秒最多输出一条。sec 为第一个参数（可传整数秒，
// 也可传小数表示亚秒级间隔，如 0.5）。每个调用点（__FILE__:__LINE__）独立
// 计时；多线程并发命中同一调用点时，仅一条能通过 CAS 抢占到本次时间窗口。
#define LYF_INTERVAL_BASE(sec, lv, ...)                                        \
  do {                                                                         \
    ::lyflog::Logger &_lg = ::lyflog::Logger::instance();                      \
    if (!_lg.running()) {                                                      \
      ::lyflog::ensure_default_init();                                         \
    }                                                                          \
    if ((lv) >= _lg.level()) {                                                 \
      static std::atomic<int64_t> _lyf_last_ns{0};                             \
      int64_t _lyf_now = ::lyflog::detail::now_ns();                           \
      int64_t _lyf_last = _lyf_last_ns.load(std::memory_order_relaxed);        \
      int64_t _lyf_interval = static_cast<int64_t>((sec) * 1000000000LL);      \
      if (_lyf_now - _lyf_last >= _lyf_interval) {                             \
        if (_lyf_last_ns.compare_exchange_strong(_lyf_last, _lyf_now,          \
                                                 std::memory_order_relaxed)) { \
          _lg.submit((lv), __FILE__, __LINE__, __VA_ARGS__);                   \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  } while (0)

#define LYF_INTERVAL_DEBUG(sec, ...)                                           \
  LYF_INTERVAL_BASE(sec, ::lyflog::Level::Debug, __VA_ARGS__)
#define LYF_INTERVAL_INFO(sec, ...)                                            \
  LYF_INTERVAL_BASE(sec, ::lyflog::Level::Info, __VA_ARGS__)
#define LYF_INTERVAL_WARN(sec, ...)                                            \
  LYF_INTERVAL_BASE(sec, ::lyflog::Level::Warn, __VA_ARGS__)
#define LYF_INTERVAL_ERROR(sec, ...)                                           \
  LYF_INTERVAL_BASE(sec, ::lyflog::Level::Error, __VA_ARGS__)
#define LYF_INTERVAL_FATAL(sec, ...)                                           \
  LYF_INTERVAL_BASE(sec, ::lyflog::Level::Fatal, __VA_ARGS__)
