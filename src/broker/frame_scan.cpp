#include "broker/frame_scan.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>

#include <cstdint>
#include <limits>

namespace cell::broker {

bool scan_frame_info(const void* data, std::size_t size, FrameInfo& out) {
  using google::protobuf::internal::WireFormatLite;
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  out.Clear();
  // Array-backed stream: Skip() and SkipField() only advance a pointer.
  google::protobuf::io::CodedInputStream in(static_cast<const std::uint8_t*>(data),
                                            static_cast<int>(size));
  bool have_header = false;
  while (true) {
    const std::uint32_t tag = in.ReadTag();
    if (tag == 0) {
      // 0 is both "clean end of buffer" and "invalid tag".
      return have_header && in.ConsumedEntireMessage();
    }
    const int field = WireFormatLite::GetTagFieldNumber(tag);
    const auto wire_type = WireFormatLite::GetTagWireType(tag);
    if (wire_type == WireFormatLite::WIRETYPE_LENGTH_DELIMITED && (field == 1 || field == 4)) {
      std::uint32_t len = 0;
      if (!in.ReadVarint32(&len)) {
        return false;
      }
      if (field == 4) {
        out.set_bytes(len);
      } else if (len == 0) {
        out.mutable_h()->Clear();  // empty header: nothing to point at
        have_header = true;
      } else {
        const void* ptr = nullptr;
        int avail = 0;
        if (!in.GetDirectBufferPointer(&ptr, &avail) || static_cast<std::uint32_t>(avail) < len ||
            !out.mutable_h()->ParseFromArray(ptr, static_cast<int>(len))) {
          return false;
        }
        have_header = true;
      }
      // Fails if the declared length runs past the end of the buffer.
      if (len > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
          !in.Skip(static_cast<int>(len))) {
        return false;
      }
    } else if (wire_type == WireFormatLite::WIRETYPE_VARINT && (field == 2 || field == 3)) {
      std::uint32_t value = 0;
      if (!in.ReadVarint32(&value)) {
        return false;
      }
      if (field == 2) {
        out.set_width(value);
      } else {
        out.set_height(value);
      }
    } else if (!WireFormatLite::SkipField(&in, tag)) {
      return false;
    }
  }
}

}  // namespace cell::broker
