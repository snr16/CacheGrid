#include "cachegrid/CommandHandler.h"

#include <cctype>
#include <limits>
#include <utility>

#include "cachegrid/Resp.h"

namespace cachegrid {

namespace {

std::string upper(const std::string& s) {
  std::string u = s;
  for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return u;
}

void wrong_arity(std::string& out, const std::string& cmd) {
  std::string name = cmd;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  resp::write_error(out, "ERR wrong number of arguments for '" + name + "' command");
}

// Strict positive integer parse (no sign, no junk, no overflow).
bool parse_positive(const std::string& s, std::int64_t& out) {
  if (s.empty() || s.size() > 18) return false;
  std::int64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  if (v <= 0) return false;
  out = v;
  return true;
}

}  // namespace

CommandHandler::CommandHandler(Cache& cache, InfoFn server_info)
    : cache_(cache), server_info_(std::move(server_info)) {}

CommandHandler::Action CommandHandler::execute(std::vector<std::string>& args, std::string& out) {
  const std::string cmd = upper(args[0]);
  const std::size_t argc = args.size();

  if (cmd == "GET") {
    if (argc != 2) {
      wrong_arity(out, cmd);
      return Action::Continue;
    }
    if (auto v = cache_.get(args[1])) {
      resp::write_bulk(out, *v);
    } else {
      resp::write_null(out);
    }
  } else if (cmd == "SET") {
    cmd_set(args, out);
  } else if (cmd == "DEL") {
    if (argc < 2) {
      wrong_arity(out, cmd);
      return Action::Continue;
    }
    std::int64_t removed = 0;
    for (std::size_t i = 1; i < argc; ++i) removed += cache_.del(args[i]) ? 1 : 0;
    resp::write_integer(out, removed);
  } else if (cmd == "PING") {
    if (argc == 1) {
      resp::write_simple(out, "PONG");
    } else if (argc == 2) {
      resp::write_bulk(out, args[1]);
    } else {
      wrong_arity(out, cmd);
    }
  } else if (cmd == "DBSIZE") {
    resp::write_integer(out, static_cast<std::int64_t>(cache_.size()));
  } else if (cmd == "INFO") {
    cmd_info(out);
  } else if (cmd == "QUIT") {
    resp::write_simple(out, "OK");
    return Action::Close;
  } else if (cmd == "COMMAND") {
    // redis-cli sends "COMMAND DOCS" on connect to fetch help text; an empty
    // list is a valid answer and lets it work normally.
    resp::write_array_header(out, 0);
  } else {
    resp::write_error(out, "ERR unknown command '" + args[0] + "'");
  }
  return Action::Continue;
}

void CommandHandler::cmd_set(std::vector<std::string>& args, std::string& out) {
  if (args.size() != 3 && args.size() != 5) {
    wrong_arity(out, "SET");
    return;
  }

  std::optional<Millis> ttl;
  if (args.size() == 5) {
    const std::string opt = upper(args[3]);
    std::int64_t n = 0;
    if (opt != "EX" && opt != "PX") {
      resp::write_error(out, "ERR syntax error");
      return;
    }
    if (!parse_positive(args[4], n)) {
      resp::write_error(out, "ERR invalid expire time in 'set' command");
      return;
    }
    if (opt == "EX") {
      if (n > std::numeric_limits<std::int64_t>::max() / 1000) {
        resp::write_error(out, "ERR invalid expire time in 'set' command");
        return;
      }
      n *= 1000;
    }
    ttl = Millis(n);
  }

  switch (cache_.set(args[1], std::move(args[2]), ttl)) {
    case SetResult::Ok:
      resp::write_simple(out, "OK");
      break;
    case SetResult::InvalidTtl:
      resp::write_error(out, "ERR invalid expire time in 'set' command");
      break;
    case SetResult::TooLarge:
      resp::write_error(out, "ERR value too large for cache shard");
      break;
    case SetResult::OutOfMemory:
      resp::write_error(out, "OOM command not allowed when used memory > 'maxmemory'");
      break;
  }
}

void CommandHandler::cmd_info(std::string& out) {
  const CacheStats s = cache_.stats();
  std::string text = "# Server\r\n";
  if (server_info_) server_info_(text);
  text += "# Stats\r\n";
  text += "keyspace_hits:" + std::to_string(s.hits) + "\r\n";
  text += "keyspace_misses:" + std::to_string(s.misses) + "\r\n";
  text += "expired_keys:" + std::to_string(s.expirations) + "\r\n";
  text += "evicted_keys:" + std::to_string(s.evictions) + "\r\n";
  text += "# Memory\r\n";
  text += "used_memory:" + std::to_string(cache_.used_bytes()) + "\r\n";
  text += "maxmemory:" + std::to_string(cache_.capacity_bytes()) + "\r\n";
  text += "# Keyspace\r\n";
  text += "keys:" + std::to_string(cache_.size()) + "\r\n";
  resp::write_bulk(out, text);
}

}  // namespace cachegrid
