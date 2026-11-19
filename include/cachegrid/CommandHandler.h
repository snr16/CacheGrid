#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cachegrid/Cache.h"

namespace cachegrid {

// Layout of one entry in a SCAN reply: [key, version, ttl_ms|-1, tombstone, value].
// Shared by the node (writes it) and the proxy (reads it during recovery).
namespace scan_entry {
inline constexpr std::size_t kKey = 0;
inline constexpr std::size_t kVersion = 1;
inline constexpr std::size_t kTtlMs = 2;
inline constexpr std::size_t kTombstone = 3;
inline constexpr std::size_t kValue = 4;
inline constexpr std::size_t kFields = 5;
}  // namespace scan_entry

// Turns one parsed command into cache calls and a RESP reply.
//
// Knows nothing about sockets, so every command can be unit-tested directly.
//
// Supported commands (case-insensitive):
//   PING [message]                      -> +PONG | bulk message
//   GET key                             -> bulk value | null
//   SET key value [EX seconds|PX ms]    -> +OK | error
//   DEL key [key ...]                   -> integer (number of keys removed)
//   DBSIZE                              -> integer (number of keys)
//   INFO                                -> bulk text of stats
//   QUIT                                -> +OK, then the server closes the connection
//   COMMAND [...]                       -> empty array (lets redis-cli connect)
//
// Internal commands used by the proxy for replication and recovery:
//   VSET version key value [EX|PX n]    -> +OK | +STALE (a newer version is stored)
//   VDEL version key                    -> :1 if a live value was removed, else :0
//   SCAN shard                          -> one shard's entries, with versions and TTLs
//   FLUSHALL                            -> +OK (removes everything)
//   RUNID                               -> bulk: random id chosen when this node
//                                          process started (a changed id = it restarted)
class CommandHandler {
 public:
  enum class Action { Continue, Close };

  // `server_info` appends server-level stats (connections etc.) to INFO
  // output, as "name:value\r\n" lines.
  using InfoFn = std::function<void(std::string&)>;

  CommandHandler(Cache& cache, InfoFn server_info, std::string run_id);

  // `args` must be non-empty. Arguments may be moved from (SET moves the value
  // into the cache to avoid a copy).
  Action execute(std::vector<std::string>& args, std::string& out);

 private:
  void cmd_set(std::vector<std::string>& args, std::string& out);
  void cmd_vset(std::vector<std::string>& args, std::string& out);
  void cmd_vdel(std::vector<std::string>& args, std::string& out);
  void cmd_scan(std::vector<std::string>& args, std::string& out);
  void cmd_info(std::string& out);

  Cache& cache_;
  InfoFn server_info_;
  std::string run_id_;
};

}  // namespace cachegrid
