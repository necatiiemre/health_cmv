/**
 * DPDK External TX System
 *
 * External TX system that operates INDEPENDENTLY from existing DPDK TX.
 * Sends packets from Port 0,1,2,3 to Port 12 through the switch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

#include "DpdkExternalTx.h"
#include "Packet.h"
#include "TxRxManager.h"

#if DPDK_EXT_TX_ENABLED

// ==========================================
// GLOBAL VARIABLES
// ==========================================

// External TX statistics
struct dpdk_ext_tx_stats dpdk_ext_tx_stats_per_port[DPDK_EXT_TX_PORT_COUNT];

// External TX ports
struct dpdk_ext_tx_port dpdk_ext_tx_ports[DPDK_EXT_TX_PORT_COUNT];

// Per-port per-VL-ID sequence numbers (separate from main system)
static uint64_t ext_tx_sequences[DPDK_EXT_TX_PORT_COUNT][MAX_VL_ID + 1];

// Worker parameters storage
static struct dpdk_ext_tx_worker_params ext_worker_params[DPDK_EXT_TX_PORT_COUNT * DPDK_EXT_TX_QUEUES_PER_PORT];

// Configuration
static struct dpdk_ext_tx_port_config ext_tx_configs[] = DPDK_EXT_TX_PORTS_CONFIG_INIT;

// ==========================================
// RATE LIMITER (Token Bucket)
// ==========================================
struct ext_rate_limiter {
    uint64_t tokens;
    uint64_t max_tokens;
    uint64_t tokens_per_sec;
    uint64_t last_update;
    uint64_t tsc_hz;
};

static void ext_rate_limiter_init(struct ext_rate_limiter *limiter, uint32_t rate_mbps)
{
    limiter->tsc_hz = rte_get_tsc_hz();
    limiter->tokens_per_sec = (uint64_t)rate_mbps * 1000000ULL / 8; // bytes per second

    // FIX: 100ms -> 0.5ms burst window (prevents switch buffer overflow)
    limiter->max_tokens = limiter->tokens_per_sec / 2000; // 0.5ms worth (was /10 = 100ms)

    // Minimum bucket: only 1 packet (burst prevention)
    const uint64_t min_bucket = 1 * 1520; // ~1.5KB minimum (was 32 packets)
    if (limiter->max_tokens < min_bucket) {
        limiter->max_tokens = min_bucket;
    }

    // FIX: Soft start - begin with empty bucket (prevent sudden burst)
    limiter->tokens = 0;  // was limiter->max_tokens
    limiter->last_update = rte_rdtsc();
}

static inline bool ext_consume_tokens(struct ext_rate_limiter *limiter, uint64_t bytes)
{
    uint64_t now = rte_rdtsc();
    uint64_t elapsed = now - limiter->last_update;

    // Add tokens based on elapsed time
    uint64_t new_tokens = (elapsed * limiter->tokens_per_sec) / limiter->tsc_hz;
    if (new_tokens > 0) {
        limiter->tokens += new_tokens;
        if (limiter->tokens > limiter->max_tokens) {
            limiter->tokens = limiter->max_tokens;
        }
        limiter->last_update = now;
    }

    if (limiter->tokens >= bytes) {
        limiter->tokens -= bytes;
        return true;
    }
    return false;
}

// ==========================================
// HELPER FUNCTIONS
// ==========================================

static inline uint64_t get_ext_tx_sequence(uint16_t port_idx, uint16_t vl_id)
{
    if (port_idx >= DPDK_EXT_TX_PORT_COUNT || vl_id > MAX_VL_ID) {
        return 0;
    }
    return ext_tx_sequences[port_idx][vl_id]++;
}

// Peek without incrementing — for send-then-commit pattern
static inline uint64_t peek_ext_tx_sequence(uint16_t port_idx, uint16_t vl_id)
{
    if (port_idx >= DPDK_EXT_TX_PORT_COUNT || vl_id > MAX_VL_ID) {
        return 0;
    }
    return ext_tx_sequences[port_idx][vl_id];
}

// Commit after successful send
static inline void commit_ext_tx_sequence(uint16_t port_idx, uint16_t vl_id)
{
    if (port_idx >= DPDK_EXT_TX_PORT_COUNT || vl_id > MAX_VL_ID) {
        return;
    }
    ext_tx_sequences[port_idx][vl_id]++;
}

int dpdk_ext_tx_get_source_port(uint16_t vl_id)
{
    // VL-ID ranges must match config.h DPDK_EXT_TX_PORT_*_TARGETS
    //
    // Targeting Port 12 (Port 2,3,4,5):
    //   Port 2 → VL-ID 4291-4322 (32 VL-ID)
    //   Port 3 → VL-ID 4323-4354 (32 VL-ID)
    //   Port 4 → VL-ID 4355-4386 (32 VL-ID)
    //   Port 5 → VL-ID 4387-4418 (32 VL-ID)
    //
    // Targeting Port 13 (Port 0,6):
    //   Port 0 → VL-ID 4099-4114 (16 VL-ID)
    //   Port 6 → VL-ID 4115-4130 (16 VL-ID)

    // Targeting Port 12
    if (vl_id >= 4291 && vl_id < 4323) return 2;
    if (vl_id >= 4323 && vl_id < 4355) return 3;
    if (vl_id >= 4355 && vl_id < 4387) return 4;
    if (vl_id >= 4387 && vl_id < 4419) return 5;

    // Targeting Port 13
    if (vl_id >= 4099 && vl_id < 4115) return 0;
    if (vl_id >= 4115 && vl_id < 4131) return 6;

    return -1; // Not an external TX VL-ID
}

// ==========================================
// INITIALIZATION
// ==========================================

int dpdk_ext_tx_init(struct rte_mempool *mbuf_pools[])
{
    printf("\n=== Initializing DPDK External TX System ===\n");
    printf("Ports: %d, Queues per port: %d\n", DPDK_EXT_TX_PORT_COUNT, DPDK_EXT_TX_QUEUES_PER_PORT);

    // Initialize statistics
    for (int i = 0; i < DPDK_EXT_TX_PORT_COUNT; i++) {
        rte_atomic64_init(&dpdk_ext_tx_stats_per_port[i].tx_pkts);
        rte_atomic64_init(&dpdk_ext_tx_stats_per_port[i].tx_bytes);
    }

    // Initialize sequence counters
    memset(ext_tx_sequences, 0, sizeof(ext_tx_sequences));

    // Initialize port structures
    for (int i = 0; i < DPDK_EXT_TX_PORT_COUNT; i++) {
        struct dpdk_ext_tx_port *port = &dpdk_ext_tx_ports[i];
        port->port_id = ext_tx_configs[i].port_id;
        port->config = ext_tx_configs[i];
        port->mbuf_pool = mbuf_pools[i];  // Use index i, not port_id

        // Check for NULL mbuf_pool - port won't be able to TX without it
        if (port->mbuf_pool == NULL) {
            printf("  Port %u: mbuf_pool is NULL! External TX DISABLED for this port.\n",
                   port->port_id);
            port->initialized = false;
            continue;
        }
        port->initialized = true;

        printf("  Port %u: %d targets, mbuf_pool=%p\n",
               port->port_id, port->config.target_count, (void*)port->mbuf_pool);

        for (int t = 0; t < port->config.target_count; t++) {
            struct dpdk_ext_tx_target *target = &port->config.targets[t];
            printf("    Target %d: VLAN %u, VL-ID %u-%u, Rate %u Mbps\n",
                   t, target->vlan_id, target->vl_id_start,
                   target->vl_id_start + target->vl_id_count - 1,
                   target->rate_mbps);
        }
    }

    printf("=== DPDK External TX System Initialized ===\n\n");
    return 0;
}

// ==========================================
// TX WORKER
// ==========================================

int dpdk_ext_tx_worker(void *arg)
{
    struct dpdk_ext_tx_worker_params *params = (struct dpdk_ext_tx_worker_params *)arg;
    struct rte_mbuf *pkts[1];  // Single packet mode for smooth pacing
    bool first_burst = false;

    // VLAN header length
    const uint16_t l2_len = sizeof(struct rte_ether_hdr) + 4; // +4 for VLAN tag

#if IMIX_ENABLED
    // IMIX: Worker-specific offset for pattern rotation
    const uint8_t imix_offset = (uint8_t)((params->port_id * 4 + params->queue_id) % IMIX_PATTERN_SIZE);
    uint64_t imix_counter = 0;
#endif

    // Find port index and config for multi-target handling
    int port_idx = -1;
    struct dpdk_ext_tx_port_config *port_config = NULL;
    for (int i = 0; i < DPDK_EXT_TX_PORT_COUNT; i++) {
        if (ext_tx_configs[i].port_id == params->port_id) {
            port_idx = i;
            port_config = &ext_tx_configs[i];
            break;
        }
    }
    if (port_idx < 0 || port_config == NULL) {
        printf("Error: Port %u not found in ext config\n", params->port_id);
        return -1;
    }

    // Check for valid mbuf_pool
    if (!params->mbuf_pool) {
        printf("Error: mbuf_pool is NULL for port %u\n", params->port_id);
        return -1;
    }

    // Verify port has enough TX queues (we use queue 4)
    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(params->port_id, &dev_info);
    if (ret != 0) {
        printf("Error: Cannot get device info for port %u: %d\n", params->port_id, ret);
        return -1;
    }
    if (params->queue_id >= dev_info.nb_tx_queues) {
        printf("Error: Port %u only has %u TX queues, need queue %u\n",
               params->port_id, dev_info.nb_tx_queues, params->queue_id);
        return -1;
    }

    // Get PRBS cache (reuse existing per-port cache)
    uint8_t *prbs_cache_ext = get_prbs_cache_ext_for_port(params->port_id);
    if (!prbs_cache_ext) {
        printf("Error: PRBS cache not available for port %u\n", params->port_id);
        return -1;
    }

    // Multi-target state: round-robin through all targets
    uint16_t target_count = port_config->target_count;
    uint16_t current_target = 0;
    uint16_t *vl_offsets = calloc(target_count, sizeof(uint16_t));  // Per-target VL offset
    if (!vl_offsets) {
        printf("Error: Failed to allocate VL offset array\n");
        return -1;
    }

#if IMIX_ENABLED
    const uint64_t avg_pkt_size = IMIX_AVG_PACKET_SIZE;
#else
    const uint64_t avg_pkt_size = PACKET_SIZE_VLAN;
#endif

    // ==========================================
    // PACING SETUP
    // ==========================================
    uint64_t tsc_hz = rte_get_tsc_hz();

#if TOKEN_BUCKET_TX_ENABLED
    // TOKEN BUCKET: Each VL-IDX gets TB_PACKETS_PER_VL_PER_WINDOW packets per TB_WINDOW_MS
    // Total VL count across all targets
    uint16_t total_tb_vl_count = 0;
    for (int t = 0; t < target_count; t++)
        total_tb_vl_count += port_config->targets[t].vl_id_count;
    uint64_t packets_per_sec = (uint64_t)((double)total_tb_vl_count * TB_PACKETS_PER_VL_PER_WINDOW * 1000.0 / TB_WINDOW_MS);
    uint64_t delay_cycles = (packets_per_sec > 0) ? (tsc_hz / packets_per_sec) : tsc_hz;
#else
    // Precise calculation: rate_mbps -> bytes/sec -> packets/sec -> cycles/packet
    // IMIX: Use average packet size
    uint64_t bytes_per_sec = (uint64_t)params->rate_mbps * 125000ULL;  // Mbit/s -> bytes/s
    uint64_t packets_per_sec = bytes_per_sec / avg_pkt_size;
    uint64_t delay_cycles = (packets_per_sec > 0) ? (tsc_hz / packets_per_sec) : tsc_hz;
#endif

    // Inter-packet interval in microseconds (for debug)
    double inter_packet_us = (double)delay_cycles * 1000000.0 / (double)tsc_hz;

    // Stagger: Each port starts at a different time (switch buffer protection)
    // Port 2=0ms, Port 3=50ms, Port 4=100ms, Port 5=150ms
    uint64_t stagger_offset = port_idx * (tsc_hz / 20);  // 50ms per port

#if TOKEN_BUCKET_TX_ENABLED
    // Ext TX phase: Distribute workers evenly within the packet period
    // When stagger offset is an exact multiple of delay_cycles, all ext TX workers
    // fire in the same phase (e.g. 50ms / 62.5us = 800.0 integer -> all at once).
    // This fix shifts each worker to a 1/DPDK_EXT_TX_PORT_COUNT slice of the period.
    uint64_t ext_phase = port_idx * (delay_cycles / DPDK_EXT_TX_PORT_COUNT);
    uint64_t next_send_time = rte_get_tsc_cycles() + stagger_offset + ext_phase;
#else
    uint64_t next_send_time = rte_get_tsc_cycles() + stagger_offset;
#endif

    printf("ExtTX Worker started: Port %u Q%u, %u targets, Rate %u Mbps\n",
           params->port_id, params->queue_id, target_count, params->rate_mbps);
#if TOKEN_BUCKET_TX_ENABLED
    printf("  *** TOKEN BUCKET MODE - %u total VL-IDX, 1ms window ***\n", total_tb_vl_count);
#elif IMIX_ENABLED
    printf("  *** IMIX MODE + SMOOTH PACING ***\n");
    printf("  -> IMIX pattern: 100, 200, 400, 800, 1200x3, 1518x3 (avg=%lu bytes)\n", avg_pkt_size);
    printf("  -> Worker offset: %u (hybrid shuffle)\n", imix_offset);
#else
    printf("  *** SMOOTH PACING - traffic spread over 1 second ***\n");
#endif
    for (int t = 0; t < target_count; t++) {
        struct dpdk_ext_tx_target *target = &port_config->targets[t];
        printf("  Target %d: VLAN %u, VL-ID [%u..%u)\n",
               t, target->vlan_id, target->vl_id_start,
               target->vl_id_start + target->vl_id_count);
    }
    printf("  -> Pacing: %.1f us/pkt (%.0f pkt/s), stagger=%lums\n",
           inter_packet_us, (double)packets_per_sec, stagger_offset * 1000 / tsc_hz);

    uint64_t local_tx_pkts = 0;
    uint64_t local_tx_bytes = 0;
    const uint32_t STATS_FLUSH = 1024;

    while (!(*params->stop_flag))
    {
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
        // IMPORTANT: If we fall behind, use PHASE-PRESERVING SKIP (burst prevention)
        // Advance by N * delay_cycles instead of setting next_send_time = now
        // This preserves the inter-port phase offset
        if (next_send_time + delay_cycles < now) {
            uint64_t periods_behind = (now - next_send_time) / delay_cycles;
            next_send_time += periods_behind * delay_cycles;
        }
#else
        // If we fall behind, restart from now (accept packet loss)
        if (next_send_time + delay_cycles < now) {
            next_send_time = now;
        }
#endif
        next_send_time += delay_cycles;

        // Packet allocation - TIMING IS PRESERVED EVEN IF ALLOCATION FAILS
        pkts[0] = rte_pktmbuf_alloc(params->mbuf_pool);
        if (unlikely(pkts[0] == NULL)) {
            continue;
        }

        struct rte_mbuf *m = pkts[0];
        uint8_t *pkt = rte_pktmbuf_mtod(m, uint8_t *);

        // Get current target (round-robin between all targets)
        struct dpdk_ext_tx_target *target = &port_config->targets[current_target];

        // Current VL-ID (round-robin within target's range)
        uint16_t curr_vl = target->vl_id_start + vl_offsets[current_target];
        vl_offsets[current_target] = (vl_offsets[current_target] + 1) % target->vl_id_count;

        // Move to next target for next packet
        current_target = (current_target + 1) % target_count;

        // Peek sequence WITHOUT incrementing — only commit after successful send
        uint64_t seq = peek_ext_tx_sequence(port_idx, curr_vl);

        // ==========================================
        // BUILD ETHERNET HEADER
        // ==========================================
        struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt;

        // Source MAC: 02:00:00:00:00:PP (PP = port)
        eth->src_addr.addr_bytes[0] = 0x02;
        eth->src_addr.addr_bytes[1] = 0x00;
        eth->src_addr.addr_bytes[2] = 0x00;
        eth->src_addr.addr_bytes[3] = 0x00;
        eth->src_addr.addr_bytes[4] = 0x00;
        eth->src_addr.addr_bytes[5] = (uint8_t)params->port_id;

        // Destination MAC: 03:00:00:00:VV:VV (VV = VL-ID)
        eth->dst_addr.addr_bytes[0] = 0x03;
        eth->dst_addr.addr_bytes[1] = 0x00;
        eth->dst_addr.addr_bytes[2] = 0x00;
        eth->dst_addr.addr_bytes[3] = 0x00;
        eth->dst_addr.addr_bytes[4] = (uint8_t)(curr_vl >> 8);
        eth->dst_addr.addr_bytes[5] = (uint8_t)(curr_vl & 0xFF);

        // VLAN tag (802.1Q) - use target's VLAN ID
        eth->ether_type = rte_cpu_to_be_16(0x8100);
        uint8_t *vlan_tag = pkt + sizeof(struct rte_ether_hdr);
        uint16_t vlan_tci = target->vlan_id;
        *(uint16_t *)vlan_tag = rte_cpu_to_be_16(vlan_tci);
        *(uint16_t *)(vlan_tag + 2) = rte_cpu_to_be_16(0x0800); // IPv4

#if IMIX_ENABLED
        // IMIX: Get packet size from pattern
        uint16_t pkt_size = get_imix_packet_size(imix_counter, imix_offset);
        uint16_t prbs_len = calc_prbs_size(pkt_size);
        uint16_t payload_size = pkt_size - l2_len - sizeof(struct rte_ipv4_hdr) - sizeof(struct rte_udp_hdr);
        imix_counter++;
#else
        const uint16_t pkt_size = PACKET_SIZE_VLAN;
        const uint16_t prbs_len = NUM_PRBS_BYTES;
        const uint16_t payload_size = PAYLOAD_SIZE_VLAN;
#endif

        // ==========================================
        // BUILD IP HEADER (dynamic total_length)
        // ==========================================
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(pkt + l2_len);
        ip->version_ihl = 0x45;
        ip->type_of_service = 0;
        ip->total_length = rte_cpu_to_be_16(pkt_size - l2_len);
        ip->packet_id = 0;
        ip->fragment_offset = 0;
        ip->time_to_live = 1;
        ip->next_proto_id = IPPROTO_UDP;
        ip->src_addr = rte_cpu_to_be_32(0x0A000000); // 10.0.0.0
        // Destination IP: 224.224.VV.VV
        ip->dst_addr = rte_cpu_to_be_32((224U << 24) | (224U << 16) |
                                        ((curr_vl >> 8) << 8) | (curr_vl & 0xFF));
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);

        // ==========================================
        // BUILD UDP HEADER (dynamic dgram_len)
        // ==========================================
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(pkt + l2_len + sizeof(struct rte_ipv4_hdr));
        udp->src_port = rte_cpu_to_be_16(100);
        udp->dst_port = rte_cpu_to_be_16(100);
        udp->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_size);
        udp->dgram_cksum = 0;

        // ==========================================
        // BUILD PAYLOAD (Sequence + PRBS)
        // ==========================================
        uint8_t *payload = pkt + l2_len + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);

        // Sequence number (8 bytes)
        *(uint64_t *)payload = seq;

        // PRBS data (IMIX: offset is always calculated with MAX, size is dynamic)
#if IMIX_ENABLED
        uint64_t prbs_offset = (seq * (uint64_t)MAX_PRBS_BYTES) % PRBS_CACHE_SIZE;
        memcpy(payload + 8, prbs_cache_ext + prbs_offset, prbs_len);
#else
        uint64_t prbs_offset = (seq * (uint64_t)NUM_PRBS_BYTES) % PRBS_CACHE_SIZE;
        memcpy(payload + 8, prbs_cache_ext + prbs_offset, NUM_PRBS_BYTES);
#endif

        // Set packet length (dynamic)
        m->data_len = pkt_size;
        m->pkt_len = pkt_size;

        // Send single packet
        uint16_t nb_tx = rte_eth_tx_burst(params->port_id, params->queue_id, pkts, 1);

        if (!first_burst && nb_tx > 0) {
            printf("ExtTX: First packet on Port %u Q%u\n", params->port_id, params->queue_id);
            first_burst = true;
        }

        if (nb_tx > 0) {
            // Only increment sequence after successful send
            commit_ext_tx_sequence(port_idx, curr_vl);
            local_tx_pkts++;
            local_tx_bytes += pkt_size;
        } else {
            // TX queue full — drop packet, don't increment sequence (will be retried)
            rte_pktmbuf_free(pkts[0]);
        }

        // Flush stats periodically
        if (local_tx_pkts >= STATS_FLUSH) {
            rte_atomic64_add(&dpdk_ext_tx_stats_per_port[port_idx].tx_pkts, local_tx_pkts);
            rte_atomic64_add(&dpdk_ext_tx_stats_per_port[port_idx].tx_bytes, local_tx_bytes);
            local_tx_pkts = 0;
            local_tx_bytes = 0;
        }
    }

    // Final stats flush
    if (local_tx_pkts > 0) {
        rte_atomic64_add(&dpdk_ext_tx_stats_per_port[port_idx].tx_pkts, local_tx_pkts);
        rte_atomic64_add(&dpdk_ext_tx_stats_per_port[port_idx].tx_bytes, local_tx_bytes);
    }

    free(vl_offsets);
    printf("ExtTX Worker stopped: Port %u Q%u\n", params->port_id, params->queue_id);
    return 0;
}

// ==========================================
// START WORKERS (DEDICATED LCORES)
// ==========================================
// Each External TX port (0-3) gets a dedicated lcore and uses queue 4.
// Normal TX uses queues 0-3, External TX uses queue 4 (extra queue).
// This separation eliminates timing interference and provides stable TX rate.

int dpdk_ext_tx_start_workers(struct ports_config *ports_config, volatile bool *stop_flag)
{
    printf("\n=== Starting DPDK External TX Workers ===\n");
    printf("Mode: DEDICATED LCORES (queue 4 for external TX)\n");

    int worker_idx = 0;

    for (int p = 0; p < DPDK_EXT_TX_PORT_COUNT; p++)
    {
        uint16_t port_id = ext_tx_configs[p].port_id;
        struct dpdk_ext_tx_port *ext_port = &dpdk_ext_tx_ports[p];

        // Skip ports that weren't properly initialized (no mbuf_pool)
        if (!ext_port->initialized) {
            printf("  Port %u: Not initialized (no mbuf_pool), skipping\n", port_id);
            continue;
        }

        // Get dedicated external TX lcore from port config
        uint16_t ext_lcore = ports_config->ports[port_id].used_ext_tx_core;
        if (ext_lcore == 0)
        {
            printf("  Port %u: No dedicated ext TX lcore assigned, skipping\n", port_id);
            continue;
        }

        // Launch one worker per port that handles all targets (round-robin)
        // Worker uses queue 4 (extra dedicated queue for external TX)
        struct dpdk_ext_tx_worker_params *params = &ext_worker_params[worker_idx];
        params->port_id = port_id;
        params->queue_id = 4;  // Use queue 4 for external TX (queue 0-3 for normal TX)
        params->lcore_id = ext_lcore;
        params->mbuf_pool = ext_port->mbuf_pool;
        params->stop_flag = stop_flag;

        // For the worker, use SINGLE target's rate (all targets share the same port bandwidth)
        // Config has 220 Mbps per target, but since we round-robin through all targets
        // on the same port, the total rate should be 220 Mbps (not sum of all targets)
        params->rate_mbps = ext_port->config.targets[0].rate_mbps;

        // VL-ID range covers all targets
        params->vl_id_start = ext_port->config.targets[0].vl_id_start;
        uint16_t last_target = ext_port->config.target_count - 1;
        uint16_t vl_end = ext_port->config.targets[last_target].vl_id_start +
                          ext_port->config.targets[last_target].vl_id_count;
        params->vl_id_count = vl_end - params->vl_id_start;
        params->vlan_id = ext_port->config.targets[0].vlan_id;  // Will cycle through all VLANs

        printf("  Port %u: Lcore %u, Queue 4, Rate %u Mbps, VL-ID [%u..%u)\n",
               port_id, ext_lcore, params->rate_mbps,
               params->vl_id_start, params->vl_id_start + params->vl_id_count);

        int ret = rte_eal_remote_launch(dpdk_ext_tx_worker, params, ext_lcore);
        if (ret != 0)
        {
            printf("  ERROR: Failed to launch ext TX worker on lcore %u: %d\n", ext_lcore, ret);
            return ret;
        }

        worker_idx++;
    }

    printf("=== %d External TX Workers Started ===\n\n", worker_idx);
    return 0;
}

// ==========================================
// STATISTICS
// ==========================================

void dpdk_ext_tx_get_stats(uint16_t port_id, uint64_t *tx_pkts, uint64_t *tx_bytes)
{
    for (int i = 0; i < DPDK_EXT_TX_PORT_COUNT; i++) {
        if (ext_tx_configs[i].port_id == port_id) {
            *tx_pkts = rte_atomic64_read(&dpdk_ext_tx_stats_per_port[i].tx_pkts);
            *tx_bytes = rte_atomic64_read(&dpdk_ext_tx_stats_per_port[i].tx_bytes);
            return;
        }
    }
    *tx_pkts = 0;
    *tx_bytes = 0;
}

void dpdk_ext_tx_print_stats(void)
{
    static uint64_t prev_bytes[DPDK_EXT_TX_PORT_COUNT] = {0};
    static uint64_t last_time_ns = 0;

    uint64_t now_ns = rte_get_tsc_cycles() * 1000000000ULL / rte_get_tsc_hz();
    double elapsed_sec = 1.0;
    if (last_time_ns > 0) {
        elapsed_sec = (double)(now_ns - last_time_ns) / 1000000000.0;
        if (elapsed_sec < 0.1) elapsed_sec = 1.0;
    }
    last_time_ns = now_ns;

    printf("\n╔═══════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                         DPDK External TX Statistics                               ║\n");
    printf("╠════════╦═════════╦══════════════╦═══════════════╦═══════════╦═════════════════════╣\n");
    printf("║ Source ║  Dest   ║  TX Pkts     ║  TX Bytes     ║  TX Mbps  ║  VL-ID Range        ║\n");
    printf("╠════════╬═════════╬══════════════╬═══════════════╬═══════════╬═════════════════════╣\n");

    // Group by destination port
    uint64_t total_to_12_pkts = 0, total_to_12_bytes = 0;
    uint64_t total_to_13_pkts = 0, total_to_13_bytes = 0;
    double total_to_12_mbps = 0, total_to_13_mbps = 0;

    for (int i = 0; i < DPDK_EXT_TX_PORT_COUNT; i++) {
        uint64_t pkts = rte_atomic64_read(&dpdk_ext_tx_stats_per_port[i].tx_pkts);
        uint64_t bytes = rte_atomic64_read(&dpdk_ext_tx_stats_per_port[i].tx_bytes);

        uint64_t bytes_delta = bytes - prev_bytes[i];
        double mbps = (bytes_delta * 8.0) / (elapsed_sec * 1000000.0);
        prev_bytes[i] = bytes;

        // Get VL-ID range from config
        uint16_t vl_start = ext_tx_configs[i].targets[0].vl_id_start;
        uint16_t vl_end = ext_tx_configs[i].targets[ext_tx_configs[i].target_count - 1].vl_id_start +
                          ext_tx_configs[i].targets[ext_tx_configs[i].target_count - 1].vl_id_count;

        printf("║  P%-3u  ║  P%-4u  ║ %12lu ║ %13lu ║ %9.2f ║  %5u - %-5u      ║\n",
               ext_tx_configs[i].port_id, ext_tx_configs[i].dest_port,
               pkts, bytes, mbps, vl_start, vl_end - 1);

        if (ext_tx_configs[i].dest_port == 12) {
            total_to_12_pkts += pkts;
            total_to_12_bytes += bytes;
            total_to_12_mbps += mbps;
        } else if (ext_tx_configs[i].dest_port == 13) {
            total_to_13_pkts += pkts;
            total_to_13_bytes += bytes;
            total_to_13_mbps += mbps;
        }
    }

    printf("╠════════╩═════════╬══════════════╬═══════════════╬═══════════╬═════════════════════╣\n");
    printf("║  → Port 12 Total ║ %12lu ║ %13lu ║ %9.2f ║  (from P2,3,4,5)    ║\n",
           total_to_12_pkts, total_to_12_bytes, total_to_12_mbps);
    printf("║  → Port 13 Total ║ %12lu ║ %13lu ║ %9.2f ║  (from P0,6)        ║\n",
           total_to_13_pkts, total_to_13_bytes, total_to_13_mbps);
    printf("╚══════════════════╩══════════════╩═══════════════╩═══════════╩═════════════════════╝\n");
}

#endif /* DPDK_EXT_TX_ENABLED */