#pragma once

// Topic (first frame) and process-name constants shared by every process.
// PUB/SUB messages are two frames: [topic][serialized protobuf].
namespace cell::topic {
inline constexpr const char* kArmJoint = "arm.joint";        // JointState
inline constexpr const char* kSensorFrame = "sensor.frame";  // PointCloudFrame / FrameInfo
inline constexpr const char* kPlcConveyor = "plc.conveyor";  // ConveyorState
inline constexpr const char* kHeartbeat = "hb";              // Heartbeat
inline constexpr const char* kCellStats = "cell.stats";      // CellStats (broker)
inline constexpr const char* kCellState = "cell.state";      // CellState (broker)
}  // namespace cell::topic

namespace cell::process {
inline constexpr const char* kArmSim = "arm_sim";
inline constexpr const char* kSensorSim = "sensor_sim";
inline constexpr const char* kPlcBridge = "plc_bridge";
inline constexpr const char* kPlcSim = "plc_sim";
inline constexpr const char* kBroker = "broker";
}  // namespace cell::process
