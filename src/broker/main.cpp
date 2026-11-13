// broker: subscribes to every device stream, measures latency and sequence
// continuity, republishes downsampled telemetry, cell state and statistics for
// consumers, and routes external commands to the devices.
#include "broker/broker.hpp"
#include "broker/broker_config.hpp"
#include "common/app.hpp"
#include "common/topics.hpp"

namespace {

int run(const cell::Json& config) {
  return cell::broker::run_broker(cell::broker::BrokerConfig::from_json(config));
}

}  // namespace

int main(int argc, char** argv) {
  return cell::run_app(
      argc, argv,
      {cell::process::kBroker, "Cell broker: telemetry aggregation, statistics, command routing"},
      run);
}
