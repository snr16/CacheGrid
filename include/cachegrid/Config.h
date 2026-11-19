#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace cachegrid {

struct ConfigError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Minimal `key = value` configuration file.
//
//   # full-line comments start with '#'
//   cache.capacity   = 64MB
//   cache.num_shards = 64
//
// Rules: one setting per line, whitespace around keys/values is trimmed,
// duplicate keys are an error, and every getter throws ConfigError naming the
// key if it's missing or malformed. There are deliberately no fallback
// defaults: every tunable must be written in the file, so it's visible and
// reviewable in one place.
class Config {
 public:
  static Config parse(const std::string& text, const std::string& source = "<string>");
  static Config load_file(const std::string& path);

  bool has(const std::string& key) const { return values_.count(key) != 0; }

  std::string get_string(const std::string& key) const;
  std::int64_t get_int(const std::string& key) const;
  // Non-negative integer.
  std::size_t get_size(const std::string& key) const;
  // Byte size with optional unit: 512, 512B, 64KB, 64MB, 2GB (1024-based,
  // case-insensitive).
  std::size_t get_bytes(const std::string& key) const;
  // Comma-separated list; items are trimmed, empty items rejected.
  std::vector<std::string> get_list(const std::string& key) const;

 private:
  [[noreturn]] void fail(const std::string& key, const std::string& why) const;

  std::string source_;
  std::map<std::string, std::string> values_;
};

}  // namespace cachegrid
