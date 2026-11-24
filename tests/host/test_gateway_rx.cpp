#include "gateway_core.h"
#include "harness.h"

static uint64_t esp_timer_get_time() { return 1234; }

#define CAN_GATEWAY_HAS_RX_TAPS
#define USE_CAN_GATEWAY_STATS
#define RX_TEST_NAME gateway_route_less_statistics_only_receives
namespace statistics_only {
#include "gateway_rx_test.inc"
}
#undef RX_TEST_NAME
#undef USE_CAN_GATEWAY_STATS

#define USE_CAN_GATEWAY_SNAPSHOT
#define RX_TEST_NAME gateway_route_less_snapshot_only_receives
namespace snapshot_only {
#include "gateway_rx_test.inc"
}
#undef RX_TEST_NAME
#undef USE_CAN_GATEWAY_SNAPSHOT

#define USE_CAN_GATEWAY_OBSERVE
#define RX_TEST_NAME gateway_route_less_observe_only_receives
namespace observe_only {
#include "gateway_rx_test.inc"
}
#undef RX_TEST_NAME
#undef USE_CAN_GATEWAY_OBSERVE

#define USE_CAN_GATEWAY_LOG_TAP
#define RX_TEST_NAME gateway_route_less_log_tap_only_receives
namespace log_tap_only {
#include "gateway_rx_test.inc"
}
#undef RX_TEST_NAME
#undef USE_CAN_GATEWAY_LOG_TAP

#define USE_CAN_GATEWAY_ISR_HOOK
#define RX_TEST_NAME gateway_route_less_hook_only_receives
namespace hook_only {
#include "gateway_rx_test.inc"
}
#undef RX_TEST_NAME
#undef USE_CAN_GATEWAY_ISR_HOOK
#undef CAN_GATEWAY_HAS_RX_TAPS

#define RX_TEST_NAME gateway_route_less_without_consumers_discards
namespace no_consumers {
#include "gateway_rx_test.inc"
}
#undef RX_TEST_NAME
