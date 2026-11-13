#pragma once

#include <cstddef>

#include "cell.pb.h"

namespace cell::broker {

// Build a FrameInfo from a serialized PointCloudFrame without parsing it:
// walks the top-level wire fields, decodes the header (field 1) and the two
// varints width (2) and height (3), and for `points` (field 4) reads only the
// length prefix and steps over the body. Nothing is copied or allocated for
// the multi-megabyte cloud, and the cost does not depend on its size.
//
// FrameInfo.bytes is the size of the `points` blob (width * height * 12 for a
// well-formed frame), i.e. the size of the cloud itself, not of the envelope.
//
// Returns false if the buffer is not a well-formed message or has no header;
// `out` is then unspecified.
bool scan_frame_info(const void* data, std::size_t size, FrameInfo& out);

}  // namespace cell::broker
