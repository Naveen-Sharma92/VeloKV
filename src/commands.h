// The command table and its handlers: looks a command up by name, checks its
// argument count, runs it against the keyspace, appends the RESP reply, and
// reports every change it made so the AOF can record it.
#pragma once

#include <functional>
#include <string>
#include <vector>

class Keyspace;

using Args = std::vector<std::string>;

// Receives each change a write command made, as a command that reproduces the
// change exactly when replayed later: relative TTLs become absolute PEXPIREAT /
// PXAT, and writes that changed nothing are not reported.
using Propagate = std::function<void(const Args& change)>;

// Runs one parsed command (args[0] is the command name, matched
// case-insensitively) and appends its reply to `out`. `args` must not be empty.
void executeCommand(Keyspace& db, const Args& args, std::string& out, const Propagate& propagate = {});
