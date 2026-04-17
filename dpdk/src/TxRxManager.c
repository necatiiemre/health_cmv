#include "TxRxManager.h"
#include "RawSocketPort.h"  // For external packet PRBS verification
#include "DpdkExternalTx.h" // For integrated external TX
#include "EmbeddedLatency/EmbeddedLatency.h" // For ate_mode_enabled()
#include <rte_lcore.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_flow.h>
#include <stdlib.h>
#include <string.h>

// ==========================================
// GLOBAL VARIABLES
// ==========================================

// Global VLAN configuration for all ports (default: normal mode)
struct port_vlan_config port_vlans[MAX_PORTS_CONFIG] = PORT_VLAN_CONFIG_INIT;

// ATE mode VLAN configuration (loaded at runtime if ATE mode selected)
static const struct port_vlan_config ate_port_vlans[MAX_PORTS_CONFIG] = ATE_PORT_VLAN_CONFIG_INIT;

void port_vlans_load_config(bool ate_mode)
{
    if (ate_mode) {
        memcpy(port_vlans, ate_port_vlans, sizeof(port_vlans));
        printf("[ATE] ATE mode VLAN configuration loaded\n");
    } else {
        // Already initialized with normal config at compile time
        printf("[CONFIG] Normal mode VLAN configuration active\n");
    }
}

// Global RX statistics per port
struct rx_stats rx_stats_per_port[MAX_PORTS];

#if STATS_MODE_DTN
// DTN per-port statistics
struct dtn_port_stats dtn_stats[DTN_PORT_COUNT];

// DTN port mapping table
struct dtn_port_map_entry dtn_port_map[DTN_PORT_COUNT] = DTN_PORT_MAP_INIT;

// VLAN → DTN port fast lookup table
uint8_t vlan_to_dtn_port[DTN_VLAN_LOOKUP_SIZE];

// VLAN flow rule handles (for cleanup)
#define DTN_MAX_FLOW_RULES_PER_PORT 4
static struct rte_flow *dtn_flow_handles[MAX_PORTS][DTN_MAX_FLOW_RULES_PER_PORT] = {{NULL}};
#endif /* STATS_MODE_DTN */

// Global VL-ID sequence trackers per port (for RX validation)
struct port_vl_tracker port_vl_trackers[MAX_PORTS];

// Per-port, per-VL-ID TX sequence counter
struct tx_vl_sequence
{
    uint64_t sequence[MAX_VL_ID + 1];    // Sequence counter for each VL-ID
    rte_spinlock_t locks[MAX_VL_ID + 1]; // Lock per VL-ID
};

static struct tx_vl_sequence tx_vl_sequences[MAX_PORTS];

// ==========================================
// VL ID RANGE DEFINITIONS (Port-Aware)
// ==========================================
// tx_vl_ids and rx_vl_ids are read from config for each port.
// Each queue has a 128 VL-ID range.
// Example: If tx_vl_ids[0]=1027 for Port 0, Queue 0 → VL range [1027, 1155)

// ==========================================
// HELPER FUNCTIONS - VL-ID BASED SEQUENCE
// ==========================================

/**
 * Initialize TX VL-ID sequences
 */
static void init_tx_vl_sequences(void)
{
    for (int port = 0; port < MAX_PORTS; port++)
    {
        for (int vl = 0; vl <= MAX_VL_ID; vl++)
        {
            tx_vl_sequences[port].sequence[vl] = 0;
            rte_spinlock_init(&tx_vl_sequences[port].locks[vl]);
        }
    }
    printf("TX VL-ID sequence counters initialized\n");
}

/**
 * Get next sequence for a VL-ID (thread-safe)
 */
static inline uint64_t get_next_tx_sequence(uint16_t port_id, uint16_t vl_id)
{
    if (vl_id > MAX_VL_ID || port_id >= MAX_PORTS)
        return 0;

    rte_spinlock_lock(&tx_vl_sequences[port_id].locks[vl_id]);
    uint64_t seq = tx_vl_sequences[port_id].sequence[vl_id]++;
    rte_spinlock_unlock(&tx_vl_sequences[port_id].locks[vl_id]);

    return seq;
}

/**
 * Peek current sequence for a VL-ID without incrementing (thread-safe)
 * Use with commit_tx_sequence() for send-then-commit pattern.
 */
static inline uint64_t peek_tx_sequence(uint16_t port_id, uint16_t vl_id)
{
    if (vl_id > MAX_VL_ID || port_id >= MAX_PORTS)
        return 0;

    return tx_vl_sequences[port_id].sequence[vl_id];
}

/**
 * Commit (increment) sequence for a VL-ID after successful send.
 * Only call after confirming the packet was actually transmitted.
 */
static inline void commit_tx_sequence(uint16_t port_id, uint16_t vl_id)
{
    if (vl_id > MAX_VL_ID || port_id >= MAX_PORTS)
        return;

    tx_vl_sequences[port_id].sequence[vl_id]++;
}

/**
 * Extract VL-ID from packet DST MAC
 */
static inline uint16_t extract_vl_id_from_packet(uint8_t *pkt_data, uint16_t l2_len)
{
    (void)l2_len; // Unused
    // DST MAC is at offset 0 in Ethernet header
    // VL-ID is in last 2 bytes of DST MAC
    uint8_t *dst_mac = pkt_data;
    uint16_t vl_id = ((uint16_t)dst_mac[4] << 8) | dst_mac[5];
    return vl_id;
}

/**
 * Get TX VL ID range start for a port and queue (PORT-AWARE)
 * Returns the tx_vl_ids value from config
 */
static inline uint16_t get_tx_vl_id_range_start(uint16_t port_id, uint16_t queue_index)
{
    if (port_id >= MAX_PORTS_CONFIG)
    {
        printf("Warning: Invalid port_id %u for TX VL ID range start\n", port_id);
        return 3; // Fallback
    }
    if (queue_index >= port_vlans[port_id].tx_vlan_count)
    {
        printf("Warning: Invalid queue_index %u for port %u TX VL ID range start\n",
               queue_index, port_id);
        return port_vlans[port_id].tx_vl_ids[0]; // Fallback to first
    }
    return port_vlans[port_id].tx_vl_ids[queue_index];
}

/**
 * Get TX VL ID range end for a port and queue (exclusive, PORT-AWARE)
 * Returns start + 128
 */
static inline uint16_t get_tx_vl_id_range_end(uint16_t port_id, uint16_t queue_index)
{
    return get_tx_vl_id_range_start(port_id, queue_index) + VL_RANGE_SIZE_PER_QUEUE;
}

/**
 * Get RX VL ID range start for a port and queue (PORT-AWARE)
 * Returns the rx_vl_ids value from config
 */
static inline uint16_t get_rx_vl_id_range_start(uint16_t port_id, uint16_t queue_index)
{
    if (port_id >= MAX_PORTS_CONFIG)
    {
        printf("Warning: Invalid port_id %u for RX VL ID range start\n", port_id);
        return 3; // Fallback
    }
    if (queue_index >= port_vlans[port_id].rx_vlan_count)
    {
        printf("Warning: Invalid queue_index %u for port %u RX VL ID range start\n",
               queue_index, port_id);
        return port_vlans[port_id].rx_vl_ids[0]; // Fallback to first
    }
    return port_vlans[port_id].rx_vl_ids[queue_index];
}

/**
 * Get RX VL ID range end for a port and queue (exclusive, PORT-AWARE)
 * Returns start + 128
 */
static inline uint16_t get_rx_vl_id_range_end(uint16_t port_id, uint16_t queue_index)
{
    return get_rx_vl_id_range_start(port_id, queue_index) + VL_RANGE_SIZE_PER_QUEUE;
}

/**
 * Get VL ID range size (always 128)
 */
static inline uint16_t get_vl_id_range_size(void)
{
    return VL_RANGE_SIZE_PER_QUEUE;
}

/**
 * Validate if a VL ID is within the valid TX range for a port and queue
 */
static inline bool is_valid_tx_vl_id_for_queue(uint16_t vl_id, uint16_t port_id, uint16_t queue_index)
{
    uint16_t start = get_tx_vl_id_range_start(port_id, queue_index);
    uint16_t end = get_tx_vl_id_range_end(port_id, queue_index);
    return (vl_id >= start && vl_id < end);
}

/**
 * Validate if a VL ID is within the valid RX range for a port and queue
 */
static inline bool is_valid_rx_vl_id_for_queue(uint16_t vl_id, uint16_t port_id, uint16_t queue_index)
{
    uint16_t start = get_rx_vl_id_range_start(port_id, queue_index);
    uint16_t end = get_rx_vl_id_range_end(port_id, queue_index);
    return (vl_id >= start && vl_id < end);
}

// ==========================================
// RATE LIMITER FUNCTIONS
// ==========================================

/**
 * Initialize rate limiter
 */
static void init_rate_limiter(struct rate_limiter *limiter,
                              double target_gbps,
                              uint16_t num_queues)
{
    limiter->tsc_hz = rte_get_tsc_hz();

    // bytes/sec per queue
    double gbps_per_queue = target_gbps / (double)num_queues;
    limiter->tokens_per_sec = (uint64_t)(gbps_per_queue * 1000000000.0 / 8.0);

    // Burst window: Use per-queue rate, not total rate
    // Smaller burst = smoother traffic = less switch buffer pressure
    // /1000 = ~1ms burst window (was /100 = ~10ms, too large)
    limiter->max_tokens = limiter->tokens_per_sec / 10000;

    // Minimum bucket size to allow at least one burst
    // IMIX: Use average packet size
#if IMIX_ENABLED
    uint64_t min_bucket = BURST_SIZE * IMIX_AVG_PACKET_SIZE * 2;
#else
    uint64_t min_bucket = BURST_SIZE * PACKET_SIZE * 2;
#endif
    if (limiter->max_tokens < min_bucket) {
        limiter->max_tokens = min_bucket;
    }

    // SOFT START: Start with empty bucket to prevent initial burst
    limiter->tokens = 0;
    limiter->last_update = rte_get_tsc_cycles();

    printf("Rate limiter initialized (SOFT START, ~1ms burst window):\n");
    printf("  Target: %.2f Gbps total / %u queues = %.2f Gbps per queue\n",
           target_gbps, num_queues, gbps_per_queue);
    printf("  Bytes/sec: %lu (%.2f MB/s)\n",
           limiter->tokens_per_sec, limiter->tokens_per_sec / (1024.0 * 1024.0));
    printf("  Bucket size: %lu bytes\n", limiter->max_tokens);
}

/**
 * Update tokens - ADD tokens based on elapsed time
 */
static inline void update_tokens(struct rate_limiter *limiter)
{
    uint64_t now = rte_get_tsc_cycles();
    uint64_t elapsed_cycles = now - limiter->last_update;

    if (elapsed_cycles < (limiter->tsc_hz / 1000000))
    {
        return;
    }

    __uint128_t tokens_to_add_128 = (__uint128_t)elapsed_cycles * (__uint128_t)limiter->tokens_per_sec;
    uint64_t tokens_to_add = (uint64_t)(tokens_to_add_128 / limiter->tsc_hz);

    if (tokens_to_add > 0)
    {
        limiter->tokens += tokens_to_add;

        if (limiter->tokens > limiter->max_tokens)
        {
            limiter->tokens = limiter->max_tokens;
        }

        limiter->last_update = now;
    }
}

/**
 * Try to consume tokens for sending bytes
 */
static inline bool consume_tokens(struct rate_limiter *limiter, uint64_t bytes)
{
    update_tokens(limiter);

    if (limiter->tokens >= bytes)
    {
        limiter->tokens -= bytes;
        return true;
    }

    return false;
}

/**
 * Initialize external TX rate limiter (Mbps based)
 *
 * AGGRESSIVE optimization for minimal packet loss:
 * - 0.5ms burst window (not 1ms) for VERY smooth traffic
 * - Tiny min_bucket (4 packets) to minimize micro-bursts
 * - Stagger support via initial token offset
 */
static void init_ext_rate_limiter_with_stagger(struct rate_limiter *limiter,
                                                uint32_t rate_mbps,
                                                uint16_t port_id,
                                                uint16_t queue_id)
{
    limiter->tsc_hz = rte_get_tsc_hz();

    // bytes/sec - use explicit calculation for Mbps
    // rate_mbps * 1e6 bits/sec / 8 = bytes/sec
    limiter->tokens_per_sec = (uint64_t)rate_mbps * 125000ULL;  // 1e6/8 = 125000

    // Burst window: ~0.5ms worth of tokens for ULTRA SMOOTH rate limiting
    // 1ms was still causing some loss, 0.5ms = even smaller bursts
    limiter->max_tokens = limiter->tokens_per_sec / 2000;

    // SINGLE PACKET bucket: 1 packet only
    // Maximum smoothness - no bursting at all
    // 1 packet × 1509 bytes = ~1.5KB per send
    const uint64_t EXT_MIN_BURST_PKTS = 1;
    uint64_t min_bucket = EXT_MIN_BURST_PKTS * PACKET_SIZE;
    if (limiter->max_tokens < min_bucket) {
        limiter->max_tokens = min_bucket;
    }

    // STAGGERED START: Each port/queue gets different initial tokens
    // This spreads out the first bursts across time
    // port_id 0-3, queue_id 0-3 → 16 different offsets
    // Offset = (port * 4 + queue) / 16 of max_tokens
    uint32_t stagger_slot = (port_id * 4 + queue_id) % 16;
    limiter->tokens = (limiter->max_tokens * stagger_slot) / 16;
    limiter->last_update = rte_get_tsc_cycles();

    printf("  [ExtRateLimiter] rate=%u Mbps, tokens/s=%lu (%.2f MB/s), bucket=%lu (~0.5ms), stagger=%u/16\n",
           rate_mbps, limiter->tokens_per_sec, limiter->tokens_per_sec / (1024.0 * 1024.0),
           limiter->max_tokens, stagger_slot);
}

// Wrapper for backward compatibility
static void init_ext_rate_limiter(struct rate_limiter *limiter, uint32_t rate_mbps)
{
    init_ext_rate_limiter_with_stagger(limiter, rate_mbps, 0, 0);
}

// ==========================================
// INITIALIZATION FUNCTIONS
// ==========================================

void init_vlan_config(void)
{
    printf("\n=== VLAN Configuration Initialized ===\n");
    printf("Loaded VLAN configuration for %d ports\n", MAX_PORTS_CONFIG);
}

void init_rx_stats(void)
{
    for (int i = 0; i < MAX_PORTS; i++)
    {
        // Initialize atomic counters
        rte_atomic64_init(&rx_stats_per_port[i].total_rx_pkts);
        rte_atomic64_init(&rx_stats_per_port[i].good_pkts);
        rte_atomic64_init(&rx_stats_per_port[i].bad_pkts);
        rte_atomic64_init(&rx_stats_per_port[i].bit_errors);
        rte_atomic64_init(&rx_stats_per_port[i].out_of_order_pkts);
        rte_atomic64_init(&rx_stats_per_port[i].lost_pkts);
        rte_atomic64_init(&rx_stats_per_port[i].duplicate_pkts);
        rte_atomic64_init(&rx_stats_per_port[i].short_pkts);
        rte_atomic64_init(&rx_stats_per_port[i].external_pkts);
        // Raw socket RX counters (non-VLAN packets from raw socket ports)
        rte_atomic64_init(&rx_stats_per_port[i].raw_socket_rx_pkts);
        rte_atomic64_init(&rx_stats_per_port[i].raw_socket_rx_bytes);

        // Initialize VL-ID sequence trackers (lock-free, watermark-based)
        for (int vl = 0; vl <= MAX_VL_ID; vl++)
        {
            port_vl_trackers[i].vl_trackers[vl].max_seq = 0;
            port_vl_trackers[i].vl_trackers[vl].pkt_count = 0;
            port_vl_trackers[i].vl_trackers[vl].initialized = 0;  // 0=false, 1=true
        }
    }
    printf("RX statistics and VL-ID sequence trackers initialized for all ports\n");
}

#if STATS_MODE_DTN
// ==========================================
// DTN PORT MAPPING & STATISTICS INIT
// ==========================================

void init_dtn_port_map(void)
{
    // Fill the VLAN → DTN port lookup table
    memset(vlan_to_dtn_port, DTN_VLAN_INVALID, sizeof(vlan_to_dtn_port));

    for (int i = 0; i < DTN_DPDK_PORT_COUNT; i++) {
        // DTN TX VLAN (DTN→Server, seen on server RX)
        if (dtn_port_map[i].tx_vlan < DTN_VLAN_LOOKUP_SIZE) {
            vlan_to_dtn_port[dtn_port_map[i].tx_vlan] = (uint8_t)i;
        }
        // DTN RX VLAN (Server→DTN, used in server TX)
        // These VLANs should also be in the lookup (can be used in TX worker)
        if (dtn_port_map[i].rx_vlan < DTN_VLAN_LOOKUP_SIZE) {
            // We could also put rx_vlan in a separate lookup, tx_vlan is sufficient for now
            // because PRBS check is done on server RX = via tx_vlan
        }
    }

    printf("\n=== DTN Port Mapping Initialized ===\n");
    printf("DTN Ports: %d (DPDK: %d, Raw: 2)\n", DTN_PORT_COUNT, DTN_DPDK_PORT_COUNT);
    printf("VLAN → DTN Port lookup table built\n");

    // Print summary table
    printf("\n  DTN Port | DTN RX (Srv TX)      | DTN TX (Srv RX)\n");
    printf("  ---------+----------------------+----------------------\n");
    for (int i = 0; i < DTN_DPDK_PORT_COUNT; i++) {
        printf("    %2d     | SrvPort%u Q%u VLAN%-3u | SrvPort%u Q%u VLAN%-3u\n",
               dtn_port_map[i].dtn_port_id,
               dtn_port_map[i].rx_server_port, dtn_port_map[i].rx_server_queue,
               dtn_port_map[i].rx_vlan,
               dtn_port_map[i].tx_server_port, dtn_port_map[i].tx_server_queue,
               dtn_port_map[i].tx_vlan);
    }
    printf("    32     | Port 12 (1G)         | Port 12 (1G)\n");
    printf("    33     | Port 13 (100M)       | Port 13 (100M)\n");
    printf("================================================\n\n");
}

void init_dtn_stats(void)
{
    for (int i = 0; i < DTN_PORT_COUNT; i++) {
        rte_atomic64_init(&dtn_stats[i].good_pkts);
        rte_atomic64_init(&dtn_stats[i].bad_pkts);
        rte_atomic64_init(&dtn_stats[i].bit_errors);
        rte_atomic64_init(&dtn_stats[i].lost_pkts);
        rte_atomic64_init(&dtn_stats[i].out_of_order_pkts);
        rte_atomic64_init(&dtn_stats[i].duplicate_pkts);
        rte_atomic64_init(&dtn_stats[i].short_pkts);
        rte_atomic64_init(&dtn_stats[i].total_rx_pkts);
    }
    printf("DTN port statistics initialized for %d ports\n", DTN_PORT_COUNT);
}

// ==========================================
// DTN VLAN-BASED FLOW STEERING
// ==========================================
// Steers each RX VLAN to its corresponding queue (1:1 mapping)
// This way HW per-queue stats = per-VLAN = per-DTN port stats

int dtn_flow_rules_install(uint16_t port_id)
{
    struct rte_flow_error error;
    int installed = 0;

    // How many RX VLANs does this port have?
    uint16_t rx_vlan_count = port_vlans[port_id].rx_vlan_count;
    if (rx_vlan_count == 0 || rx_vlan_count > DTN_MAX_FLOW_RULES_PER_PORT) {
        printf("DTN Flow: Port %u has %u RX VLANs, skipping\n", port_id, rx_vlan_count);
        return 0;
    }

    printf("DTN Flow: Installing VLAN→Queue rules on Port %u (%u VLANs)...\n",
           port_id, rx_vlan_count);

    for (uint16_t q = 0; q < rx_vlan_count; q++) {
        uint16_t vlan_id = port_vlans[port_id].rx_vlans[q];

        struct rte_flow_attr attr;
        struct rte_flow_item pattern[4];
        struct rte_flow_action action[2];

        memset(&attr, 0, sizeof(attr));
        memset(pattern, 0, sizeof(pattern));
        memset(action, 0, sizeof(action));

        // Flow attributes
        attr.ingress = 1;
        attr.priority = 1;  // Lower priority than PTP (PTP priority=0)

        // Action: Steer to queue
        struct rte_flow_action_queue queue_action;
        queue_action.index = q;

        action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
        action[0].conf = &queue_action;
        action[1].type = RTE_FLOW_ACTION_TYPE_END;

        // Pattern: ETH (any) → VLAN (tci match)
        // ETH layer
        pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
        pattern[0].spec = NULL;
        pattern[0].mask = NULL;

        // VLAN layer: match on VLAN ID
        struct rte_flow_item_vlan vlan_spec;
        struct rte_flow_item_vlan vlan_mask;
        memset(&vlan_spec, 0, sizeof(vlan_spec));
        memset(&vlan_mask, 0, sizeof(vlan_mask));

        // TCI = (priority << 13) | (dei << 12) | vlan_id
        // Match only by VLAN ID (lower 12 bits)
        vlan_spec.tci = rte_cpu_to_be_16(vlan_id);
        vlan_mask.tci = rte_cpu_to_be_16(0x0FFF);  // VLAN ID field only

        pattern[1].type = RTE_FLOW_ITEM_TYPE_VLAN;
        pattern[1].spec = &vlan_spec;
        pattern[1].mask = &vlan_mask;

        pattern[2].type = RTE_FLOW_ITEM_TYPE_END;

        // Validate + Create
        int ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
        if (ret != 0) {
            printf("DTN Flow: Port %u VLAN %u validate failed: %s\n",
                   port_id, vlan_id,
                   error.message ? error.message : "unknown");
            continue;
        }

        struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, action, &error);
        if (!flow) {
            printf("DTN Flow: Port %u VLAN %u create failed: %s\n",
                   port_id, vlan_id,
                   error.message ? error.message : "unknown");
            continue;
        }

        dtn_flow_handles[port_id][q] = flow;
        installed++;
        printf("  VLAN %u → Queue %u\n", vlan_id, q);
    }

    printf("DTN Flow: Port %u: %d/%u rules installed\n", port_id, installed, rx_vlan_count);
    return (installed == rx_vlan_count) ? 0 : -1;
}

void dtn_flow_rules_remove(uint16_t port_id)
{
    if (port_id >= MAX_PORTS) return;

    for (int q = 0; q < DTN_MAX_FLOW_RULES_PER_PORT; q++) {
        if (dtn_flow_handles[port_id][q]) {
            struct rte_flow_error error;
            rte_flow_destroy(port_id, dtn_flow_handles[port_id][q], &error);
            dtn_flow_handles[port_id][q] = NULL;
        }
    }
}
#endif /* STATS_MODE_DTN */

// ==========================================
// VLAN CONFIGURATION FUNCTIONS
// ==========================================

uint16_t get_tx_vlan_for_queue(uint16_t port_id, uint16_t queue_id)
{
    if (port_id >= MAX_PORTS_CONFIG)
    {
        printf("Error: Invalid port_id %u for TX VLAN lookup\n", port_id);
        return 100;
    }

    if (queue_id >= port_vlans[port_id].tx_vlan_count)
    {
        printf("Warning: TX Queue %u exceeds VLAN count (%u) for port %u, using modulo\n",
               queue_id, port_vlans[port_id].tx_vlan_count, port_id);
        queue_id = queue_id % port_vlans[port_id].tx_vlan_count;
    }

    return port_vlans[port_id].tx_vlans[queue_id];
}

uint16_t get_rx_vlan_for_queue(uint16_t port_id, uint16_t queue_id)
{
    if (port_id >= MAX_PORTS_CONFIG)
    {
        printf("Error: Invalid port_id %u for RX VLAN lookup\n", port_id);
        return 100;
    }

    if (queue_id >= port_vlans[port_id].rx_vlan_count)
    {
        printf("Warning: RX Queue %u exceeds VLAN count (%u) for port %u, using modulo\n",
               queue_id, port_vlans[port_id].rx_vlan_count, port_id);
        queue_id = queue_id % port_vlans[port_id].rx_vlan_count;
    }

    return port_vlans[port_id].rx_vlans[queue_id];
}

uint16_t get_tx_vl_id_for_queue(uint16_t port_id, uint16_t queue_id)
{
    if (port_id >= MAX_PORTS_CONFIG)
    {
        printf("Error: Invalid port_id %u for TX VL ID lookup\n", port_id);
        return 0;
    }

    if (queue_id >= port_vlans[port_id].tx_vlan_count)
    {
        printf("Warning: TX Queue %u exceeds VL ID count for port %u, using modulo\n",
               queue_id, port_id);
        queue_id = queue_id % port_vlans[port_id].tx_vlan_count;
    }

    return port_vlans[port_id].tx_vl_ids[queue_id];
}

uint16_t get_rx_vl_id_for_queue(uint16_t port_id, uint16_t queue_id)
{
    if (port_id >= MAX_PORTS_CONFIG)
    {
        printf("Error: Invalid port_id %u for RX VL ID lookup\n", port_id);
        return 0;
    }

    if (queue_id >= port_vlans[port_id].rx_vlan_count)
    {
        printf("Warning: RX Queue %u exceeds VL ID count for port %u, using modulo\n",
               queue_id, port_id);
        queue_id = queue_id % port_vlans[port_id].rx_vlan_count;
    }

    return port_vlans[port_id].rx_vl_ids[queue_id];
}

void print_vlan_config(void)
{
    printf("\n=== Port VLAN & VL ID Configuration (PORT-AWARE) ===\n");
    printf("tx_vl_ids and rx_vl_ids are read from config for each port.\n");
    printf("Each queue has a VL-ID range size of %u.\n\n", VL_RANGE_SIZE_PER_QUEUE);

    for (uint16_t port = 0; port < MAX_PORTS_CONFIG; port++)
    {
        if (port_vlans[port].tx_vlan_count == 0 && port_vlans[port].rx_vlan_count == 0)
        {
            continue;
        }

        printf("Port %u:\n", port);

        printf("  TX VLANs (%u): [", port_vlans[port].tx_vlan_count);
        for (uint16_t i = 0; i < port_vlans[port].tx_vlan_count; i++)
        {
            printf("%u", port_vlans[port].tx_vlans[i]);
            if (i < port_vlans[port].tx_vlan_count - 1)
                printf(", ");
        }
        printf("]\n");

        // TX VL-ID ranges (PORT-AWARE)
        printf("  TX VL-ID Ranges:\n");
        for (uint16_t i = 0; i < port_vlans[port].tx_vlan_count; i++)
        {
            uint16_t vl_start = get_tx_vl_id_range_start(port, i);
            uint16_t vl_end = get_tx_vl_id_range_end(port, i);
            printf("    Queue %u -> [%u, %u) (%u VL-IDs)\n",
                   i, vl_start, vl_end, VL_RANGE_SIZE_PER_QUEUE);
        }

        printf("  RX VLANs (%u): [", port_vlans[port].rx_vlan_count);
        for (uint16_t i = 0; i < port_vlans[port].rx_vlan_count; i++)
        {
            printf("%u", port_vlans[port].rx_vlans[i]);
            if (i < port_vlans[port].rx_vlan_count - 1)
                printf(", ");
        }
        printf("]\n");

        // RX VL-ID ranges (PORT-AWARE)
        printf("  RX VL-ID Ranges:\n");
        for (uint16_t i = 0; i < port_vlans[port].rx_vlan_count; i++)
        {
            uint16_t vl_start = get_rx_vl_id_range_start(port, i);
            uint16_t vl_end = get_rx_vl_id_range_end(port, i);
            printf("    Queue %u -> [%u, %u) (%u VL-IDs)\n",
                   i, vl_start, vl_end, VL_RANGE_SIZE_PER_QUEUE);
        }
    }
    printf("\n");
}

// ==========================================
// PORT SETUP FUNCTIONS
// ==========================================

struct rte_mempool *create_mbuf_pool(uint16_t socket_id, uint16_t port_id)
{
    char pool_name[32];
    snprintf(pool_name, sizeof(pool_name), "mbuf_pool_%u_%u", socket_id, port_id);

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create(
        pool_name,
        NUM_MBUFS,
        MBUF_CACHE_SIZE,
        0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        socket_id);

    if (mbuf_pool == NULL)
    {
        printf("Error: Cannot create mbuf pool for socket %u, port %u\n",
               socket_id, port_id);
        return NULL;
    }

    printf("Created mbuf pool '%s' on socket %u\n", pool_name, socket_id);
    return mbuf_pool;
}

int setup_tx_queue(uint16_t port_id, uint16_t queue_id, uint16_t socket_id)
{
    struct rte_eth_txconf txconf;
    struct rte_eth_dev_info dev_info;

    int ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0)
    {
        printf("Error getting device info for port %u\n", port_id);
        return ret;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = 0;

    ret = rte_eth_tx_queue_setup(
        port_id,
        queue_id,
        TX_RING_SIZE,
        socket_id,
        &txconf);

    if (ret < 0)
    {
        printf("Error setting up TX queue %u on port %u\n", queue_id, port_id);
        return ret;
    }

    printf("Setup TX queue %u on port %u (socket %u)\n",
           queue_id, port_id, socket_id);
    return 0;
}

int setup_rx_queue(uint16_t port_id, uint16_t queue_id, uint16_t socket_id,
                   struct rte_mempool *mbuf_pool)
{
    struct rte_eth_rxconf rxconf;
    struct rte_eth_dev_info dev_info;

    int ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0)
    {
        printf("Error getting device info for port %u\n", port_id);
        return ret;
    }

    rxconf = dev_info.default_rxconf;
    rxconf.offloads = 0;
    rxconf.rx_thresh.pthresh = 8;
    rxconf.rx_thresh.hthresh = 8;
    rxconf.rx_thresh.wthresh = 0;
    rxconf.rx_free_thresh = 32;
    rxconf.rx_drop_en = 0;

    ret = rte_eth_rx_queue_setup(
        port_id,
        queue_id,
        RX_RING_SIZE,
        socket_id,
        &rxconf,
        mbuf_pool);

    if (ret < 0)
    {
        printf("Error setting up RX queue %u on port %u\n", queue_id, port_id);
        return ret;
    }

    printf("Setup RX queue %u on port %u (socket %u, ring=%u)\n",
           queue_id, port_id, socket_id, RX_RING_SIZE);
    return 0;
}

int init_port_txrx(uint16_t port_id, struct txrx_config *config)
{
    struct rte_eth_conf port_conf;
    struct rte_eth_dev_info dev_info;
    int ret;

    memset(&port_conf, 0, sizeof(port_conf));

    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0)
    {
        printf("Error getting device info for port %u\n", port_id);
        return ret;
    }

#if STATS_MODE_DTN
    // DTN mode: RSS disabled, rte_flow VLAN steering will be used (after port start)
    if (config->nb_rx_queues > 1)
    {
        // We still enable RSS because fallback is needed for packets not matching rte_flow
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        uint64_t rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP;
        rss_hf &= dev_info.flow_type_rss_offloads;
        port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
        port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
        printf("Port %u DTN mode: RSS as fallback, VLAN flow steering will be installed\n", port_id);
    }
    else
    {
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    }
#else
    // Legacy mode: RSS hash-based distribution
    if (config->nb_rx_queues > 1)
    {
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        uint64_t rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP;
        rss_hf &= dev_info.flow_type_rss_offloads;
        port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
        port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
        printf("Port %u RSS capabilities: 0x%lx, using: 0x%lx\n",
               port_id, dev_info.flow_type_rss_offloads, rss_hf);
    }
    else
    {
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    }
#endif

    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    ret = rte_eth_dev_configure(
        port_id,
        config->nb_rx_queues,
        config->nb_tx_queues,
        &port_conf);

    if (ret < 0)
    {
        printf("Error configuring port %u: %d\n", port_id, ret);
        return ret;
    }

    printf("Configured port %u with %u TX queues and %u RX queues (RSS: %s)\n",
           port_id, config->nb_tx_queues, config->nb_rx_queues,
           config->nb_rx_queues > 1 ? "Enabled" : "Disabled");

    int socket_id = rte_eth_dev_socket_id(port_id);
    if (socket_id < 0)
    {
        socket_id = 0;
    }

    for (uint16_t q = 0; q < config->nb_tx_queues; q++)
    {
        ret = setup_tx_queue(port_id, q, socket_id);
        if (ret < 0)
        {
            return ret;
        }
    }

    for (uint16_t q = 0; q < config->nb_rx_queues; q++)
    {
        ret = setup_rx_queue(port_id, q, socket_id, config->mbuf_pool);
        if (ret < 0)
        {
            return ret;
        }
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0)
    {
        printf("Error starting port %u\n", port_id);
        return ret;
    }

#if STATS_MODE_DTN
    // DTN mode: Set up VLAN-based flow steering (after port start)
    if (config->nb_rx_queues > 1)
    {
        printf("Port %u: Installing DTN VLAN→Queue flow rules...\n", port_id);
        int flow_ret = dtn_flow_rules_install(port_id);
        if (flow_ret != 0) {
            printf("Warning: DTN flow rules incomplete on port %u, falling back to RSS\n", port_id);
        }

        // Still configure RETA (fallback in case of flow rule miss)
        struct rte_eth_rss_reta_entry64 reta_conf[16];
        memset(reta_conf, 0, sizeof(reta_conf));
        uint16_t reta_size = dev_info.reta_size;
        if (reta_size > 0) {
            const uint16_t num_data_rx_queues = NUM_RX_CORES;
            for (uint16_t i = 0; i < reta_size; i++) {
                uint16_t idx = i / RTE_ETH_RETA_GROUP_SIZE;
                uint16_t shift = i % RTE_ETH_RETA_GROUP_SIZE;
                if (i % RTE_ETH_RETA_GROUP_SIZE == 0)
                    reta_conf[idx].mask = ~0ULL;
                reta_conf[idx].reta[shift] = i % num_data_rx_queues;
            }
            rte_eth_dev_rss_reta_update(port_id, reta_conf, reta_size);
        }
    }
#else
    // Legacy mode: RSS RETA config
    if (config->nb_rx_queues > 1)
    {
        struct rte_eth_rss_reta_entry64 reta_conf[16];
        memset(reta_conf, 0, sizeof(reta_conf));

        uint16_t reta_size = dev_info.reta_size;
        if (reta_size > 0)
        {
            const uint16_t num_data_rx_queues = NUM_RX_CORES;
            printf("Port %u: Configuring RETA (size: %u) for %u data RX queues (total RX queues: %u)\n",
                   port_id, reta_size, num_data_rx_queues, config->nb_rx_queues);

            for (uint16_t i = 0; i < reta_size; i++)
            {
                uint16_t idx = i / RTE_ETH_RETA_GROUP_SIZE;
                uint16_t shift = i % RTE_ETH_RETA_GROUP_SIZE;

                if (i % RTE_ETH_RETA_GROUP_SIZE == 0)
                {
                    reta_conf[idx].mask = ~0ULL;
                }

                reta_conf[idx].reta[shift] = i % num_data_rx_queues;
            }

            ret = rte_eth_dev_rss_reta_update(port_id, reta_conf, reta_size);
            if (ret != 0)
            {
                printf("Warning: Failed to update RETA for port %u: %d\n", port_id, ret);
            }
            else
            {
                printf("Port %u: RETA configured successfully\n", port_id);
            }
        }
    }
#endif

    ret = rte_eth_promiscuous_enable(port_id);
    if (ret != 0)
    {
        printf("Warning: Cannot enable promiscuous mode for port %u\n", port_id);
    }

    // Enable allmulticast mode for PTP multicast reception
    ret = rte_eth_allmulticast_enable(port_id);
    if (ret != 0)
    {
        printf("Warning: Cannot enable allmulticast mode for port %u\n", port_id);
    }
    else
    {
        printf("Port %u: Allmulticast mode enabled\n", port_id);
    }

    printf("Port %u started successfully\n", port_id);
    return 0;
}

void print_port_stats(struct ports_config *ports_config)
{
    struct rte_eth_stats stats;

    printf("\n=== Port Hardware Statistics ===\n");

    for (uint16_t i = 0; i < ports_config->nb_ports; i++)
    {
        uint16_t port_id = ports_config->ports[i].port_id;

        int ret = rte_eth_stats_get(port_id, &stats);
        if (ret != 0)
        {
            printf("Port %u: Failed to get stats\n", port_id);
            continue;
        }

        printf("\nPort %u:\n", port_id);
        printf("  RX Packets: %lu\n", stats.ipackets);
        printf("  TX Packets: %lu\n", stats.opackets);
        printf("  RX Bytes:   %lu\n", stats.ibytes);
        printf("  TX Bytes:   %lu\n", stats.obytes);
        printf("  RX Errors:  %lu\n", stats.ierrors);
        printf("  TX Errors:  %lu\n", stats.oerrors);
        printf("  RX Missed:  %lu\n", stats.imissed);

        printf("  Per-Queue RX:\n");
        // Show all queues including PTP queue (Q5)
        for (uint16_t q = 0; q < RTE_ETHDEV_QUEUE_STAT_CNTRS && q <= 5; q++)
        {
            if (stats.q_ipackets[q] > 0 || q == 5)  // Always show Q5 (PTP)
            {
                printf("    Queue %u: %lu packets, %lu bytes%s\n",
                       q, stats.q_ipackets[q], stats.q_ibytes[q],
                       (q == 5) ? " [PTP]" : "");
            }
        }

        printf("  Per-Queue TX:\n");
        for (uint16_t q = 0; q < RTE_ETHDEV_QUEUE_STAT_CNTRS && q < NUM_TX_CORES; q++)
        {
            if (stats.q_opackets[q] > 0)
            {
                printf("    Queue %u: %lu packets, %lu bytes\n",
                       q, stats.q_opackets[q], stats.q_obytes[q]);
            }
        }
    }
    printf("\n");
}
// ==========================================
// TX WORKER - VL-ID BASED SEQUENCE
// ==========================================

// TEST MODE: Skip 1 packet every 100 packets, stop after 100000 TX per port
#define TX_TEST_MODE_ENABLED 0
#define TX_SKIP_EVERY_N_PACKETS 1000000     // Every 100th packet will be skipped (100, 200, 300...)
#define TX_MAX_PACKETS_PER_PORT 100000 // Maximum TX count per port
#define TX_WAIT_FOR_RX_FLUSH_MS 5000   // Wait time for RX counters to update (ms)

// Per-port TX packet counter (thread-safe)
static rte_atomic64_t tx_packet_count_per_port[MAX_PORTS];

// Flag to ensure only one worker triggers the shutdown sequence
static volatile int tx_shutdown_triggered = 0;

// Initialize TX test counters
static void init_tx_test_counters(void)
{
    for (int i = 0; i < MAX_PORTS; i++)
    {
        rte_atomic64_init(&tx_packet_count_per_port[i]);
    }
    tx_shutdown_triggered = 0;
    printf("TX Test Mode: ENABLED - Skip every %d packets, stop at %d packets per port\n",
           TX_SKIP_EVERY_N_PACKETS, TX_MAX_PACKETS_PER_PORT);
    printf("TX Test Mode: Will wait %d ms for RX counters to flush before stopping\n",
           TX_WAIT_FOR_RX_FLUSH_MS);
}

int tx_worker(void *arg)
{
    struct tx_worker_params *params = (struct tx_worker_params *)arg;
    struct rte_mbuf *pkt;  // Single packet mode (instead of burst)
    bool first_pkt_sent = false;

#if VLAN_ENABLED
    const uint16_t l2_len = sizeof(struct rte_ether_hdr) + sizeof(struct vlan_hdr);
#else
    const uint16_t l2_len = sizeof(struct rte_ether_hdr);
#endif

    if (params->port_id >= MAX_PRBS_CACHE_PORTS)
    {
        printf("Error: Invalid port_id %u in TX worker\n", params->port_id);
        return -1;
    }

    if (!port_prbs_cache[params->port_id].initialized)
    {
        printf("Error: PRBS cache not initialized for port %u\n", params->port_id);
        return -1;
    }

    // PORT-AWARE: Use tx_vl_ids values from config for each port
    const uint16_t vl_start = get_tx_vl_id_range_start(params->port_id, params->queue_id);
#if TOKEN_BUCKET_TX_ENABLED
    const uint16_t vl_range_size = GET_TB_VL_RANGE_SIZE(params->port_id);
    const uint16_t vl_end = vl_start + vl_range_size;
#else
    const uint16_t vl_end = get_tx_vl_id_range_end(params->port_id, params->queue_id);
    const uint16_t vl_range_size = get_vl_id_range_size(); // Always 128
#endif

#if IMIX_ENABLED
    // IMIX: Worker-specific offset for pattern rotation (hybrid shuffle)
    const uint8_t imix_offset = (uint8_t)((params->port_id * 4 + params->queue_id) % IMIX_PATTERN_SIZE);
    uint64_t imix_counter = 0;  // Packet counter (for IMIX pattern)
#endif

    // ==========================================
    // PACING SETUP
    // ==========================================
    uint64_t tsc_hz = rte_get_tsc_hz();

#if TOKEN_BUCKET_TX_ENABLED
    // TOKEN BUCKET: Each VL-IDX sends TB_PACKETS_PER_VL_PER_WINDOW packets per TB_WINDOW_MS
    // Rate = vl_range_size × TB_PACKETS_PER_VL_PER_WINDOW × (1000 / TB_WINDOW_MS) pkt/s
    uint64_t packets_per_sec = (uint64_t)((double)vl_range_size * TB_PACKETS_PER_VL_PER_WINDOW * 1000.0 / TB_WINDOW_MS);
    uint64_t delay_cycles = tsc_hz / packets_per_sec;
#else
    // Rate calculation: limiter.tokens_per_sec is already bytes/sec
    // IMIX: Use average packet size to calculate packets_per_sec
#if IMIX_ENABLED
    const uint64_t avg_bytes_per_packet = IMIX_AVG_PACKET_SIZE;
#else
    const uint64_t avg_bytes_per_packet = PACKET_SIZE;
#endif
    uint64_t packets_per_sec = params->limiter.tokens_per_sec / avg_bytes_per_packet;
    uint64_t delay_cycles = (packets_per_sec > 0) ? (tsc_hz / packets_per_sec) : tsc_hz;
#endif

    // Inter-packet interval in microseconds
    double inter_packet_us = (double)delay_cycles * 1000000.0 / (double)tsc_hz;

    // Stagger: Each port/queue starts at a different time (soft-start burst prevention)
    uint32_t stagger_slot = (params->port_id * 4 + params->queue_id) % 16;
    uint64_t stagger_offset = stagger_slot * (tsc_hz / 200);  // 5ms per slot

#if TOKEN_BUCKET_TX_ENABLED
    // Global worker phase: Distribute all TX workers evenly within the packet period
    // When stagger offset is an exact multiple of delay_cycles, all workers fire in
    // the same phase (e.g., 5ms / 14.286us = 350.0 integer -> 32 packets at once!).
    // With this fix, each worker shifts to a 1/total_workers slice of the period.
    uint32_t total_workers = params->nb_ports * NUM_TX_CORES;
    uint32_t worker_idx = params->port_id * NUM_TX_CORES + params->queue_id;
    uint64_t global_phase = worker_idx * (delay_cycles / total_workers);
    uint64_t next_send_time = rte_get_tsc_cycles() + stagger_offset + global_phase;
#else
    uint64_t next_send_time = rte_get_tsc_cycles() + stagger_offset;
#endif

    printf("TX Worker started: Port %u, Queue %u, Lcore %u, VLAN %u, VL_RANGE [%u..%u)\n",
           params->port_id, params->queue_id, params->lcore_id, params->vlan_id, vl_start, vl_end);
#if TOKEN_BUCKET_TX_ENABLED
    printf("  *** TOKEN BUCKET MODE - %u VL-IDX, 1ms window ***\n", vl_range_size);
#elif IMIX_ENABLED
    printf("  *** IMIX MODE ENABLED - Variable packet sizes ***\n");
    printf("  -> IMIX pattern: 100, 200, 400, 800, 1200x3, 1518x3 (avg=%lu bytes)\n", avg_bytes_per_packet);
    printf("  -> Worker offset: %u (hybrid shuffle)\n", imix_offset);
#else
    printf("  *** SMOOTH PACING - Traffic spread over 1 second ***\n");
#endif
    printf("  -> Pacing: %.1f us/pkt (%.0f pkt/s), stagger=%ums\n",
           inter_packet_us, (double)packets_per_sec, (unsigned)(stagger_offset * 1000 / tsc_hz));
    printf("  VL-ID Based Sequence: Each VL-ID has independent sequence counter\n");
    printf("  Strategy: Round-robin through ALL VL-IDs in range (%u VL-IDs)\n", vl_range_size);

#if TX_TEST_MODE_ENABLED
    printf("  TEST MODE: Skipping every %d-th packet, max %d packets per port\n",
           TX_SKIP_EVERY_N_PACKETS, TX_MAX_PACKETS_PER_PORT);
#endif

    // Current VL-ID index (cycles through entire range)
    uint16_t current_vl_offset = 0; // 0 to (vl_range_size - 1)

    // Local packet counter for this worker
    uint64_t local_pkt_counter = 0;

    while (!(*params->stop_flag))
    {
#if TX_TEST_MODE_ENABLED
        // Check if port reached max packet limit
        uint64_t current_port_count = rte_atomic64_read(&tx_packet_count_per_port[params->port_id]);
        if (current_port_count >= TX_MAX_PACKETS_PER_PORT)
        {
            if (__sync_val_compare_and_swap(&tx_shutdown_triggered, 0, 1) == 0)
            {
                printf("\n========================================\n");
                printf("TX Worker Port %u Queue %u: Reached %lu packets limit\n",
                       params->port_id, params->queue_id, current_port_count);
                printf("Waiting %d ms for RX counters to flush...\n", TX_WAIT_FOR_RX_FLUSH_MS);
                printf("========================================\n");
                rte_delay_ms(TX_WAIT_FOR_RX_FLUSH_MS);
                printf("\n========================================\n");
                printf("RX flush wait complete. Stopping all workers...\n");
                printf("========================================\n");
                *((volatile bool *)params->stop_flag) = true;
            }
            break;
        }
#endif

        // ==========================================
        // SMOOTH PACING: Each packet is sent exactly on time
        // NO burst - traffic is evenly spread over 1 second
        // ==========================================
        uint64_t now = rte_get_tsc_cycles();

        // Wait until it's time (busy-wait for precision)
        while (now < next_send_time) {
            rte_pause();
            now = rte_get_tsc_cycles();
        }

#if TOKEN_BUCKET_TX_ENABLED
        // If we fall behind, do PHASE-PRESERVING SKIP (burst prevention)
        // Instead of next_send_time = now, advance by N * delay_cycles
        // This preserves the phase offset between workers
        if (next_send_time + delay_cycles < now) {
            uint64_t periods_behind = (now - next_send_time) / delay_cycles;
            next_send_time += periods_behind * delay_cycles;
        }
#else
        // If we fall behind, DO NOT CATCH UP (burst prevention)
        if (next_send_time + delay_cycles < now) {
            next_send_time = now;
        }
#endif
        next_send_time += delay_cycles;

        // Single packet allocation
        pkt = rte_pktmbuf_alloc(params->mbuf_pool);
        if (unlikely(pkt == NULL)) {
            continue;  // Timing preserved, just skip this slot
        }

#if TX_TEST_MODE_ENABLED
        uint64_t port_count = rte_atomic64_read(&tx_packet_count_per_port[params->port_id]);
        if (port_count >= TX_MAX_PACKETS_PER_PORT)
        {
            rte_pktmbuf_free(pkt);
            continue;
        }
        uint64_t pkt_num = rte_atomic64_add_return(&tx_packet_count_per_port[params->port_id], 1);
        local_pkt_counter++;

        uint16_t curr_vl = vl_start + current_vl_offset;

        if (pkt_num % TX_SKIP_EVERY_N_PACKETS == 0)
        {
            // Test mode: intentional skip — consume sequence to create gap
            uint64_t skip_seq = get_next_tx_sequence(params->port_id, curr_vl);
            printf("TX Worker Port %u: SKIPPING packet #%lu (VL %u, seq %lu)\n",
                   params->port_id, pkt_num, curr_vl, skip_seq);
            rte_pktmbuf_free(pkt);
            current_vl_offset++;
            if (current_vl_offset >= vl_range_size)
                current_vl_offset = 0;
            continue;
        }

        // Peek sequence WITHOUT incrementing — only commit after successful send
        uint64_t seq = peek_tx_sequence(params->port_id, curr_vl);
#else
        uint16_t curr_vl = vl_start + current_vl_offset;

        // Peek sequence WITHOUT incrementing — only commit after successful send
        uint64_t seq = peek_tx_sequence(params->port_id, curr_vl);
#endif

        // Build packet
        struct packet_config cfg = params->pkt_config;
        cfg.vl_id = curr_vl;
        cfg.dst_mac.addr_bytes[0] = 0x03;
        cfg.dst_mac.addr_bytes[1] = 0x00;
        cfg.dst_mac.addr_bytes[2] = 0x00;
        cfg.dst_mac.addr_bytes[3] = 0x00;
        cfg.dst_mac.addr_bytes[4] = (uint8_t)((curr_vl >> 8) & 0xFF);
        cfg.dst_mac.addr_bytes[5] = (uint8_t)(curr_vl & 0xFF);
        cfg.dst_ip = (uint32_t)((224U << 24) | (224U << 16) |
                                ((uint32_t)((curr_vl >> 8) & 0xFF) << 8) |
                                (uint32_t)(curr_vl & 0xFF));

#if IMIX_ENABLED
        // IMIX: Get packet size from pattern
        uint16_t pkt_size = get_imix_packet_size(imix_counter, imix_offset);
        uint16_t prbs_len = calc_prbs_size(pkt_size);
        imix_counter++;

        // Build dynamically-sized packet
        build_packet_dynamic(pkt, &cfg, pkt_size);
        fill_payload_with_prbs31_dynamic(pkt, params->port_id, seq, l2_len, prbs_len);
#else
        build_packet_mbuf(pkt, &cfg);
        fill_payload_with_prbs31(pkt, params->port_id, seq, l2_len);
#endif

        // Send single packet
        uint16_t nb_tx = rte_eth_tx_burst(params->port_id, params->queue_id, &pkt, 1);

        if (unlikely(!first_pkt_sent && nb_tx > 0))
        {
            printf("TX Worker: First packet sent on Port %u Queue %u\n",
                   params->port_id, params->queue_id);
            first_pkt_sent = true;
        }

        if (likely(nb_tx > 0))
        {
            // Increment sequence only after packet is successfully sent
            commit_tx_sequence(params->port_id, curr_vl);
        }
        else
        {
            // TX queue full — drop packet but do not increment sequence
            // The same sequence will be reused on the next attempt
            rte_pktmbuf_free(pkt);
        }

        current_vl_offset++;
        if (current_vl_offset >= vl_range_size)
            current_vl_offset = 0;
    }

#if TX_TEST_MODE_ENABLED
    printf("TX Worker stopped: Port %u, Queue %u (sent %lu packets locally, port total: %lu)\n",
           params->port_id, params->queue_id, local_pkt_counter,
           rte_atomic64_read(&tx_packet_count_per_port[params->port_id]));
#else
    printf("TX Worker stopped: Port %u, Queue %u\n", params->port_id, params->queue_id);
#endif
    return 0;
}

// ==========================================
// RX WORKER - VL-ID BASED SEQUENCE VALIDATION
// ==========================================

/**
 * Check if VL-ID is within valid TX range for source port (any queue)
 * This is used to detect external packets - if VL-ID doesn't match
 * what the source port would send, it's from an external source.
 */
static inline bool is_valid_tx_vl_id_for_source_port(uint16_t vl_id, uint16_t src_port_id)
{
    if (src_port_id >= MAX_PORTS_CONFIG)
        return false;

    // Check all TX queues for source port
    uint16_t queue_count = port_vlans[src_port_id].tx_vlan_count;
    for (uint16_t q = 0; q < queue_count; q++)
    {
        uint16_t start = port_vlans[src_port_id].tx_vl_ids[q];
        uint16_t end = start + VL_RANGE_SIZE_PER_QUEUE;
        if (vl_id >= start && vl_id < end)
            return true;
    }
    return false;
}

/**
 * Find raw socket port that sent this VL-ID (for external packet PRBS verification)
 * Returns pointer to raw_socket_port if found, NULL otherwise
 */
static inline struct raw_socket_port* find_raw_socket_port_by_vl_id(uint16_t vl_id)
{
    for (int p = 0; p < MAX_RAW_SOCKET_PORTS; p++)
    {
        struct raw_socket_port *port = &raw_ports[p];
        if (!port->prbs_initialized)
            continue;

        // Check all TX targets of this raw socket port
        for (int t = 0; t < port->tx_target_count; t++)
        {
            struct raw_tx_target_config *target = &port->config.tx_targets[t];
            if (vl_id >= target->vl_id_start &&
                vl_id < target->vl_id_start + target->vl_id_count)
            {
                return port;
            }
        }
    }
    return NULL;
}

int rx_worker(void *arg)
{
    struct rx_worker_params *params = (struct rx_worker_params *)arg;
    struct rte_mbuf *pkts[BURST_SIZE];
    bool first_packet_received = false;

    // L2 header lengths for dynamic detection
    const uint16_t l2_len_vlan = sizeof(struct rte_ether_hdr) + sizeof(struct vlan_hdr);  // 18
    const uint16_t l2_len_novlan = sizeof(struct rte_ether_hdr);                           // 14

    // Minimum packet lengths for each type
    const uint32_t min_len_vlan = l2_len_vlan + 20 + 8 + SEQ_BYTES + NUM_PRBS_BYTES;      // 1509
    const uint32_t min_len_novlan = l2_len_novlan + 20 + 8 + SEQ_BYTES + NUM_PRBS_BYTES;  // 1505

    if (params->src_port_id >= MAX_PRBS_CACHE_PORTS)
    {
        printf("Error: Invalid src_port_id %u\n", params->src_port_id);
        return -1;
    }

    if (!port_prbs_cache[params->src_port_id].initialized)
    {
        printf("Error: PRBS cache not initialized for source port %u\n", params->src_port_id);
        return -1;
    }

    uint8_t *prbs_cache_ext = port_prbs_cache[params->src_port_id].cache_ext;
    if (!prbs_cache_ext)
    {
        printf("Error: PRBS cache_ext is NULL\n");
        return -1;
    }

    printf("RX Worker: Port %u, Queue %u, VLAN %u (VL-ID Based Sequence Validation)\n",
           params->port_id, params->queue_id, params->vlan_id);
    printf("  Source Port: %u (for PRBS verification)\n", params->src_port_id);
    printf("  Dynamic L2 detection: VLAN (0x8100)->%u bytes, Non-VLAN (0x0800)->%u bytes\n",
           l2_len_vlan, l2_len_novlan);

    uint64_t local_rx = 0, local_good = 0, local_bad = 0, local_bits = 0;
    uint64_t local_lost = 0, local_ooo = 0, local_dup = 0, local_short = 0;
    uint64_t local_external = 0;  // External packets (VL-ID outside expected range)
    uint64_t local_raw_rx = 0, local_raw_bytes = 0;  // Raw socket packet counters
    const uint32_t FLUSH = 131072;

#if STATS_MODE_DTN
    // In DTN mode: queue_id -> VLAN -> DTN port (1:1 mapping, flow steering active)
    const uint16_t rx_vlan_for_queue = port_vlans[params->port_id].rx_vlans[params->queue_id];
    const uint8_t my_dtn_port = (rx_vlan_for_queue < DTN_VLAN_LOOKUP_SIZE)
                                    ? vlan_to_dtn_port[rx_vlan_for_queue]
                                    : DTN_VLAN_INVALID;
#endif

    bool first_good = false, first_bad = false;
    bool first_raw_rx = false;  // Track first raw socket packet

    // Get VL-ID tracker for this port
    struct port_vl_tracker *vl_tracker = &port_vl_trackers[params->port_id];

    const uint16_t INNER_LOOPS = 8;

    while (!(*params->stop_flag))
    {
        for (int iter = 0; iter < INNER_LOOPS; iter++)
        {
            uint16_t nb_rx = rte_eth_rx_burst(params->port_id, params->queue_id,
                                              pkts, BURST_SIZE);

            if (unlikely(nb_rx == 0))
                continue;

            if (unlikely(!first_packet_received))
            {
                printf("RX: First packet on Port %u Queue %u\n", params->port_id, params->queue_id);
                first_packet_received = true;
            }

            local_rx += nb_rx;

            // Aggressive prefetch
            for (uint16_t i = 0; i + 7 < nb_rx; i++)
            {
                rte_prefetch0(rte_pktmbuf_mtod(pkts[i + 4], void *));
                rte_prefetch0(rte_pktmbuf_mtod(pkts[i + 7], void *));
            }

            // Process packets
            for (uint16_t i = 0; i < nb_rx; i++)
            {
                struct rte_mbuf *m = pkts[i];
                uint8_t *pkt = rte_pktmbuf_mtod(m, uint8_t *);

                // ==========================================
                // DYNAMIC PACKET TYPE DETECTION
                // EtherType at offset 12-13:
                //   0x8100 = VLAN tagged (from DPDK ports)
                //   0x0800 = IPv4 (from raw socket, no VLAN)
                // ==========================================
                uint16_t ether_type = ((uint16_t)pkt[12] << 8) | pkt[13];

                // DEBUG: Check if PTP packets (0x88F7) are arriving on wrong queue
                // PTP should go to Queue 5 via rte_flow, not here (Queue 0-3)
                if (ether_type == 0x88F7) {
                    // Untagged PTP packet on normal queue!
                    static uint64_t ptp_wrong_queue_count = 0;
                    if (ptp_wrong_queue_count++ < 10) {
                        printf("⚠ PTP PACKET on Port %u Q%u (should be Q5)! EtherType=0x%04X UNTAGGED\n",
                               params->port_id, params->queue_id, ether_type);
                    }
                } else if (ether_type == 0x8100) {
                    // VLAN-tagged - check inner EtherType
                    uint16_t inner_type = ((uint16_t)pkt[16] << 8) | pkt[17];
                    if (inner_type == 0x88F7) {
                        static uint64_t ptp_vlan_wrong_queue_count = 0;
                        if (ptp_vlan_wrong_queue_count++ < 10) {
                            uint16_t vlan_id = (((uint16_t)pkt[14] << 8) | pkt[15]) & 0x0FFF;
                            printf("⚠ PTP PACKET on Port %u Q%u (should be Q5)! VLAN=%u EtherType=0x%04X\n",
                                   params->port_id, params->queue_id, vlan_id, inner_type);
                        }
                    }
                }

                if (ether_type == 0x0800)
                {
                    // ==========================================
                    // NON-VLAN PACKET (from raw socket Port 12/13)
                    // PRBS validation using raw socket port's cache
                    // ==========================================
                    local_raw_rx++;
                    local_raw_bytes += m->pkt_len;

                    if (unlikely(!first_raw_rx))
                    {
                        printf("RX: First RAW SOCKET packet on Port %u Queue %u (len=%u)\n",
                               params->port_id, params->queue_id, m->pkt_len);
                        first_raw_rx = true;
                    }

                    // Minimum length check for raw socket packets
                    // IMIX minimum: ETH(14) + IP(20) + UDP(8) + SEQ(8) + MIN_PRBS = 100 bytes
#if IMIX_ENABLED
                    const uint32_t min_raw_pkt_len = IMIX_MIN_PACKET_SIZE - VLAN_HDR_SIZE;  // Raw socket has no VLAN
#else
                    const uint32_t min_raw_pkt_len = l2_len_novlan + 20 + 8 + RAW_PKT_SEQ_BYTES + RAW_PKT_PRBS_BYTES;
#endif
                    if (unlikely(m->pkt_len < min_raw_pkt_len))
                    {
                        local_short++;
                        continue;
                    }

                    // Payload offset for non-VLAN packets: ETH(14) + IP(20) + UDP(8) = 42
                    const uint32_t raw_payload_off = l2_len_novlan + 20 + 8;

                    // Extract VL-ID from DST MAC (offset 4-5, last 2 bytes of DST MAC)
                    // DST MAC format: 03:00:00:00:XX:XX where XX:XX = VL-ID
                    uint16_t raw_vl_id = ((uint16_t)pkt[4] << 8) | pkt[5];

                    // Find raw socket port that sent this packet
                    struct raw_socket_port *raw_port = find_raw_socket_port_by_vl_id(raw_vl_id);
                    if (raw_port != NULL && raw_port->prbs_cache_ext != NULL)
                    {
                        // Get sequence number from payload
                        uint64_t raw_seq = *(uint64_t *)(pkt + raw_payload_off);

#if IMIX_ENABLED
                        // IMIX: PRBS offset calculation ALWAYS uses MAX_PRBS_BYTES
                        // PRBS size is calculated from packet size
                        uint16_t raw_prbs_len = m->pkt_len - l2_len_novlan - 20 - 8 - RAW_PKT_SEQ_BYTES;
                        if (raw_prbs_len > MAX_PRBS_BYTES) raw_prbs_len = MAX_PRBS_BYTES;

                        uint64_t prbs_offset = (raw_seq * (uint64_t)MAX_PRBS_BYTES) % 268435456ULL;
                        uint8_t *expected_prbs = raw_port->prbs_cache_ext + prbs_offset;
                        uint8_t *recv_prbs = pkt + raw_payload_off + RAW_PKT_SEQ_BYTES;

                        // Compare PRBS data (dynamic size)
                        if (memcmp(recv_prbs, expected_prbs, raw_prbs_len) == 0)
                        {
                            local_good++;
                        }
                        else
                        {
                            local_bad++;
                            // Count bit errors
                            for (uint32_t b = 0; b < raw_prbs_len; b++)
                            {
                                local_bits += __builtin_popcount(recv_prbs[b] ^ expected_prbs[b]);
                            }
                        }
#else
                        // Calculate PRBS offset (same formula as raw_socket_port.c)
                        // RAW_PRBS_CACHE_SIZE = 268435456 (256MB)
                        uint64_t prbs_offset = (raw_seq * (uint64_t)RAW_PKT_PRBS_BYTES) % 268435456ULL;
                        uint8_t *expected_prbs = raw_port->prbs_cache_ext + prbs_offset;
                        uint8_t *recv_prbs = pkt + raw_payload_off + RAW_PKT_SEQ_BYTES;

                        // Compare PRBS data
                        if (memcmp(recv_prbs, expected_prbs, RAW_PKT_PRBS_BYTES) == 0)
                        {
                            local_good++;
                        }
                        else
                        {
                            local_bad++;
                            // Count bit errors
                            for (uint32_t b = 0; b < RAW_PKT_PRBS_BYTES; b++)
                            {
                                local_bits += __builtin_popcount(recv_prbs[b] ^ expected_prbs[b]);
                            }
                        }
#endif

                        // Sequence tracking for raw socket packets
                        if (raw_vl_id <= MAX_VL_ID)
                        {
                            struct vl_sequence_tracker *raw_seq_tracker = &vl_tracker->vl_trackers[raw_vl_id];

                            // Check if initialized
                            int was_init = __atomic_load_n(&raw_seq_tracker->initialized, __ATOMIC_ACQUIRE);
                            if (!was_init)
                            {
                                int expected_init = 0;
                                if (__atomic_compare_exchange_n(&raw_seq_tracker->initialized,
                                                                &expected_init, 1,
                                                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                                {
#if TOKEN_BUCKET_TX_ENABLED
                                    __atomic_store_n(&raw_seq_tracker->min_seq, raw_seq, __ATOMIC_RELEASE);
#endif
                                    __atomic_store_n(&raw_seq_tracker->expected_seq, raw_seq + 1, __ATOMIC_RELEASE);
                                }
                            }
                            else
                            {
#if TOKEN_BUCKET_TX_ENABLED
                                // Multi-queue NOTE: Since raw socket packets arrive without VLAN tags,
                                // the NIC RSS can distribute them across different RX queues. In that case,
                                // Q1 may process a higher seq before Q0 -> false positive gap.
                                // Therefore real-time gap detection is NOT USED.
                                // Loss detection is done watermark-based (max_seq+1 - pkt_count)
#else
                                // Real-time gap detection
                                uint64_t expected = __atomic_load_n(&raw_seq_tracker->expected_seq, __ATOMIC_ACQUIRE);
                                if (raw_seq > expected)
                                {
                                    local_lost += (raw_seq - expected);
                                }
                                if (raw_seq >= expected)
                                {
                                    __atomic_store_n(&raw_seq_tracker->expected_seq, raw_seq + 1, __ATOMIC_RELEASE);
                                }
#endif
                            }

                            // Update max_seq if this sequence is higher
                            uint64_t current_max;
                            do {
                                current_max = __atomic_load_n(&raw_seq_tracker->max_seq, __ATOMIC_ACQUIRE);
                                if (raw_seq <= current_max) {
                                    break;
                                }
                            } while (!__atomic_compare_exchange_n(&raw_seq_tracker->max_seq,
                                                                   &current_max, raw_seq,
                                                                   false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

                            // Increment packet count
                            __atomic_fetch_add(&raw_seq_tracker->pkt_count, 1, __ATOMIC_RELAXED);
                        }
                    }
                    continue;  // Done with raw socket packet
                }

                // ==========================================
                // VLAN PACKET (from DPDK ports) - process normally
                // ==========================================
#if IMIX_ENABLED
                // IMIX minimum: 100 bytes
                if (unlikely(m->pkt_len < IMIX_MIN_PACKET_SIZE))
#else
                if (unlikely(m->pkt_len < min_len_vlan))
#endif
                {
                    local_short++;
                    continue;
                }

                // Payload offset for VLAN packets
                const uint32_t payload_off = l2_len_vlan + 20 + 8;

                //  Extract VL-ID from DST MAC (last 2 bytes)
                uint16_t vl_id = extract_vl_id_from_packet(pkt, l2_len_vlan);

                // ==========================================
                // VL-ID RANGE CHECK - External packet detection
                // If VL-ID doesn't match what the source port (paired DPDK port)
                // would send, it's from an external source (1G/100M lines)
                // ==========================================
                if (!is_valid_tx_vl_id_for_source_port(vl_id, params->src_port_id))
                {
                    local_external++;

                    // Try to find the raw socket port that sent this packet
                    struct raw_socket_port *raw_port = find_raw_socket_port_by_vl_id(vl_id);
                    if (raw_port != NULL && raw_port->prbs_cache_ext != NULL)
                    {
                        // Get sequence number from payload
                        uint64_t ext_seq = *(uint64_t *)(pkt + payload_off);

#if IMIX_ENABLED
                        // IMIX: PRBS offset calculation ALWAYS uses MAX_PRBS_BYTES
                        // PRBS size is calculated from packet size
                        uint16_t ext_prbs_len = m->pkt_len - l2_len_vlan - 20 - 8 - SEQ_BYTES;
                        if (ext_prbs_len > MAX_PRBS_BYTES) ext_prbs_len = MAX_PRBS_BYTES;

                        uint64_t prbs_offset = (ext_seq * (uint64_t)MAX_PRBS_BYTES) % 268435456ULL;
                        uint8_t *expected_prbs = raw_port->prbs_cache_ext + prbs_offset;
                        uint8_t *recv_prbs = pkt + payload_off + SEQ_BYTES;

                        // Compare PRBS data (dynamic size)
                        if (memcmp(recv_prbs, expected_prbs, ext_prbs_len) == 0)
                        {
                            local_good++;
                        }
                        else
                        {
                            local_bad++;
                            // Count bit errors
                            for (uint32_t i = 0; i < ext_prbs_len; i++)
                            {
                                local_bits += __builtin_popcount(recv_prbs[i] ^ expected_prbs[i]);
                            }
                        }
#else
                        // Calculate PRBS offset (same formula as raw_socket_port.c)
                        // RAW_PRBS_CACHE_SIZE = 268435456 (256MB)
                        uint64_t prbs_offset = (ext_seq * (uint64_t)RAW_PKT_PRBS_BYTES) % 268435456ULL;
                        uint8_t *expected_prbs = raw_port->prbs_cache_ext + prbs_offset;
                        uint8_t *recv_prbs = pkt + payload_off + SEQ_BYTES;

                        // Compare PRBS data (use smaller size for comparison)
                        // RAW_PKT_PRBS_BYTES = 1459, NUM_PRBS_BYTES may differ
                        uint32_t cmp_len = RAW_PKT_PRBS_BYTES;
                        if (cmp_len > NUM_PRBS_BYTES) cmp_len = NUM_PRBS_BYTES;

                        if (memcmp(recv_prbs, expected_prbs, cmp_len) == 0)
                        {
                            local_good++;
                        }
                        else
                        {
                            local_bad++;
                            // Count bit errors
                            for (uint32_t i = 0; i < cmp_len; i++)
                            {
                                local_bits += __builtin_popcount(recv_prbs[i] ^ expected_prbs[i]);
                            }
                        }
#endif

                        // ==========================================
                        // SEQUENCE TRACKING FOR EXTERNAL PACKETS
                        // Real-time gap detection + watermark tracking
                        // ==========================================
                        if (vl_id <= MAX_VL_ID)
                        {
                            struct vl_sequence_tracker *ext_seq_tracker = &vl_tracker->vl_trackers[vl_id];

                            // Check if initialized
                            int was_init = __atomic_load_n(&ext_seq_tracker->initialized, __ATOMIC_ACQUIRE);
                            if (!was_init)
                            {
                                int expected_init = 0;
                                if (__atomic_compare_exchange_n(&ext_seq_tracker->initialized,
                                                                &expected_init, 1,
                                                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                                {
#if TOKEN_BUCKET_TX_ENABLED
                                    __atomic_store_n(&ext_seq_tracker->min_seq, ext_seq, __ATOMIC_RELEASE);
#endif
                                    __atomic_store_n(&ext_seq_tracker->expected_seq, ext_seq + 1, __ATOMIC_RELEASE);
                                }
                            }
                            else
                            {
#if TOKEN_BUCKET_TX_ENABLED
                                // Multi-queue NOTE: Since external/raw socket packets arrive without
                                // VLAN tags, NIC RSS can distribute them across different RX queues.
                                // Gap detection (expected_seq) gives false positives in multi-queue OOO.
                                // Loss detection is done watermark-based (max_seq+1 - pkt_count).
#else
                                // Real-time gap detection
                                uint64_t expected = __atomic_load_n(&ext_seq_tracker->expected_seq, __ATOMIC_ACQUIRE);
                                if (ext_seq > expected)
                                {
                                    local_lost += (ext_seq - expected);
                                }
                                if (ext_seq >= expected)
                                {
                                    __atomic_store_n(&ext_seq_tracker->expected_seq, ext_seq + 1, __ATOMIC_RELEASE);
                                }
#endif
                            }

                            // Update max_seq if this sequence is higher (CAS loop)
                            uint64_t current_max;
                            do {
                                current_max = __atomic_load_n(&ext_seq_tracker->max_seq, __ATOMIC_ACQUIRE);
                                if (ext_seq <= current_max) {
                                    break;  // Not higher, no update needed
                                }
                            } while (!__atomic_compare_exchange_n(&ext_seq_tracker->max_seq,
                                                                   &current_max, ext_seq,
                                                                   false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

                            // Increment packet count
                            __atomic_fetch_add(&ext_seq_tracker->pkt_count, 1, __ATOMIC_RELAXED);
                        }
                    }
                    // If raw_port not found, just count as external (no PRBS check)

                    continue;  // Skip internal PRBS validation
                }

                // Get sequence number from payload
                uint64_t seq = *(uint64_t *)(pkt + payload_off);

                // ==========================================
                // VL-ID BASED SEQUENCE TRACKING
                // Real-time gap detection + watermark tracking
                // ==========================================
                if (vl_id <= MAX_VL_ID)
                {
                    struct vl_sequence_tracker *seq_tracker = &vl_tracker->vl_trackers[vl_id];

                    // Check if initialized
                    int was_init = __atomic_load_n(&seq_tracker->initialized, __ATOMIC_ACQUIRE);
                    if (!was_init)
                    {
                        // First packet for this VL-ID
                        int expected_init = 0;
                        if (__atomic_compare_exchange_n(&seq_tracker->initialized,
                                                        &expected_init, 1,
                                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                        {
                            // We won the race - initialize expected_seq
#if TOKEN_BUCKET_TX_ENABLED
                            __atomic_store_n(&seq_tracker->min_seq, seq, __ATOMIC_RELEASE);
#endif
                            __atomic_store_n(&seq_tracker->expected_seq, seq + 1, __ATOMIC_RELEASE);
                        }
                    }
                    else
                    {
                        // Real-time gap detection
                        uint64_t expected = __atomic_load_n(&seq_tracker->expected_seq, __ATOMIC_ACQUIRE);
                        if (seq > expected)
                        {
                            // Gap detected - packets lost
                            local_lost += (seq - expected);
#if TOKEN_BUCKET_TX_ENABLED
                            printf("*** LOSS DETECTED [DPDK] Port %u Q%u: VL-ID=%u expected_seq=%lu got_seq=%lu gap=%lu (src_port=%u) ***\n",
                                   params->port_id, params->queue_id, vl_id, expected, seq, seq - expected, params->src_port_id);
#endif
                        }
                        // Update expected_seq (even if seq < expected, move forward)
                        if (seq >= expected)
                        {
                            __atomic_store_n(&seq_tracker->expected_seq, seq + 1, __ATOMIC_RELEASE);
                        }
                    }

                    // Update max_seq if this sequence is higher (CAS loop)
                    uint64_t current_max;
                    do {
                        current_max = __atomic_load_n(&seq_tracker->max_seq, __ATOMIC_ACQUIRE);
                        if (seq <= current_max) {
                            break;  // Not higher, no update needed
                        }
                    } while (!__atomic_compare_exchange_n(&seq_tracker->max_seq,
                                                           &current_max, seq,
                                                           false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

                    // Increment packet count
                    __atomic_fetch_add(&seq_tracker->pkt_count, 1, __ATOMIC_RELAXED);
                }

                // ==========================================
                // PRBS-31 VERIFICATION
                // ==========================================
                uint8_t *recv = pkt + payload_off + SEQ_BYTES;

#if IMIX_ENABLED
                // IMIX: PRBS offset calculation ALWAYS uses MAX_PRBS_BYTES
                // PRBS size is calculated from packet size
                uint16_t prbs_len = m->pkt_len - l2_len_vlan - 20 - 8 - SEQ_BYTES;
                if (prbs_len > MAX_PRBS_BYTES) prbs_len = MAX_PRBS_BYTES;

                uint64_t off = (seq * (uint64_t)MAX_PRBS_BYTES) % (uint64_t)PRBS_CACHE_SIZE;
                uint8_t *exp = prbs_cache_ext + off;

                int diff = memcmp(recv, exp, prbs_len);
#else
                uint64_t off = (seq * (uint64_t)NUM_PRBS_BYTES) % (uint64_t)PRBS_CACHE_SIZE;
                uint8_t *exp = prbs_cache_ext + off;

                int diff = memcmp(recv, exp, NUM_PRBS_BYTES);
#endif

                if (likely(diff == 0))
                {
                    local_good++;
                    if (unlikely(!first_good))
                    {
                        printf("✓ GOOD: Port %u Q%u VL-ID %u Seq %lu\n",
                               params->port_id, params->queue_id, vl_id, seq);
                        first_good = true;
                    }
                }
                else
                {
                    local_bad++;
                    if (unlikely(!first_bad))
                    {
                        printf("✗ BAD: Port %u Q%u VL-ID %u Seq %lu\n",
                               params->port_id, params->queue_id, vl_id, seq);
                        first_bad = true;
                    }

                    // Bit error counting
                    uint64_t berr = 0;
                    const uint64_t *r64 = (const uint64_t *)recv;
                    const uint64_t *e64 = (const uint64_t *)exp;
#if IMIX_ENABLED
                    const uint16_t nq = prbs_len / 8;
#else
                    const uint16_t nq = NUM_PRBS_BYTES / 8;
#endif

                    for (uint16_t k = 0; k < nq; k++)
                    {
                        berr += __builtin_popcountll(r64[k] ^ e64[k]);
                    }

#if IMIX_ENABLED
                    uint16_t rem = prbs_len & 7;
#else
                    uint16_t rem = NUM_PRBS_BYTES & 7;
#endif
                    if (rem)
                    {
                        const uint8_t *r8 = (const uint8_t *)(r64 + nq);
                        const uint8_t *e8 = (const uint8_t *)(e64 + nq);
                        for (uint16_t k = 0; k < rem; k++)
                        {
                            berr += __builtin_popcount(r8[k] ^ e8[k]);
                        }
                    }

                    local_bits += berr;
                }
            }

            // Batch free
            for (uint16_t i = 0; i < nb_rx; i++)
            {
                rte_pktmbuf_free(pkts[i]);
            }

            if (unlikely(local_rx >= FLUSH))
            {
                rte_atomic64_add(&rx_stats_per_port[params->port_id].total_rx_pkts, local_rx);
                rte_atomic64_add(&rx_stats_per_port[params->port_id].good_pkts, local_good);
                rte_atomic64_add(&rx_stats_per_port[params->port_id].bad_pkts, local_bad);
                rte_atomic64_add(&rx_stats_per_port[params->port_id].bit_errors, local_bits);
                rte_atomic64_add(&rx_stats_per_port[params->port_id].lost_pkts, local_lost);
                rte_atomic64_add(&rx_stats_per_port[params->port_id].out_of_order_pkts, local_ooo);
                rte_atomic64_add(&rx_stats_per_port[params->port_id].duplicate_pkts, local_dup);
                rte_atomic64_add(&rx_stats_per_port[params->port_id].short_pkts, local_short);
                rte_atomic64_add(&rx_stats_per_port[params->port_id].external_pkts, local_external);
                // Raw socket RX counters
                rte_atomic64_add(&rx_stats_per_port[params->port_id].raw_socket_rx_pkts, local_raw_rx);
                rte_atomic64_add(&rx_stats_per_port[params->port_id].raw_socket_rx_bytes, local_raw_bytes);

#if STATS_MODE_DTN
                // DTN per-port PRBS stats (queue = VLAN = DTN port)
                if (my_dtn_port != DTN_VLAN_INVALID) {
                    rte_atomic64_add(&dtn_stats[my_dtn_port].total_rx_pkts, local_rx);
                    rte_atomic64_add(&dtn_stats[my_dtn_port].good_pkts, local_good);
                    rte_atomic64_add(&dtn_stats[my_dtn_port].bad_pkts, local_bad);
                    rte_atomic64_add(&dtn_stats[my_dtn_port].bit_errors, local_bits);
                    rte_atomic64_add(&dtn_stats[my_dtn_port].lost_pkts, local_lost);
                    rte_atomic64_add(&dtn_stats[my_dtn_port].out_of_order_pkts, local_ooo);
                    rte_atomic64_add(&dtn_stats[my_dtn_port].duplicate_pkts, local_dup);
                    rte_atomic64_add(&dtn_stats[my_dtn_port].short_pkts, local_short);
                }
#endif
                local_rx = local_good = local_bad = local_bits = 0;
                local_lost = local_ooo = local_dup = local_short = local_external = 0;
                local_raw_rx = local_raw_bytes = 0;
            }
        }
    }

    // Final flush
    if (local_rx || local_raw_rx || local_external)
    {
        rte_atomic64_add(&rx_stats_per_port[params->port_id].total_rx_pkts, local_rx);
        rte_atomic64_add(&rx_stats_per_port[params->port_id].good_pkts, local_good);
        rte_atomic64_add(&rx_stats_per_port[params->port_id].bad_pkts, local_bad);
        rte_atomic64_add(&rx_stats_per_port[params->port_id].bit_errors, local_bits);
        rte_atomic64_add(&rx_stats_per_port[params->port_id].lost_pkts, local_lost);
        rte_atomic64_add(&rx_stats_per_port[params->port_id].out_of_order_pkts, local_ooo);
        rte_atomic64_add(&rx_stats_per_port[params->port_id].duplicate_pkts, local_dup);
        rte_atomic64_add(&rx_stats_per_port[params->port_id].short_pkts, local_short);
        rte_atomic64_add(&rx_stats_per_port[params->port_id].external_pkts, local_external);
        // Raw socket RX counters
        rte_atomic64_add(&rx_stats_per_port[params->port_id].raw_socket_rx_pkts, local_raw_rx);
        rte_atomic64_add(&rx_stats_per_port[params->port_id].raw_socket_rx_bytes, local_raw_bytes);

#if STATS_MODE_DTN
        if (my_dtn_port != DTN_VLAN_INVALID) {
            rte_atomic64_add(&dtn_stats[my_dtn_port].total_rx_pkts, local_rx);
            rte_atomic64_add(&dtn_stats[my_dtn_port].good_pkts, local_good);
            rte_atomic64_add(&dtn_stats[my_dtn_port].bad_pkts, local_bad);
            rte_atomic64_add(&dtn_stats[my_dtn_port].bit_errors, local_bits);
            rte_atomic64_add(&dtn_stats[my_dtn_port].lost_pkts, local_lost);
            rte_atomic64_add(&dtn_stats[my_dtn_port].out_of_order_pkts, local_ooo);
            rte_atomic64_add(&dtn_stats[my_dtn_port].duplicate_pkts, local_dup);
            rte_atomic64_add(&dtn_stats[my_dtn_port].short_pkts, local_short);
        }
#endif
    }

    // ==========================================
    // CALCULATE LOST PACKETS (watermark-based)
    // Lost = (max_seq + 1) - pkt_count for each VL-ID
    // ==========================================
#if STATS_MODE_DTN
    // DTN mode: Each queue calculates lost for its own VL-ID range
    // (With flow steering, each queue = 1 VLAN = 1 DTN port, ranges do not overlap)
    {
        uint64_t queue_lost = 0;
        uint16_t vl_start = get_rx_vl_id_range_start(params->port_id, params->queue_id);
        uint16_t vl_end = get_rx_vl_id_range_end(params->port_id, params->queue_id);

        for (uint16_t vl = vl_start; vl < vl_end && vl <= MAX_VL_ID; vl++)
        {
            struct vl_sequence_tracker *seq_tracker = &vl_tracker->vl_trackers[vl];
            if (__atomic_load_n(&seq_tracker->initialized, __ATOMIC_ACQUIRE))
            {
                uint64_t max_seq = __atomic_load_n(&seq_tracker->max_seq, __ATOMIC_ACQUIRE);
                uint64_t pkt_count = __atomic_load_n(&seq_tracker->pkt_count, __ATOMIC_ACQUIRE);

#if TOKEN_BUCKET_TX_ENABLED
                uint64_t min_seq = __atomic_load_n(&seq_tracker->min_seq, __ATOMIC_ACQUIRE);
                uint64_t expected_count = max_seq - min_seq + 1;
#else
                uint64_t expected_count = max_seq + 1;
#endif
                if (expected_count > pkt_count)
                {
                    queue_lost += (expected_count - pkt_count);
                }
            }
        }

        if (queue_lost > 0)
        {
            rte_atomic64_add(&rx_stats_per_port[params->port_id].lost_pkts, queue_lost);
            if (my_dtn_port != DTN_VLAN_INVALID) {
                rte_atomic64_add(&dtn_stats[my_dtn_port].lost_pkts, queue_lost);
            }
            printf("RX Worker Port %u Q%u (DTN %u): %lu lost packets (VL-ID %u-%u)\n",
                   params->port_id, params->queue_id,
                   my_dtn_port != DTN_VLAN_INVALID ? my_dtn_port : 0xFF,
                   queue_lost, vl_start, vl_end - 1);
        }
    }
#else
    // Legacy mode: Only queue 0 calculates for all VL-IDs (prevents double-counting)
    if (params->queue_id == 0)
    {
        uint64_t total_lost = 0;

        for (uint16_t vl = 0; vl <= MAX_VL_ID; vl++)
        {
            struct vl_sequence_tracker *seq_tracker = &vl_tracker->vl_trackers[vl];
            if (__atomic_load_n(&seq_tracker->initialized, __ATOMIC_ACQUIRE))
            {
                uint64_t max_seq = __atomic_load_n(&seq_tracker->max_seq, __ATOMIC_ACQUIRE);
                uint64_t pkt_count = __atomic_load_n(&seq_tracker->pkt_count, __ATOMIC_ACQUIRE);

#if TOKEN_BUCKET_TX_ENABLED
                uint64_t min_seq = __atomic_load_n(&seq_tracker->min_seq, __ATOMIC_ACQUIRE);
                uint64_t expected_count = max_seq - min_seq + 1;
#else
                uint64_t expected_count = max_seq + 1;
#endif
                if (expected_count > pkt_count)
                {
                    total_lost += (expected_count - pkt_count);
                }
            }
        }

        if (total_lost > 0)
        {
            rte_atomic64_add(&rx_stats_per_port[params->port_id].lost_pkts, total_lost);
            printf("RX Worker Port %u Q%u: Calculated %lu lost packets (watermark-based)\n",
                   params->port_id, params->queue_id, total_lost);
        }
    }
#endif

    printf("RX Worker stopped: Port %u Q%u\n", params->port_id, params->queue_id);
    return 0;
}

// ==========================================
// START TX/RX WORKERS
// ==========================================

int start_txrx_workers(struct ports_config *ports_config, volatile bool *stop_flag)
{
    printf("\n=== Starting TX/RX Workers with VL-ID Based Sequence Validation ===\n");
    printf("TX Cores per port: %d\n", NUM_TX_CORES);
    printf("RX Cores per port: %d\n", NUM_RX_CORES);
    printf("PRBS method: Sequence-based with ~268MB cache per port\n");
    printf("Sequence Method: ⭐ VL-ID BASED (Each VL-ID has independent sequence)\n");
    printf("\nPacket Format:\n");
    printf("  SRC MAC: 02:00:00:00:00:20 (fixed)\n");
    printf("  DST MAC: 03:00:00:00:XX:XX (last 2 bytes = VL-ID)\n");
    printf("  SRC IP:  10.0.0.0 (fixed)\n");
    printf("  DST IP:  224.224.XX.XX (last 2 bytes = VL-ID)\n");
    printf("  UDP Ports: 100 -> 100\n");
    printf("  TTL: 1\n");
    printf("  VLAN: Header tag (separate from VL ID)\n\n");

    //  CRITICAL: Initialize VL-ID based TX sequences
    printf("Initializing VL-ID based sequence counters...\n");
    init_tx_vl_sequences();

#if TX_TEST_MODE_ENABLED
    // Initialize TX test mode counters
    init_tx_test_counters();
#endif

    static struct tx_worker_params tx_params[MAX_PORTS * NUM_TX_CORES];
    static struct rx_worker_params rx_params[MAX_PORTS * NUM_RX_CORES];

    uint16_t tx_param_idx = 0;
    uint16_t rx_param_idx = 0;

    // ==========================================
    // PHASE 1: Start ALL RX workers first
    // ==========================================
    printf("\n=== Phase 1: Starting ALL RX Workers First ===\n");

    for (uint16_t port_idx = 0; port_idx < ports_config->nb_ports; port_idx++)
    {
        struct port *port = &ports_config->ports[port_idx];
        uint16_t port_id = port->port_id;
        uint16_t paired_port_id = (port_id % 2 == 0) ? (uint16_t)(port_id + 1) : (uint16_t)(port_id - 1);

        printf("\n--- Port %u RX (Receiving from Port %u) ---\n", port_id, paired_port_id);

        for (uint16_t q = 0; q < NUM_RX_CORES; q++)
        {
            uint16_t lcore_id = port->used_rx_cores[q];

            if (lcore_id == 0 || lcore_id >= RTE_MAX_LCORE)
            {
                printf("Warning: Invalid RX lcore %u for port %u queue %u\n",
                       lcore_id, port_id, q);
                continue;
            }

            uint16_t rx_vlan = get_rx_vlan_for_queue(port_id, q);
            uint16_t rx_vl_id = get_rx_vl_id_for_queue(port_id, q);

            rx_params[rx_param_idx].port_id = port_id;
            rx_params[rx_param_idx].src_port_id = paired_port_id;
            rx_params[rx_param_idx].queue_id = q;
            rx_params[rx_param_idx].lcore_id = lcore_id;
            rx_params[rx_param_idx].vlan_id = rx_vlan;
            rx_params[rx_param_idx].vl_id = rx_vl_id;
            rx_params[rx_param_idx].stop_flag = stop_flag;

            printf("  RX Queue %u -> Lcore %2u -> VLAN %u <- Port %u (VL-ID Based Seq Validation)\n",
                   q, lcore_id, rx_vlan, paired_port_id);

            int ret = rte_eal_remote_launch(rx_worker,
                                            &rx_params[rx_param_idx],
                                            lcore_id);
            if (ret != 0)
            {
                printf("Error launching RX worker on lcore %u: %d\n", lcore_id, ret);
                return ret;
            }
            else
            {
                printf("    ✓ RX Worker launched successfully\n");
            }

            rx_param_idx++;
        }
    }

    printf("\n>>> All RX workers started. Waiting 100ms for RX to be ready...\n");
    rte_delay_ms(100);

    // ==========================================
    // PHASE 2: Start ALL TX workers
    // ==========================================
    printf("\n=== Phase 2: Starting ALL TX Workers ===\n");

    for (uint16_t port_idx = 0; port_idx < ports_config->nb_ports; port_idx++)
    {
        struct port *port = &ports_config->ports[port_idx];
        uint16_t port_id = port->port_id;
        uint16_t paired_port_id = (port_id % 2 == 0) ? (uint16_t)(port_id + 1) : (uint16_t)(port_id - 1);

        printf("\n--- Port %u TX (Sending to Port %u) ---\n", port_id, paired_port_id);

        for (uint16_t q = 0; q < NUM_TX_CORES; q++)
        {
            uint16_t lcore_id = port->used_tx_cores[q];

            if (lcore_id == 0 || lcore_id >= RTE_MAX_LCORE)
            {
                printf("Warning: Invalid TX lcore %u for port %u queue %u\n",
                       lcore_id, port_id, q);
                continue;
            }

            double port_target_gbps = ate_mode_enabled() ? TARGET_GBPS_FAST : GET_PORT_TARGET_GBPS(port_id);
            init_rate_limiter(&tx_params[tx_param_idx].limiter, port_target_gbps, NUM_TX_CORES);

            uint16_t tx_vlan = get_tx_vlan_for_queue(port_id, q);

            tx_params[tx_param_idx].port_id = port_id;
            tx_params[tx_param_idx].dst_port_id = paired_port_id;
            tx_params[tx_param_idx].queue_id = q;
            tx_params[tx_param_idx].lcore_id = lcore_id;
            tx_params[tx_param_idx].vlan_id = tx_vlan;
            tx_params[tx_param_idx].stop_flag = stop_flag;
#if TOKEN_BUCKET_TX_ENABLED
            tx_params[tx_param_idx].nb_ports = ports_config->nb_ports;
#endif

            char pool_name[32];
            snprintf(pool_name, sizeof(pool_name), "mbuf_pool_%u_%u",
                     port->numa_node, port_id);
            tx_params[tx_param_idx].mbuf_pool = rte_mempool_lookup(pool_name);
            if (tx_params[tx_param_idx].mbuf_pool == NULL)
            {
                printf("Error: Cannot find mbuf pool for port %u\n", port_id);
                return -1;
            }

            init_packet_config(&tx_params[tx_param_idx].pkt_config);

#if VLAN_ENABLED
            tx_params[tx_param_idx].pkt_config.vlan_id = tx_vlan;
#endif
            tx_params[tx_param_idx].pkt_config.src_mac.addr_bytes[0] = 0x02;
            tx_params[tx_param_idx].pkt_config.src_mac.addr_bytes[1] = 0x00;
            tx_params[tx_param_idx].pkt_config.src_mac.addr_bytes[2] = 0x00;
            tx_params[tx_param_idx].pkt_config.src_mac.addr_bytes[3] = 0x00;
            tx_params[tx_param_idx].pkt_config.src_mac.addr_bytes[4] = 0x00;
            tx_params[tx_param_idx].pkt_config.src_mac.addr_bytes[5] = 0x20;

            tx_params[tx_param_idx].pkt_config.src_ip = (10U << 24);

            tx_params[tx_param_idx].pkt_config.src_port = DEFAULT_SRC_PORT;
            tx_params[tx_param_idx].pkt_config.dst_port = DEFAULT_DST_PORT;
            tx_params[tx_param_idx].pkt_config.ttl = DEFAULT_TTL;

            printf("  TX Queue %u -> Lcore %2u -> VLAN %u, VL RANGE [%u..%u) Rate: %.1f Gbps (%s)\n",
                   q, lcore_id, tx_vlan,
                   get_tx_vl_id_range_start(port_id, q), get_tx_vl_id_range_end(port_id, q),
                   port_target_gbps, IS_FAST_PORT(port_id) ? "FAST" : "SLOW");

            int ret = rte_eal_remote_launch(tx_worker,
                                            &tx_params[tx_param_idx],
                                            lcore_id);
            if (ret != 0)
            {
                printf("Error launching TX worker on lcore %u: %d\n", lcore_id, ret);
                return ret;
            }
            else
            {
                printf("    ✓ TX Worker launched successfully\n");
            }

            tx_param_idx++;
        }
    }

    // NOTE: DPDK External TX workers are started AFTER raw socket workers
    // in main.c to ensure Port 12 RX is ready before receiving packets.

    printf("\n=== All TX/RX workers started successfully ===\n");
    printf("Total RX workers: %u (started first)\n", rx_param_idx);
    printf("Total TX workers: %u (started after 100ms delay)\n", tx_param_idx);
    return 0;
}

// ==========================================
// LATENCY TEST IMPLEMENTATION
// ==========================================

#if LATENCY_TEST_ENABLED

// Global latency test state
struct latency_test_state g_latency_test;

/**
 * Reset latency test state
 */
void reset_latency_test(void)
{
    memset(&g_latency_test, 0, sizeof(g_latency_test));
    g_latency_test.tsc_hz = rte_get_tsc_hz();
    printf("Latency test state reset. TSC frequency: %lu Hz\n", g_latency_test.tsc_hz);
}

/**
 * Build latency test packet
 * Format: [ETH][VLAN][IP][UDP][SEQ 8B][TX_TIMESTAMP 8B][PRBS]
 */
static int build_latency_test_packet(struct rte_mbuf *mbuf,
                                      uint16_t port_id,
                                      uint16_t vlan_id,
                                      uint16_t vl_id,
                                      uint64_t sequence,
                                      uint64_t tx_timestamp)
{
    uint8_t *pkt = rte_pktmbuf_mtod(mbuf, uint8_t *);

    // ==========================================
    // ETHERNET HEADER
    // ==========================================
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt;

    // Source MAC: 02:00:00:00:00:PP (PP = port)
    eth->src_addr.addr_bytes[0] = 0x02;
    eth->src_addr.addr_bytes[1] = 0x00;
    eth->src_addr.addr_bytes[2] = 0x00;
    eth->src_addr.addr_bytes[3] = 0x00;
    eth->src_addr.addr_bytes[4] = 0x00;
    eth->src_addr.addr_bytes[5] = (uint8_t)port_id;

    // Destination MAC: 03:00:00:00:VV:VV (VV = VL-ID)
    eth->dst_addr.addr_bytes[0] = 0x03;
    eth->dst_addr.addr_bytes[1] = 0x00;
    eth->dst_addr.addr_bytes[2] = 0x00;
    eth->dst_addr.addr_bytes[3] = 0x00;
    eth->dst_addr.addr_bytes[4] = (uint8_t)(vl_id >> 8);
    eth->dst_addr.addr_bytes[5] = (uint8_t)(vl_id & 0xFF);

#if VLAN_ENABLED
    eth->ether_type = rte_cpu_to_be_16(0x8100);  // VLAN

    // VLAN header
    struct vlan_hdr *vlan = (struct vlan_hdr *)(pkt + sizeof(struct rte_ether_hdr));
    vlan->tci = rte_cpu_to_be_16(vlan_id);
    vlan->eth_proto = rte_cpu_to_be_16(0x0800);  // IPv4

    const uint16_t l2_len = sizeof(struct rte_ether_hdr) + sizeof(struct vlan_hdr);
#else
    eth->ether_type = rte_cpu_to_be_16(0x0800);  // IPv4
    const uint16_t l2_len = sizeof(struct rte_ether_hdr);
#endif

    // ==========================================
    // IP HEADER
    // ==========================================
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(pkt + l2_len);
    uint16_t payload_len = LATENCY_TEST_PACKET_SIZE - l2_len - sizeof(struct rte_ipv4_hdr) - sizeof(struct rte_udp_hdr);

    ip->version_ihl = 0x45;
    ip->type_of_service = 0;
    ip->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + payload_len);
    ip->packet_id = 0;
    ip->fragment_offset = 0;
    ip->time_to_live = 1;
    ip->next_proto_id = IPPROTO_UDP;
    ip->src_addr = rte_cpu_to_be_32(0x0A000000);  // 10.0.0.0
    ip->dst_addr = rte_cpu_to_be_32((224U << 24) | (224U << 16) |
                                    ((vl_id >> 8) << 8) | (vl_id & 0xFF));
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    // ==========================================
    // UDP HEADER
    // ==========================================
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(pkt + l2_len + sizeof(struct rte_ipv4_hdr));
    udp->src_port = rte_cpu_to_be_16(100);
    udp->dst_port = rte_cpu_to_be_16(100);
    udp->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_len);
    udp->dgram_cksum = 0;

    // ==========================================
    // PAYLOAD: [SEQ 8B][TX_TIMESTAMP 8B][PRBS]
    // ==========================================
    uint8_t *payload = pkt + l2_len + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);

    // Sequence number (8 bytes)
    *(uint64_t *)payload = sequence;

    // TX Timestamp (8 bytes)
    *(uint64_t *)(payload + SEQ_BYTES) = tx_timestamp;

    // PRBS data
    uint16_t prbs_len = payload_len - SEQ_BYTES - TX_TIMESTAMP_BYTES;
    uint8_t *prbs_cache = get_prbs_cache_ext_for_port(port_id);
    if (prbs_cache) {
        uint64_t prbs_offset = (sequence * (uint64_t)MAX_PRBS_BYTES) % PRBS_CACHE_SIZE;
        memcpy(payload + LATENCY_PAYLOAD_OFFSET, prbs_cache + prbs_offset, prbs_len);
    }

    // Set mbuf lengths
    mbuf->data_len = LATENCY_TEST_PACKET_SIZE;
    mbuf->pkt_len = LATENCY_TEST_PACKET_SIZE;

    return 0;
}

/**
 * Get the paired port for latency test
 * Port mapping for direct connection (no switch):
 *   Port 0 <-> Port 7
 *   Port 1 <-> Port 6
 *   Port 2 <-> Port 5
 *   Port 3 <-> Port 4
 */
static uint16_t get_latency_paired_port(uint16_t port_id)
{
    switch (port_id) {
        case 0: return 7;
        case 1: return 6;
        case 2: return 5;
        case 3: return 4;
        case 4: return 3;
        case 5: return 2;
        case 6: return 1;
        case 7: return 0;
        default: return port_id;  // Unknown port, return same
    }
}

/**
 * Latency test TX worker - sends 1 packet per VLAN with timestamp
 */
static int latency_tx_worker(void *arg)
{
    struct tx_worker_params *params = (struct tx_worker_params *)arg;
    uint16_t port_id = params->port_id;

    printf("Latency TX Worker started: Port %u\n", port_id);

    // Get port VLAN config
    if (port_id >= MAX_PORTS_CONFIG) {
        printf("Error: Invalid port_id %u\n", port_id);
        return -1;
    }

    struct port_vlan_config *vlan_cfg = &port_vlans[port_id];
    uint16_t vlan_count = vlan_cfg->tx_vlan_count;

    if (vlan_count == 0) {
        printf("Warning: No TX VLANs configured for port %u\n", port_id);
        g_latency_test.ports[port_id].tx_complete = true;
        return 0;
    }

    // Initialize port test state
    g_latency_test.ports[port_id].port_id = port_id;
    g_latency_test.ports[port_id].test_count = vlan_count;

    // ============ WARM-UP PHASE ============
    // Send warm-up packets to eliminate first-packet penalty (cache, DMA, TX ring warm-up)
    #define WARMUP_PACKETS_PER_QUEUE 8

    printf("  Port %u: Warm-up phase (%u packets per queue)...\n", port_id, WARMUP_PACKETS_PER_QUEUE);

    // Use first VLAN for warm-up (these packets will be ignored by RX - different VL-ID marker)
    uint16_t warmup_vlan = vlan_cfg->tx_vlans[0];

    for (uint16_t q = 0; q < NUM_TX_CORES; q++) {
        for (uint16_t w = 0; w < WARMUP_PACKETS_PER_QUEUE; w++) {
            struct rte_mbuf *mbuf = rte_pktmbuf_alloc(params->mbuf_pool);
            if (!mbuf) continue;

            // Build warm-up packet with special VL-ID (0xFFFF) so RX ignores it
            uint64_t dummy_ts = rte_rdtsc();
            build_latency_test_packet(mbuf, port_id, warmup_vlan, 0xFFFF, w, dummy_ts);

            uint16_t nb_tx = rte_eth_tx_burst(port_id, q, &mbuf, 1);
            if (nb_tx == 0) {
                rte_pktmbuf_free(mbuf);
            }
        }
    }

    // Wait for warm-up packets to be processed
    rte_delay_us(500);

    // ============ ACTUAL TEST ============
    // Send multiple packets per VLAN to eliminate first-packet overhead
    // RX will record minimum latency (first packet absorbs overhead)
    #define PACKETS_PER_VLAN 1
    #define PACKET_DELAY_US 16  // Delay between packets in same VLAN

    printf("  Port %u: Sending %u packets per VLAN (%u VLANs)...\n",
           port_id, PACKETS_PER_VLAN, vlan_count);

    // Send multiple packets per VLAN using first VL-ID
    for (uint16_t v = 0; v < vlan_count; v++) {
        uint16_t vlan_id = vlan_cfg->tx_vlans[v];
        uint16_t vl_id = vlan_cfg->tx_vl_ids[v];  // First VL-ID for this VLAN

        struct latency_result *result = &g_latency_test.ports[port_id].results[v];
        result->tx_count = 0;

        // Send multiple packets per VLAN
        for (uint16_t p = 0; p < PACKETS_PER_VLAN; p++) {
            struct rte_mbuf *mbuf = rte_pktmbuf_alloc(params->mbuf_pool);
            if (!mbuf) {
                printf("  Error: Failed to allocate mbuf for Port %u VLAN %u pkt %u\n",
                       port_id, vlan_id, p);
                continue;
            }

            // Get TX timestamp using TSC
            uint64_t tx_timestamp = rte_rdtsc();

            // Build packet with sequence number = p
            build_latency_test_packet(mbuf, port_id, vlan_id, vl_id, p, tx_timestamp);

            // Send packet using ONLY queue 0 for latency test (eliminates multi-queue effects)
            uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, &mbuf, 1);

            if (nb_tx == 0) {
                rte_pktmbuf_free(mbuf);
            } else {
                result->tx_count++;
                result->tx_timestamp = tx_timestamp;  // Last TX timestamp
            }

            // Small delay between packets in same VLAN
            rte_delay_us(PACKET_DELAY_US);
        }

        printf("  TX: Port %u -> VLAN %u, VL-ID %u (%u packets)\n",
               port_id, vlan_id, vl_id, result->tx_count);

        // Larger delay between different VLANs
        rte_delay_us(32);
    }

    g_latency_test.ports[port_id].tx_complete = true;
    printf("Latency TX Worker completed: Port %u\n", port_id);
    return 0;
}

/**
 * Latency test RX worker - SINGLE QUEUE mode for accurate latency measurement
 * Uses only queue 0 to eliminate multi-queue effects
 */
static int latency_rx_worker(void *arg)
{
    struct rx_worker_params *params = (struct rx_worker_params *)arg;
    uint16_t port_id = params->port_id;
    uint16_t src_port_id = params->src_port_id;

    printf("Latency RX Worker started: Port %u (SINGLE QUEUE mode, queue 0 only)\n", port_id);

#if VLAN_ENABLED
    const uint16_t l2_len = sizeof(struct rte_ether_hdr) + sizeof(struct vlan_hdr);
#else
    const uint16_t l2_len = sizeof(struct rte_ether_hdr);
#endif
    const uint32_t payload_offset = l2_len + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);

    uint32_t total_received = 0;

    struct rte_mbuf *pkts[BURST_SIZE];
    uint64_t timeout_cycles = g_latency_test.tsc_hz * LATENCY_TEST_TIMEOUT_SEC;
    uint64_t start_time = rte_rdtsc();

    uint32_t loop_count = 0;

    // FAST POLLING LOOP - use ONLY queue 0 for latency test
    while (1) {
        // Check timeout every 1000 loops (reduces rdtsc overhead)
        if (++loop_count >= 1000) {
            loop_count = 0;
            if ((rte_rdtsc() - start_time) > timeout_cycles) {
                break;
            }
        }

        // Poll ONLY queue 0 for latency test
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, pkts, BURST_SIZE);
        if (nb_rx == 0) {
            continue;
        }

        for (uint16_t j = 0; j < nb_rx; j++) {
            struct rte_mbuf *m = pkts[j];
            uint8_t *pkt = rte_pktmbuf_mtod(m, uint8_t *);

            // Quick length check
            if (m->pkt_len < payload_offset + LATENCY_PAYLOAD_OFFSET) {
                rte_pktmbuf_free(m);
                continue;
            }

            // Get RX timestamp immediately
            uint64_t rx_timestamp = rte_rdtsc();

            // Extract TX timestamp from payload
            uint8_t *payload = pkt + payload_offset;
            uint64_t tx_timestamp = *(uint64_t *)(payload + SEQ_BYTES);

            // Extract VL-ID from DST MAC (bytes 4-5)
            uint16_t vl_id = ((uint16_t)pkt[4] << 8) | pkt[5];

            // Calculate latency using TSC cycles
            double latency_us = (double)(rx_timestamp - tx_timestamp) * 1000000.0 / g_latency_test.tsc_hz;

            // Find matching result in source port's results
            for (uint16_t r = 0; r < g_latency_test.ports[src_port_id].test_count; r++) {
                struct latency_result *result = &g_latency_test.ports[src_port_id].results[r];

                if (result->vl_id == vl_id) {
                    result->rx_count++;
                    result->sum_latency_us += latency_us;

                    if (!result->received || latency_us < result->min_latency_us) {
                        result->min_latency_us = latency_us;
                    }
                    if (!result->received || latency_us > result->max_latency_us) {
                        result->max_latency_us = latency_us;
                    }

                    result->received = true;
                    result->prbs_ok = true;
                    result->rx_timestamp = rx_timestamp;
                    result->latency_cycles = rx_timestamp - tx_timestamp;

                    total_received++;
                    break;
                }
            }

            rte_pktmbuf_free(m);
        }
    }

    // Calculate averages
    for (uint16_t r = 0; r < g_latency_test.ports[src_port_id].test_count; r++) {
        struct latency_result *result = &g_latency_test.ports[src_port_id].results[r];
        if (result->rx_count > 0) {
            result->latency_us = result->sum_latency_us / result->rx_count;
        }
    }

    g_latency_test.ports[port_id].rx_complete = true;
    printf("Latency RX Worker completed: Port %u (%u packets received)\n", port_id, total_received);
    return 0;
}

/**
 * Print latency test results - per-VLAN with minimum latency (eliminates first-packet overhead)
 */
void print_latency_results(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                    LATENCY TEST RESULTS (Minimum Latency)                                 ║\n");
    printf("╠══════════╦══════════╦══════════╦══════════╦═══════════╦═══════════╦═══════════╦══════════╣\n");
    printf("║ TX Port  ║ RX Port  ║  VLAN    ║  VL-ID   ║  Min (us) ║  Avg (us) ║  Max (us) ║  RX/TX   ║\n");
    printf("╠══════════╬══════════╬══════════╬══════════╬═══════════╬═══════════╬═══════════╬══════════╣\n");

    uint32_t total_tx = 0;
    uint32_t total_rx = 0;
    double total_min_latency = 0.0;

    for (uint16_t p = 0; p < MAX_PORTS; p++) {
        struct port_latency_test *port_test = &g_latency_test.ports[p];

        for (uint16_t t = 0; t < port_test->test_count; t++) {
            struct latency_result *result = &port_test->results[t];

            if (result->tx_count == 0) continue;
            total_tx++;

            if (result->received && result->rx_count > 0) {
                total_rx++;
                double avg_latency = result->sum_latency_us / result->rx_count;
                total_min_latency += result->min_latency_us;

                printf("║    %2u    ║    %2u    ║   %4u   ║   %4u   ║   %7.2f ║   %7.2f ║   %7.2f ║   %2u/%-2u  ║\n",
                       result->tx_port, result->rx_port, result->vlan_id, result->vl_id,
                       result->min_latency_us, avg_latency, result->max_latency_us,
                       result->rx_count, result->tx_count);
            } else {
                printf("║    %2u    ║    %2u    ║   %4u   ║   %4u   ║       -   ║       -   ║       -   ║   0/%-2u   ║\n",
                       result->tx_port, result->rx_port, result->vlan_id, result->vl_id, result->tx_count);
            }
        }
    }

    printf("╠══════════╩══════════╩══════════╩══════════╩═══════════╩═══════════╩═══════════╩══════════╣\n");

    if (total_rx > 0) {
        double avg_min_latency = total_min_latency / total_rx;
        printf("║  SUMMARY: %u/%u VLAN successful | Avg Min Latency: %.2f us                              ║\n",
               total_rx, total_tx, avg_min_latency);
    } else {
        printf("║  SUMMARY: %u/%u successful | No packets received!                                       ║\n",
               total_rx, total_tx);
    }

    printf("╚══════════════════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/**
 * Start latency test
 */
int start_latency_test(struct ports_config *ports_config, volatile bool *stop_flag)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                    LATENCY TEST STARTING                         ║\n");
    printf("║  Packet size: %4u bytes                                         ║\n", LATENCY_TEST_PACKET_SIZE);
    printf("║  Timeout: %u seconds                                              ║\n", LATENCY_TEST_TIMEOUT_SEC);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    // Reset test state
    reset_latency_test();
    g_latency_test.test_running = true;
    g_latency_test.test_start_time = rte_rdtsc();

    // ==========================================
    // Configure RSS to send ALL packets to queue 0
    // This ensures all VLANs are received on queue 0
    // ==========================================
    printf("=== Configuring RSS RETA for Queue 0 ===\n");
    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        if (!ports_config->ports[i].is_valid) continue;
        uint16_t port_id = ports_config->ports[i].port_id;

        struct rte_eth_dev_info dev_info;
        int ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret != 0) {
            printf("  Port %u: Cannot get dev info\n", port_id);
            continue;
        }

        uint16_t reta_size = dev_info.reta_size;
        if (reta_size == 0) {
            printf("  Port %u: RSS RETA not supported\n", port_id);
            continue;
        }

        // Allocate RETA config
        uint16_t num_entries = (reta_size + RTE_ETH_RETA_GROUP_SIZE - 1) / RTE_ETH_RETA_GROUP_SIZE;
        struct rte_eth_rss_reta_entry64 reta_conf[num_entries];

        // Set all RETA entries to queue 0
        for (uint16_t j = 0; j < num_entries; j++) {
            reta_conf[j].mask = UINT64_MAX;
            for (uint16_t k = 0; k < RTE_ETH_RETA_GROUP_SIZE; k++) {
                reta_conf[j].reta[k] = 0;  // All to queue 0
            }
        }

        ret = rte_eth_dev_rss_reta_update(port_id, reta_conf, reta_size);
        if (ret == 0) {
            printf("  Port %u: RSS RETA updated - all %u entries to queue 0\n", port_id, reta_size);
        } else {
            printf("  Port %u: RSS RETA update failed (%d)\n", port_id, ret);
        }
    }
    printf("\n");

    // Pre-initialize test_count and vl_id values for all ports BEFORE starting workers
    // This fixes the race condition where RX workers check test_count before TX workers set it
    printf("=== Pre-initializing Latency Test Data ===\n");
    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        struct port *port = &ports_config->ports[i];
        uint16_t port_id = port->port_id;

        if (port_id >= MAX_PORTS_CONFIG) continue;

        struct port_vlan_config *vlan_cfg = &port_vlans[port_id];
        uint16_t vlan_count = vlan_cfg->tx_vlan_count;

        g_latency_test.ports[port_id].port_id = port_id;
        g_latency_test.ports[port_id].test_count = vlan_count;

        // Pre-populate results with expected vl_id values
        for (uint16_t v = 0; v < vlan_count; v++) {
            struct latency_result *result = &g_latency_test.ports[port_id].results[v];
            result->tx_port = port_id;
            result->rx_port = get_latency_paired_port(port_id);
            result->vlan_id = vlan_cfg->tx_vlans[v];
            result->vl_id = vlan_cfg->tx_vl_ids[v];
            result->received = false;
            result->prbs_ok = false;
        }

        printf("  Port %u: %u VLANs initialized\n", port_id, vlan_count);
    }
    printf("\n");

    // Worker parameters storage
    static struct tx_worker_params tx_params[MAX_PORTS];
    static struct rx_worker_params rx_params[MAX_PORTS];

    // Start RX workers first
    printf("=== Starting Latency RX Workers ===\n");
    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        struct port *port = &ports_config->ports[i];
        uint16_t port_id = port->port_id;
        uint16_t paired_port_id = get_latency_paired_port(port_id);  // Use latency test port mapping
        uint16_t lcore_id = port->used_rx_cores[0];  // Use first RX core

        if (lcore_id == 0 || lcore_id >= RTE_MAX_LCORE) continue;

        rx_params[i].port_id = port_id;
        rx_params[i].src_port_id = paired_port_id;
        rx_params[i].queue_id = 0;
        rx_params[i].lcore_id = lcore_id;
        rx_params[i].stop_flag = stop_flag;

        int ret = rte_eal_remote_launch(latency_rx_worker, &rx_params[i], lcore_id);
        if (ret != 0) {
            printf("Error: Failed to launch latency RX worker on lcore %u\n", lcore_id);
        }
    }

    // Wait for RX workers to be ready (increased delay for stability)
    rte_delay_ms(500);

    // Start TX workers
    printf("\n=== Starting Latency TX Workers ===\n");
    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        struct port *port = &ports_config->ports[i];
        uint16_t port_id = port->port_id;
        uint16_t lcore_id = port->used_tx_cores[0];  // Use first TX core

        if (lcore_id == 0 || lcore_id >= RTE_MAX_LCORE) continue;

        // Get mbuf pool
        char pool_name[32];
        snprintf(pool_name, sizeof(pool_name), "mbuf_pool_%u_%u", port->numa_node, port_id);
        struct rte_mempool *mbuf_pool = rte_mempool_lookup(pool_name);
        if (!mbuf_pool) {
            printf("Error: Cannot find mbuf pool for port %u\n", port_id);
            continue;
        }

        tx_params[i].port_id = port_id;
        tx_params[i].queue_id = 0;
        tx_params[i].lcore_id = lcore_id;
        tx_params[i].mbuf_pool = mbuf_pool;
        tx_params[i].stop_flag = stop_flag;

        int ret = rte_eal_remote_launch(latency_tx_worker, &tx_params[i], lcore_id);
        if (ret != 0) {
            printf("Error: Failed to launch latency TX worker on lcore %u\n", lcore_id);
        }
    }

    // Wait for all workers to complete (with timeout)
    printf("\n=== Waiting for Latency Test to Complete ===\n");
    uint64_t wait_start = rte_rdtsc();
    uint64_t wait_timeout = g_latency_test.tsc_hz * (LATENCY_TEST_TIMEOUT_SEC + 2);

    while (!(*stop_flag)) {
        bool all_complete = true;

        for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
            uint16_t port_id = ports_config->ports[i].port_id;
            if (!g_latency_test.ports[port_id].tx_complete ||
                !g_latency_test.ports[port_id].rx_complete) {
                all_complete = false;
                break;
            }
        }

        if (all_complete) break;

        // Check timeout
        if ((rte_rdtsc() - wait_start) > wait_timeout) {
            printf("Warning: Latency test global timeout reached\n");
            break;
        }

        rte_delay_ms(100);
    }

    // Wait for lcores to finish
    rte_eal_mp_wait_lcore();

    g_latency_test.test_running = false;
    g_latency_test.test_complete = true;

    // ==========================================
    // Restore RSS RETA to distribute across all queues
    // ==========================================
    printf("\n=== Restoring RSS RETA for Normal Operation ===\n");
    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        if (!ports_config->ports[i].is_valid) continue;
        uint16_t port_id = ports_config->ports[i].port_id;

        struct rte_eth_dev_info dev_info;
        int ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret != 0) continue;

        uint16_t reta_size = dev_info.reta_size;
        if (reta_size == 0) continue;

        uint16_t num_entries = (reta_size + RTE_ETH_RETA_GROUP_SIZE - 1) / RTE_ETH_RETA_GROUP_SIZE;
        struct rte_eth_rss_reta_entry64 reta_conf[num_entries];

        // Distribute across all RX queues (round-robin)
        for (uint16_t j = 0; j < num_entries; j++) {
            reta_conf[j].mask = UINT64_MAX;
            for (uint16_t k = 0; k < RTE_ETH_RETA_GROUP_SIZE; k++) {
                uint16_t idx = j * RTE_ETH_RETA_GROUP_SIZE + k;
                reta_conf[j].reta[k] = idx % NUM_RX_CORES;  // Distribute to all RX queues
            }
        }

        ret = rte_eth_dev_rss_reta_update(port_id, reta_conf, reta_size);
        if (ret == 0) {
            printf("  Port %u: RSS RETA restored - distributed to %u queues\n", port_id, NUM_RX_CORES);
        }
    }
    printf("\n");

    // Print results
    print_latency_results();
    sleep(25);
    printf("=== Latency Test Complete, Switching to Normal Mode ===\n\n");
    return 0;
}

#endif /* LATENCY_TEST_ENABLED */