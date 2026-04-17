#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#define stringify(x) #x

// ==========================================
// TOKEN BUCKET TX MODE (must be defined early, used throughout)
// ==========================================
// 0 = Current smooth pacing mode (rate limiter based)
// 1 = Token bucket mode: 1 packet from each VL-IDX every 1ms
#ifndef TOKEN_BUCKET_TX_ENABLED
#define TOKEN_BUCKET_TX_ENABLED 0
#endif

#if TOKEN_BUCKET_TX_ENABLED
// Per-port VL range size for token bucket mode
#define TB_VL_RANGE_SIZE_DEFAULT 70
#define TB_VL_RANGE_SIZE_NO_EXT  74   // Port 1, 7 (no external TX)
#define GET_TB_VL_RANGE_SIZE(port_id) \
    (((port_id) == 1 || (port_id) == 7) ? TB_VL_RANGE_SIZE_NO_EXT : TB_VL_RANGE_SIZE_DEFAULT)

// Token bucket window (ms) - can be fractional (e.g., 1.0, 1.4, 2.5)
#define TB_WINDOW_MS 1.05
#define TB_PACKETS_PER_VL_PER_WINDOW 1
#endif

// ==========================================
// LATENCY TEST CONFIGURATION
// ==========================================
// When enabled:
// - 1 packet is sent from each VLAN (first VL-ID is used)
// - TX timestamp is written to payload
// - Latency is calculated and displayed on RX
// - 5 second timeout
// - Normal mode resumes after test
// - IMIX disabled, MAX packet size (1518) is used

#ifndef LATENCY_TEST_ENABLED
#define LATENCY_TEST_ENABLED 0
#endif

#define LATENCY_TEST_TIMEOUT_SEC 5    // Packet wait timeout (seconds)
#define LATENCY_TEST_PACKET_SIZE 1518 // Test packet size (MAX)

// ==========================================
// IMIX (Internet Mix) CONFIGURATION
// ==========================================
// Custom IMIX profile: Distribution of different packet sizes
// Total ratio: 10% + 10% + 10% + 10% + 30% + 30% = 100%
//
// In a 10 packet cycle:
//   1x 100 byte  (10%)
//   1x 200 byte  (10%)
//   1x 400 byte  (10%)
//   1x 800 byte  (10%)
//   3x 1200 byte (30%)
//   3x 1518 byte (30%)  - MTU limit
//
// Average packet size: ~964 bytes

#define IMIX_ENABLED 0

// IMIX size levels (Ethernet frame size, including VLAN)
#define IMIX_SIZE_1 100 // Smallest
#define IMIX_SIZE_2 200
#define IMIX_SIZE_3 400
#define IMIX_SIZE_4 800
#define IMIX_SIZE_5 1200
#define IMIX_SIZE_6 1518 // MTU limit (1522 with VLAN, but 1518 is safe)

// IMIX pattern size (10 packet cycle)
#define IMIX_PATTERN_SIZE 10

// IMIX average packet size (for rate limiting)
// (100 + 200 + 400 + 800 + 1200*3 + 1518*3) / 10 = 964.4
#define IMIX_AVG_PACKET_SIZE 964

// IMIX minimum and maximum sizes
#define IMIX_MIN_PACKET_SIZE IMIX_SIZE_1
#define IMIX_MAX_PACKET_SIZE IMIX_SIZE_6

// IMIX pattern array (static definition - each worker uses its own offset)
// Order: 100, 200, 400, 800, 1200, 1200, 1200, 1518, 1518, 1518
#define IMIX_PATTERN_INIT {                             \
    IMIX_SIZE_1, IMIX_SIZE_2, IMIX_SIZE_3, IMIX_SIZE_4, \
    IMIX_SIZE_5, IMIX_SIZE_5, IMIX_SIZE_5,              \
    IMIX_SIZE_6, IMIX_SIZE_6, IMIX_SIZE_6}

// ==========================================
// VLAN & VL-ID MAPPING (PORT-AWARE)
// ==========================================
//
// tx_vl_ids and rx_vl_ids can be DIFFERENT for each port!
// Ranges contain 128 VL-IDs and are defined as [start, start+128).
//
// Example (Port 0):
//   tx_vl_ids = {1027, 1155, 1283, 1411}
//   Queue 0 → VL ID [1027, 1155)  → 1027..1154 (128 entries)
//   Queue 1 → VL ID [1155, 1283)  → 1155..1282 (128 entries)
//   Queue 2 → VL ID [1283, 1411)  → 1283..1410 (128 entries)
//   Queue 3 → VL ID [1411, 1539)  → 1411..1538 (128 entries)
//
// Example (Port 2 - old default values):
//   tx_vl_ids = {3, 131, 259, 387}
//   Queue 0 → VL ID [  3, 131)  → 3..130   (128 entries)
//   Queue 1 → VL ID [131, 259)  → 131..258 (128 entries)
//   Queue 2 → VL ID [259, 387)  → 259..386 (128 entries)
//   Queue 3 → VL ID [387, 515)  → 387..514 (128 entries)
//
// Note: VLAN ID in the VLAN header (802.1Q tag) and VL-ID are different concepts.
// VL-ID is written to the last 2 bytes of the packet's DST MAC and DST IP.
// VLAN ID comes from the .tx_vlans / .rx_vlans arrays.
//
// When building packets:
//   DST MAC: 03:00:00:00:VV:VV  (VV: 16-bit of VL-ID)
//   DST IP : 224.224.VV.VV      (VV: 16-bit of VL-ID)
//
// NOTE: g_vlid_ranges is NO LONGER USED! Kept for reference only.
// Actual VL-ID ranges are read from port_vlans[].tx_vl_ids and rx_vl_ids.

typedef struct
{
  uint16_t start; // inclusive
  uint16_t end;   // exclusive
} vlid_range_t;

// DEPRECATED: These constant values are no longer used!
// tx_vl_ids/rx_vl_ids values from config are used for each port.
#define VLID_RANGE_COUNT 4
static const vlid_range_t g_vlid_ranges[VLID_RANGE_COUNT] = {
    {3, 131},   // Queue 0 (reference only)
    {131, 259}, // Queue 1 (reference only)
    {259, 387}, // Queue 2 (reference only)
    {387, 515}  // Queue 3 (reference only)
};

// DEPRECATED: These macros are no longer used!
// Use the port-aware functions in tx_rx_manager.c.
#define VL_RANGE_START(q) (g_vlid_ranges[(q)].start)
#define VL_RANGE_END(q) (g_vlid_ranges[(q)].end)
#define VL_RANGE_SIZE(q) (uint16_t)(VL_RANGE_END(q) - VL_RANGE_START(q)) // 128

// ==========================================
/* VLAN CONFIGURATION */
// ==========================================MM
#define MAX_TX_VLANS_PER_PORT 32
#define MAX_RX_VLANS_PER_PORT 32
#define MAX_PORTS_CONFIG 16

struct port_vlan_config
{
  uint16_t tx_vlans[MAX_TX_VLANS_PER_PORT]; // VLAN header tags
  uint16_t tx_vlan_count;
  uint16_t rx_vlans[MAX_RX_VLANS_PER_PORT]; // VLAN header tags
  uint16_t rx_vlan_count;

  // Initial VL-IDs for init (matches queue index)
  // In dynamic usage, you will iterate within these VL ranges.
  uint16_t tx_vl_ids[MAX_TX_VLANS_PER_PORT]; // VL-ID start per queue
  uint16_t rx_vl_ids[MAX_RX_VLANS_PER_PORT]; // VL-ID start per queue

  // Per-queue VL-ID count (0 = use default VL_RANGE_SIZE_PER_QUEUE)
  uint16_t tx_vl_counts[MAX_TX_VLANS_PER_PORT];
};

// Per-port VLAN/VL-ID template (queue index ↔ VL range start matches)
// Unit test mode: loopback (same port TX→RX), cross for J6-2↔J6-4
// VL-ID counts are variable per queue (tx_vl_counts required)
#define PORT_VLAN_CONFIG_INIT                                                                                                                                                                                                                               \
  {                                                                                                                                                                                                                                                         \
    /* Port 0: TX 105-108, RX 233-236 (J6-1 loopback, J6-2/J6-4 cross, J6-3 loopback) */                                                                                                                                                                  \
    {.tx_vlans = {105, 106, 107, 108}, .tx_vlan_count = 4, .rx_vlans = {233, 234, 235, 236}, .rx_vlan_count = 4, .tx_vl_ids = {602, 622, 346, 366}, .rx_vl_ids = {592, 612, 336, 356}, .tx_vl_counts = {10, 6, 10, 6}},       /* Port 1 */                 \
        {.tx_vlans = {109, 110, 111, 112}, .tx_vlan_count = 4, .rx_vlans = {237, 238, 239, 240}, .rx_vlan_count = 4, .tx_vl_ids = {522, 542, 562, 582}, .rx_vl_ids = {512, 532, 552, 572}, .tx_vl_counts = {10, 10, 10, 10}}, /* Port 2 */                 \
        {.tx_vlans = {97, 98, 99, 100}, .tx_vlan_count = 4, .rx_vlans = {225, 226, 227, 228}, .rx_vlan_count = 4, .tx_vl_ids = {160, 224, 417, 480}, .rx_vl_ids = {128, 192, 385, 448}, .tx_vl_counts = {32, 30, 31, 32}},    /* Port 3 */                 \
        {.tx_vlans = {101, 102, 103, 104}, .tx_vlan_count = 4, .rx_vlans = {229, 230, 231, 232}, .rx_vlan_count = 4, .tx_vl_ids = {266, 286, 306, 326}, .rx_vl_ids = {256, 276, 296, 316}, .tx_vl_counts = {10, 10, 10, 10}},                              \
  }

// ==========================================
// ATE TEST MODE - PORT VLAN CONFIGURATION (Loopback)
// ==========================================
// Each port sends with TX VLAN and receives back on RX VLAN with same VL-IDX.
// Selected at runtime based on g_ate_mode flag.

#define ATE_PORT_VLAN_CONFIG_INIT                                                                                                                                                                                                                        \
  {                                                                                                                                                                                                                                                      \
    /* Port 0: TX VLAN 105-108, RX VLAN 233-236, VL-IDX: 602-611, 622-627, 346-355, 366-371 */                                                                                                                                                          \
    {.tx_vlans = {105, 106, 107, 108}, .tx_vlan_count = 4, .rx_vlans = {233, 234, 235, 236}, .rx_vlan_count = 4, .tx_vl_ids = {602, 622, 346, 366}, .rx_vl_ids = {602, 622, 346, 366}, .tx_vl_counts = {10, 6, 10, 6}},       /* Port 1 */              \
        {.tx_vlans = {109, 110, 111, 112}, .tx_vlan_count = 4, .rx_vlans = {237, 238, 239, 240}, .rx_vlan_count = 4, .tx_vl_ids = {522, 542, 562, 582}, .rx_vl_ids = {522, 542, 562, 582}, .tx_vl_counts = {10, 10, 10, 10}}, /* Port 2 */              \
        {.tx_vlans = {97, 98, 99, 100}, .tx_vlan_count = 4, .rx_vlans = {225, 226, 227, 228}, .rx_vlan_count = 4, .tx_vl_ids = {161, 224, 416, 480}, .rx_vl_ids = {161, 224, 416, 480}, .tx_vl_counts = {29, 30, 32, 32}},    /* Port 3 */              \
        {.tx_vlans = {101, 102, 103, 104}, .tx_vlan_count = 4, .rx_vlans = {229, 230, 231, 232}, .rx_vlan_count = 4, .tx_vl_ids = {266, 286, 306, 326}, .rx_vl_ids = {266, 286, 306, 326}, .tx_vl_counts = {10, 10, 10, 10}},                          \
  }

// ==========================================
// TX/RX CORE CONFIGURATION
// ==========================================
// (Can be overridden via Makefile)
#ifndef NUM_TX_CORES
#define NUM_TX_CORES 2
#endif

#ifndef NUM_RX_CORES
#define NUM_RX_CORES 4
#endif

// ==========================================
// PORT-BASED RATE LIMITING
// ==========================================
// All ports use the same target rate

#ifndef TARGET_GBPS
#define TARGET_GBPS 3.6
#endif

#ifndef RATE_LIMITER_ENABLED
#define RATE_LIMITER_ENABLED 1
#endif

// Queue counts equal core counts
#define NUM_TX_QUEUES_PER_PORT NUM_TX_CORES
#define NUM_RX_QUEUES_PER_PORT NUM_RX_CORES

// ==========================================
// PACKET CONFIGURATION (Fixed fields)
// ==========================================
#define DEFAULT_TTL 1
#define DEFAULT_TOS 0
#define DEFAULT_VLAN_PRIORITY 0

// MAC/IP templates
#define DEFAULT_SRC_MAC "02:00:00:00:00:20"  // Fixed source MAC
#define DEFAULT_DST_MAC_PREFIX "03:00:00:00" // Last 2 bytes = VL-ID

#define DEFAULT_SRC_IP "10.0.0.0"       // Fixed source IP
#define DEFAULT_DST_IP_PREFIX "224.224" // Last 2 bytes = VL-ID

// UDP ports
#define DEFAULT_SRC_PORT 100
#define DEFAULT_DST_PORT 100

// ==========================================
// STATISTICS CONFIGURATION
// ==========================================
#define STATS_INTERVAL_SEC 1 // Write statistics every N seconds

// DPDK External TX removed (was for raw socket ports 12/13)
#define DPDK_EXT_TX_ENABLED 0
#define DPDK_EXT_TX_PORT_COUNT 6 // Port 2,3,4,5 → Port 12 | Port 0,6 → Port 13
#define DPDK_EXT_TX_QUEUES_PER_PORT 4

// External TX target configuration
struct dpdk_ext_tx_target
{
  uint16_t queue_id;    // Queue index (0-3)
  uint16_t vlan_id;     // VLAN tag
  uint16_t vl_id_start; // VL-ID start
  uint16_t vl_id_count; // VL-ID count (32)
  uint32_t rate_mbps;   // Target rate (Mbps)
};

// External TX port configuration
struct dpdk_ext_tx_port_config
{
  uint16_t port_id;      // DPDK port ID
  uint16_t dest_port;    // Destination raw socket port (12 or 13)
  uint16_t target_count; // Number of targets (4)
  struct dpdk_ext_tx_target targets[DPDK_EXT_TX_QUEUES_PER_PORT];
};

#if TOKEN_BUCKET_TX_ENABLED
// ==========================================
// TOKEN BUCKET: DPDK External TX → Port 12
// ==========================================
// 4 VL-IDX per VLAN, each VL-IDX sends 1 packet per 1ms
// Per port: 4 VLAN × 4 VL = 16 VL → 16000 pkt/s

// Port 2: VLAN 97-100 → Port 12 (4 VL per VLAN)
#define DPDK_EXT_TX_PORT_2_TARGETS {                                                          \
    {.queue_id = 0, .vlan_id = 97, .vl_id_start = 4291, .vl_id_count = 4, .rate_mbps = 49},   \
    {.queue_id = 1, .vlan_id = 98, .vl_id_start = 4299, .vl_id_count = 4, .rate_mbps = 49},   \
    {.queue_id = 2, .vlan_id = 99, .vl_id_start = 4307, .vl_id_count = 4, .rate_mbps = 49},   \
    {.queue_id = 3, .vlan_id = 100, .vl_id_start = 4315, .vl_id_count = 4, .rate_mbps = 49},  \
}

// Port 3: VLAN 101-104 → Port 12 (4 VL per VLAN)
#define DPDK_EXT_TX_PORT_3_TARGETS {                                                          \
    {.queue_id = 0, .vlan_id = 101, .vl_id_start = 4323, .vl_id_count = 4, .rate_mbps = 49},  \
    {.queue_id = 1, .vlan_id = 102, .vl_id_start = 4331, .vl_id_count = 4, .rate_mbps = 49},  \
    {.queue_id = 2, .vlan_id = 103, .vl_id_start = 4339, .vl_id_count = 4, .rate_mbps = 49},  \
    {.queue_id = 3, .vlan_id = 104, .vl_id_start = 4347, .vl_id_count = 4, .rate_mbps = 49},  \
}

// Port 4: VLAN 113-116 → Port 12 (4 VL per VLAN)
#define DPDK_EXT_TX_PORT_4_TARGETS {                                                          \
    {.queue_id = 0, .vlan_id = 113, .vl_id_start = 4355, .vl_id_count = 4, .rate_mbps = 49},  \
    {.queue_id = 1, .vlan_id = 114, .vl_id_start = 4363, .vl_id_count = 4, .rate_mbps = 49},  \
    {.queue_id = 2, .vlan_id = 115, .vl_id_start = 4371, .vl_id_count = 4, .rate_mbps = 49},  \
    {.queue_id = 3, .vlan_id = 116, .vl_id_start = 4379, .vl_id_count = 4, .rate_mbps = 49},  \
}

// Port 5: VLAN 117-120 → Port 12 (4 VL per VLAN)
#define DPDK_EXT_TX_PORT_5_TARGETS {                                                          \
    {.queue_id = 0, .vlan_id = 117, .vl_id_start = 4387, .vl_id_count = 4, .rate_mbps = 49},  \
    {.queue_id = 1, .vlan_id = 118, .vl_id_start = 4395, .vl_id_count = 4, .rate_mbps = 49},  \
    {.queue_id = 2, .vlan_id = 119, .vl_id_start = 4403, .vl_id_count = 4, .rate_mbps = 49},  \
    {.queue_id = 3, .vlan_id = 120, .vl_id_start = 4411, .vl_id_count = 4, .rate_mbps = 49},  \
}

#else
// ==========================================
// NORMAL MODE: DPDK External TX → Port 12
// ==========================================
// Port 2: VLAN 97-100, VL-ID 4291-4322
// NOTE: Total external TX must not exceed Port 12's 1G capacity
// 4 ports × 220 Mbps = 880 Mbps total (within 1G limit)
#define DPDK_EXT_TX_PORT_2_TARGETS {                                                          \
    {.queue_id = 0, .vlan_id = 97, .vl_id_start = 4291, .vl_id_count = 8, .rate_mbps = 230},  \
    {.queue_id = 1, .vlan_id = 98, .vl_id_start = 4299, .vl_id_count = 8, .rate_mbps = 230},  \
    {.queue_id = 2, .vlan_id = 99, .vl_id_start = 4307, .vl_id_count = 8, .rate_mbps = 230},  \
    {.queue_id = 3, .vlan_id = 100, .vl_id_start = 4315, .vl_id_count = 8, .rate_mbps = 230}, \
}

// Port 3: VLAN 101-104, VL-ID 4323-4354 (8 per queue, no overlap)
#define DPDK_EXT_TX_PORT_3_TARGETS {                                                          \
    {.queue_id = 0, .vlan_id = 101, .vl_id_start = 4323, .vl_id_count = 8, .rate_mbps = 230}, \
    {.queue_id = 1, .vlan_id = 102, .vl_id_start = 4331, .vl_id_count = 8, .rate_mbps = 230}, \
    {.queue_id = 2, .vlan_id = 103, .vl_id_start = 4339, .vl_id_count = 8, .rate_mbps = 230}, \
    {.queue_id = 3, .vlan_id = 104, .vl_id_start = 4347, .vl_id_count = 8, .rate_mbps = 230}, \
}

// Port 4: VLAN 113-116, VL-ID 4355-4386
#define DPDK_EXT_TX_PORT_4_TARGETS {                                                          \
    {.queue_id = 0, .vlan_id = 113, .vl_id_start = 4355, .vl_id_count = 8, .rate_mbps = 230}, \
    {.queue_id = 1, .vlan_id = 114, .vl_id_start = 4363, .vl_id_count = 8, .rate_mbps = 230}, \
    {.queue_id = 2, .vlan_id = 115, .vl_id_start = 4371, .vl_id_count = 8, .rate_mbps = 230}, \
    {.queue_id = 3, .vlan_id = 116, .vl_id_start = 4379, .vl_id_count = 8, .rate_mbps = 230}, \
}

// Port 5: VLAN 117-120, VL-ID 4387-4418 → Port 12
#define DPDK_EXT_TX_PORT_5_TARGETS {                                                          \
    {.queue_id = 0, .vlan_id = 117, .vl_id_start = 4387, .vl_id_count = 8, .rate_mbps = 230}, \
    {.queue_id = 1, .vlan_id = 118, .vl_id_start = 4395, .vl_id_count = 8, .rate_mbps = 230}, \
    {.queue_id = 2, .vlan_id = 119, .vl_id_start = 4403, .vl_id_count = 8, .rate_mbps = 230}, \
    {.queue_id = 3, .vlan_id = 120, .vl_id_start = 4411, .vl_id_count = 8, .rate_mbps = 230}, \
}
#endif

// ==========================================
// PORT 0 and PORT 6 → PORT 13 (100M copper)
// ==========================================
// Port 0: 45 Mbps, Port 6: 45 Mbps = Total 90 Mbps

#if TOKEN_BUCKET_TX_ENABLED
// ==========================================
// TOKEN BUCKET: DPDK External TX → Port 13
// ==========================================
// Port 0: 3 VLAN × 1 VL = 3 VL → 3000 pkt/s (VLAN 108 excluded)
// Port 6: 3 VLAN × 1 VL = 3 VL → 3000 pkt/s (VLAN 124 excluded)
#define DPDK_EXT_TX_PORT_0_TARGETS {                                                         \
    {.queue_id = 0, .vlan_id = 105, .vl_id_start = 4099, .vl_id_count = 1, .rate_mbps = 13}, \
    {.queue_id = 1, .vlan_id = 106, .vl_id_start = 4103, .vl_id_count = 1, .rate_mbps = 13}, \
    {.queue_id = 2, .vlan_id = 107, .vl_id_start = 4107, .vl_id_count = 1, .rate_mbps = 13}, \
}

#define DPDK_EXT_TX_PORT_6_TARGETS {                                                         \
    {.queue_id = 0, .vlan_id = 121, .vl_id_start = 4115, .vl_id_count = 1, .rate_mbps = 13}, \
    {.queue_id = 1, .vlan_id = 122, .vl_id_start = 4119, .vl_id_count = 1, .rate_mbps = 13}, \
    {.queue_id = 2, .vlan_id = 123, .vl_id_start = 4123, .vl_id_count = 1, .rate_mbps = 13}, \
}

#else
// Port 0: VLAN 105-108, VL-ID 4099-4114 → Port 13 (total 45 Mbps)
#define DPDK_EXT_TX_PORT_0_TARGETS {                                                         \
    {.queue_id = 0, .vlan_id = 105, .vl_id_start = 4099, .vl_id_count = 4, .rate_mbps = 45}, \
    {.queue_id = 1, .vlan_id = 106, .vl_id_start = 4103, .vl_id_count = 4, .rate_mbps = 45}, \
    {.queue_id = 2, .vlan_id = 107, .vl_id_start = 4107, .vl_id_count = 4, .rate_mbps = 45}, \
    {.queue_id = 3, .vlan_id = 108, .vl_id_start = 4111, .vl_id_count = 4, .rate_mbps = 45}, \
}

// Port 6: VLAN 121-124, VL-ID 4115-4130 → Port 13 (total 45 Mbps)
#define DPDK_EXT_TX_PORT_6_TARGETS {                                                         \
    {.queue_id = 0, .vlan_id = 121, .vl_id_start = 4115, .vl_id_count = 4, .rate_mbps = 45}, \
    {.queue_id = 1, .vlan_id = 122, .vl_id_start = 4119, .vl_id_count = 4, .rate_mbps = 45}, \
    {.queue_id = 2, .vlan_id = 123, .vl_id_start = 4123, .vl_id_count = 4, .rate_mbps = 45}, \
    {.queue_id = 3, .vlan_id = 124, .vl_id_start = 4127, .vl_id_count = 4, .rate_mbps = 45}, \
}
#endif

// All external TX port configurations
// Port 2,3,4,5 → Port 12 (1G) | Port 0,6 → Port 13 (100M)
#if TOKEN_BUCKET_TX_ENABLED
// Token bucket: Port 0,6 have 3 targets (VLAN 108/124 excluded from P13)
#define DPDK_EXT_TX_PORTS_CONFIG_INIT {                                                        \
    {.port_id = 2, .dest_port = 12, .target_count = 4, .targets = DPDK_EXT_TX_PORT_2_TARGETS}, \
    {.port_id = 3, .dest_port = 12, .target_count = 4, .targets = DPDK_EXT_TX_PORT_3_TARGETS}, \
    {.port_id = 4, .dest_port = 12, .target_count = 4, .targets = DPDK_EXT_TX_PORT_4_TARGETS}, \
    {.port_id = 5, .dest_port = 12, .target_count = 4, .targets = DPDK_EXT_TX_PORT_5_TARGETS}, \
    {.port_id = 0, .dest_port = 13, .target_count = 3, .targets = DPDK_EXT_TX_PORT_0_TARGETS}, \
    {.port_id = 6, .dest_port = 13, .target_count = 3, .targets = DPDK_EXT_TX_PORT_6_TARGETS}, \
}
#else
#define DPDK_EXT_TX_PORTS_CONFIG_INIT {                                                        \
    {.port_id = 2, .dest_port = 12, .target_count = 4, .targets = DPDK_EXT_TX_PORT_2_TARGETS}, \
    {.port_id = 3, .dest_port = 12, .target_count = 4, .targets = DPDK_EXT_TX_PORT_3_TARGETS}, \
    {.port_id = 4, .dest_port = 12, .target_count = 4, .targets = DPDK_EXT_TX_PORT_4_TARGETS}, \
    {.port_id = 5, .dest_port = 12, .target_count = 4, .targets = DPDK_EXT_TX_PORT_5_TARGETS}, \
    {.port_id = 0, .dest_port = 13, .target_count = 4, .targets = DPDK_EXT_TX_PORT_0_TARGETS}, \
    {.port_id = 6, .dest_port = 13, .target_count = 4, .targets = DPDK_EXT_TX_PORT_6_TARGETS}, \
}
#endif

// ==========================================
// VMC PORT-BASED STATISTICS MODE
// ==========================================
// STATS_MODE_VMC=1: VMC per-port statistics table (16 rows, VMC Port 0-15)
//   - RX queue steering: rte_flow VLAN match (each queue = 1 VLAN = 1 VMC port)
//   - Zero overhead Gbps calculation via HW per-queue stats
//   - PRBS validation per VMC port
//
// STATS_MODE_VMC=0: Legacy server per-port table (4 rows, Server Port 0-3)
//   - RX queue steering: RSS (hash based)
//   - HW total port stats
//   - PRBS validation per server port

#ifndef STATS_MODE_VMC
#define STATS_MODE_VMC 1
#endif

// VMC port count: 16 DPDK ports (4 server ports × 4 VLANs)
#define VMC_PORT_COUNT 16
#define VMC_DPDK_PORT_COUNT 16

// 1 VLAN per VMC port
#define VMC_VLANS_PER_PORT 1

// ==========================================
// VMC PORT MAPPING TABLE
// ==========================================
// Each VMC port: TX/RX from VMC perspective
//   VMC RX = Server sends → VMC receives (server_tx_port, rx_vlan)
//   VMC TX = VMC sends → Server receives (server_rx_port, tx_vlan)
//
// VMC Port 0-15: DPDK ports (4 ports × 4 VLANs)
// Pairs: Port 0↔1, Port 2↔3

struct vmc_port_map_entry {
    uint16_t vmc_port_id;       // VMC port number (0-15)

    // VMC RX (Server → VMC): Server sends from this VLAN
    uint16_t rx_vlan;           // Server TX VLAN (VMC receives this VLAN)
    uint16_t rx_server_port;    // Server DPDK port (the one that TXs)
    uint16_t rx_server_queue;   // Server TX queue index (0-3)

    // VMC TX (VMC → Server): VMC sends from this VLAN
    uint16_t tx_vlan;           // Server RX VLAN (VMC sends from this VLAN)
    uint16_t tx_server_port;    // Server DPDK port (the one that RXs)
    uint16_t tx_server_queue;   // Server RX queue index (0-3)
};

// VMC Port Mapping Table (4 server ports × 4 VLANs = 16 VMC ports)
// Loopback: same port TX→RX, except Cross (J6-2↔J6-4)
// Format: {vmc_port, rx_vlan, rx_srv_port, rx_srv_queue, tx_vlan, tx_srv_port, tx_srv_queue}
#define VMC_PORT_MAP_INIT {                                                                         \
    /* VMC 0-3: Port 0 (J6-1 loopback, J6-2/J6-4 cross, J6-3 loopback) */                          \
    {.vmc_port_id = 0,  .rx_vlan = 105, .rx_server_port = 0, .rx_server_queue = 0,                  \
                         .tx_vlan = 233, .tx_server_port = 0, .tx_server_queue = 0},                 \
    {.vmc_port_id = 1,  .rx_vlan = 106, .rx_server_port = 0, .rx_server_queue = 1,                  \
                         .tx_vlan = 236, .tx_server_port = 0, .tx_server_queue = 3},  /* Cross */    \
    {.vmc_port_id = 2,  .rx_vlan = 107, .rx_server_port = 0, .rx_server_queue = 2,                  \
                         .tx_vlan = 235, .tx_server_port = 0, .tx_server_queue = 2},                 \
    {.vmc_port_id = 3,  .rx_vlan = 108, .rx_server_port = 0, .rx_server_queue = 3,                  \
                         .tx_vlan = 234, .tx_server_port = 0, .tx_server_queue = 1},  /* Cross */    \
    /* VMC 4-7: Port 1 (J7-1..J7-4, all loopback) */                                                \
    {.vmc_port_id = 4,  .rx_vlan = 109, .rx_server_port = 1, .rx_server_queue = 0,                  \
                         .tx_vlan = 237, .tx_server_port = 1, .tx_server_queue = 0},                 \
    {.vmc_port_id = 5,  .rx_vlan = 110, .rx_server_port = 1, .rx_server_queue = 1,                  \
                         .tx_vlan = 238, .tx_server_port = 1, .tx_server_queue = 1},                 \
    {.vmc_port_id = 6,  .rx_vlan = 111, .rx_server_port = 1, .rx_server_queue = 2,                  \
                         .tx_vlan = 239, .tx_server_port = 1, .tx_server_queue = 2},                 \
    {.vmc_port_id = 7,  .rx_vlan = 112, .rx_server_port = 1, .rx_server_queue = 3,                  \
                         .tx_vlan = 240, .tx_server_port = 1, .tx_server_queue = 3},                 \
    /* VMC 8-11: Port 2 (J4-1..J4-4, all loopback) */                                               \
    {.vmc_port_id = 8,  .rx_vlan = 97,  .rx_server_port = 2, .rx_server_queue = 0,                  \
                         .tx_vlan = 225, .tx_server_port = 2, .tx_server_queue = 0},                 \
    {.vmc_port_id = 9,  .rx_vlan = 98,  .rx_server_port = 2, .rx_server_queue = 1,                  \
                         .tx_vlan = 226, .tx_server_port = 2, .tx_server_queue = 1},                 \
    {.vmc_port_id = 10, .rx_vlan = 99,  .rx_server_port = 2, .rx_server_queue = 2,                  \
                         .tx_vlan = 227, .tx_server_port = 2, .tx_server_queue = 2},                 \
    {.vmc_port_id = 11, .rx_vlan = 100, .rx_server_port = 2, .rx_server_queue = 3,                  \
                         .tx_vlan = 228, .tx_server_port = 2, .tx_server_queue = 3},                 \
    /* VMC 12-15: Port 3 (J5-1..J5-4, all loopback) */                                              \
    {.vmc_port_id = 12, .rx_vlan = 101, .rx_server_port = 3, .rx_server_queue = 0,                  \
                         .tx_vlan = 229, .tx_server_port = 3, .tx_server_queue = 0},                 \
    {.vmc_port_id = 13, .rx_vlan = 102, .rx_server_port = 3, .rx_server_queue = 1,                  \
                         .tx_vlan = 230, .tx_server_port = 3, .tx_server_queue = 1},                 \
    {.vmc_port_id = 14, .rx_vlan = 103, .rx_server_port = 3, .rx_server_queue = 2,                  \
                         .tx_vlan = 231, .tx_server_port = 3, .tx_server_queue = 2},                 \
    {.vmc_port_id = 15, .rx_vlan = 104, .rx_server_port = 3, .rx_server_queue = 3,                  \
                         .tx_vlan = 232, .tx_server_port = 3, .tx_server_queue = 3},                 \
}

// VLAN → VMC port lookup (fast access)
// Index = VLAN ID, Value = VMC port number (0xFF = undefined)
#define VMC_VLAN_LOOKUP_SIZE 257  // VLAN 0-256
#define VMC_VLAN_INVALID 0xFF

// ==========================================
// VMC PORT DISPLAY GROUPING (VSCPU / FCPU / CROSS)
// ==========================================
// Groups existing VMC ports (0-15) into 3 display tables.
// VMC port map stays unchanged - only the display is reorganized.
//
// VMC index mapping (from VMC_PORT_MAP_INIT):
//   VMC 0-3  = Port 0 Q0-Q3 (J6-1, J6-2, J6-3, J6-4)
//   VMC 4-7  = Port 1 Q0-Q3 (J7-1, J7-2, J7-3, J7-4)
//   VMC 8-11 = Port 2 Q0-Q3 (J4-1, J4-2, J4-3, J4-4)
//   VMC 12-15= Port 3 Q0-Q3 (J5-1, J5-2, J5-3, J5-4)
//
// VSCPU: Port 2 (J4-1,J4-2), Port 3 (J5-1..J5-4), Port 0 (J6-3)
// FCPU:  Port 2 (J4-3,J4-4), Port 1 (J7-1..J7-4), Port 0 (J6-1)
// Cross: Port 0 (J6-2 TX→J6-4 RX, J6-4 TX→J6-2 RX)

#define VSCPU_COUNT  7
#define FCPU_COUNT   7
#define CROSS_COUNT  2

static const uint16_t vscpu_vmc_indices[VSCPU_COUNT] = {8, 9, 12, 13, 14, 15, 2};
static const uint16_t fcpu_vmc_indices[FCPU_COUNT]    = {10, 11, 4, 5, 6, 7, 0};
static const uint16_t cross_vmc_indices[CROSS_COUNT]  = {1, 3};

// J-label for each VMC port (indexed by VMC port number 0-15)
static const char * const vmc_port_labels[VMC_PORT_COUNT] = {
    "J6-1", "J6-2", "J6-3", "J6-4",   /* VMC 0-3:  Port 0 Q0-Q3 */
    "J7-1", "J7-2", "J7-3", "J7-4",   /* VMC 4-7:  Port 1 Q0-Q3 */
    "J4-1", "J4-2", "J4-3", "J4-4",   /* VMC 8-11: Port 2 Q0-Q3 */
    "J5-1", "J5-2", "J5-3", "J5-4"    /* VMC 12-15: Port 3 Q0-Q3 */
};

#endif /* CONFIG_H */
