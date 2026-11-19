#pragma once

// RESP2 (REdis Serialization Protocol) — the subset CacheGrid speaks.
//
// Requests come in two forms:
//   1. Array of bulk strings (what real clients send):
//        *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n
//   2. Inline (handy for typing into `nc`):
//        SET key value\r\n
//
// Replies use five types:
//   +OK\r\n            simple string
//   -ERR message\r\n   error
//   :42\r\n            integer
//   $5\r\nhello\r\n    bulk string (binary-safe, length-prefixed)
//   $-1\r\n            null (missing key)
//   *N\r\n...          array header (followed by N elements)

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cachegrid::resp {

struct Limits {
  std::size_t max_request_bytes;  // largest single command, all args included
  std::size_t max_args;           // most arguments in one command
};

enum class ParseStatus {
  Ok,          // one full command parsed; `consumed` bytes can be dropped
  Incomplete,  // need more bytes; call again when more data arrives
  Error,       // malformed or over a limit; the connection should be closed
};

struct ParseResult {
  ParseStatus status = ParseStatus::Incomplete;
  std::vector<std::string> args;  // empty args + Ok = blank line / empty array, ignore
  std::size_t consumed = 0;
  std::string error;
};

// Parses at most one command from the front of `buf`. Never reads past the
// end and never copies argument bytes until the whole command has arrived.
ParseResult parse_command(std::string_view buf, const Limits& limits);

// Finds the end of one complete *reply* at the front of `buf` without
// interpreting it (the proxy forwards reply bytes verbatim). On Ok, `consumed`
// is the reply's length. Replies longer than `max_bytes` are an Error.
ParseStatus frame_reply(std::string_view buf, std::size_t max_bytes, std::size_t& consumed);

// Encodes arguments as a RESP array of bulk strings (how commands are sent to
// a node, whatever form the client used).
std::string encode_command(const std::vector<std::string>& args);

void write_simple(std::string& out, std::string_view s);
void write_error(std::string& out, std::string_view message);
void write_integer(std::string& out, std::int64_t n);
void write_bulk(std::string& out, std::string_view s);
void write_null(std::string& out);
void write_array_header(std::string& out, std::size_t n);

}  // namespace cachegrid::resp
