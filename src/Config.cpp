#include "cachegrid/Config.h"

#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>

namespace cachegrid {

namespace {

std::string trim(const std::string& s) {
  const auto first = s.find_first_not_of(" \t\r");
  if (first == std::string::npos) return "";
  const auto last = s.find_last_not_of(" \t\r");
  return s.substr(first, last - first + 1);
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Parses a whole string as a base-10 integer; false on any junk or overflow.
bool parse_int(const std::string& s, std::int64_t& out) {
  if (s.empty()) return false;
  try {
    std::size_t pos = 0;
    out = std::stoll(s, &pos, 10);
    return pos == s.size();
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

Config Config::parse(const std::string& text, const std::string& source) {
  Config cfg;
  cfg.source_ = source;
  std::istringstream in(text);
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;

    const auto eq = t.find('=');
    const std::string where = source + ":" + std::to_string(line_no);
    if (eq == std::string::npos) throw ConfigError(where + ": expected 'key = value'");
    const std::string key = trim(t.substr(0, eq));
    const std::string value = trim(t.substr(eq + 1));
    if (key.empty()) throw ConfigError(where + ": empty key");
    if (!cfg.values_.emplace(key, value).second) {
      throw ConfigError(where + ": duplicate key '" + key + "'");
    }
  }
  return cfg;
}

Config Config::load_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw ConfigError("cannot open config file '" + path + "'");
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse(ss.str(), path);
}

void Config::fail(const std::string& key, const std::string& why) const {
  throw ConfigError(source_ + ": '" + key + "' " + why);
}

std::string Config::get_string(const std::string& key) const {
  auto it = values_.find(key);
  if (it == values_.end()) fail(key, "is required but missing");
  if (it->second.empty()) fail(key, "has an empty value");
  return it->second;
}

std::int64_t Config::get_int(const std::string& key) const {
  const std::string v = get_string(key);
  std::int64_t n = 0;
  if (!parse_int(v, n)) fail(key, "must be an integer, got '" + v + "'");
  return n;
}

std::size_t Config::get_size(const std::string& key) const {
  const std::int64_t n = get_int(key);
  if (n < 0) fail(key, "must not be negative");
  return static_cast<std::size_t>(n);
}

std::size_t Config::get_bytes(const std::string& key) const {
  const std::string v = get_string(key);
  std::size_t i = 0;
  while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
  const std::string number = v.substr(0, i);
  const std::string unit = lower(trim(v.substr(i)));

  std::int64_t n = 0;
  if (!parse_int(number, n)) fail(key, "must be a byte size like 512, 64KB, 64MB, got '" + v + "'");

  std::size_t mult = 0;
  if (unit.empty() || unit == "b") mult = 1;
  else if (unit == "kb") mult = 1024;
  else if (unit == "mb") mult = 1024 * 1024;
  else if (unit == "gb") mult = 1024 * 1024 * 1024;
  else fail(key, "has unknown unit '" + unit + "' (use B, KB, MB, GB)");

  const auto un = static_cast<std::size_t>(n);
  if (un > std::numeric_limits<std::size_t>::max() / mult) fail(key, "is too large");
  return un * mult;
}

std::vector<std::string> Config::get_list(const std::string& key) const {
  const std::string v = get_string(key);
  std::vector<std::string> out;
  std::istringstream ss(v);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = trim(item);
    if (item.empty()) fail(key, "has an empty list item");
    out.push_back(item);
  }
  if (!v.empty() && v.back() == ',') fail(key, "has an empty list item");
  return out;
}

}  // namespace cachegrid
