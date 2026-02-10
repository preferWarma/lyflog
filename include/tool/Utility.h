#pragma once
#include "third/toml.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef LYF_INNER_LOG
#include <format>
#include <iostream>
#define lyf_inner_log(fmt, ...)                                                \
  std::cout << std::format(fmt, ##__VA_ARGS__) << std::endl;
#else
#define lyf_inner_log(fmt, ...) ((void)0)
#endif

namespace lyf {
namespace chrono = std::chrono;

using std::string, std::vector, std::shared_ptr;
using system_clock = chrono::system_clock;

class TomlHelper {
private:
  toml::table cfg;

public:
  TomlHelper() = default;
  bool LoadFromFile(const string &file_path) {
    try {
      cfg = toml::parse_file(file_path);
      return true;
    } catch (const toml::parse_error &e) {
      lyf_inner_log("[lyf-error] 配置文件解析失败: {}\n", e.what());
      return false;
    }
  }

  bool SaveToFile(const string &file_path) {
    try {
      std::ofstream file(file_path);
      file << cfg;
      return true;
    } catch (const std::exception &e) {
      lyf_inner_log("[lyf-error] 配置文件保存失败: {}\n", e.what());
      return false;
    }
  }

  toml::node_view<toml::node> operator[](std::string_view key) {
    return cfg[key];
  }
};

inline int64_t CurrentTsms() {
  return duration_cast<chrono::milliseconds>(
             system_clock::now().time_since_epoch())
      .count();
}

inline string CurrentTimeToString(const string &format = "%Y-%m-%d %H:%M:%S") {
  auto now = system_clock::now();
  auto time_t = system_clock::to_time_t(now);
  std::stringstream ss;
#ifdef _WIN32
  std::tm tm_buf;
  localtime_s(&tm_buf, &time_t);
  ss << std::put_time(&tm_buf, format.c_str());
#else
  std::tm tm_buf;
  localtime_r(&time_t, &tm_buf);
  ss << std::put_time(&tm_buf, format.c_str());
#endif
  return ss.str();
}

class FlagGuard {
public:
  FlagGuard(std::atomic<bool> &flag) : _flag(flag) {}
  ~FlagGuard() { _flag.store(false); }

private:
  std::atomic<bool> &_flag;
};

inline bool CreateLogDirectory(const string &path) {
  try {
    auto dir = std::filesystem::path(path).parent_path();
    if (!std::filesystem::exists(dir)) {
      std::filesystem::create_directories(dir);
    }
    return true;
  } catch (const std::exception &e) {
    lyf_inner_log("Failed to create log directory: {}\n", e.what());
    return false;
  }
}

inline std::filesystem::file_time_type
GetFileLastWriteTime(std::string_view filePath) {
  try {
    return std::filesystem::last_write_time(filePath);
  } catch (const std::exception &e) {
    lyf_inner_log("Failed to get last write time: {}\n", e.what());
    return std::filesystem::file_time_type::min();
  }
}

[[nodiscard]] constexpr bool starts_with(std::string_view sv,
                                         std::string_view prefix) noexcept {
  return sv.size() >= prefix.size() &&
         sv.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] constexpr bool ends_with(std::string_view sv,
                                       std::string_view suffix) noexcept {
  return sv.size() >= suffix.size() &&
         sv.compare(sv.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace lyf
