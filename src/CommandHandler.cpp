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

// Strict unsigned decimal parse (no sign, no junk, no overflow).
bool parse_u64(const std::string& s, std::uint64_t& out) {
  if (s.empty()) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (v > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
    v = v * 10 + digit;
  }
  out = v;
  return true;
}

bool parse_positive(const std::string& s, std::int64_t& out) {
  std::uint64_t v = 0;
  if (!parse_u64(s, v) || v == 0 || v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  out = static_cast<std::int64_t>(v);
  return true;
}

// Parses the optional "[EX seconds | PX milliseconds]" pair starting at
// args[at]. Writes an error reply and returns false if it's invalid.
bool parse_expiry(const std::vector<std::string>& args, std::size_t at, std::optional<Millis>& ttl,
                  std::string& out) {
  if (args.size() == at) return true;  // no expiry given
  const std::string opt = upper(args[at]);
  if (opt != "EX" && opt != "PX") {
    resp::write_error(out, "ERR syntax error");
    return false;
  }
  std::int64_t n = 0;
  const bool ok = parse_positive(args[at + 1], n) &&
                  (opt == "PX" || n <= std::numeric_limits<std::int64_t>::max() / 1000);
  if (!ok) {
    resp::write_error(out, "ERR invalid expire time in 'set' command");
    return false;
  }
  ttl = Millis(opt == "EX" ? n * 1000 : n);
  return true;
}

void write_set_result(SetResult r, std::string& out) {
  switch (r) {
    case SetResult::Ok:
      resp::write_simple(out, "OK");
      break;
    case SetResult::Stale:
      // Not an error: the node already has this write or a newer one.
      resp::write_simple(out, "STALE");
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

}  // namespace

CommandHandler::CommandHandler(Cache& cache, InfoFn server_info, std::string run_id)
    : cache_(cache), server_info_(std::move(server_info)), run_id_(std::move(run_id)) {}

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
  } else if (cmd == "VSET") {
    cmd_vset(args, out);
  } else if (cmd == "VDEL") {
    cmd_vdel(args, out);
  } else if (cmd == "SCAN") {
    cmd_scan(args, out);
  } else if (cmd == "RUNID") {
    resp::write_bulk(out, run_id_);
  } else if (cmd == "FLUSHALL") {
    cache_.clear();
    resp::write_simple(out, "OK");
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

// SET key value [EX seconds | PX milliseconds]
void CommandHandler::cmd_set(std::vector<std::string>& args, std::string& out) {
  if (args.size() != 3 && args.size() != 5) {
    wrong_arity(out, "SET");
    return;
  }
  std::optional<Millis> ttl;
  if (!parse_expiry(args, 3, ttl, out)) return;
  write_set_result(cache_.set(args[1], std::move(args[2]), ttl), out);
}

// VSET version key value [EX seconds | PX milliseconds]
// A SET that only applies if `version` is newer than what's stored.
void CommandHandler::cmd_vset(std::vector<std::string>& args, std::string& out) {
  if (args.size() != 4 && args.size() != 6) {
    wrong_arity(out, "VSET");
    return;
  }
  std::uint64_t version = 0;
  if (!parse_u64(args[1], version) || version == 0) {
    resp::write_error(out, "ERR invalid version");
    return;
  }
  std::optional<Millis> ttl;
  if (!parse_expiry(args, 4, ttl, out)) return;
  write_set_result(cache_.set_versioned(args[2], std::move(args[3]), ttl, version), out);
}

// VDEL version key  ->  :1 if a live value was removed, else :0
// Leaves a tombstone unless a newer version is already stored.
void CommandHandler::cmd_vdel(std::vector<std::string>& args, std::string& out) {
  if (args.size() != 3) {
    wrong_arity(out, "VDEL");
    return;
  }
  std::uint64_t version = 0;
  if (!parse_u64(args[1], version) || version == 0) {
    resp::write_error(out, "ERR invalid version");
    return;
  }
  const DelResult r = cache_.del_versioned(args[2], version);
  if (r.status != SetResult::Ok && r.status != SetResult::Stale) {
    write_set_result(r.status, out);
    return;
  }
  resp::write_integer(out, r.removed_live ? 1 : 0);
}

// SCAN shard  ->  [shard_count, [[key, version, ttl_ms|-1, tombstone, value], ...]]
// Returns one shard's entries; a recovering node is refilled by scanning
// shards 0..shard_count-1 on its peers.
void CommandHandler::cmd_scan(std::vector<std::string>& args, std::string& out) {
  if (args.size() != 2) {
    wrong_arity(out, "SCAN");
    return;
  }
  std::uint64_t shard = 0;
  if (!parse_u64(args[1], shard) || shard >= cache_.num_shards()) {
    resp::write_error(out, "ERR shard index out of range");
    return;
  }
  const std::vector<ExportedEntry> entries = cache_.export_shard(static_cast<std::size_t>(shard));
  resp::write_array_header(out, 2);
  resp::write_integer(out, static_cast<std::int64_t>(cache_.num_shards()));
  resp::write_array_header(out, entries.size());
  for (const ExportedEntry& e : entries) {
    // Field order defined by scan_entry:: in CommandHandler.h.
    resp::write_array_header(out, scan_entry::kFields);
    resp::write_bulk(out, e.key);
    resp::write_bulk(out, std::to_string(e.version));  // text: versions are unsigned 64-bit
    resp::write_integer(out, e.ttl_remaining ? e.ttl_remaining->count() : -1);
    resp::write_integer(out, e.tombstone ? 1 : 0);
    resp::write_bulk(out, e.value);
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
