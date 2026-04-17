#include "TxRxManager.h"
#include "AteMode.h"
#include <rte_lcore.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_flow.h>
#include <stdlib.h>
#include <string.h>
#include <health_monitor.h>

// ==========================================
// SPLITMIX64 + CRC32C PAYLOAD VERIFICATION
// ==========================================
// VMC XORs payload[8..71] with splitmix64, writes CRC32C at [72..75].
// Server verifies CRC32C, then checks PRBS on remaining payload[76+].
// Used in unit test mode only (not ATE loopback).
#define SPLITMIX_XOR_BYTES   64
#define SPLITMIX_CRC_BYTES   4
#define SPLITMIX_TOTAL_OVERHEAD (SPLITMIX_XOR_BYTES + SPLITMIX_CRC_BYTES)  // 68
#define SPLITMIX_MIN_PAYLOAD (SEQ_BYTES + SPLITMIX_TOTAL_OVERHEAD)         // 76

static inline uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* CRC32C (Castagnoli) - software lookup-table implementation.
 * Reflected polynomial: 0x82F63B78. */
static const uint32_t crc32c_table[256] = {
    0x00000000, 0xF26B8303, 0xE13B70F7, 0x1350F3F4, 0xC79A971F, 0x35F1141C, 0x26A1E7E8, 0xD4CA64EB,
    0x8AD958CF, 0x78B2DBCC, 0x6BE22838, 0x9989AB3B, 0x4D43CFD0, 0xBF284CD3, 0xAC78BF27, 0x5E133C24,
    0x105EC76F, 0xE235446C, 0xF165B798, 0x030E349B, 0xD7C45070, 0x25AFD373, 0x36FF2087, 0xC494A384,
    0x9A879FA0, 0x68EC1CA3, 0x7BBCEF57, 0x89D76C54, 0x5D1D08BF, 0xAF768BBC, 0xBC267848, 0x4E4DFB4B,
    0x20BD8EDE, 0xD2D60DDD, 0xC186FE29, 0x33ED7D2A, 0xE72719C1, 0x154C9AC2, 0x061C6936, 0xF477EA35,
    0xAA64D611, 0x580F5512, 0x4B5FA6E6, 0xB93425E5, 0x6DFE410E, 0x9F95C20D, 0x8CC531F9, 0x7EAEB2FA,
    0x30E349B1, 0xC288CAB2, 0xD1D83946, 0x23B3BA45, 0xF779DEAE, 0x05125DAD, 0x1642AE59, 0xE4292D5A,
    0xBA3A117E, 0x4851927D, 0x5B016189, 0xA96AE28A, 0x7DA08661, 0x8FCB0562, 0x9C9BF696, 0x6EF07595,
    0x417B1DBC, 0xB3109EBF, 0xA0406D4B, 0x5228EE48, 0x86E28AA3, 0x748909A0, 0x67D9FA54, 0x95B27957,
    0xCBA14573, 0x39CAC670, 0x2A9A3584, 0xD8F1B687, 0x0C3BD26C, 0xFE50516F, 0xED00A29B, 0x1F6B2198,
    0x5125DAD3, 0xA34E59D0, 0xB01EAA24, 0x42752927, 0x96BF4DCC, 0x64D4CECF, 0x77843D3B, 0x85EFBE38,
    0xDBFC821C, 0x2997011F, 0x3AC7F2EB, 0xC8AC71E8, 0x1C661503, 0xEE0D9600, 0xFD5D65F4, 0x0F36E6F7,
    0x61C69362, 0x93AD1061, 0x80FDE395, 0x72966096, 0xA65C047D, 0x5437877E, 0x4767748A, 0xB50CF789,
    0xEB1FCBAD, 0x197448AE, 0x0A24BB5A, 0xF84F3859, 0x2C855CB2, 0xDEEEDFB1, 0xCDBE2C45, 0x3FD5AF46,
    0x7198540D, 0x83F3D70E, 0x90A324FA, 0x62C8A7F9, 0xB602C312, 0x44694011, 0x5739B3E5, 0xA55230E6,
    0xFB410CC2, 0x092A8FC1, 0x1A7A7C35, 0xE811FF36, 0x3CDB9BDD, 0xCEB018DE, 0xDDE0EB2A, 0x2F8B6829,
    0x82F63B78, 0x709DB87B, 0x63CD4B8F, 0x91A6C88C, 0x456CAC67, 0xB7072F64, 0xA457DC90, 0x56385F93,
    0x082B63B7, 0xFA40E0B4, 0xE9101340, 0x1B7B9043, 0xCFB1F4A8, 0x3DDA77AB, 0x2E8A845F, 0xDCE1075C,
    0x92A6FC17, 0x60CD7F14, 0x739D8CE0, 0x81F60FE3, 0x553C6B08, 0xA757E80B, 0xB4071BFF, 0x461C98FC,
    0x180FA4D8, 0xEA6427DB, 0xF934D42F, 0x0B5F572C, 0xDF9533C7, 0x2DFEB0C4, 0x3EAE4330, 0xCCC5C033,
    0xA23551A6, 0x505ED2A5, 0x430E2151, 0xB165A252, 0x65AFC6B9, 0x97C445BA, 0x8494B64E, 0x76FF354D,
    0x28EC0969, 0xDAD78A6A, 0xC987799E, 0x3BECFA9D, 0xEF269E76, 0x1D4D1D75, 0x0E1DEE81, 0xFC766D82,
    0xB23B96C9, 0x405015CA, 0x5300E63E, 0xA16B653D, 0x75A101D6, 0x87CA82D5, 0x949A7121, 0x66F1F222,
    0x38E2CE06, 0xCA894D05, 0xD9D9BEF1, 0x2BB23DF2, 0xFF785919, 0x0D13DA1A, 0x1E4329EE, 0xEC28AAED,
    0xC5A92679, 0x37C2A57A, 0x2492568E, 0xD6F9D58D, 0x0233B166, 0xF0583265, 0xE308C191, 0x11634292,
    0x4F707EB6, 0xBD1BFDB5, 0xAE4B0E41, 0x5C208D42, 0x88EAE9A9, 0x7A816AAA, 0x69D1995E, 0x9BBA1A5D,
    0xD5F7E116, 0x279C6215, 0x34CC91E1, 0xC6A712E2, 0x126D7609, 0xE006F50A, 0xF35606FE, 0x013D85FD,
    0x5F2EB9D9, 0xAD453ADA, 0xBE15C92E, 0x4C7E4A2D, 0x985E2EC6, 0x6A35ADC5, 0x79655E31, 0x8B0EDD32,
    0xE5FEA876, 0x17952B75, 0x04C5D881, 0xF6AE5B82, 0x22643F69, 0xD00FBC6A, 0xC35F4F9E, 0x3134CC9D,
    0x6F27F0B9, 0x9D4C73BA, 0x8E1C804E, 0x7C77034D, 0xA8BD67A6, 0x5AD6E4A5, 0x49861751, 0xBBED9452,
    0xF5A06F19, 0x07CBEC1A, 0x149B1FEE, 0xE6F09CED, 0x323AF806, 0xC0517B05, 0xD30188F1, 0x216A0BF2,
    0x7F7937D6, 0x8D12B4D5, 0x9E424721, 0x6C29C422, 0xB8E3A0C9, 0x4A8823CA, 0x59D8D03E, 0xABB3533D
};

static inline uint32_t sw_crc32c(const void *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32c_table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

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

#if STATS_MODE_VMC
// VMC per-port statistics
struct vmc_port_stats vmc_stats[VMC_PORT_COUNT];

// VMC port mapping table
struct vmc_port_map_entry vmc_port_map[VMC_PORT_COUNT] = VMC_PORT_MAP_INIT;

// VLAN → VMC port fast lookup table
uint8_t vlan_to_vmc_port[VMC_VLAN_LOOKUP_SIZE];

// VLAN flow rule handles (for cleanup)
#define VMC_MAX_FLOW_RULES_PER_PORT 4
static struct rte_flow *vmc_flow_handles[MAX_PORTS][VMC_MAX_FLOW_RULES_PER_PORT] = {{NULL}};
#endif /* STATS_MODE_VMC */

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
 * Uses per-queue tx_vl_counts if set, otherwise default VL_RANGE_SIZE_PER_QUEUE
 */
static inline uint16_t get_tx_vl_id_range_end(uint16_t port_id, uint16_t queue_index)
{
    uint16_t count = port_vlans[port_id].tx_vl_counts[queue_index];
    if (count == 0) count = VL_RANGE_SIZE_PER_QUEUE;
    return get_tx_vl_id_range_start(port_id, queue_index) + count;
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
 * Uses per-queue tx_vl_counts if set, otherwise default VL_RANGE_SIZE_PER_QUEUE
 */
static inline uint16_t get_rx_vl_id_range_end(uint16_t port_id, uint16_t queue_index)
{
    uint16_t count = port_vlans[port_id].tx_vl_counts[queue_index];
    if (count == 0) count = VL_RANGE_SIZE_PER_QUEUE;
    return get_rx_vl_id_range_start(port_id, queue_index) + count;
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

#if STATS_MODE_VMC
// ==========================================
// VMC PORT MAPPING & STATISTICS INIT
// ==========================================

void init_vmc_port_map(void)
{
    // Fill the VLAN → VMC port lookup table
    memset(vlan_to_vmc_port, VMC_VLAN_INVALID, sizeof(vlan_to_vmc_port));

    for (int i = 0; i < VMC_DPDK_PORT_COUNT; i++) {
        // VMC TX VLAN (VMC→Server, seen on server RX)
        if (vmc_port_map[i].tx_vlan < VMC_VLAN_LOOKUP_SIZE) {
            vlan_to_vmc_port[vmc_port_map[i].tx_vlan] = (uint8_t)i;
        }
        // VMC RX VLAN (Server→VMC, used in server TX)
        // These VLANs should also be in the lookup (can be used in TX worker)
        if (vmc_port_map[i].rx_vlan < VMC_VLAN_LOOKUP_SIZE) {
            // We could also put rx_vlan in a separate lookup, tx_vlan is sufficient for now
            // because PRBS check is done on server RX = via tx_vlan
        }
    }

    printf("\n=== VMC Port Mapping Initialized ===\n");
    printf("VMC Ports: %d (DPDK: %d, Raw: 2)\n", VMC_PORT_COUNT, VMC_DPDK_PORT_COUNT);
    printf("VLAN → VMC Port lookup table built\n");

    // Print summary table
    printf("\n  VMC Port | VMC RX (Srv TX)      | VMC TX (Srv RX)\n");
    printf("  ---------+----------------------+----------------------\n");
    for (int i = 0; i < VMC_DPDK_PORT_COUNT; i++) {
        printf("    %2d     | SrvPort%u Q%u VLAN%-3u | SrvPort%u Q%u VLAN%-3u\n",
               vmc_port_map[i].vmc_port_id,
               vmc_port_map[i].rx_server_port, vmc_port_map[i].rx_server_queue,
               vmc_port_map[i].rx_vlan,
               vmc_port_map[i].tx_server_port, vmc_port_map[i].tx_server_queue,
               vmc_port_map[i].tx_vlan);
    }
    printf("    32     | Port 12 (1G)         | Port 12 (1G)\n");
    printf("    33     | Port 13 (100M)       | Port 13 (100M)\n");
    printf("================================================\n\n");
}

void init_vmc_stats(void)
{
    for (int i = 0; i < VMC_PORT_COUNT; i++) {
        rte_atomic64_init(&vmc_stats[i].good_pkts);
        rte_atomic64_init(&vmc_stats[i].bad_pkts);
        rte_atomic64_init(&vmc_stats[i].splitmix_fail);
        rte_atomic64_init(&vmc_stats[i].crc32_fail);
        rte_atomic64_init(&vmc_stats[i].bit_errors);
        rte_atomic64_init(&vmc_stats[i].lost_pkts);
        rte_atomic64_init(&vmc_stats[i].out_of_order_pkts);
        rte_atomic64_init(&vmc_stats[i].duplicate_pkts);
        rte_atomic64_init(&vmc_stats[i].short_pkts);
        rte_atomic64_init(&vmc_stats[i].total_rx_pkts);
    }
    printf("VMC port statistics initialized for %d ports\n", VMC_PORT_COUNT);
}

// ==========================================
// VMC VLAN-BASED FLOW STEERING
// ==========================================
// Steers each RX VLAN to its corresponding queue (1:1 mapping)
// This way HW per-queue stats = per-VLAN = per-VMC port stats

int vmc_flow_rules_install(uint16_t port_id)
{
    struct rte_flow_error error;
    int installed = 0;

    // How many RX VLANs does this port have?
    uint16_t rx_vlan_count = port_vlans[port_id].rx_vlan_count;
    if (rx_vlan_count == 0 || rx_vlan_count > VMC_MAX_FLOW_RULES_PER_PORT) {
        printf("VMC Flow: Port %u has %u RX VLANs, skipping\n", port_id, rx_vlan_count);
        return 0;
    }

    printf("VMC Flow: Installing VLAN→Queue rules on Port %u (%u VLANs)...\n",
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
        attr.priority = 1;

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
            printf("VMC Flow: Port %u VLAN %u validate failed: %s\n",
                   port_id, vlan_id,
                   error.message ? error.message : "unknown");
            continue;
        }

        struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, action, &error);
        if (!flow) {
            printf("VMC Flow: Port %u VLAN %u create failed: %s\n",
                   port_id, vlan_id,
                   error.message ? error.message : "unknown");
            continue;
        }

        vmc_flow_handles[port_id][q] = flow;
        installed++;
        printf("  VLAN %u → Queue %u\n", vlan_id, q);
    }

    printf("VMC Flow: Port %u: %d/%u rules installed\n", port_id, installed, rx_vlan_count);
    return (installed == rx_vlan_count) ? 0 : -1;
}

void vmc_flow_rules_remove(uint16_t port_id)
{
    if (port_id >= MAX_PORTS) return;

    for (int q = 0; q < VMC_MAX_FLOW_RULES_PER_PORT; q++) {
        if (vmc_flow_handles[port_id][q]) {
            struct rte_flow_error error;
            rte_flow_destroy(port_id, vmc_flow_handles[port_id][q], &error);
            vmc_flow_handles[port_id][q] = NULL;
        }
    }
}
#endif /* STATS_MODE_VMC */

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

#if STATS_MODE_VMC
    // VMC mode: RSS disabled, rte_flow VLAN steering will be used (after port start)
    if (config->nb_rx_queues > 1)
    {
        // We still enable RSS because fallback is needed for packets not matching rte_flow
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        uint64_t rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP;
        rss_hf &= dev_info.flow_type_rss_offloads;
        port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
        port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
        printf("Port %u VMC mode: RSS as fallback, VLAN flow steering will be installed\n", port_id);
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

#if STATS_MODE_VMC
    // VMC mode: Set up VLAN-based flow steering (after port start)
    if (config->nb_rx_queues > 1)
    {
        printf("Port %u: Installing VMC VLAN→Queue flow rules...\n", port_id);
        int flow_ret = vmc_flow_rules_install(port_id);
        if (flow_ret != 0) {
            printf("Warning: VMC flow rules incomplete on port %u, falling back to RSS\n", port_id);
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
        for (uint16_t q = 0; q < RTE_ETHDEV_QUEUE_STAT_CNTRS && q < NUM_RX_CORES; q++)
        {
            if (stats.q_ipackets[q] > 0)
            {
                printf("    Queue %u: %lu packets, %lu bytes\n",
                       q, stats.q_ipackets[q], stats.q_ibytes[q]);
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
    const uint16_t vl_range_size = vl_end - vl_start;
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

int rx_worker(void *arg)
{
    struct rx_worker_params *params = (struct rx_worker_params *)arg;
    struct rte_mbuf *pkts[BURST_SIZE];
    bool first_packet_received = false;

    // L2 header length for VLAN packets
    const uint16_t l2_len_vlan = sizeof(struct rte_ether_hdr) + sizeof(struct vlan_hdr);  // 18

    // Minimum packet length
    const uint32_t min_len_vlan = l2_len_vlan + 20 + 8 + SEQ_BYTES + NUM_PRBS_BYTES;      // 1509

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
    printf("  L2 header: VLAN (0x8100)->%u bytes\n", l2_len_vlan);

    uint64_t local_rx = 0, local_good = 0, local_bad = 0, local_bits = 0;
    uint64_t local_lost = 0, local_ooo = 0, local_dup = 0, local_short = 0;
    uint64_t local_external = 0;  // External packets (VL-ID outside expected range)
    uint64_t local_sm_fail = 0, local_crc_fail = 0;  // SplitMix/CRC fail counters
    const bool g_ate_mode_active = ate_mode_enabled();
    const uint32_t FLUSH = 128;

#if STATS_MODE_VMC
    // In VMC mode: queue_id -> VLAN -> VMC port (1:1 mapping, flow steering active)
    const uint16_t rx_vlan_for_queue = port_vlans[params->port_id].rx_vlans[params->queue_id];
    const uint8_t my_vmc_port = (rx_vlan_for_queue < VMC_VLAN_LOOKUP_SIZE)
                                    ? vlan_to_vmc_port[rx_vlan_for_queue]
                                    : VMC_VLAN_INVALID;
#endif

    bool first_good = false, first_bad = false;

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

                // Payload offset for VLAN packets
                const uint32_t payload_off = l2_len_vlan + 20 + 8;

                // ==========================================
                // HEALTH MONITOR EARLY BRANCH
                // HM paketleri PRBS paketlerinden çok daha küçük (ör. 182 B)
                // olduğu için PRBS min_len_vlan (~1509 B) filtresinden ÖNCE
                // kontrol edilmeli — aksi halde "short" olarak düşer.
                // Sadece L2+IP+UDP header'ını kapsayan minimum boyut garanti
                // edildikten sonra VL-ID çıkarılıyor.
                // ==========================================
                if (likely(m->pkt_len >= payload_off))
                {
                    uint16_t vl_id_hm = extract_vl_id_from_packet(pkt, l2_len_vlan);
                    if (unlikely(hm_is_health_monitor_vl_id(vl_id_hm)))
                    {
                        uint16_t hm_len = (uint16_t)(m->pkt_len - payload_off);
                        hm_handle_packet(vl_id_hm, pkt + payload_off, hm_len);
                        // mbuf batch-free ile (loop sonunda) serbest bırakılacak.
                        continue;
                    }
                }

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

                //  Extract VL-ID from DST MAC (last 2 bytes)
                uint16_t vl_id = extract_vl_id_from_packet(pkt, l2_len_vlan);
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
                // PAYLOAD VERIFICATION
                // Unit test: SplitMix64 + CRC32C + PRBS
                // ATE mode: PRBS-only (no VMC transform)
                // ==========================================
                uint8_t *payload_base = pkt + payload_off;
                uint64_t off = (seq * (uint64_t)NUM_PRBS_BYTES) % (uint64_t)PRBS_CACHE_SIZE;
                uint8_t *prbs_exp = prbs_cache_ext + off;

                if (!g_ate_mode_active) {
                    // ==========================================
                    // UNIT TEST MODE: SplitMix64 + CRC32C + PRBS
                    // ==========================================
                    // 1) CRC32C check over [0..71] (SEQ + SplitMix XOR'd zone)
                    // VMC writes CRC in big endian (network byte order)
                    uint32_t calc_crc = sw_crc32c(payload_base, SEQ_BYTES + SPLITMIX_XOR_BYTES);
                    uint32_t recv_crc = __builtin_bswap32(*(uint32_t *)(payload_base + SEQ_BYTES + SPLITMIX_XOR_BYTES));
                    bool crc_ok = (calc_crc == recv_crc);

                    // 2) Build expected SplitMix XOR zone for comparison
                    uint8_t expected_sm[SPLITMIX_XOR_BYTES];
                    uint64_t seq_be = __builtin_bswap64(seq);
                    for (int blk = 0; blk < 8; blk++) {
                        uint64_t sm_val = __builtin_bswap64(splitmix64(8 * seq_be + blk));
                        uint64_t orig_prbs;
                        memcpy(&orig_prbs, prbs_exp + blk * 8, 8);
                        uint64_t xored = orig_prbs ^ sm_val;
                        memcpy(expected_sm + blk * 8, &xored, 8);
                    }
                    bool sm_ok = (memcmp(payload_base + SEQ_BYTES, expected_sm, SPLITMIX_XOR_BYTES) == 0);

                    // 3) PRBS check on [76+] (after SplitMix+CRC overhead)
                    uint8_t *recv_prbs = payload_base + SEQ_BYTES + SPLITMIX_TOTAL_OVERHEAD;
                    uint8_t *exp_prbs = prbs_exp + SPLITMIX_TOTAL_OVERHEAD;
                    uint32_t prbs_check_len = NUM_PRBS_BYTES - SPLITMIX_TOTAL_OVERHEAD - 1; // -1 for DTN_SEQ
                    bool prbs_ok = (memcmp(recv_prbs, exp_prbs, prbs_check_len) == 0);

                    if (likely(crc_ok && sm_ok && prbs_ok)) {
                        local_good++;
                        if (unlikely(!first_good)) {
                            printf("✓ GOOD: Port %u Q%u VL-ID %u Seq %lu\n",
                                   params->port_id, params->queue_id, vl_id, seq);
                            first_good = true;
                        }
                    } else {
                        local_bad++;
                        if (!sm_ok) local_sm_fail++;
                        if (!crc_ok) local_crc_fail++;
                        if (unlikely(!first_bad)) {
                            printf("✗ BAD: Port %u Q%u VL-ID %u Seq %lu (SM=%s CRC=%s PRBS=%s)\n",
                                   params->port_id, params->queue_id, vl_id, seq,
                                   sm_ok ? "OK" : "FAIL", crc_ok ? "OK" : "FAIL",
                                   prbs_ok ? "OK" : "FAIL");
                            first_bad = true;
                        }

                        // Bit error counting over entire payload (all zones)
                        uint64_t berr = 0;
                        // [8..71] SplitMix zone
                        for (int blk = 0; blk < SPLITMIX_XOR_BYTES / 8; blk++) {
                            uint64_t r, e;
                            memcpy(&r, payload_base + SEQ_BYTES + blk * 8, 8);
                            memcpy(&e, expected_sm + blk * 8, 8);
                            berr += __builtin_popcountll(r ^ e);
                        }
                        // [72..75] CRC zone (big endian in packet)
                        {
                            uint32_t exp_crc_be = __builtin_bswap32(calc_crc);
                            uint32_t pkt_crc_be = *(uint32_t *)(payload_base + SEQ_BYTES + SPLITMIX_XOR_BYTES);
                            berr += __builtin_popcount(pkt_crc_be ^ exp_crc_be);
                        }
                        // [76+] PRBS zone
                        if (!prbs_ok) {
                            const uint64_t *r64 = (const uint64_t *)recv_prbs;
                            const uint64_t *e64 = (const uint64_t *)exp_prbs;
                            const uint16_t nq = prbs_check_len / 8;
                            for (uint16_t k = 0; k < nq; k++)
                                berr += __builtin_popcountll(r64[k] ^ e64[k]);
                            uint16_t rem = prbs_check_len & 7;
                            if (rem) {
                                const uint8_t *r8 = (const uint8_t *)(r64 + nq);
                                const uint8_t *e8 = (const uint8_t *)(e64 + nq);
                                for (uint16_t k = 0; k < rem; k++)
                                    berr += __builtin_popcount(r8[k] ^ e8[k]);
                            }
                        }
                        local_bits += berr;
                    }
                } else {
                    // ==========================================
                    // ATE MODE: PRBS-only verification
                    // ==========================================
                    uint8_t *recv = payload_base + SEQ_BYTES;
                    uint8_t *exp = prbs_exp;
                    int diff = memcmp(recv, exp, NUM_PRBS_BYTES);

                    if (likely(diff == 0)) {
                        local_good++;
                        if (unlikely(!first_good)) {
                            printf("✓ GOOD: Port %u Q%u VL-ID %u Seq %lu\n",
                                   params->port_id, params->queue_id, vl_id, seq);
                            first_good = true;
                        }
                    } else {
                        local_bad++;
                        if (unlikely(!first_bad)) {
                            printf("✗ BAD: Port %u Q%u VL-ID %u Seq %lu\n",
                                   params->port_id, params->queue_id, vl_id, seq);
                            first_bad = true;
                        }
                        uint64_t berr = 0;
                        const uint64_t *r64 = (const uint64_t *)recv;
                        const uint64_t *e64 = (const uint64_t *)exp;
                        const uint16_t nq = NUM_PRBS_BYTES / 8;
                        for (uint16_t k = 0; k < nq; k++)
                            berr += __builtin_popcountll(r64[k] ^ e64[k]);
                        uint16_t rem = NUM_PRBS_BYTES & 7;
                        if (rem) {
                            const uint8_t *r8 = (const uint8_t *)(r64 + nq);
                            const uint8_t *e8 = (const uint8_t *)(e64 + nq);
                            for (uint16_t k = 0; k < rem; k++)
                                berr += __builtin_popcount(r8[k] ^ e8[k]);
                        }
                        local_bits += berr;
                    }
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

#if STATS_MODE_VMC
                // VMC per-port payload verification stats
                if (my_vmc_port != VMC_VLAN_INVALID) {
                    rte_atomic64_add(&vmc_stats[my_vmc_port].total_rx_pkts, local_rx);
                    rte_atomic64_add(&vmc_stats[my_vmc_port].good_pkts, local_good);
                    rte_atomic64_add(&vmc_stats[my_vmc_port].bad_pkts, local_bad);
                    rte_atomic64_add(&vmc_stats[my_vmc_port].splitmix_fail, local_sm_fail);
                    rte_atomic64_add(&vmc_stats[my_vmc_port].crc32_fail, local_crc_fail);
                    rte_atomic64_add(&vmc_stats[my_vmc_port].bit_errors, local_bits);
                    rte_atomic64_add(&vmc_stats[my_vmc_port].lost_pkts, local_lost);
                    rte_atomic64_add(&vmc_stats[my_vmc_port].out_of_order_pkts, local_ooo);
                    rte_atomic64_add(&vmc_stats[my_vmc_port].duplicate_pkts, local_dup);
                    rte_atomic64_add(&vmc_stats[my_vmc_port].short_pkts, local_short);
                }
#endif
                local_rx = local_good = local_bad = local_bits = 0;
                local_lost = local_ooo = local_dup = local_short = local_external = 0;
                local_sm_fail = local_crc_fail = 0;
            }
        }
    }

    // Final flush
    if (local_rx || local_external)
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

#if STATS_MODE_VMC
        if (my_vmc_port != VMC_VLAN_INVALID) {
            rte_atomic64_add(&vmc_stats[my_vmc_port].total_rx_pkts, local_rx);
            rte_atomic64_add(&vmc_stats[my_vmc_port].good_pkts, local_good);
            rte_atomic64_add(&vmc_stats[my_vmc_port].bad_pkts, local_bad);
            rte_atomic64_add(&vmc_stats[my_vmc_port].splitmix_fail, local_sm_fail);
            rte_atomic64_add(&vmc_stats[my_vmc_port].crc32_fail, local_crc_fail);
            rte_atomic64_add(&vmc_stats[my_vmc_port].bit_errors, local_bits);
            rte_atomic64_add(&vmc_stats[my_vmc_port].lost_pkts, local_lost);
            rte_atomic64_add(&vmc_stats[my_vmc_port].out_of_order_pkts, local_ooo);
            rte_atomic64_add(&vmc_stats[my_vmc_port].duplicate_pkts, local_dup);
            rte_atomic64_add(&vmc_stats[my_vmc_port].short_pkts, local_short);
        }
#endif
    }

    // ==========================================
    // CALCULATE LOST PACKETS (watermark-based)
    // Lost = (max_seq + 1) - pkt_count for each VL-ID
    // ==========================================
#if STATS_MODE_VMC
    // VMC mode: Each queue calculates lost for its own VL-ID range
    // (With flow steering, each queue = 1 VLAN = 1 VMC port, ranges do not overlap)
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
            if (my_vmc_port != VMC_VLAN_INVALID) {
                rte_atomic64_add(&vmc_stats[my_vmc_port].lost_pkts, queue_lost);
            }
            printf("RX Worker Port %u Q%u (VMC %u): %lu lost packets (VL-ID %u-%u)\n",
                   params->port_id, params->queue_id,
                   my_vmc_port != VMC_VLAN_INVALID ? my_vmc_port : 0xFF,
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
        // Loopback: packets return to same port (both unit test and ATE mode)
        uint16_t paired_port_id = port_id;

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
        // Loopback: TX goes to self (both unit test and ATE mode)
        uint16_t paired_port_id = port_id;

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

            double port_target_gbps = TARGET_GBPS;
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

            printf("  TX Queue %u -> Lcore %2u -> VLAN %u, VL RANGE [%u..%u) Rate: %.1f Gbps\n",
                   q, lcore_id, tx_vlan,
                   get_tx_vl_id_range_start(port_id, q), get_tx_vl_id_range_end(port_id, q),
                   port_target_gbps);

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

    // NOTE: DPDK External TX workers are started after DPDK RX/TX workers
    // in main.c to ensure RX is ready before receiving packets.

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