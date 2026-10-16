#pragma once
//
// Web API (HTTP Basic auth, plain HTTP, local network only).
//
//   GET  /api/config                    current config, passwords blanked
//   PUT  /api/config[?reboot=1]         replace the config (JSON body, max 8 KB)
//   GET  /api/logs                      {"current": "...", "files": [{name,size}, ...]}
//   GET  /api/logs?file=NAME            whole file, streamed in chunks
//   GET  /api/logs?file=NAME&tail=N     last N bytes (max 32 KB), complete lines
//
// NAME is "log-NNNN.txt" or "current". There is deliberately nothing else here.
//
namespace WebApi {

// Registers the routes and starts listening on port 80.
void begin();

}  // namespace WebApi
