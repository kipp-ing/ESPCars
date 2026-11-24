#include "gateway_core.h"
#include "harness.h"
#include <type_traits>

namespace gateway_frame_data_test {
using namespace esphome::can_gateway;
#include "gateway_frame_data.inc"

static_assert(sizeof(CanGatewayFrameData) == MAX_FRAME_DATA_LEN);
static_assert(std::is_trivially_copyable_v<CanGatewayFrameData>);

TEST(gateway_frame_data_copy_outlives_source_and_preserves_pointer_conversion) {
  const uint8_t expected[]{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
  CanGatewayFrameData retained = []() {
    uint8_t buffer[]{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    CanGatewayFrameData original;
    for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
      original.bytes[i] = buffer[i];
    CanGatewayFrameData copy = original;
    const uint8_t *original_data = original;
    const uint8_t *copy_data = copy;
    CHECK(copy_data != buffer);
    CHECK(copy_data != original_data);
    for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++) {
      buffer[i] = 0xEE;
      original.bytes[i] = 0xFF;
      CHECK_EQ(copy[i], 0x10 + i);
    }
    return copy;
  }();
  // Both the input buffer and the original value are now out of scope.
  // CHECK_BYTES takes byte pointers, exercising the implicit conversion too.
  CHECK_BYTES(retained, expected, MAX_FRAME_DATA_LEN);
}
}  // namespace gateway_frame_data_test
