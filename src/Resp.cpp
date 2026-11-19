#include "cachegrid/Resp.h"

#include <utility>

namespace cachegrid::resp {

namespace {

constexpr std::string_view kCrlf = "\r\n";

// Longest valid integer line body: optional '-' plus 19 digits (int64). Part of
// the protocol format, not a tunable: anything longer without CRLF is garbage.
constexpr std::size_t kMaxIntLineLen = 20;

enum class LineStatus { Ok, Incomplete, Error };

bool parse_int64(std::string_view s, std::int64_t& out) {
  bool neg = false;
  if (!s.empty() && s[0] == '-') {
    neg = true;
    s.remove_prefix(1);
  }
  if (s.empty() || s.size() > 18) return false;  // 18 digits can't overflow int64
  std::int64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = neg ? -v : v;
  return true;
}

// Reads "<type byte><integer>\r\n" starting at `pos` (which points at the type
// byte). On Ok, `next` is the index just past the CRLF.
LineStatus read_int_line(std::string_view buf, std::size_t pos, std::int64_t& value,
                         std::size_t& next) {
  const std::size_t start = pos + 1;
  const std::size_t end = buf.find(kCrlf, start);
  if (end == std::string_view::npos) {
    return (buf.size() - start > kMaxIntLineLen) ? LineStatus::Error : LineStatus::Incomplete;
  }
  if (!parse_int64(buf.substr(start, end - start), value)) return LineStatus::Error;
  next = end + kCrlf.size();
  return LineStatus::Ok;
}

ParseResult error(std::string msg) {
  ParseResult r;
  r.status = ParseStatus::Error;
  r.error = std::move(msg);
  return r;
}

ParseResult incomplete() { return ParseResult{}; }

ParseResult parse_multibulk(std::string_view buf, const Limits& lim) {
  std::int64_t count = 0;
  std::size_t pos = 0;
  switch (read_int_line(buf, 0, count, pos)) {
    case LineStatus::Incomplete: return incomplete();
    case LineStatus::Error: return error("invalid multibulk length");
    case LineStatus::Ok: break;
  }

  ParseResult r;
  if (count <= 0) {  // "*0" / "*-1": empty command, nothing to run
    r.status = ParseStatus::Ok;
    r.consumed = pos;
    return r;
  }
  if (static_cast<std::uint64_t>(count) > lim.max_args) return error("too many arguments");

  // First pass: validate and record where each argument lives. Copy nothing
  // until the whole command is present, so a large value arriving in many
  // small packets is not copied over and over.
  std::vector<std::pair<std::size_t, std::size_t>> spans;  // (offset, length)
  spans.reserve(static_cast<std::size_t>(count));
  for (std::int64_t i = 0; i < count; ++i) {
    if (pos >= buf.size()) return incomplete();
    if (buf[pos] != '$') return error(std::string("expected '$', got '") + buf[pos] + "'");

    std::int64_t len = 0;
    std::size_t data = 0;
    switch (read_int_line(buf, pos, len, data)) {
      case LineStatus::Incomplete: return incomplete();
      case LineStatus::Error: return error("invalid bulk length");
      case LineStatus::Ok: break;
    }
    if (len < 0 || static_cast<std::uint64_t>(len) > lim.max_request_bytes) {
      return error("invalid bulk length");
    }
    const auto ulen = static_cast<std::size_t>(len);
    if (buf.size() - data < ulen + kCrlf.size()) return incomplete();
    if (buf.substr(data + ulen, kCrlf.size()) != kCrlf) return error("missing CRLF after bulk data");

    spans.emplace_back(data, ulen);
    pos = data + ulen + kCrlf.size();
  }

  r.status = ParseStatus::Ok;
  r.consumed = pos;
  r.args.reserve(spans.size());
  for (const auto& [off, len] : spans) r.args.emplace_back(buf.substr(off, len));
  return r;
}

ParseResult parse_inline(std::string_view buf, const Limits& lim) {
  const std::size_t nl = buf.find('\n');
  if (nl == std::string_view::npos) return incomplete();

  std::string_view line = buf.substr(0, nl);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

  ParseResult r;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > start) {
      if (r.args.size() == lim.max_args) return error("too many arguments");
      r.args.emplace_back(line.substr(start, i - start));
    }
  }
  r.status = ParseStatus::Ok;
  r.consumed = nl + 1;
  return r;
}

}  // namespace

ParseResult parse_command(std::string_view buf, const Limits& limits) {
  if (buf.empty()) return incomplete();

  ParseResult r = (buf[0] == '*') ? parse_multibulk(buf, limits) : parse_inline(buf, limits);

  // A command bigger than the limit is rejected whether it's complete or still
  // arriving. Without the second check a client could stream forever and grow
  // our buffer without bound.
  if (r.status == ParseStatus::Ok && r.consumed > limits.max_request_bytes) {
    return error("request exceeds max size");
  }
  if (r.status == ParseStatus::Incomplete && buf.size() >= limits.max_request_bytes) {
    return error("request exceeds max size");
  }
  return r;
}

namespace {

ParseStatus frame_reply_inner(std::string_view buf, std::size_t max_bytes, std::size_t& consumed) {
  // Iterative walk: `remaining` counts elements still expected. An array
  // header adds its element count, so nested arrays need no recursion.
  std::size_t pos = 0;
  std::uint64_t remaining = 1;
  while (remaining > 0) {
    if (pos >= buf.size()) return ParseStatus::Incomplete;
    const char type = buf[pos];
    if (type == '+' || type == '-' || type == ':') {
      const std::size_t end = buf.find(kCrlf, pos);
      if (end == std::string_view::npos) return ParseStatus::Incomplete;
      pos = end + kCrlf.size();
      --remaining;
      continue;
    }
    if (type != '$' && type != '*') return ParseStatus::Error;

    std::int64_t n = 0;
    std::size_t next = 0;
    switch (read_int_line(buf, pos, n, next)) {
      case LineStatus::Incomplete: return ParseStatus::Incomplete;
      case LineStatus::Error: return ParseStatus::Error;
      case LineStatus::Ok: break;
    }
    --remaining;
    if (n < -1 || static_cast<std::int64_t>(max_bytes) < n) return ParseStatus::Error;
    if (type == '*') {
      if (n > 0) remaining += static_cast<std::uint64_t>(n);
      pos = next;
    } else if (n == -1) {
      pos = next;  // null bulk string
    } else {
      const auto len = static_cast<std::size_t>(n);
      if (buf.size() - next < len + kCrlf.size()) return ParseStatus::Incomplete;
      if (buf.substr(next + len, kCrlf.size()) != kCrlf) return ParseStatus::Error;
      pos = next + len + kCrlf.size();
    }
  }
  consumed = pos;
  return ParseStatus::Ok;
}

}  // namespace

ParseStatus frame_reply(std::string_view buf, std::size_t max_bytes, std::size_t& consumed) {
  const ParseStatus st = frame_reply_inner(buf, max_bytes, consumed);
  if (st == ParseStatus::Ok && consumed > max_bytes) return ParseStatus::Error;
  if (st == ParseStatus::Incomplete && buf.size() >= max_bytes) return ParseStatus::Error;
  return st;
}

std::string encode_command(const std::vector<std::string>& args) {
  std::string out;
  write_array_header(out, args.size());
  for (const auto& a : args) write_bulk(out, a);
  return out;
}

void write_simple(std::string& out, std::string_view s) {
  out += '+';
  out += s;
  out += kCrlf;
}

void write_error(std::string& out, std::string_view message) {
  out += '-';
  out += message;
  out += kCrlf;
}

void write_integer(std::string& out, std::int64_t n) {
  out += ':';
  out += std::to_string(n);
  out += kCrlf;
}

void write_bulk(std::string& out, std::string_view s) {
  out += '$';
  out += std::to_string(s.size());
  out += kCrlf;
  out += s;
  out += kCrlf;
}

void write_null(std::string& out) { out += "$-1\r\n"; }

void write_array_header(std::string& out, std::size_t n) {
  out += '*';
  out += std::to_string(n);
  out += kCrlf;
}

}  // namespace cachegrid::resp
