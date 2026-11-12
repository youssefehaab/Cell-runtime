#pragma once

namespace cell {

// Process-wide graceful-shutdown flag driven by SIGINT/SIGTERM.
//
// The handler only sets a lock-free atomic flag. It is installed without
// SA_RESTART so blocking syscalls return EINTR; the zmq helpers in
// zmq_util.hpp translate that into "nothing happened" so loops can simply be
//   while (!shutdown_requested()) { poll_in(sock, 100ms); ... }
void install_shutdown_handlers();

bool shutdown_requested() noexcept;

// Request shutdown from code (fatal error paths, tests).
void request_shutdown() noexcept;

// Test helper: clear the flag.
void reset_shutdown_flag() noexcept;

}  // namespace cell
