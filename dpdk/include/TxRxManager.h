#ifndef TX_RX_MANAGER_H
#define TX_RX_MANAGER_H

#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_atomic.h>
#include "Port.h"
#include "Packet.h"
#include "Config.h"

#define TX_RING_SIZE 2048
#define RX_RING_SIZE 8192
#define NUM_MBUFS 524287
#define MBUF_CACHE_SIZE 512
#define BURST_SIZE 32

// VL-ID range limits
// Each port may have different tx_vl_ids start values (e.g., Port 7 → 3971)
// Each queue has a 128 VL-ID range
// Extended for raw socket ports:
//   - Raw Port 0 (1G): 4099-4226 (128 VL-ID)
//   - Raw Port 1 (100M): 4227-4258 (32 VL-ID)
// Extended for DPDK External TX:
//   - Port 2: 4259-4386, Port 3: 4387-4514
//   - Port 0: 4515-4642, Port 1: 4643-4770
#define MAX_VL_ID 4800  // Increased to support DPDK external TX (up to 4770)
#define MIN_VL_ID 3
#define VL_RANGE_SIZE_PER_QUEUE 128  // 128 VL-IDs per queue

// Global VLAN configuration for all ports
extern struct port_vlan_config port_vlans[MAX_PORTS_CONFIG];

/**
 * @brief Load VLAN config based on ATE mode
 * Call after g_ate_mode is set (after latency test sequence).
 * Loads ATE_PORT_VLAN_CONFIG_INIT if ATE mode, otherwise keeps default.
 */
void port_vlans_load_config(bool ate_mode);

/**
 * Token bucket for rate limiting
 */
struct rate_limiter
{
    uint64_t tokens;         // Current tokens (in bytes)
    uint64_t max_tokens;     // Maximum tokens (bucket size)
    uint64_t tokens_per_sec; // Token generation rate (bytes/sec)
    uint64_t last_update;    // Last update timestamp (TSC cycles)
    uint64_t tsc_hz;         // TSC frequency
};

// RX Statistics per port
struct rx_stats
{
    rte_atomic64_t total_rx_pkts;
    rte_atomic64_t good_pkts;
    rte_atomic64_t bad_pkts;
    rte_atomic64_t bit_errors;
    rte_atomic64_t out_of_order_pkts;  // Out-of-order packets
    rte_atomic64_t lost_pkts;          // Lost packets (sequence gap)
    rte_atomic64_t duplicate_pkts;     // Duplicate packets
    rte_atomic64_t short_pkts;         // Packets shorter than minimum length
    rte_atomic64_t external_pkts;      // Packets from external lines (VL-ID out of range)
    // Raw socket packets (non-VLAN) - tracked separately from DPDK
    rte_atomic64_t raw_socket_rx_pkts; // Packet count from raw socket
    rte_atomic64_t raw_socket_rx_bytes; // Byte count from raw socket
};

extern struct rx_stats rx_stats_per_port[MAX_PORTS];

// ==========================================
// DTN PORT-BASED STATISTICS (STATS_MODE_DTN)
// ==========================================
#if STATS_MODE_DTN

// DTN per-port PRBS statistics
// DTN TX (DTN→Server) quality metrics: measured on Server RX side
struct dtn_port_stats {
    rte_atomic64_t good_pkts;
    rte_atomic64_t bad_pkts;
    rte_atomic64_t bit_errors;
    rte_atomic64_t lost_pkts;
    rte_atomic64_t out_of_order_pkts;
    rte_atomic64_t duplicate_pkts;
    rte_atomic64_t short_pkts;
    rte_atomic64_t total_rx_pkts;     // Server RX = DTN TX packet count
};

extern struct dtn_port_stats dtn_stats[DTN_PORT_COUNT];

// DTN port mapping table (loaded from config at runtime)
extern struct dtn_port_map_entry dtn_port_map[DTN_PORT_COUNT];

// VLAN → DTN port fast lookup table
extern uint8_t vlan_to_dtn_port[DTN_VLAN_LOOKUP_SIZE];

/**
 * Initialize DTN port mapping and VLAN lookup table
 */
void init_dtn_port_map(void);

/**
 * Initialize DTN port statistics
 */
void init_dtn_stats(void);

/**
 * Install VLAN-based rte_flow rules for RX queue steering
 * Each VLAN → routed to corresponding RX queue (1:1 mapping)
 */
int dtn_flow_rules_install(uint16_t port_id);

/**
 * Remove VLAN-based rte_flow rules
 */
void dtn_flow_rules_remove(uint16_t port_id);

#endif /* STATS_MODE_DTN */

/**
 * VL-ID based sequence tracking (lock-free, watermark-based)
 * Uses highest-seen watermark instead of expected sequence
 * This approach handles RSS-induced reordering correctly
 */
struct vl_sequence_tracker {
    volatile uint64_t max_seq;       // Highest sequence seen for this VL-ID
    volatile uint64_t min_seq;       // Lowest sequence seen (first packet - for watermark calc)
    volatile uint64_t pkt_count;     // Total packets received for this VL-ID
    volatile uint64_t expected_seq;  // Expected next sequence for real-time gap detection
    volatile int initialized;        // Has this VL-ID been seen before? (0=false, 1=true)
};

/**
 * Per-port VL-ID sequence tracking table
 * Lock-free design: each VL-ID tracker uses atomic operations
 */
struct port_vl_tracker {
    struct vl_sequence_tracker vl_trackers[MAX_VL_ID + 1];  // Index by VL-ID
    // No lock needed - using lock-free atomic operations per VL-ID
};

extern struct port_vl_tracker port_vl_trackers[MAX_PORTS];

/**
 * TX/RX configuration for a port
 */
struct txrx_config
{
    uint16_t port_id;
    uint16_t nb_tx_queues;
    uint16_t nb_rx_queues;
    struct rte_mempool *mbuf_pool;
};

/**
 * TX worker parameters
 */
struct tx_worker_params
{
    uint16_t port_id;
    uint16_t dst_port_id;
    uint16_t queue_id;
    uint16_t lcore_id;
    uint16_t vlan_id;       // VLAN header tag (802.1Q)
    uint16_t vl_id;         // VL ID for MAC/IP (different from VLAN)
    struct packet_config pkt_config;
    struct rte_mempool *mbuf_pool;
    volatile bool *stop_flag;
    uint64_t sequence_number;  // Not used anymore - VL-ID based now
    struct rate_limiter limiter;

    // External TX parameters (for Port 12 via switch)
    bool ext_tx_enabled;        // Is external TX enabled for this worker?
    uint16_t ext_vlan_id;       // External TX VLAN tag
    uint16_t ext_vl_id_start;   // External TX VL-ID start
    uint16_t ext_vl_id_count;   // External TX VL-ID count
    struct rate_limiter ext_limiter;  // Separate rate limiter for external TX

    // Phase distribution: total active port count (runtime)
    uint16_t nb_ports;
};

/**
 * RX worker parameters
 */
struct rx_worker_params
{
    uint16_t port_id;
    uint16_t src_port_id;
    uint16_t queue_id;
    uint16_t lcore_id;
    uint16_t vlan_id;       // VLAN header tag (802.1Q)
    uint16_t vl_id;         // VL ID for MAC/IP (different from VLAN)
    volatile bool *stop_flag;
};

/**
 * Initialize VLAN configuration from config.h
 */
void init_vlan_config(void);

/**
 * Get TX VLAN ID for a specific port and queue
 */
uint16_t get_tx_vlan_for_queue(uint16_t port_id, uint16_t queue_id);

/**
 * Get RX VLAN ID for a specific port and queue
 */
uint16_t get_rx_vlan_for_queue(uint16_t port_id, uint16_t queue_id);

/**
 * Get TX VL ID for a specific port and queue
 */
uint16_t get_tx_vl_id_for_queue(uint16_t port_id, uint16_t queue_id);

/**
 * Get RX VL ID for a specific port and queue
 */
uint16_t get_rx_vl_id_for_queue(uint16_t port_id, uint16_t queue_id);

/**
 * Print VLAN configuration for all ports
 */
void print_vlan_config(void);

/**
 * Initialize TX/RX for a port
 */
int init_port_txrx(uint16_t port_id, struct txrx_config *config);

/**
 * Create mbuf pool for a socket
 */
struct rte_mempool *create_mbuf_pool(uint16_t socket_id, uint16_t port_id);

/**
 * Setup TX queue
 */
int setup_tx_queue(uint16_t port_id, uint16_t queue_id, uint16_t socket_id);

/**
 * Setup RX queue
 */
int setup_rx_queue(uint16_t port_id, uint16_t queue_id, uint16_t socket_id,
                   struct rte_mempool *mbuf_pool);

/**
 * TX worker thread function with VL-ID based sequencing
 */
int tx_worker(void *arg);

/**
 * RX worker thread function with PRBS verification and VL-ID based sequence validation
 */
int rx_worker(void *arg);

/**
 * Start TX/RX workers for all ports
 */
int start_txrx_workers(struct ports_config *ports_config, volatile bool *stop_flag);

/**
 * Print port statistics from DPDK
 */
void print_port_stats(struct ports_config *ports_config);

/**
 * Initialize RX statistics and VL-ID trackers
 */
void init_rx_stats(void);

// ==========================================
// LATENCY TEST STRUCTURES & FUNCTIONS
// ==========================================

#if LATENCY_TEST_ENABLED

// Single latency measurement result (multi-sample supported)
struct latency_result {
    uint16_t tx_port;           // Sender port
    uint16_t rx_port;           // Receiver port
    uint16_t vlan_id;           // VLAN ID
    uint16_t vl_id;             // VL-ID
    uint64_t tx_timestamp;      // Last TX time (TSC cycles)
    uint64_t rx_timestamp;      // Last RX time (TSC cycles)
    uint64_t latency_cycles;    // Last latency (cycles)
    double   latency_us;        // Average latency (microseconds)
    double   min_latency_us;    // Minimum latency
    double   max_latency_us;    // Maximum latency
    double   sum_latency_us;    // Total latency (for average calculation)
    uint32_t tx_count;          // Number of packets sent
    uint32_t rx_count;          // Number of packets received
    bool     received;          // At least 1 packet received?
    bool     prbs_ok;           // PRBS validation successful?
};

// Per-port latency test state
#define MAX_LATENCY_TESTS_PER_PORT 32  // Up to max VLAN count

struct port_latency_test {
    uint16_t port_id;
    uint16_t test_count;                                    // Number of tests for this port
    struct latency_result results[MAX_LATENCY_TESTS_PER_PORT];
    volatile bool tx_complete;                              // TX completed?
    volatile bool rx_complete;                              // All RX completed?
};

// Global latency test state
struct latency_test_state {
    volatile bool test_running;             // Is test running?
    volatile bool test_complete;            // Is test complete?
    uint64_t tsc_hz;                        // TSC frequency (cycles/sec)
    uint64_t test_start_time;               // Test start time
    struct port_latency_test ports[MAX_PORTS];
};

extern struct latency_test_state g_latency_test;

/**
 * Start latency test
 * Sends 1 packet from each VLAN for each port
 */
int start_latency_test(struct ports_config *ports_config, volatile bool *stop_flag);

/**
 * Print latency test results
 */
void print_latency_results(void);

/**
 * Reset latency test state
 */
void reset_latency_test(void);

#endif /* LATENCY_TEST_ENABLED */

#endif /* TX_RX_MANAGER_H */