#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_mbuf.h>
#include "Config.h"  // IMIX configuration

/*
 *  Packet structure:
 *  ┌──────────────────────────────────────────┐
 *  │ Ethernet Header (14 B)                  │
 *  │ VLAN Header (4 B, optional)             │
 *  │ IPv4 Header (20 B)                      │
 *  │ UDP Header (8 B)                        │
 *  │ Payload (Seq + PRBS Data)               │
 *  └──────────────────────────────────────────┘
 *
 *  DST MAC and DST IP last 2 bytes = VL-ID
 *  VLAN ID = IEEE 802.1Q tag (carried in 802.1Q TCI)
 */

#ifdef USE_VLAN
# define VLAN_ENABLED 1
#else
# define VLAN_ENABLED 0
#endif

// ==========================================
// PRBS-31 CONFIGURATION
// ==========================================
#define PRBS31_PERIOD     0x7FFFFFFF
#define PRBS_CACHE_SIZE   ((PRBS31_PERIOD / 8) + 1)  // ~268 MB
#define PRBS_CACHE_MASK   (PRBS_CACHE_SIZE - 1)
#define SEQ_BYTES         8

// ==========================================
// LATENCY TEST PAYLOAD FORMAT
// ==========================================
// Normal mod:
//   [Sequence 8B][PRBS Data 1459B]
//
// Latency test mode:
//   [Sequence 8B][TX Timestamp 8B][PRBS Data 1451B]
//
// TX Timestamp = rte_rdtsc() (CPU cycle counter)

#define TX_TIMESTAMP_BYTES      8
#define LATENCY_PAYLOAD_OFFSET  (SEQ_BYTES + TX_TIMESTAMP_BYTES)  // 16 bytes header

// PRBS size in latency test mode (1518 - 18 - 20 - 8 - 16 = 1456)
#if VLAN_ENABLED
#define LATENCY_PRBS_BYTES  (LATENCY_TEST_PACKET_SIZE - ETH_HDR_SIZE - VLAN_HDR_SIZE - IP_HDR_SIZE - UDP_HDR_SIZE - SEQ_BYTES - TX_TIMESTAMP_BYTES)
#else
#define LATENCY_PRBS_BYTES  (LATENCY_TEST_PACKET_SIZE - ETH_HDR_SIZE - IP_HDR_SIZE - UDP_HDR_SIZE - SEQ_BYTES - TX_TIMESTAMP_BYTES)
#endif

// ==========================================
// PAYLOAD & PACKET SIZES
// ==========================================
#define PAYLOAD_SIZE_NO_VLAN 1471  // 8 seq + 1463 prbs
#define PAYLOAD_SIZE_VLAN    1467  // 8 seq + 1460 prbs (1518 - 14 - 4 - 20 - 8 - 5 = 1467)
#define VLAN_TAG_SIZE        4

#define ETH_HDR_SIZE  14
#define VLAN_HDR_SIZE 4
#define IP_HDR_SIZE   20
#define UDP_HDR_SIZE  8

#define PACKET_SIZE_NO_VLAN (ETH_HDR_SIZE + IP_HDR_SIZE + UDP_HDR_SIZE + PAYLOAD_SIZE_NO_VLAN)
#define PACKET_SIZE_VLAN    (ETH_HDR_SIZE + VLAN_HDR_SIZE + IP_HDR_SIZE + UDP_HDR_SIZE + PAYLOAD_SIZE_VLAN)

#if VLAN_ENABLED
# define PACKET_SIZE   PACKET_SIZE_VLAN
# define PAYLOAD_SIZE  PAYLOAD_SIZE_VLAN
# define NUM_PRBS_BYTES (PAYLOAD_SIZE_VLAN - SEQ_BYTES)
# define L2_HEADER_SIZE (ETH_HDR_SIZE + VLAN_HDR_SIZE)
#else
# define PACKET_SIZE   PACKET_SIZE_NO_VLAN
# define PAYLOAD_SIZE  PAYLOAD_SIZE_NO_VLAN
# define NUM_PRBS_BYTES (PAYLOAD_SIZE_NO_VLAN - SEQ_BYTES)
# define L2_HEADER_SIZE ETH_HDR_SIZE
#endif

// ==========================================
// IMIX SUPPORT - DYNAMIC PACKET SIZES
// ==========================================
// In IMIX mode, packet sizes are variable.
// MAX_PRBS_BYTES: Constant for PRBS offset calculation (largest PRBS size)
// This value is used with the same formula for all sizes:
//   offset = (sequence × MAX_PRBS_BYTES) % PRBS_CACHE_SIZE
// This way, the RX side can calculate the offset from the sequence.

#define MAX_PRBS_BYTES NUM_PRBS_BYTES  // 1459 (VLAN) or 1463 (non-VLAN)

// Minimum payload size for IMIX
// 100 byte packet: 100 - L2(18) - IP(20) - UDP(8) = 54 byte payload
// Payload = SEQ(8) + PRBS(46) minimum
#define MIN_IMIX_PAYLOAD_VLAN    (100 - ETH_HDR_SIZE - VLAN_HDR_SIZE - IP_HDR_SIZE - UDP_HDR_SIZE)
#define MIN_IMIX_PAYLOAD_NO_VLAN (100 - ETH_HDR_SIZE - IP_HDR_SIZE - UDP_HDR_SIZE)
#define MIN_IMIX_PRBS_BYTES      (MIN_IMIX_PAYLOAD_VLAN - SEQ_BYTES)  // 46 bytes

// Calculate PRBS size from packet size
#if VLAN_ENABLED
# define CALC_PRBS_LEN(pkt_size) ((pkt_size) - ETH_HDR_SIZE - VLAN_HDR_SIZE - IP_HDR_SIZE - UDP_HDR_SIZE - SEQ_BYTES)
# define CALC_PAYLOAD_LEN(pkt_size) ((pkt_size) - ETH_HDR_SIZE - VLAN_HDR_SIZE - IP_HDR_SIZE - UDP_HDR_SIZE)
#else
# define CALC_PRBS_LEN(pkt_size) ((pkt_size) - ETH_HDR_SIZE - IP_HDR_SIZE - UDP_HDR_SIZE - SEQ_BYTES)
# define CALC_PAYLOAD_LEN(pkt_size) ((pkt_size) - ETH_HDR_SIZE - IP_HDR_SIZE - UDP_HDR_SIZE)
#endif

#define ETHER_TYPE_IPv4 0x0800
#define ETHER_TYPE_VLAN 0x8100
#define MAX_PRBS_CACHE_PORTS 12

struct ports_config; 

// ==========================================
// VLAN HEADER
// ==========================================
struct vlan_hdr {
    uint16_t tci;        // Priority (3b) + CFI (1b) + VLAN ID (12b)
    uint16_t eth_proto;  // Next protocol (e.g. 0x0800 for IPv4)
} __rte_packed;

// ==========================================
// PACKET TEMPLATE (in-memory layout)
// ==========================================
#if VLAN_ENABLED
struct packet_template {
    struct rte_ether_hdr eth;
    struct vlan_hdr vlan;
    struct rte_ipv4_hdr ip;
    struct rte_udp_hdr udp;
    uint8_t payload[PAYLOAD_SIZE_VLAN];
} __rte_packed __rte_aligned(2);
#else
struct packet_template {
    struct rte_ether_hdr eth;
    struct rte_ipv4_hdr ip;
    struct rte_udp_hdr udp;
    uint8_t payload[PAYLOAD_SIZE_NO_VLAN];
} __rte_packed __rte_aligned(2);
#endif

// ==========================================
// PACKET CONFIGURATION STRUCT
// ==========================================
//
// VLAN ID   → 802.1Q header tag
// VL ID     → used for MAC/IP mapping within the packet
//
struct packet_config {
#if VLAN_ENABLED
    uint16_t vlan_id;          // VLAN header tag (802.1Q)
    uint8_t  vlan_priority;
#endif
    uint16_t vl_id;            // VL ID (for MAC/IP)
    struct rte_ether_addr src_mac;
    struct rte_ether_addr dst_mac;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  ttl;
    uint8_t  tos;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t *payload_data;
    uint16_t payload_size;
};

// ==========================================
// PRBS-31 CACHE STRUCTURE (per-port)
// ==========================================
struct prbs_cache {
    uint8_t  *cache;         // Main PRBS cache
    uint8_t  *cache_ext;     // Extended cache (wraparound)
    uint32_t  initial_state; // Initial PRBS-31 state
    bool      initialized;
    int       socket_id;
};

// global PRBS cache
extern struct prbs_cache port_prbs_cache[MAX_PRBS_CACHE_PORTS];

// ==========================================
// FUNCTION PROTOTYPES
// ==========================================

// PRBS utilities
void init_prbs_cache_for_all_ports(uint16_t nb_ports, const struct ports_config *ports);
void cleanup_prbs_cache(void);
uint8_t* get_prbs_cache_for_port(uint16_t port_id);
uint8_t* get_prbs_cache_ext_for_port(uint16_t port_id);

// Packet building utilities
void init_packet_config(struct packet_config *config);
int  build_packet(struct packet_template *template, const struct packet_config *config);
int  build_packet_mbuf(struct rte_mbuf *mbuf, const struct packet_config *config);
uint16_t calculate_ip_checksum(struct rte_ipv4_hdr *ip);
uint16_t calculate_udp_checksum(const struct rte_ipv4_hdr *ip, const struct rte_udp_hdr *udp,
                                const uint8_t *payload, uint16_t payload_len);
int  set_mac_from_string(struct rte_ether_addr *mac, const char *mac_str);
int  set_ip_from_string(uint32_t *ip, const char *ip_str);
void print_packet_info(const struct packet_config *config);

// Fill PRBS payload for TX (dynamic size supported)
// prbs_len: Number of PRBS bytes to write (variable for IMIX)
void fill_payload_with_prbs31_dynamic(struct rte_mbuf *mbuf, uint16_t port_id,
                                       uint64_t sequence_number, uint16_t l2_len,
                                       uint16_t prbs_len);

// Legacy wrapper (backward compatibility for fixed size)
static inline void fill_payload_with_prbs31(struct rte_mbuf *mbuf, uint16_t port_id,
                                             uint64_t sequence_number, uint16_t l2_len)
{
    fill_payload_with_prbs31_dynamic(mbuf, port_id, sequence_number, l2_len, NUM_PRBS_BYTES);
}

// ==========================================
// IMIX HELPER FUNCTIONS
// ==========================================

// Get packet size from IMIX pattern (based on worker's offset + counter)
static inline uint16_t get_imix_packet_size(uint64_t pkt_counter, uint8_t worker_offset)
{
    static const uint16_t imix_pattern[IMIX_PATTERN_SIZE] = IMIX_PATTERN_INIT;
    return imix_pattern[(pkt_counter + worker_offset) % IMIX_PATTERN_SIZE];
}

// Calculate payload size from packet size
static inline uint16_t calc_payload_size(uint16_t pkt_size)
{
    return CALC_PAYLOAD_LEN(pkt_size);
}

// Calculate PRBS size from packet size
static inline uint16_t calc_prbs_size(uint16_t pkt_size)
{
    return CALC_PRBS_LEN(pkt_size);
}

// Build packet with dynamic size
int build_packet_dynamic(struct rte_mbuf *mbuf, const struct packet_config *config,
                          uint16_t packet_size);

#endif /* PACKET_H */