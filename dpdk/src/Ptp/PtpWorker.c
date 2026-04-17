/**
 * PTP Worker
 *
 * Main worker loop for PTP slave operation.
 * One worker runs per port, handling all 4 VLAN sessions.
 *
 * Worker responsibilities:
 *   1. Poll PTP RX queue for incoming Sync and Delay_Resp packets
 *   2. Process received packets and update session state
 *   3. Run state machine tick for each session
 *   4. Send Delay_Req packets when required
 */

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <poll.h>
#include <errno.h>

#include "PtpTypes.h"
#include "PtpSlave.h"
#include "Config.h"

// Maximum packets to process per poll
#define PTP_RX_BURST_SIZE 32

// Global PTP context
static ptp_context_t g_ptp_ctx;

// Worker running flag (set to false to stop workers)
static volatile bool ptp_workers_running = false;

/**
 * Get global PTP context
 */
ptp_context_t *ptp_get_context(void)
{
    return &g_ptp_ctx;
}

/**
 * Get PTP port by port ID
 */
ptp_port_t *ptp_get_port(uint16_t port_id)
{
    if (port_id >= PTP_MAX_PORTS)
        return NULL;

    ptp_port_t *port = &g_ptp_ctx.ports[port_id];
    if (!port->enabled)
        return NULL;

    return port;
}

/**
 * Get PTP session by port and VLAN
 */
ptp_session_t *ptp_get_session(uint16_t port_id, uint16_t vlan_id)
{
    ptp_port_t *port = ptp_get_port(port_id);
    if (!port)
        return NULL;

    for (int i = 0; i < port->session_count; i++) {
        if (port->sessions[i].rx_vlan_id == vlan_id ||
            port->sessions[i].tx_vlan_id == vlan_id) {
            return &port->sessions[i];
        }
    }

    return NULL;
}

/**
 * Find session by VLAN ID within a port
 */
static ptp_session_t *find_session_by_vlan(ptp_port_t *port, uint16_t vlan_id)
{
    for (int i = 0; i < port->session_count; i++) {
        if (port->sessions[i].rx_vlan_id == vlan_id) {
            return &port->sessions[i];
        }
    }
    return NULL;
}

/**
 * PTP worker main loop
 */
int ptp_worker_main(void *arg)
{
    ptp_port_t *port = (ptp_port_t *)arg;
    if (!port || !port->enabled) {
        fprintf(stderr, "PTP Worker: Invalid port argument\n");
        return -1;
    }

    uint16_t port_id = port->port_id;
    struct rte_mbuf *rx_mbufs[PTP_RX_BURST_SIZE];

    printf("PTP Worker: Starting on lcore %u for port %u (%d sessions)\n",
           rte_lcore_id(), port_id, port->session_count);

    // Debug counters
    uint64_t total_rx = 0;
    uint64_t ptp_rx = 0;
    uint64_t non_ptp_rx = 0;
    uint64_t msg_type_count[16] = {0};  // Count per PTP message type
    uint64_t last_debug_tsc = rte_rdtsc();
    uint64_t debug_interval_tsc = rte_get_tsc_hz() * 5; // 5 seconds

    while (ptp_workers_running) {
        uint64_t current_tsc = rte_rdtsc();

        // Poll PTP RX queue
        uint16_t nb_rx = rte_eth_rx_burst(port_id, PTP_RX_QUEUE_ID,
                                          rx_mbufs, PTP_RX_BURST_SIZE);

        total_rx += nb_rx;

        // Process received packets
        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *mbuf = rx_mbufs[i];
            uint64_t rx_tsc = rte_rdtsc(); // Software RX timestamp

            // Debug: Print raw packet header for any packet on Q5
            static uint64_t raw_pkt_print_count = 0;
            if (raw_pkt_print_count < 20) {
                uint8_t *pkt = rte_pktmbuf_mtod(mbuf, uint8_t *);
                uint16_t len = rte_pktmbuf_data_len(mbuf);
                printf("PTP Q5 Raw [Port%u len=%u]: "
                       "Dst=%02X:%02X:%02X:%02X:%02X:%02X "
                       "Src=%02X:%02X:%02X:%02X:%02X:%02X "
                       "Type=0x%02X%02X",
                       port_id, len,
                       pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5],
                       pkt[6], pkt[7], pkt[8], pkt[9], pkt[10], pkt[11],
                       pkt[12], pkt[13]);
                // If VLAN tagged (0x8100), print VLAN info
                if (pkt[12] == 0x81 && pkt[13] == 0x00 && len >= 18) {
                    uint16_t vlan_tci = (pkt[14] << 8) | pkt[15];
                    uint16_t inner_type = (pkt[16] << 8) | pkt[17];
                    printf(" VLAN=%u Inner=0x%04X", vlan_tci & 0x0FFF, inner_type);
                }
                printf("\n");
                raw_pkt_print_count++;
            }

            // Check if it's a PTP packet
            if (!ptp_is_ptp_packet(mbuf)) {
                non_ptp_rx++;
                rte_pktmbuf_free(mbuf);
                continue;
            }

            ptp_rx++;

            // Count message types
            int msg_type = ptp_get_msg_type(mbuf);
            if (msg_type >= 0 && msg_type < 16) {
                msg_type_count[msg_type]++;
            }

            // Get VLAN ID to find the right session
            uint16_t vlan_id = ptp_get_vlan_id(mbuf);
            ptp_session_t *session = find_session_by_vlan(port, vlan_id);

            if (session) {
                // Process the PTP packet
                ptp_packet_process(session, mbuf, rx_tsc);
            } else {
                // Debug: No session for this VLAN
                static uint64_t no_session_count = 0;
                if (no_session_count < 10) {
                    printf("PTP: No session for Port%u VLAN=%u\n", port_id, vlan_id);
                    no_session_count++;
                }
            }

            rte_pktmbuf_free(mbuf);
        }

        // Run state machine for each session
        for (int i = 0; i < port->session_count; i++) {
            ptp_session_t *session = &port->sessions[i];
            ptp_state_machine_tick(session, port, current_tsc);
        }

        // Debug output every 5 seconds (always print, even if total=0)
        if (current_tsc - last_debug_tsc > debug_interval_tsc) {
            printf("PTP Debug Port %u Q5: total=%lu ptp=%lu non_ptp=%lu "
                   "[Sync=%lu DelReq=%lu DelResp=%lu]\n",
                   port_id, total_rx, ptp_rx, non_ptp_rx,
                   msg_type_count[PTP_MSG_SYNC],
                   msg_type_count[PTP_MSG_DELAY_REQ],
                   msg_type_count[PTP_MSG_DELAY_RESP]);
            last_debug_tsc = current_tsc;
        }

        rte_pause();
    }

    printf("PTP Worker: Stopping on lcore %u for port %u\n",
           rte_lcore_id(), port_id);

    return 0;
}

/**
 * Create mbuf pool for PTP TX
 */
static struct rte_mempool *create_ptp_mbuf_pool(uint16_t port_id)
{
    char pool_name[32];
    snprintf(pool_name, sizeof(pool_name), "ptp_pool_%u", port_id);

    struct rte_mempool *pool = rte_pktmbuf_pool_create(
        pool_name,
        PTP_MBUF_POOL_SIZE,
        PTP_MBUF_CACHE_SIZE,
        0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        rte_socket_id());

    if (!pool) {
        fprintf(stderr, "PTP: Failed to create mbuf pool for port %u\n", port_id);
    }

    return pool;
}

/**
 * Initialize PTP subsystem
 */
int ptp_init(void)
{
    printf("PTP: Initializing PTP slave subsystem...\n");

    memset(&g_ptp_ctx, 0, sizeof(g_ptp_ctx));

    // Get TSC frequency
    g_ptp_ctx.tsc_hz = rte_get_tsc_hz();
    printf("PTP: TSC frequency: %lu Hz\n", g_ptp_ctx.tsc_hz);

    // Get local MAC address (from port 0)
    rte_eth_macaddr_get(0, &g_ptp_ctx.local_mac);
    printf("PTP: Local MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           g_ptp_ctx.local_mac.addr_bytes[0],
           g_ptp_ctx.local_mac.addr_bytes[1],
           g_ptp_ctx.local_mac.addr_bytes[2],
           g_ptp_ctx.local_mac.addr_bytes[3],
           g_ptp_ctx.local_mac.addr_bytes[4],
           g_ptp_ctx.local_mac.addr_bytes[5]);

    g_ptp_ctx.initialized = true;
    return 0;
}

/**
 * Configure PTP sessions with split TX/RX port support
 * Sessions are grouped by RX port, each session can have a different TX port
 */
int ptp_configure_split_sessions(const struct ptp_session_config *sessions,
                                  uint16_t session_count)
{
    if (!g_ptp_ctx.initialized) {
        fprintf(stderr, "PTP: Not initialized\n");
        return -1;
    }

    printf("PTP: Configuring %u sessions with split TX/RX port support\n", session_count);

    for (int i = 0; i < session_count; i++) {
        const struct ptp_session_config *cfg = &sessions[i];

        // Validate port IDs
        if (cfg->rx_port_id >= PTP_MAX_PORTS || cfg->tx_port_id >= PTP_MAX_PORTS) {
            fprintf(stderr, "PTP: Invalid port IDs (RX=%u, TX=%u)\n",
                    cfg->rx_port_id, cfg->tx_port_id);
            return -1;
        }

        // Get or initialize RX port
        ptp_port_t *rx_port = &g_ptp_ctx.ports[cfg->rx_port_id];

        bool is_raw = PTP_IS_RAW_SOCKET_PORT(cfg->rx_port_id);

        if (!rx_port->enabled) {
            rx_port->port_id = cfg->rx_port_id;
            rx_port->is_raw_socket = is_raw;
            rx_port->raw_rx_fd = -1;
            rx_port->raw_tx_fd = -1;
            rx_port->raw_worker_running = false;

            if (!is_raw) {
                // DPDK port: create mbuf pool
                rx_port->tx_mbuf_pool = create_ptp_mbuf_pool(cfg->rx_port_id);
                if (!rx_port->tx_mbuf_pool) {
                    fprintf(stderr, "PTP: Failed to create mbuf pool for RX port %u\n",
                            cfg->rx_port_id);
                    return -1;
                }
            }

            rx_port->enabled = true;
            g_ptp_ctx.port_count++;
        }

        // Create mbuf pool for DPDK ports if not already created
        if (!is_raw && !rx_port->tx_mbuf_pool) {
            rx_port->tx_mbuf_pool = create_ptp_mbuf_pool(cfg->rx_port_id);
            if (!rx_port->tx_mbuf_pool) {
                fprintf(stderr, "PTP: Failed to create mbuf pool for RX port %u\n",
                        cfg->rx_port_id);
                return -1;
            }
        }

        // Check if we have room for another session
        if (rx_port->session_count >= PTP_SESSIONS_PER_PORT) {
            fprintf(stderr, "PTP: RX port %u already has max sessions (%d)\n",
                    cfg->rx_port_id, PTP_SESSIONS_PER_PORT);
            return -1;
        }

        // Initialize session with split TX/RX ports
        uint8_t session_idx = rx_port->session_count;
        ptp_session_init(&rx_port->sessions[session_idx],
                        cfg->rx_port_id,   // RX port (session owner)
                        cfg->tx_port_id,   // TX port for Delay_Req
                        session_idx,
                        cfg->rx_vlan,
                        cfg->tx_vlan,
                        cfg->tx_vl_idx,
                        cfg->rx_vl_idx);

        rx_port->session_count++;

        printf("PTP: Session %d configured - RX Port %u (VLAN %u, RespVL %u) / TX Port %u (VLAN %u, ReqVL %u)\n",
               i, cfg->rx_port_id, cfg->rx_vlan, cfg->rx_vl_idx,
               cfg->tx_port_id, cfg->tx_vlan, cfg->tx_vl_idx);
    }

    return 0;
}

/**
 * Configure PTP for a specific port using session config (legacy interface)
 * Note: This assumes TX and RX ports are the same
 */
int ptp_port_configure_sessions(uint16_t port_id,
                                const struct ptp_session_config *sessions,
                                uint16_t session_count)
{
    if (!g_ptp_ctx.initialized) {
        fprintf(stderr, "PTP: Not initialized\n");
        return -1;
    }

    if (port_id >= PTP_MAX_PORTS) {
        fprintf(stderr, "PTP: Invalid port_id %u\n", port_id);
        return -1;
    }

    if (session_count > PTP_SESSIONS_PER_PORT) {
        fprintf(stderr, "PTP: Too many sessions %u (max %d)\n",
                session_count, PTP_SESSIONS_PER_PORT);
        return -1;
    }

    ptp_port_t *port = &g_ptp_ctx.ports[port_id];

    // Create mbuf pool for TX
    port->tx_mbuf_pool = create_ptp_mbuf_pool(port_id);
    if (!port->tx_mbuf_pool) {
        return -1;
    }

    port->port_id = port_id;
    port->session_count = session_count;

    // Initialize sessions (legacy: same TX/RX port)
    for (int i = 0; i < session_count; i++) {
        // Use rx_port_id from config if available, otherwise use port_id for both
        uint16_t rx_port = (sessions[i].rx_port_id != 0) ? sessions[i].rx_port_id : port_id;
        uint16_t tx_port = (sessions[i].tx_port_id != 0) ? sessions[i].tx_port_id : port_id;

        ptp_session_init(&port->sessions[i],
                        rx_port, tx_port, i,
                        sessions[i].rx_vlan, sessions[i].tx_vlan,
                        sessions[i].tx_vl_idx,
                        sessions[i].rx_vl_idx);
    }

    port->enabled = true;
    g_ptp_ctx.port_count++;

    printf("PTP: Configured port %u with %u sessions\n", port_id, session_count);
    return 0;
}

/**
 * Configure PTP for a specific port (legacy interface, uses default VL-IDX)
 * Note: This assumes TX and RX ports are the same (port_id)
 */
int ptp_port_configure(uint16_t port_id,
                       const uint16_t *rx_vlans,
                       const uint16_t *tx_vlans,
                       uint16_t vlan_count)
{
    // Convert to session config format (same TX/RX port for legacy)
    struct ptp_session_config sessions[PTP_SESSIONS_PER_PORT];
    for (int i = 0; i < vlan_count && i < PTP_SESSIONS_PER_PORT; i++) {
        sessions[i].rx_port_id = port_id;  // Same port for legacy
        sessions[i].tx_port_id = port_id;  // Same port for legacy
        sessions[i].rx_vlan = rx_vlans[i];
        sessions[i].tx_vlan = tx_vlans[i];
        sessions[i].tx_vl_idx = 0;  // Default: not used
    }
    return ptp_port_configure_sessions(port_id, sessions, vlan_count);
}

/**
 * Assign lcore to PTP port
 */
int ptp_assign_lcore(uint16_t port_id, uint16_t lcore_id)
{
    if (port_id >= PTP_MAX_PORTS) {
        fprintf(stderr, "PTP: Invalid port_id %u\n", port_id);
        return -1;
    }

    ptp_port_t *port = &g_ptp_ctx.ports[port_id];
    if (!port->enabled) {
        fprintf(stderr, "PTP: Port %u not configured\n", port_id);
        return -1;
    }

    port->ptp_lcore_id = lcore_id;
    printf("PTP: Assigned lcore %u to port %u\n", lcore_id, port_id);
    return 0;
}

/**
 * Start PTP workers
 */
int ptp_start(void)
{
    if (!g_ptp_ctx.initialized) {
        fprintf(stderr, "PTP: Not initialized\n");
        return -1;
    }

    // Install flow rules
    printf("PTP: Installing flow rules...\n");
    ptp_flow_rules_install_all();

    // Start workers
    ptp_workers_running = true;

    for (int i = 0; i < PTP_MAX_PORTS; i++) {
        ptp_port_t *port = &g_ptp_ctx.ports[i];
        if (!port->enabled)
            continue;

        if (port->is_raw_socket) {
            // Raw socket port: launch pthread worker
            if (port->raw_rx_fd < 0) {
                fprintf(stderr, "PTP: Raw socket not initialized for port %u, skipping\n",
                        port->port_id);
                continue;
            }

            port->raw_worker_running = true;
            printf("PTP: Launching raw socket worker thread for port %u\n", port->port_id);

            int ret = pthread_create(&port->raw_worker_thread, NULL,
                                     ptp_raw_worker_main, port);
            if (ret != 0) {
                fprintf(stderr, "PTP: Failed to create raw socket worker thread for port %u\n",
                        port->port_id);
                port->raw_worker_running = false;
            }
        } else {
            // DPDK port: launch on lcore
            if (port->ptp_lcore_id == 0)
                continue;

            printf("PTP: Launching worker on lcore %u for port %u\n",
                   port->ptp_lcore_id, port->port_id);

            int ret = rte_eal_remote_launch(ptp_worker_main, port,
                                            port->ptp_lcore_id);
            if (ret != 0) {
                fprintf(stderr, "PTP: Failed to launch worker on lcore %u\n",
                        port->ptp_lcore_id);
            }
        }
    }

    g_ptp_ctx.running = true;
    printf("PTP: Started\n");
    return 0;
}

/**
 * Stop PTP workers
 */
void ptp_stop(void)
{
    if (!g_ptp_ctx.running)
        return;

    printf("PTP: Stopping workers...\n");
    ptp_workers_running = false;

    // Wait for workers to stop
    for (int i = 0; i < PTP_MAX_PORTS; i++) {
        ptp_port_t *port = &g_ptp_ctx.ports[i];
        if (!port->enabled)
            continue;

        if (port->is_raw_socket) {
            // Stop raw socket worker thread
            port->raw_worker_running = false;
            if (port->raw_worker_thread) {
                pthread_join(port->raw_worker_thread, NULL);
                port->raw_worker_thread = 0;
            }
        } else if (port->ptp_lcore_id != 0) {
            rte_eal_wait_lcore(port->ptp_lcore_id);
        }
    }

    // Remove flow rules
    ptp_flow_rules_remove_all();

    g_ptp_ctx.running = false;
    printf("PTP: Stopped\n");
}

/**
 * Cleanup PTP subsystem
 */
void ptp_cleanup(void)
{
    ptp_stop();

    // Free mbuf pools and close raw sockets
    for (int i = 0; i < PTP_MAX_PORTS; i++) {
        ptp_port_t *port = &g_ptp_ctx.ports[i];
        if (port->tx_mbuf_pool) {
            rte_mempool_free(port->tx_mbuf_pool);
            port->tx_mbuf_pool = NULL;
        }
        if (port->raw_rx_fd >= 0) {
            close(port->raw_rx_fd);
            port->raw_rx_fd = -1;
        }
        if (port->raw_tx_fd >= 0) {
            close(port->raw_tx_fd);
            port->raw_tx_fd = -1;
        }
    }

    g_ptp_ctx.initialized = false;
    printf("PTP: Cleanup complete\n");
}

/**
 * Get statistics for all PTP sessions
 */
void ptp_get_stats(ptp_session_stats_t *stats, uint8_t *count)
{
    uint8_t idx = 0;

    for (int p = 0; p < PTP_MAX_PORTS; p++) {
        ptp_port_t *port = &g_ptp_ctx.ports[p];
        if (!port->enabled)
            continue;

        for (int s = 0; s < port->session_count; s++) {
            if (idx < PTP_MAX_SESSIONS) {
                ptp_session_get_stats(&port->sessions[s], &stats[idx]);
                idx++;
            }
        }
    }

    *count = idx;
}

/**
 * Get statistics for a specific port
 */
void ptp_get_port_stats(uint16_t port_id,
                        ptp_session_stats_t *stats,
                        uint8_t *count)
{
    ptp_port_t *port = ptp_get_port(port_id);
    if (!port) {
        *count = 0;
        return;
    }

    for (int i = 0; i < port->session_count; i++) {
        ptp_session_get_stats(&port->sessions[i], &stats[i]);
    }

    *count = port->session_count;
}

/**
 * Print PTP statistics
 * Note: Uses dashes (-) for separators to avoid interfering with main stats
 * grep pattern that looks for '==========' to find stats boundaries
 */
void ptp_print_stats(void)
{
    printf("\n--- PTP Statistics ---\n");
    printf("%-6s %-6s %-7s %-7s %-12s %12s %12s %8s %8s %8s %6s\n",
           "Port", "VLAN", "ReqVL", "RespVL", "State", "Offset(ns)", "Delay(ns)",
           "Sync RX", "Req TX", "Resp RX", "Synced");
    printf("------------------------------------------------------------------------------------------------------\n");

    /* Collect all sessions with their DTN port numbers, then sort by DTN port */
    struct {
        int dtn_port;
        ptp_session_t *sess;
    } entries[PTP_MAX_SESSIONS];
    int entry_count = 0;

    for (int p = 0; p < PTP_MAX_PORTS; p++) {
        ptp_port_t *port = &g_ptp_ctx.ports[p];
        if (!port->enabled)
            continue;

        for (int s = 0; s < port->session_count; s++) {
            ptp_session_t *sess = &port->sessions[s];

            /* Calculate DTN port number:
             * VLAN 225-256 -> DTN port 0-31
             * Physical port 12 -> DTN port 32
             * Physical port 13 -> DTN port 33 */
            int dtn_port;
            if (sess->rx_vlan_id >= 225 && sess->rx_vlan_id <= 256)
                dtn_port = sess->rx_vlan_id - 225;
            else if (sess->port_id == 12)
                dtn_port = 32;
            else if (sess->port_id == 13)
                dtn_port = 33;
            else
                dtn_port = -1;

            entries[entry_count].dtn_port = dtn_port;
            entries[entry_count].sess = sess;
            entry_count++;
        }
    }

    /* Sort entries by DTN port number (simple insertion sort) */
    for (int i = 1; i < entry_count; i++) {
        typeof(entries[0]) tmp = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].dtn_port > tmp.dtn_port) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = tmp;
    }

    /* Print sorted entries */
    for (int i = 0; i < entry_count; i++) {
        ptp_session_t *sess = entries[i].sess;
        printf("%-6d %-6u %-7u %-7u %-12s %12ld %12ld %8lu %8lu %8lu %6s\n",
               entries[i].dtn_port,
               sess->rx_vlan_id,
               sess->tx_vl_idx,
               sess->rx_vl_idx,
               ptp_state_to_str(sess->state),
               sess->offset_ns,
               sess->delay_ns,
               sess->sync_rx_count,
               sess->delay_req_tx_count,
               sess->delay_resp_rx_count,
               sess->is_synced ? "YES" : "NO");
    }
    printf("------------------------------------------------------------------------------------------------------\n");

    // Print Queue 5 hardware stats per port (DPDK ports only)
    printf("Q5 HW Stats: ");
    for (int p = 0; p < PTP_MAX_PORTS; p++) {
        ptp_port_t *port = &g_ptp_ctx.ports[p];
        if (!port->enabled || port->is_raw_socket)
            continue;

        struct rte_eth_stats stats;
        if (rte_eth_stats_get(port->port_id, &stats) == 0) {
            printf("P%u=%lu ", port->port_id, stats.q_ipackets[5]);
        }
    }
    printf("\n\n");
}

/**
 * Reset all PTP statistics
 */
void ptp_reset_stats(void)
{
    for (int p = 0; p < PTP_MAX_PORTS; p++) {
        ptp_port_t *port = &g_ptp_ctx.ports[p];
        if (!port->enabled)
            continue;

        for (int s = 0; s < port->session_count; s++) {
            ptp_session_reset_stats(&port->sessions[s]);
        }
    }
}

// ==========================================
// RAW SOCKET PTP (Port 12/13)
// ==========================================

/**
 * Initialize raw socket for PTP on a given interface
 * Creates AF_PACKET socket filtered for PTP EtherType (0x88F7)
 */
int ptp_raw_socket_init(ptp_port_t *port, const char *iface_name)
{
    if (!port || !iface_name) {
        fprintf(stderr, "PTP RAW: Invalid arguments\n");
        return -1;
    }

    // Get interface index
    unsigned int if_index = if_nametoindex(iface_name);
    if (if_index == 0) {
        fprintf(stderr, "PTP RAW: Interface '%s' not found\n", iface_name);
        return -1;
    }

    // Check interface is UP
    int check_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (check_fd >= 0) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);
        if (ioctl(check_fd, SIOCGIFFLAGS, &ifr) == 0) {
            if (!(ifr.ifr_flags & IFF_UP)) {
                fprintf(stderr, "PTP RAW: Interface '%s' is DOWN, cannot init PTP\n", iface_name);
                close(check_fd);
                return -1;
            }
            printf("PTP RAW: Interface '%s' is UP (flags=0x%x)\n", iface_name, ifr.ifr_flags);
        } else {
            fprintf(stderr, "PTP RAW: Warning: Could not check interface '%s' flags: %s\n",
                    iface_name, strerror(errno));
        }
        close(check_fd);
    }

    port->raw_if_index = if_index;
    snprintf(port->raw_iface_name, sizeof(port->raw_iface_name), "%s", iface_name);

    // Create RX socket - filtered for PTP EtherType
    int rx_fd = socket(AF_PACKET, SOCK_RAW, htons(PTP_ETHERTYPE));
    if (rx_fd < 0) {
        fprintf(stderr, "PTP RAW: Failed to create RX socket for %s: %s\n",
                iface_name, strerror(errno));
        return -1;
    }

    // Bind RX socket to interface
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_index;
    sll.sll_protocol = htons(PTP_ETHERTYPE);

    if (bind(rx_fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        fprintf(stderr, "PTP RAW: Failed to bind RX socket to %s: %s\n",
                iface_name, strerror(errno));
        close(rx_fd);
        return -1;
    }

    // Set promiscuous mode
    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = if_index;
    mreq.mr_type = PACKET_MR_PROMISC;
    setsockopt(rx_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Create TX socket
    int tx_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (tx_fd < 0) {
        fprintf(stderr, "PTP RAW: Failed to create TX socket for %s: %s\n",
                iface_name, strerror(errno));
        close(rx_fd);
        return -1;
    }

    // Bind TX socket to interface
    struct sockaddr_ll tx_sll;
    memset(&tx_sll, 0, sizeof(tx_sll));
    tx_sll.sll_family = AF_PACKET;
    tx_sll.sll_ifindex = if_index;
    tx_sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(tx_fd, (struct sockaddr *)&tx_sll, sizeof(tx_sll)) < 0) {
        fprintf(stderr, "PTP RAW: Failed to bind TX socket to %s: %s\n",
                iface_name, strerror(errno));
        close(rx_fd);
        close(tx_fd);
        return -1;
    }

    port->raw_rx_fd = rx_fd;
    port->raw_tx_fd = tx_fd;

    printf("PTP RAW: Initialized port %u on %s (ifindex=%u, rx_fd=%d, tx_fd=%d)\n",
           port->port_id, iface_name, if_index, rx_fd, tx_fd);

    return 0;
}

/**
 * Raw socket PTP worker thread
 *
 * Polls the raw socket for PTP packets (Sync, Delay_Resp)
 * and runs the state machine which sends Delay_Req via raw socket.
 *
 * Since Port 12/13 have no VLAN and 1 session per port,
 * all PTP packets on the interface go to the single session.
 */
void *ptp_raw_worker_main(void *arg)
{
    ptp_port_t *port = (ptp_port_t *)arg;
    if (!port || !port->is_raw_socket || port->raw_rx_fd < 0) {
        fprintf(stderr, "PTP RAW Worker: Invalid port argument\n");
        return NULL;
    }

    printf("PTP RAW Worker: Starting for port %u (%s, %d sessions)\n",
           port->port_id, port->raw_iface_name, port->session_count);

    uint8_t rx_buf[256];  // PTP packets are small (<128 bytes)
    struct pollfd pfd;
    pfd.fd = port->raw_rx_fd;
    pfd.events = POLLIN;

    // Debug counters
    uint64_t total_rx = 0;
    uint64_t ptp_rx = 0;
    uint64_t outgoing_skipped = 0;
    uint64_t last_debug_tsc = rte_rdtsc();
    uint64_t debug_interval_tsc = rte_get_tsc_hz() * 5; // 5 seconds

    while (port->raw_worker_running && ptp_workers_running) {
        uint64_t current_tsc = rte_rdtsc();

        // Poll for incoming PTP packets (1ms timeout)
        int ret = poll(&pfd, 1, 1);

        if (ret > 0 && (pfd.revents & POLLIN)) {
            // Use recvfrom to get sockaddr_ll with pkttype info
            struct sockaddr_ll sll_rx;
            socklen_t sll_len = sizeof(sll_rx);
            ssize_t len = recvfrom(port->raw_rx_fd, rx_buf, sizeof(rx_buf), 0,
                                   (struct sockaddr *)&sll_rx, &sll_len);
            if (len > 0) {
                // Skip our own outgoing TX packets (kernel marks them as PACKET_OUTGOING)
                if (sll_rx.sll_pkttype == PACKET_OUTGOING) {
                    outgoing_skipped++;
                } else {
                    uint64_t rx_tsc = rte_rdtsc();
                    total_rx++;

                    // Check if it's a PTP packet (EtherType 0x88F7)
                    if (len >= 14) {
                        uint16_t ether_type = ((uint16_t)rx_buf[12] << 8) | rx_buf[13];
                        if (ether_type == PTP_ETHERTYPE) {
                            ptp_rx++;

                            // Since no VLAN and 1 session per port, use first session
                            if (port->session_count > 0) {
                                ptp_session_t *session = &port->sessions[0];
                                ptp_raw_packet_process(session, rx_buf, (uint32_t)len, rx_tsc);
                            }
                        }
                    }
                }
            }
        }

        // Run state machine for each session
        for (int i = 0; i < port->session_count; i++) {
            ptp_session_t *session = &port->sessions[i];
            ptp_state_machine_tick(session, port, current_tsc);
        }

        // Debug output every 5 seconds
        if (current_tsc - last_debug_tsc > debug_interval_tsc) {
            printf("PTP RAW Debug Port %u (%s): total=%lu ptp=%lu outgoing_skip=%lu\n",
                   port->port_id, port->raw_iface_name, total_rx, ptp_rx, outgoing_skipped);
            last_debug_tsc = current_tsc;
        }
    }

    printf("PTP RAW Worker: Stopping for port %u\n", port->port_id);
    return NULL;
}