#pragma once

#include "broker/broker_config.hpp"

namespace cell::broker {

// Run the broker until cell::shutdown_requested(). Returns the process exit
// code: 0 after a clean shutdown, 1 if a thread failed.
//
// Threads (each owns its sockets exclusively; see shared.hpp):
//   io          SUB sockets -> header decode -> SPSC ring          (io_thread.cpp)
//   processing  ring -> stats / health / downsampling -> XPUB      (processing_thread.cpp)
//   command     ROUTER <-> per-device DEALERs                      (command_thread.cpp)
//   main        supervisor: start-up, systemd watchdog, shutdown   (broker.cpp)
int run_broker(const BrokerConfig& config);

}  // namespace cell::broker
