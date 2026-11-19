#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cachegrid/Cache.h"

namespace cachegrid {

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
class CommandHandler {
 public:
  enum class Action { Continue, Close };

  // `server_info` appends server-level stats (connections etc.) to INFO
  // output, as "name:value\r\n" lines.
  using InfoFn = std::function<void(std::string&)>;

  CommandHandler(Cache& cache, InfoFn server_info);

  // `args` must be non-empty. Arguments may be moved from (SET moves the value
  // into the cache to avoid a copy).
  Action execute(std::vector<std::string>& args, std::string& out);

 private:
  void cmd_set(std::vector<std::string>& args, std::string& out);
  void cmd_info(std::string& out);

  Cache& cache_;
  InfoFn server_info_;
};

}  // namespace cachegrid
