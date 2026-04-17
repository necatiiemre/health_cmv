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
// RAW SOCKET PORT CONFIGURATION (Non-DPDK)
// ==========================================
// These ports use raw socket + zero copy for NICs that don't support DPDK.
// Multi-target: A single port can send to multiple destinations at different rates.
//
// Port 12 (1G copper): 5 targets (total 960 Mbps)
//   - Target 0: To Port 13  80 Mbps, VL-ID 4099-4226 (128)
//   - Target 1: To Port 5  220 Mbps, VL-ID 4227-4738 (512)
//   - Target 2: To Port 4  220 Mbps, VL-ID 4739-5250 (512)
//   - Target 3: To Port 7  220 Mbps, VL-ID 5251-5762 (512)
//   - Target 4: To Port 6  220 Mbps, VL-ID 5763-6274 (512)
//
// Port 13 (100M copper): 1 target (80 Mbps)
//   - Target 0: To Port 12 80 Mbps, VL-ID 6275-6306 (32)

#define MAX_RAW_SOCKET_PORTS 4
#define RAW_SOCKET_PORT_ID_START 12
#define MAX_RAW_TARGETS 8 // Maximum number of targets per port

// Port 12 configuration (1G copper)
#define RAW_SOCKET_PORT_12_PCI "01:00.0"
#define RAW_SOCKET_PORT_12_IFACE "eno12399"
#define RAW_SOCKET_PORT_12_IS_1G true

// Port 13 configuration (100M copper)
#define RAW_SOCKET_PORT_13_PCI "01:00.1"
#define RAW_SOCKET_PORT_13_IFACE "eno12409"
#define RAW_SOCKET_PORT_13_IS_1G false

// Port 14 configuration (1G copper - ATE mode only)
#define RAW_SOCKET_PORT_14_PCI "01:00.2"
#define RAW_SOCKET_PORT_14_IFACE "eno12419"
#define RAW_SOCKET_PORT_14_IS_1G true

// Port 15 configuration (100M copper - ATE mode only)
#define RAW_SOCKET_PORT_15_PCI "01:00.3"
#define RAW_SOCKET_PORT_15_IFACE "eno12429"
#define RAW_SOCKET_PORT_15_IS_1G false

// ==========================================
// MULTI-TARGET CONFIGURATION
// ==========================================

// TX Target: Destination a port sends to
struct raw_tx_target_config
{
  uint16_t target_id;   // Target ID (0, 1, 2, ...)
  uint16_t dest_port;   // Destination port number (13, 5, 4, 7, 6, 12)
  uint32_t rate_mbps;   // Rate for this target (Mbps)
  uint16_t vl_id_start; // VL-ID start
  uint16_t vl_id_count; // VL-ID count
};

// RX Source: Source accepted by a port (for validation)
struct raw_rx_source_config
{
  uint16_t source_port; // Source port number
  uint16_t vl_id_start; // Expected VL-ID start
  uint16_t vl_id_count; // Expected VL-ID count
};

#if TOKEN_BUCKET_TX_ENABLED
// ==========================================
// TOKEN BUCKET: Port 12 TX (non-contiguous VL-IDs, without VLAN)
// ==========================================
// 4 target × 16 VL = 64 VL total → 64000 pkt/s
// VL-IDs are non-contiguous (blocks of 4, step of 8)
// raw_tx_worker will use VL-ID lookup tables
#define PORT_12_TX_TARGET_COUNT 4
#define PORT_12_TX_TARGETS_INIT {                                                                \
    {.target_id = 0, .dest_port = 5, .rate_mbps = 195, .vl_id_start = 4163, .vl_id_count = 16}, \
    {.target_id = 1, .dest_port = 4, .rate_mbps = 195, .vl_id_start = 4195, .vl_id_count = 16}, \
    {.target_id = 2, .dest_port = 3, .rate_mbps = 195, .vl_id_start = 4227, .vl_id_count = 16}, \
    {.target_id = 3, .dest_port = 2, .rate_mbps = 195, .vl_id_start = 4259, .vl_id_count = 16}, \
}

// Token bucket Port 12 VL-ID lookup tables (non-contiguous ranges)
// Each target has 16 VL-IDs in 4 blocks of 4 (step=8)
#define TB_PORT_12_VL_BLOCK_SIZE  4
#define TB_PORT_12_VL_BLOCK_STEP  8
// VL-ID calculation: vl_id_start + (offset / block_size) * block_step + (offset % block_size)

#else
// Port 12 TX Targets (4 targets, total 880 Mbps)
// Transmission to Port 13 removed, only sending to DPDK ports (2,3,4,5)
// 4 × 220 Mbps = 880 Mbps total (1G link, ~89% utilization — safe margin)
#define PORT_12_TX_TARGET_COUNT 4
#define PORT_12_TX_TARGETS_INIT {                                                               \
    {.target_id = 0, .dest_port = 2, .rate_mbps = 230, .vl_id_start = 4259, .vl_id_count = 32}, \
    {.target_id = 1, .dest_port = 3, .rate_mbps = 230, .vl_id_start = 4227, .vl_id_count = 32}, \
    {.target_id = 2, .dest_port = 4, .rate_mbps = 230, .vl_id_start = 4195, .vl_id_count = 32}, \
    {.target_id = 3, .dest_port = 5, .rate_mbps = 230, .vl_id_start = 4163, .vl_id_count = 32}, \
}
#endif

// Port 12 RX Sources (packets from Port 13 removed)
// Now only receiving DPDK External TX (Port 2,3,4,5) packets
#define PORT_12_RX_SOURCE_COUNT 0
#define PORT_12_RX_SOURCES_INIT \
  {                             \
  }

#if TOKEN_BUCKET_TX_ENABLED
// ==========================================
// TOKEN BUCKET: Port 13 TX (non-contiguous VL-IDs, without VLAN)
// ==========================================
// 2 target × 3 VL = 6 VL total → 6000 pkt/s
// VL-IDs: individual VL-IDXs with step=4 intervals
#define PORT_13_TX_TARGET_COUNT 2
#define PORT_13_TX_TARGETS_INIT {                                                              \
    {.target_id = 0, .dest_port = 7, .rate_mbps = 37, .vl_id_start = 4131, .vl_id_count = 3}, \
    {.target_id = 1, .dest_port = 1, .rate_mbps = 37, .vl_id_start = 4147, .vl_id_count = 3}, \
}

// Token bucket Port 13 VL-ID lookup: step=4, block_size=1
#define TB_PORT_13_VL_BLOCK_SIZE  1
#define TB_PORT_13_VL_BLOCK_STEP  4
// VL-ID calculation: vl_id_start + offset * block_step

#else
// Port 13 TX Targets (2 targets, total ~90 Mbps)
// Transmission to Port 12 removed, sending to DPDK ports (7, 1) added
#define PORT_13_TX_TARGET_COUNT 2
#define PORT_13_TX_TARGETS_INIT {                                                              \
    {.target_id = 0, .dest_port = 7, .rate_mbps = 45, .vl_id_start = 4131, .vl_id_count = 16}, \
    {.target_id = 1, .dest_port = 1, .rate_mbps = 45, .vl_id_start = 4147, .vl_id_count = 16}, \
}
#endif

// Port 13 RX Sources (packets from Port 12 removed)
// Port 13 now only transmits (to Port 7 and Port 1)
#define PORT_13_RX_SOURCE_COUNT 0
#define PORT_13_RX_SOURCES_INIT \
  {                             \
  }

// ==========================================
// ATE MODE TX/RX CONFIGURATION
// ==========================================
// In ATE mode Port 12↔14, Port 13↔15 establish full-duplex communication.
// Each port sends to the other side with a single target, using the same VL-ID ranges.

// Port 12 ATE TX: 1 target → Port 14 (960 Mbps, VL-ID 4163-4290)
#define ATE_PORT_12_TX_TARGET_COUNT 1
#define ATE_PORT_12_TX_TARGETS_INIT {                                                                \
    {.target_id = 0, .dest_port = 14, .rate_mbps = 960, .vl_id_start = 4163, .vl_id_count = 128},   \
}

// Port 12 ATE RX: Packets from Port 14
#define ATE_PORT_12_RX_SOURCE_COUNT 1
#define ATE_PORT_12_RX_SOURCES_INIT {                                   \
    {.source_port = 14, .vl_id_start = 4163, .vl_id_count = 128},      \
}

// Port 14 ATE TX: 1 target → Port 12 (960 Mbps, VL-ID 4163-4290)
#define ATE_PORT_14_TX_TARGET_COUNT 1
#define ATE_PORT_14_TX_TARGETS_INIT {                                                                \
    {.target_id = 0, .dest_port = 12, .rate_mbps = 960, .vl_id_start = 4163, .vl_id_count = 128},   \
}

// Port 14 ATE RX: Packets from Port 12
#define ATE_PORT_14_RX_SOURCE_COUNT 1
#define ATE_PORT_14_RX_SOURCES_INIT {                                   \
    {.source_port = 12, .vl_id_start = 4163, .vl_id_count = 128},      \
}

// Port 13 ATE TX: 1 target → Port 15 (92 Mbps, VL-ID 4131-4162)
#define ATE_PORT_13_TX_TARGET_COUNT 1
#define ATE_PORT_13_TX_TARGETS_INIT {                                                               \
    {.target_id = 0, .dest_port = 15, .rate_mbps = 92, .vl_id_start = 4131, .vl_id_count = 32},    \
}

// Port 13 ATE RX: Packets from Port 15
#define ATE_PORT_13_RX_SOURCE_COUNT 1
#define ATE_PORT_13_RX_SOURCES_INIT {                                   \
    {.source_port = 15, .vl_id_start = 4131, .vl_id_count = 32},       \
}

// Port 15 ATE TX: 1 target → Port 13 (92 Mbps, VL-ID 4131-4162)
#define ATE_PORT_15_TX_TARGET_COUNT 1
#define ATE_PORT_15_TX_TARGETS_INIT {                                                               \
    {.target_id = 0, .dest_port = 13, .rate_mbps = 92, .vl_id_start = 4131, .vl_id_count = 32},    \
}

// Port 15 ATE RX: Packets from Port 13
#define ATE_PORT_15_RX_SOURCE_COUNT 1
#define ATE_PORT_15_RX_SOURCES_INIT {                                   \
    {.source_port = 13, .vl_id_start = 4131, .vl_id_count = 32},       \
}

// Raw socket port configuration structure
struct raw_socket_port_config
{
  uint16_t port_id;           // Global port ID (12 or 13)
  const char *pci_addr;       // PCI address (for identification)
  const char *interface_name; // Kernel interface name
  bool is_1g_port;            // true for 1G, false for 100M

  // TX targets
  uint16_t tx_target_count;
  struct raw_tx_target_config tx_targets[MAX_RAW_TARGETS];

  // RX sources (for validation)
  uint16_t rx_source_count;
  struct raw_rx_source_config rx_sources[MAX_RAW_TARGETS];
};

// Helper macro for tx_targets initialization
#define INIT_TX_TARGETS_12 PORT_12_TX_TARGETS_INIT
#define INIT_TX_TARGETS_13 PORT_13_TX_TARGETS_INIT
#define INIT_RX_SOURCES_12 PORT_12_RX_SOURCES_INIT
#define INIT_RX_SOURCES_13 PORT_13_RX_SOURCES_INIT

// Raw socket port configurations
#define RAW_SOCKET_PORTS_CONFIG_INIT                   \
  {                                                    \
    /* Port 12: 1G copper, 5 TX targets, 1 RX source */   \
    {.port_id = 12,                                    \
     .pci_addr = RAW_SOCKET_PORT_12_PCI,               \
     .interface_name = RAW_SOCKET_PORT_12_IFACE,       \
     .is_1g_port = RAW_SOCKET_PORT_12_IS_1G,           \
     .tx_target_count = PORT_12_TX_TARGET_COUNT,       \
     .tx_targets = INIT_TX_TARGETS_12,                 \
     .rx_source_count = PORT_12_RX_SOURCE_COUNT,       \
     .rx_sources = INIT_RX_SOURCES_12},                \
    /* Port 13: 100M copper, 1 TX target, 1 RX source */ \
    {                                                  \
      .port_id = 13,                                   \
      .pci_addr = RAW_SOCKET_PORT_13_PCI,              \
      .interface_name = RAW_SOCKET_PORT_13_IFACE,      \
      .is_1g_port = RAW_SOCKET_PORT_13_IS_1G,          \
      .tx_target_count = PORT_13_TX_TARGET_COUNT,      \
      .tx_targets = INIT_TX_TARGETS_13,                \
      .rx_source_count = PORT_13_RX_SOURCE_COUNT,      \
      .rx_sources = INIT_RX_SOURCES_13                 \
    },                                                 \
    /* Port 14/15: Placeholder (unused in normal mode) */ \
    {0}, {0}                                           \
  }

// Active port count in normal mode (Port 12, 13 only)
#define NORMAL_RAW_SOCKET_PORT_COUNT 2

// ATE mode raw socket port configurations (4 port: 12↔14, 13↔15 full-duplex)
#define ATE_RAW_SOCKET_PORTS_CONFIG_INIT                               \
  {                                                                     \
    /* Port 12: 1G → Port 14 (960 Mbps) */                             \
    {.port_id = 12,                                                     \
     .pci_addr = RAW_SOCKET_PORT_12_PCI,                                \
     .interface_name = RAW_SOCKET_PORT_12_IFACE,                        \
     .is_1g_port = RAW_SOCKET_PORT_12_IS_1G,                            \
     .tx_target_count = ATE_PORT_12_TX_TARGET_COUNT,                    \
     .tx_targets = ATE_PORT_12_TX_TARGETS_INIT,                         \
     .rx_source_count = ATE_PORT_12_RX_SOURCE_COUNT,                    \
     .rx_sources = ATE_PORT_12_RX_SOURCES_INIT},                        \
    /* Port 13: 100M → Port 15 (92 Mbps) */                            \
    {.port_id = 13,                                                     \
     .pci_addr = RAW_SOCKET_PORT_13_PCI,                                \
     .interface_name = RAW_SOCKET_PORT_13_IFACE,                        \
     .is_1g_port = RAW_SOCKET_PORT_13_IS_1G,                            \
     .tx_target_count = ATE_PORT_13_TX_TARGET_COUNT,                    \
     .tx_targets = ATE_PORT_13_TX_TARGETS_INIT,                         \
     .rx_source_count = ATE_PORT_13_RX_SOURCE_COUNT,                    \
     .rx_sources = ATE_PORT_13_RX_SOURCES_INIT},                        \
    /* Port 14: 1G → Port 12 (960 Mbps) */                             \
    {.port_id = 14,                                                     \
     .pci_addr = RAW_SOCKET_PORT_14_PCI,                                \
     .interface_name = RAW_SOCKET_PORT_14_IFACE,                        \
     .is_1g_port = RAW_SOCKET_PORT_14_IS_1G,                            \
     .tx_target_count = ATE_PORT_14_TX_TARGET_COUNT,                    \
     .tx_targets = ATE_PORT_14_TX_TARGETS_INIT,                         \
     .rx_source_count = ATE_PORT_14_RX_SOURCE_COUNT,                    \
     .rx_sources = ATE_PORT_14_RX_SOURCES_INIT},                        \
    /* Port 15: 100M → Port 13 (92 Mbps) */                            \
    {.port_id = 15,                                                     \
     .pci_addr = RAW_SOCKET_PORT_15_PCI,                                \
     .interface_name = RAW_SOCKET_PORT_15_IFACE,                        \
     .is_1g_port = RAW_SOCKET_PORT_15_IS_1G,                            \
     .tx_target_count = ATE_PORT_15_TX_TARGET_COUNT,                    \
     .tx_targets = ATE_PORT_15_TX_TARGETS_INIT,                         \
     .rx_source_count = ATE_PORT_15_RX_SOURCE_COUNT,                    \
     .rx_sources = ATE_PORT_15_RX_SOURCES_INIT}                         \
  }

// Active port count in ATE mode (Port 12, 13, 14, 15)
#define ATE_RAW_SOCKET_PORT_COUNT 4

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
// ==========================================
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
  uint16_t tx_vl_ids[MAX_TX_VLANS_PER_PORT]; // {3,131,259,387}
  uint16_t rx_vl_ids[MAX_RX_VLANS_PER_PORT]; // {3,131,259,387}
};

// Per-port VLAN/VL-ID template (queue index ↔ VL range start matches)
#define PORT_VLAN_CONFIG_INIT                                                                                                                                                                               \
  {                                                                                                                                                                                                         \
    /* Port 0 */                                                                                                                                                                                            \
    {.tx_vlans = {105, 106, 107, 108}, .tx_vlan_count = 4, .rx_vlans = {253, 254, 255, 256}, .rx_vlan_count = 4, .tx_vl_ids = {1027, 1155, 1283, 1411}, .rx_vl_ids = {3, 131, 259, 387}},     /* Port 1 */  \
        {.tx_vlans = {109, 110, 111, 112}, .tx_vlan_count = 4, .rx_vlans = {249, 250, 251, 252}, .rx_vlan_count = 4, .tx_vl_ids = {1539, 1667, 1795, 1923}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 2 */  \
        {.tx_vlans = {97, 98, 99, 100}, .tx_vlan_count = 4, .rx_vlans = {245, 246, 247, 248}, .rx_vlan_count = 4, .tx_vl_ids = {3, 131, 259, 387}, .rx_vl_ids = {3, 131, 259, 387}},          /* Port 3 */  \
        {.tx_vlans = {101, 102, 103, 104}, .tx_vlan_count = 4, .rx_vlans = {241, 242, 243, 244}, .rx_vlan_count = 4, .tx_vl_ids = {515, 643, 771, 899}, .rx_vl_ids = {3, 131, 259, 387}},     /* Port 4 */  \
        {.tx_vlans = {113, 114, 115, 116}, .tx_vlan_count = 4, .rx_vlans = {229, 230, 231, 232}, .rx_vlan_count = 4, .tx_vl_ids = {2051, 2179, 2307, 2435}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 5 */  \
        {.tx_vlans = {117, 118, 119, 120}, .tx_vlan_count = 4, .rx_vlans = {225, 226, 227, 228}, .rx_vlan_count = 4, .tx_vl_ids = {2563, 2691, 2819, 2947}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 6 */  \
        {.tx_vlans = {121, 122, 123, 124}, .tx_vlan_count = 4, .rx_vlans = {237, 238, 239, 240}, .rx_vlan_count = 4, .tx_vl_ids = {3075, 3203, 3331, 3459}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 7 */  \
        {.tx_vlans = {125, 126, 127, 128}, .tx_vlan_count = 4, .rx_vlans = {233, 234, 235, 236}, .rx_vlan_count = 4, .tx_vl_ids = {3587, 3715, 3843, 3971}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 8 */  \
        {.tx_vlans = {129, 130, 131, 132}, .tx_vlan_count = 4, .rx_vlans = {133, 134, 135, 136}, .rx_vlan_count = 4, .tx_vl_ids = {3, 131, 259, 387}, .rx_vl_ids = {3, 131, 259, 387}},       /* Port 9 */  \
        {.tx_vlans = {129, 130, 131, 132}, .tx_vlan_count = 4, .rx_vlans = {133, 134, 135, 136}, .rx_vlan_count = 4, .tx_vl_ids = {3, 131, 259, 387}, .rx_vl_ids = {3, 131, 259, 387}},       /* Port 10 */ \
        {.tx_vlans = {137, 138, 139, 140}, .tx_vlan_count = 4, .rx_vlans = {141, 142, 143, 144}, .rx_vlan_count = 4, .tx_vl_ids = {3, 131, 259, 387}, .rx_vl_ids = {3, 131, 259, 387}},                     \
    /* Port 11 */                                                                                                                                                                                           \
    {                                                                                                                                                                                                       \
      .tx_vlans = {137, 138, 139, 140}, .tx_vlan_count = 4,                                                                                                                                                 \
      .rx_vlans = {141, 142, 143, 144}, .rx_vlan_count = 4,                                                                                                                                                 \
      .tx_vl_ids = {3, 131, 259, 387},                                                                                                                                                                      \
      .rx_vl_ids = {3, 131, 259, 387}                                                                                                                                                                       \
    }                                                                                                                                                                                                       \
  }

// ==========================================
// ATE TEST MODE - PORT VLAN CONFIGURATION
// ==========================================
// DPDK port VLAN/VL-ID mapping table for ATE test mode.
// Same structure as PORT_VLAN_CONFIG_INIT in normal mode.
// NOTE: These values are placeholders, to be changed according to ATE topology!
// Selected at runtime based on g_ate_mode flag.

#define ATE_PORT_VLAN_CONFIG_INIT                                                                                                                                                                           \
  {                                                                                                                                                                                                         \
    /* Port 0 */                                                                                                                                                                                            \
    {.tx_vlans = {105, 106, 107, 108}, .tx_vlan_count = 4, .rx_vlans = {237, 238, 239, 240}, .rx_vlan_count = 4, .tx_vl_ids = {1027, 1155, 1283, 1411}, .rx_vl_ids = {3, 131, 259, 387}},     /* Port 1 */  \
        {.tx_vlans = {109, 110, 111, 112}, .tx_vlan_count = 4, .rx_vlans = {233, 234, 235, 236}, .rx_vlan_count = 4, .tx_vl_ids = {1539, 1667, 1795, 1923}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 2 */  \
        {.tx_vlans = {97, 98, 99, 100}, .tx_vlan_count = 4, .rx_vlans = {229, 230, 231, 232}, .rx_vlan_count = 4, .tx_vl_ids = {3, 131, 259, 387}, .rx_vl_ids = {3, 131, 259, 387}},          /* Port 3 */  \
        {.tx_vlans = {101, 102, 103, 104}, .tx_vlan_count = 4, .rx_vlans = {225, 226, 227, 228}, .rx_vlan_count = 4, .tx_vl_ids = {515, 643, 771, 899}, .rx_vl_ids = {3, 131, 259, 387}},     /* Port 4 */  \
        {.tx_vlans = {113, 114, 115, 116}, .tx_vlan_count = 4, .rx_vlans = {245, 246, 247, 248}, .rx_vlan_count = 4, .tx_vl_ids = {2051, 2179, 2307, 2435}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 5 */  \
        {.tx_vlans = {117, 118, 119, 120}, .tx_vlan_count = 4, .rx_vlans = {241, 242, 243, 244}, .rx_vlan_count = 4, .tx_vl_ids = {2563, 2691, 2819, 2947}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 6 */  \
        {.tx_vlans = {121, 122, 123, 124}, .tx_vlan_count = 4, .rx_vlans = {253, 254, 255, 256}, .rx_vlan_count = 4, .tx_vl_ids = {3075, 3203, 3331, 3459}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 7 */  \
        {.tx_vlans = {125, 126, 127, 128}, .tx_vlan_count = 4, .rx_vlans = {249, 250, 251, 252}, .rx_vlan_count = 4, .tx_vl_ids = {3587, 3715, 3843, 3971}, .rx_vl_ids = {3, 131, 259, 387}}, /* Port 8 */  \
        {.tx_vlans = {129, 130, 131, 132}, .tx_vlan_count = 4, .rx_vlans = {133, 134, 135, 136}, .rx_vlan_count = 4, .tx_vl_ids = {3, 131, 259, 387}, .rx_vl_ids = {3, 131, 259, 387}},       /* Port 9 */  \
        {.tx_vlans = {129, 130, 131, 132}, .tx_vlan_count = 4, .rx_vlans = {133, 134, 135, 136}, .rx_vlan_count = 4, .tx_vl_ids = {3, 131, 259, 387}, .rx_vl_ids = {3, 131, 259, 387}},       /* Port 10 */ \
        {.tx_vlans = {137, 138, 139, 140}, .tx_vlan_count = 4, .rx_vlans = {141, 142, 143, 144}, .rx_vlan_count = 4, .tx_vl_ids = {3, 131, 259, 387}, .rx_vl_ids = {3, 131, 259, 387}},                     \
    /* Port 11 */                                                                                                                                                                                           \
    {                                                                                                                                                                                                       \
      .tx_vlans = {137, 138, 139, 140}, .tx_vlan_count = 4,                                                                                                                                                 \
      .rx_vlans = {141, 142, 143, 144}, .rx_vlan_count = 4,                                                                                                                                                 \
      .tx_vl_ids = {3, 131, 259, 387},                                                                                                                                                                      \
      .rx_vl_ids = {3, 131, 259, 387}                                                                                                                                                                       \
    }                                                                                                                                                                                                       \
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
// Port 0, 1, 6, 7, 8: Fast (not connected to Port 12)
// Port 2, 3, 4, 5: Slow (connected to Port 12, doing external TX)

#ifndef TARGET_GBPS_FAST
#define TARGET_GBPS_FAST 3.6
#endif

#ifndef TARGET_GBPS_MID
#define TARGET_GBPS_MID 3.4
#endif

#ifndef TARGET_GBPS_SLOW
#define TARGET_GBPS_SLOW 3.4
#endif

// DPDK-DPDK ports (fast)
#define IS_FAST_PORT(port_id) ((port_id) == 1 || (port_id) == 7 || (port_id) == 8)

// DPDK ports connected to Port 12 (medium speed)
#define IS_MID_PORT(port_id) ((port_id) == 2 || (port_id) == 3 || \
                              (port_id) == 4 || (port_id) == 5)

// DPDK ports connected to Port 13 (slow)
#define IS_SLOW_PORT(port_id) ((port_id) == 0 || (port_id) == 6)

// Per-port target rate (Gbps)
// FAST: DPDK-DPDK ports (1,7,8)
// MID: Ports connected to Port 12 (2,3,4,5)
// SLOW: Ports connected to Port 13 (0,6)
#define GET_PORT_TARGET_GBPS(port_id)                                                \
  (IS_FAST_PORT(port_id) ? TARGET_GBPS_FAST : IS_MID_PORT(port_id) ? TARGET_GBPS_MID \
                                                                   : TARGET_GBPS_SLOW)

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

// ==========================================
// DPDK EXTERNAL TX CONFIGURATION
// ==========================================
// This system operates INDEPENDENTLY from the existing DPDK TX.
// Uses DPDK Port 0,1,2,3 → Switch → Port 12 (raw socket) path.
// Each port sends 4 different VLAN/VL-ID combinations via 4 queues.
//
// Flow:
//   DPDK Port TX → Physical wire → Switch → Port 12 NIC → Raw socket RX
//
// Port 12 (raw socket) receives these packets and performs PRBS and sequence validation.

#define DPDK_EXT_TX_ENABLED 1
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

#if TOKEN_BUCKET_TX_ENABLED
// Token bucket: RX sources for DPDK external packets
// Port 12 receives from Port 2,3,4,5 (16 VL each → non-contiguous)
#define PORT_12_DPDK_EXT_RX_SOURCE_COUNT 4
#define PORT_12_DPDK_EXT_RX_SOURCES_INIT {                      \
    {.source_port = 2, .vl_id_start = 4291, .vl_id_count = 16}, \
    {.source_port = 3, .vl_id_start = 4323, .vl_id_count = 16}, \
    {.source_port = 4, .vl_id_start = 4355, .vl_id_count = 16}, \
    {.source_port = 5, .vl_id_start = 4387, .vl_id_count = 16}, \
}

// Port 13 receives from Port 0,6 (3 VL each → non-contiguous)
#define PORT_13_DPDK_EXT_RX_SOURCE_COUNT 2
#define PORT_13_DPDK_EXT_RX_SOURCES_INIT {                      \
    {.source_port = 0, .vl_id_start = 4099, .vl_id_count = 3},  \
    {.source_port = 6, .vl_id_start = 4115, .vl_id_count = 3},  \
}

#else
// Port 12 RX sources for DPDK external packets (from Port 2,3,4,5)
// VL-ID ranges must match what each port's DPDK_EXT_TX actually sends
#define PORT_12_DPDK_EXT_RX_SOURCE_COUNT 4
#define PORT_12_DPDK_EXT_RX_SOURCES_INIT {                      \
    {.source_port = 2, .vl_id_start = 4291, .vl_id_count = 32}, \
    {.source_port = 3, .vl_id_start = 4323, .vl_id_count = 32}, \
    {.source_port = 4, .vl_id_start = 4355, .vl_id_count = 32}, \
    {.source_port = 5, .vl_id_start = 4387, .vl_id_count = 32}, \
}

// Port 13 RX sources for DPDK external packets (from Port 0,6)
// VL-ID 4099-4130 range (Port 0: 4099-4114, Port 6: 4115-4130)
#define PORT_13_DPDK_EXT_RX_SOURCE_COUNT 2
#define PORT_13_DPDK_EXT_RX_SOURCES_INIT {                      \
    {.source_port = 0, .vl_id_start = 4099, .vl_id_count = 16}, \
    {.source_port = 6, .vl_id_start = 4115, .vl_id_count = 16}, \
}
#endif

// ==========================================
// PTP (IEEE 1588v2) CONFIGURATION
// ==========================================
// PTP Slave implementation for synchronizing with DTN switch (master)
//
// Topology: PC → Server (DPDK/Slave) → Mellanox Switch → DTN Switch (Master)
//
// Each server port connects to 4 DTN ports via VLANs:
//   - 8 ports × 4 VLANs = 32 PTP sessions total
//
// Mode: One-step (no Follow_Up messages)
// Transport: Layer 2 (EtherType 0x88F7)
// Timestamps: Software (rte_rdtsc) for t2 and t3
//             Hardware timestamps from DTN for t1 and t4

#ifndef PTP_ENABLED
#define PTP_ENABLED 1
#endif

#ifndef ATE_PTP_ENABLED
#define ATE_PTP_ENABLED 0
#endif

// PTP Queue configuration
// Using Queue 5 for both TX and RX (Queue 4 is used by External TX)
#define PTP_TX_QUEUE 5
#define PTP_RX_QUEUE 5

// PTP core configuration
// 1 PTP core per port (8 total) for accurate software timestamps
#ifndef NUM_PTP_CORES_PER_PORT
#define NUM_PTP_CORES_PER_PORT 1
#endif

// PTP VL-ID base (must not overlap with existing VL-IDs)
// Existing VL-IDs go up to ~4418, PTP starts at 4500
#define PTP_VL_ID_START 4500

// PTP timeouts (in seconds)
#define PTP_SYNC_TIMEOUT_SEC 3       // Max time to wait for Sync
#define PTP_DELAY_RESP_TIMEOUT_SEC 2 // Max time to wait for Delay_Resp

// PTP Delay_Req interval (in ms)
// After receiving Sync, wait this long before sending Delay_Req
#define PTP_DELAY_REQ_INTERVAL_MS 100

// PTP mbuf pool configuration
#define PTP_MBUF_POOL_SIZE 1024
#define PTP_MBUF_CACHE_SIZE 32

// PTP packet size (Layer 2: Ethernet + VLAN + PTP)
// Sync: 14 (ETH) + 4 (VLAN) + 44 (PTP) = 62 bytes
// Delay_Req: 14 (ETH) + 4 (VLAN) + 44 (PTP) = 62 bytes
// Delay_Resp: 14 (ETH) + 4 (VLAN) + 54 (PTP) = 72 bytes
#define PTP_MAX_PACKET_SIZE 128

// PTP statistics update interval (in seconds)
#define PTP_STATS_INTERVAL_SEC 1

// PTP raw packet debug printing (0=disabled, 1=enabled)
// Note: PTP Calc results are ALWAYS printed regardless of this setting
#ifndef PTP_RAW_DEBUG_PRINT
#define PTP_RAW_DEBUG_PRINT 0
#endif

// Number of PTP ports (DPDK ports 0-7)
#define PTP_PORT_COUNT 8

// Number of PTP sessions per port (one per VLAN)
#define PTP_SESSIONS_PER_PORT_COUNT 4

// ==========================================
// PTP SESSION CONFIGURATION (Static Table)
// ==========================================
// Split TX/RX Port Architecture:
//   - RX port: Receives Sync and Delay_Resp packets (session lives on this port)
//   - TX port: Sends Delay_Req packet (can be a different port)
//
// Example (DTN Port 0):
//   - TX: Server Port 2, VLAN 97 → Mellanox → DTN Port 0
//   - RX: DTN Port 0 → Mellanox → Server Port 5, VLAN 225
//
// For each session:
//   - rx_port_id: Port receiving Sync/Delay_Resp (session owner)
//   - rx_vlan: RX VLAN ID
//   - tx_port_id: Port sending Delay_Req
//   - tx_vlan: TX VLAN ID
//   - tx_vl_idx: VL-IDX to write into Delay_Req packet
struct ptp_session_config
{
  uint16_t rx_port_id; // RX port ID (session lives here)
  uint16_t rx_vlan;    // RX VLAN ID (Sync/Delay_Resp)
  uint16_t tx_port_id; // TX port ID (for Delay_Req)
  uint16_t tx_vlan;    // TX VLAN ID (Delay_Req)
  uint16_t tx_vl_idx;  // TX VL-IDX (for Delay_Req) - request
  uint16_t rx_vl_idx;  // RX VL-IDX (for Sync/Delay_Resp) - response (tx_vl_idx + 1)
};

// PTP Port Configuration Table
// Each entry defines a PTP session
// Session lives on rx_port_id and sends via tx_port_id
#define PTP_SESSION_COUNT 34 // 32 DPDK + 2 raw socket (Port 12, 13)

#define PTP_SESSIONS_CONFIG_INIT {                                                                                                                                          \
    /* DTN Port 0-3: RX=Port5, TX=Port2 */                                                                                                                                    \
    {.rx_port_id = 5, .rx_vlan = 225, .tx_port_id = 2, .tx_vlan = 97, .tx_vl_idx = 4420, .rx_vl_idx = 4421},  /* DTN Port 0  */                                              \
    {.rx_port_id = 5, .rx_vlan = 226, .tx_port_id = 2, .tx_vlan = 98, .tx_vl_idx = 4422, .rx_vl_idx = 4423},  /* DTN Port 1  */                                              \
    {.rx_port_id = 5, .rx_vlan = 227, .tx_port_id = 2, .tx_vlan = 99, .tx_vl_idx = 4424, .rx_vl_idx = 4425},  /* DTN Port 2  */                                              \
    {.rx_port_id = 5, .rx_vlan = 228, .tx_port_id = 2, .tx_vlan = 100, .tx_vl_idx = 4426, .rx_vl_idx = 4427}, /* DTN Port 3  */                                              \
    /* DTN Port 4-7: RX=Port4, TX=Port3 */                                                                                                                                    \
    {.rx_port_id = 4, .rx_vlan = 229, .tx_port_id = 3, .tx_vlan = 101, .tx_vl_idx = 4428, .rx_vl_idx = 4429}, /* DTN Port 4  */                                              \
    {.rx_port_id = 4, .rx_vlan = 230, .tx_port_id = 3, .tx_vlan = 102, .tx_vl_idx = 4430, .rx_vl_idx = 4431}, /* DTN Port 5  */                                              \
    {.rx_port_id = 4, .rx_vlan = 231, .tx_port_id = 3, .tx_vlan = 103, .tx_vl_idx = 4432, .rx_vl_idx = 4433}, /* DTN Port 6  */                                              \
    {.rx_port_id = 4, .rx_vlan = 232, .tx_port_id = 3, .tx_vlan = 104, .tx_vl_idx = 4434, .rx_vl_idx = 4435}, /* DTN Port 7  */                                              \
    /* DTN Port 8-11: RX=Port7, TX=Port0 */                                                                                                                                   \
    {.rx_port_id = 7, .rx_vlan = 233, .tx_port_id = 0, .tx_vlan = 105, .tx_vl_idx = 4436, .rx_vl_idx = 4437}, /* DTN Port 8  */                                              \
    {.rx_port_id = 7, .rx_vlan = 234, .tx_port_id = 0, .tx_vlan = 106, .tx_vl_idx = 4438, .rx_vl_idx = 4439}, /* DTN Port 9  */                                              \
    {.rx_port_id = 7, .rx_vlan = 235, .tx_port_id = 0, .tx_vlan = 107, .tx_vl_idx = 4440, .rx_vl_idx = 4441}, /* DTN Port 10 */                                              \
    {.rx_port_id = 7, .rx_vlan = 236, .tx_port_id = 0, .tx_vlan = 108, .tx_vl_idx = 4442, .rx_vl_idx = 4443}, /* DTN Port 11 */                                              \
    /* DTN Port 12-15: RX=Port6, TX=Port1 */                                                                                                                                  \
    {.rx_port_id = 6, .rx_vlan = 237, .tx_port_id = 1, .tx_vlan = 109, .tx_vl_idx = 4444, .rx_vl_idx = 4445}, /* DTN Port 12 */                                              \
    {.rx_port_id = 6, .rx_vlan = 238, .tx_port_id = 1, .tx_vlan = 110, .tx_vl_idx = 4446, .rx_vl_idx = 4447}, /* DTN Port 13 */                                              \
    {.rx_port_id = 6, .rx_vlan = 239, .tx_port_id = 1, .tx_vlan = 111, .tx_vl_idx = 4448, .rx_vl_idx = 4449}, /* DTN Port 14 */                                              \
    {.rx_port_id = 6, .rx_vlan = 240, .tx_port_id = 1, .tx_vlan = 112, .tx_vl_idx = 4450, .rx_vl_idx = 4451}, /* DTN Port 15 */                                              \
    /* DTN Port 16-19: RX=Port3, TX=Port4 */                                                                                                                                  \
    {.rx_port_id = 3, .rx_vlan = 241, .tx_port_id = 4, .tx_vlan = 113, .tx_vl_idx = 4452, .rx_vl_idx = 4453}, /* DTN Port 16 */                                              \
    {.rx_port_id = 3, .rx_vlan = 242, .tx_port_id = 4, .tx_vlan = 114, .tx_vl_idx = 4454, .rx_vl_idx = 4455}, /* DTN Port 17 */                                              \
    {.rx_port_id = 3, .rx_vlan = 243, .tx_port_id = 4, .tx_vlan = 115, .tx_vl_idx = 4456, .rx_vl_idx = 4457}, /* DTN Port 18 */                                              \
    {.rx_port_id = 3, .rx_vlan = 244, .tx_port_id = 4, .tx_vlan = 116, .tx_vl_idx = 4458, .rx_vl_idx = 4459}, /* DTN Port 19 */                                              \
    /* DTN Port 20-23: RX=Port2, TX=Port5 */                                                                                                                                  \
    {.rx_port_id = 2, .rx_vlan = 245, .tx_port_id = 5, .tx_vlan = 117, .tx_vl_idx = 4460, .rx_vl_idx = 4461}, /* DTN Port 20 */                                              \
    {.rx_port_id = 2, .rx_vlan = 246, .tx_port_id = 5, .tx_vlan = 118, .tx_vl_idx = 4462, .rx_vl_idx = 4463}, /* DTN Port 21 */                                              \
    {.rx_port_id = 2, .rx_vlan = 247, .tx_port_id = 5, .tx_vlan = 119, .tx_vl_idx = 4464, .rx_vl_idx = 4465}, /* DTN Port 22 */                                              \
    {.rx_port_id = 2, .rx_vlan = 248, .tx_port_id = 5, .tx_vlan = 120, .tx_vl_idx = 4466, .rx_vl_idx = 4467}, /* DTN Port 23 */                                              \
    /* DTN Port 24-27: RX=Port1, TX=Port6 */                                                                                                                                  \
    {.rx_port_id = 1, .rx_vlan = 249, .tx_port_id = 6, .tx_vlan = 121, .tx_vl_idx = 4468, .rx_vl_idx = 4469}, /* DTN Port 24 */                                              \
    {.rx_port_id = 1, .rx_vlan = 250, .tx_port_id = 6, .tx_vlan = 122, .tx_vl_idx = 4470, .rx_vl_idx = 4471}, /* DTN Port 25 */                                              \
    {.rx_port_id = 1, .rx_vlan = 251, .tx_port_id = 6, .tx_vlan = 123, .tx_vl_idx = 4472, .rx_vl_idx = 4473}, /* DTN Port 26 */                                              \
    {.rx_port_id = 1, .rx_vlan = 252, .tx_port_id = 6, .tx_vlan = 124, .tx_vl_idx = 4474, .rx_vl_idx = 4475}, /* DTN Port 27 */                                              \
    /* DTN Port 28-31: RX=Port0, TX=Port7 */                                                                                                                                  \
    {.rx_port_id = 0, .rx_vlan = 253, .tx_port_id = 7, .tx_vlan = 125, .tx_vl_idx = 4476, .rx_vl_idx = 4477}, /* DTN Port 28 */                                              \
    {.rx_port_id = 0, .rx_vlan = 254, .tx_port_id = 7, .tx_vlan = 126, .tx_vl_idx = 4478, .rx_vl_idx = 4479}, /* DTN Port 29 */                                              \
    {.rx_port_id = 0, .rx_vlan = 255, .tx_port_id = 7, .tx_vlan = 127, .tx_vl_idx = 4480, .rx_vl_idx = 4481}, /* DTN Port 30 */                                              \
    {.rx_port_id = 0, .rx_vlan = 256, .tx_port_id = 7, .tx_vlan = 128, .tx_vl_idx = 4482, .rx_vl_idx = 4483}, /* DTN Port 31 */                                              \
    /* Raw socket ports: No VLAN (rx_vlan=0, tx_vlan=0), TX=RX same port */                                                                                                   \
    {.rx_port_id = 12, .rx_vlan = 0, .tx_port_id = 12, .tx_vlan = 0, .tx_vl_idx = 4484, .rx_vl_idx = 4485},   /* DTN Port 32: 1G copper */                                   \
    {.rx_port_id = 13, .rx_vlan = 0, .tx_port_id = 13, .tx_vlan = 0, .tx_vl_idx = 4486, .rx_vl_idx = 4487},   /* DTN Port 33: 100M copper */                                 \
}

// ==========================================
// HEALTH MONITOR CONFIGURATION
// ==========================================
// Health Monitor sends periodic queries to DTN and receives status responses.
// Runs on Port 13 (eno12409) independently from PRBS traffic.
//
// Query: 64 byte packet sent every 1 second
// Response: 6 packets with VL_IDX=4484 (0x1184) in DST MAC[4:5]
// Timeout: 500ms per cycle

#ifndef HEALTH_MONITOR_ENABLED
#define HEALTH_MONITOR_ENABLED 1
#endif

#ifndef ATE_HEALTH_MONITOR_ENABLED
#define ATE_HEALTH_MONITOR_ENABLED 0
#endif

// ==========================================
// DTN PORT-BASED STATISTICS MODE
// ==========================================
// STATS_MODE_DTN=1: DTN per-port statistics table (34 rows, DTN Port 0-33)
//   - RX queue steering: rte_flow VLAN match (each queue = 1 VLAN = 1 DTN port)
//   - Zero overhead Gbps calculation via HW per-queue stats
//   - PRBS validation per DTN port
//
// STATS_MODE_DTN=0: Legacy server per-port table (8 rows, Server Port 0-7)
//   - RX queue steering: RSS (hash based)
//   - HW total port stats
//   - PRBS validation per server port

#ifndef STATS_MODE_DTN
#define STATS_MODE_DTN 1
#endif

// DTN port count: 32 DPDK + 2 raw socket (Port 12=DTN32, Port 13=DTN33)
#define DTN_PORT_COUNT 34
#define DTN_DPDK_PORT_COUNT 32  // DTN ports connected via DPDK
#define DTN_RAW_PORT_12 32      // Port 12 (1G copper) = DTN Port 32
#define DTN_RAW_PORT_13 33      // Port 13 (100M copper) = DTN Port 33

// 1 VLAN per DTN port
#define DTN_VLANS_PER_PORT 1

// ==========================================
// DTN PORT MAPPING TABLE
// ==========================================
// Each DTN port: TX/RX from DTN perspective
//   DTN RX = Server sends → DTN receives (server_tx_port, rx_vlan)
//   DTN TX = DTN sends → Server receives (server_rx_port, tx_vlan)
//
// DTN Port 0-31: DPDK ports (1 VLAN each)
// DTN Port 32:   Port 12 (1G raw socket, aggregate)
// DTN Port 33:   Port 13 (100M raw socket, aggregate)

struct dtn_port_map_entry {
    uint16_t dtn_port_id;       // DTN port number (0-33)

    // DTN RX (Server → DTN): Server sends from this VLAN
    uint16_t rx_vlan;           // Server TX VLAN (DTN receives this VLAN)
    uint16_t rx_server_port;    // Server DPDK port (the one that TXs)
    uint16_t rx_server_queue;   // Server TX queue index (0-3)

    // DTN TX (DTN → Server): DTN sends from this VLAN
    uint16_t tx_vlan;           // Server RX VLAN (DTN sends from this VLAN)
    uint16_t tx_server_port;    // Server DPDK port (the one that RXs)
    uint16_t tx_server_queue;   // Server RX queue index (0-3)
};

// DTN Port Mapping Table (Source: derived from PTP session table)
// Format: {dtn_port, rx_vlan, rx_srv_port, rx_srv_queue, tx_vlan, tx_srv_port, tx_srv_queue}
#define DTN_PORT_MAP_INIT {                                                                         \
    /* DTN 0-3:   Server TX=Port2(VLAN 97-100),  Server RX=Port5(VLAN 225-228) */                   \
    {.dtn_port_id = 0,  .rx_vlan = 97,  .rx_server_port = 2, .rx_server_queue = 0,                  \
                         .tx_vlan = 225, .tx_server_port = 5, .tx_server_queue = 0},                 \
    {.dtn_port_id = 1,  .rx_vlan = 98,  .rx_server_port = 2, .rx_server_queue = 1,                  \
                         .tx_vlan = 226, .tx_server_port = 5, .tx_server_queue = 1},                 \
    {.dtn_port_id = 2,  .rx_vlan = 99,  .rx_server_port = 2, .rx_server_queue = 2,                  \
                         .tx_vlan = 227, .tx_server_port = 5, .tx_server_queue = 2},                 \
    {.dtn_port_id = 3,  .rx_vlan = 100, .rx_server_port = 2, .rx_server_queue = 3,                  \
                         .tx_vlan = 228, .tx_server_port = 5, .tx_server_queue = 3},                 \
    /* DTN 4-7:   Server TX=Port3(VLAN 101-104), Server RX=Port4(VLAN 229-232) */                   \
    {.dtn_port_id = 4,  .rx_vlan = 101, .rx_server_port = 3, .rx_server_queue = 0,                  \
                         .tx_vlan = 229, .tx_server_port = 4, .tx_server_queue = 0},                 \
    {.dtn_port_id = 5,  .rx_vlan = 102, .rx_server_port = 3, .rx_server_queue = 1,                  \
                         .tx_vlan = 230, .tx_server_port = 4, .tx_server_queue = 1},                 \
    {.dtn_port_id = 6,  .rx_vlan = 103, .rx_server_port = 3, .rx_server_queue = 2,                  \
                         .tx_vlan = 231, .tx_server_port = 4, .tx_server_queue = 2},                 \
    {.dtn_port_id = 7,  .rx_vlan = 104, .rx_server_port = 3, .rx_server_queue = 3,                  \
                         .tx_vlan = 232, .tx_server_port = 4, .tx_server_queue = 3},                 \
    /* DTN 8-11:  Server TX=Port0(VLAN 105-108), Server RX=Port7(VLAN 233-236) */                   \
    {.dtn_port_id = 8,  .rx_vlan = 105, .rx_server_port = 0, .rx_server_queue = 0,                  \
                         .tx_vlan = 233, .tx_server_port = 7, .tx_server_queue = 0},                 \
    {.dtn_port_id = 9,  .rx_vlan = 106, .rx_server_port = 0, .rx_server_queue = 1,                  \
                         .tx_vlan = 234, .tx_server_port = 7, .tx_server_queue = 1},                 \
    {.dtn_port_id = 10, .rx_vlan = 107, .rx_server_port = 0, .rx_server_queue = 2,                  \
                         .tx_vlan = 235, .tx_server_port = 7, .tx_server_queue = 2},                 \
    {.dtn_port_id = 11, .rx_vlan = 108, .rx_server_port = 0, .rx_server_queue = 3,                  \
                         .tx_vlan = 236, .tx_server_port = 7, .tx_server_queue = 3},                 \
    /* DTN 12-15: Server TX=Port1(VLAN 109-112), Server RX=Port6(VLAN 237-240) */                   \
    {.dtn_port_id = 12, .rx_vlan = 109, .rx_server_port = 1, .rx_server_queue = 0,                  \
                         .tx_vlan = 237, .tx_server_port = 6, .tx_server_queue = 0},                 \
    {.dtn_port_id = 13, .rx_vlan = 110, .rx_server_port = 1, .rx_server_queue = 1,                  \
                         .tx_vlan = 238, .tx_server_port = 6, .tx_server_queue = 1},                 \
    {.dtn_port_id = 14, .rx_vlan = 111, .rx_server_port = 1, .rx_server_queue = 2,                  \
                         .tx_vlan = 239, .tx_server_port = 6, .tx_server_queue = 2},                 \
    {.dtn_port_id = 15, .rx_vlan = 112, .rx_server_port = 1, .rx_server_queue = 3,                  \
                         .tx_vlan = 240, .tx_server_port = 6, .tx_server_queue = 3},                 \
    /* DTN 16-19: Server TX=Port4(VLAN 113-116), Server RX=Port3(VLAN 241-244) */                   \
    {.dtn_port_id = 16, .rx_vlan = 113, .rx_server_port = 4, .rx_server_queue = 0,                  \
                         .tx_vlan = 241, .tx_server_port = 3, .tx_server_queue = 0},                 \
    {.dtn_port_id = 17, .rx_vlan = 114, .rx_server_port = 4, .rx_server_queue = 1,                  \
                         .tx_vlan = 242, .tx_server_port = 3, .tx_server_queue = 1},                 \
    {.dtn_port_id = 18, .rx_vlan = 115, .rx_server_port = 4, .rx_server_queue = 2,                  \
                         .tx_vlan = 243, .tx_server_port = 3, .tx_server_queue = 2},                 \
    {.dtn_port_id = 19, .rx_vlan = 116, .rx_server_port = 4, .rx_server_queue = 3,                  \
                         .tx_vlan = 244, .tx_server_port = 3, .tx_server_queue = 3},                 \
    /* DTN 20-23: Server TX=Port5(VLAN 117-120), Server RX=Port2(VLAN 245-248) */                   \
    {.dtn_port_id = 20, .rx_vlan = 117, .rx_server_port = 5, .rx_server_queue = 0,                  \
                         .tx_vlan = 245, .tx_server_port = 2, .tx_server_queue = 0},                 \
    {.dtn_port_id = 21, .rx_vlan = 118, .rx_server_port = 5, .rx_server_queue = 1,                  \
                         .tx_vlan = 246, .tx_server_port = 2, .tx_server_queue = 1},                 \
    {.dtn_port_id = 22, .rx_vlan = 119, .rx_server_port = 5, .rx_server_queue = 2,                  \
                         .tx_vlan = 247, .tx_server_port = 2, .tx_server_queue = 2},                 \
    {.dtn_port_id = 23, .rx_vlan = 120, .rx_server_port = 5, .rx_server_queue = 3,                  \
                         .tx_vlan = 248, .tx_server_port = 2, .tx_server_queue = 3},                 \
    /* DTN 24-27: Server TX=Port6(VLAN 121-124), Server RX=Port1(VLAN 249-252) */                   \
    {.dtn_port_id = 24, .rx_vlan = 121, .rx_server_port = 6, .rx_server_queue = 0,                  \
                         .tx_vlan = 249, .tx_server_port = 1, .tx_server_queue = 0},                 \
    {.dtn_port_id = 25, .rx_vlan = 122, .rx_server_port = 6, .rx_server_queue = 1,                  \
                         .tx_vlan = 250, .tx_server_port = 1, .tx_server_queue = 1},                 \
    {.dtn_port_id = 26, .rx_vlan = 123, .rx_server_port = 6, .rx_server_queue = 2,                  \
                         .tx_vlan = 251, .tx_server_port = 1, .tx_server_queue = 2},                 \
    {.dtn_port_id = 27, .rx_vlan = 124, .rx_server_port = 6, .rx_server_queue = 3,                  \
                         .tx_vlan = 252, .tx_server_port = 1, .tx_server_queue = 3},                 \
    /* DTN 28-31: Server TX=Port7(VLAN 125-128), Server RX=Port0(VLAN 253-256) */                   \
    {.dtn_port_id = 28, .rx_vlan = 125, .rx_server_port = 7, .rx_server_queue = 0,                  \
                         .tx_vlan = 253, .tx_server_port = 0, .tx_server_queue = 0},                 \
    {.dtn_port_id = 29, .rx_vlan = 126, .rx_server_port = 7, .rx_server_queue = 1,                  \
                         .tx_vlan = 254, .tx_server_port = 0, .tx_server_queue = 1},                 \
    {.dtn_port_id = 30, .rx_vlan = 127, .rx_server_port = 7, .rx_server_queue = 2,                  \
                         .tx_vlan = 255, .tx_server_port = 0, .tx_server_queue = 2},                 \
    {.dtn_port_id = 31, .rx_vlan = 128, .rx_server_port = 7, .rx_server_queue = 3,                  \
                         .tx_vlan = 256, .tx_server_port = 0, .tx_server_queue = 3},                 \
    /* DTN 32: Port 12 (1G raw socket) - aggregate */                                               \
    {.dtn_port_id = 32, .rx_vlan = 0, .rx_server_port = 12, .rx_server_queue = 0,                   \
                         .tx_vlan = 0, .tx_server_port = 12, .tx_server_queue = 0},                  \
    /* DTN 33: Port 13 (100M raw socket) - aggregate */                                             \
    {.dtn_port_id = 33, .rx_vlan = 0, .rx_server_port = 13, .rx_server_queue = 0,                   \
                         .tx_vlan = 0, .tx_server_port = 13, .tx_server_queue = 0},                  \
}

// VLAN → DTN port lookup (fast access)
// Index = VLAN ID, Value = DTN port number (0xFF = undefined)
#define DTN_VLAN_LOOKUP_SIZE 257  // VLAN 0-256
#define DTN_VLAN_INVALID 0xFF

#endif /* CONFIG_H */
