#include "Helpers.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <rte_ethdev.h>
#include <rte_atomic.h>

#include "Config.h"
#include "TxRxManager.h"  // for rx_stats_per_port
#include "DpdkExternalTx.h" // for External TX stats
#include "RawSocketPort.h"  // for reset_raw_socket_stats

// Daemon mode flag - when true, ANSI escape codes are disabled
bool g_daemon_mode = false;

void helper_set_daemon_mode(bool enabled) {
    g_daemon_mode = enabled;
}

// Helper functions
static inline double to_gbps(uint64_t bytes) {
    return (bytes * 8.0) / 1e9;
}

void helper_reset_stats(const struct ports_config *ports_config,
                        uint64_t prev_tx_bytes[], uint64_t prev_rx_bytes[])
{
    // Reset HW statistics and zero out prev_* counters
    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        uint16_t port_id = ports_config->ports[i].port_id;
        rte_eth_stats_reset(port_id);
        prev_tx_bytes[port_id] = 0;
        prev_rx_bytes[port_id] = 0;
    }

    // Reset RX validation statistics (PRBS)
    init_rx_stats();

#if STATS_MODE_DTN
    init_dtn_stats();
#endif

    // Reset raw socket and global sequence tracking
    reset_raw_socket_stats();
}

#if STATS_MODE_DTN
// ==========================================
// DTN PORT-BASED STATISTICS TABLE
// ==========================================
// 34 rows: DTN Port 0-31 (DPDK) + DTN Port 32 (Port12) + DTN Port 33 (Port13)
// Columns: TX Pkts/Bytes/Gbps | RX Pkts/Bytes/Gbps | Good/Bad/Lost/BitErr/BER
//
// DTN TX (DTN→Server) = Server RX = HW q_ipackets[queue]
// DTN RX (Server→DTN) = Server TX = HW q_opackets[queue]
// PRBS = dtn_stats[dtn_port] (from RX worker)

// Per-queue prev bytes (for per-DTN-port delta calculation)
// [dtn_port][0=tx_bytes, 1=rx_bytes]
static uint64_t dtn_prev_tx_bytes[DTN_PORT_COUNT];
static uint64_t dtn_prev_rx_bytes[DTN_PORT_COUNT];

static void helper_print_dtn_stats(const struct ports_config *ports_config,
                                   bool warmup_complete, unsigned loop_count,
                                   unsigned test_time)
{
    // Clear the screen
    if (!g_daemon_mode) {
        printf("\033[2J\033[H");
    } else {
        printf("\n========== [%s %u sec] ==========\n",
               warmup_complete ? "TEST" : "WARM-UP",
               warmup_complete ? test_time : loop_count);
    }

    // Header
    printf("╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
    if (!warmup_complete) {
        printf("║                                                              DTN PORT STATS - WARM-UP (%3u/120 sec)                                                                                                                          ║\n", loop_count);
    } else {
        printf("║                                                              DTN PORT STATS - TEST Duration: %5u sec                                                                                                                         ║\n", test_time);
    }
    printf("╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝\n\n");

    // Table header
    printf("┌──────┬─────────────────────────────────────────────────────────────────────┬─────────────────────────────────────────────────────────────────────┬───────────────────────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│ DTN  │                          DTN TX (DTN→Server)                        │                          DTN RX (Server→DTN)                        │                                      PRBS Verification                                               │\n");
    printf("│ Port ├─────────────────────┬─────────────────────┬─────────────────────────┼─────────────────────┬─────────────────────┬─────────────────────────┼─────────────────────┬─────────────────────┬─────────────────────┬─────────────────────┬─────────────┤\n");
    printf("│      │       Packets       │        Bytes        │          Gbps           │       Packets       │        Bytes        │          Gbps           │        Good         │         Bad         │        Lost         │      Bit Error      │     BER     │\n");
    printf("├──────┼─────────────────────┼─────────────────────┼─────────────────────────┼─────────────────────┼─────────────────────┼─────────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┼─────────────┤\n");

    // Fetch HW stats once (per port)
    struct rte_eth_stats port_hw_stats[MAX_PORTS];
    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        uint16_t port_id = ports_config->ports[i].port_id;
        if (rte_eth_stats_get(port_id, &port_hw_stats[port_id]) != 0) {
            memset(&port_hw_stats[port_id], 0, sizeof(struct rte_eth_stats));
        }
    }

    // Port 12 per-target stats: DTN 32 → DTN 0-7, 16-23 evenly distributed
    // Each target is evenly distributed across 4 DTN ports (target bytes / 4)
    struct raw_socket_port *port12 = &raw_ports[0];
    uint64_t port12_target_tx_bytes[MAX_RAW_TARGETS] = {0};
    uint64_t port12_target_tx_pkts[MAX_RAW_TARGETS] = {0};
    uint16_t port12_target_dest[MAX_RAW_TARGETS] = {0};
    for (uint16_t t = 0; t < port12->tx_target_count; t++) {
        pthread_spin_lock(&port12->tx_targets[t].stats.lock);
        port12_target_tx_bytes[t] = port12->tx_targets[t].stats.tx_bytes;
        port12_target_tx_pkts[t] = port12->tx_targets[t].stats.tx_packets;
        pthread_spin_unlock(&port12->tx_targets[t].stats.lock);
        port12_target_dest[t] = port12->tx_targets[t].config.dest_port;
    }

    // DTN Port 0-31 (DPDK ports)
    for (uint16_t dtn = 0; dtn < DTN_DPDK_PORT_COUNT; dtn++) {
        const struct dtn_port_map_entry *entry = &dtn_port_map[dtn];

        // DTN TX (DTN→Server) = Server RX = HW q_ipackets[queue] on tx_server_port
        uint16_t srv_rx_port = entry->tx_server_port;
        uint16_t srv_rx_queue = entry->tx_server_queue;
        uint64_t dtn_tx_pkts = port_hw_stats[srv_rx_port].q_ipackets[srv_rx_queue];
        uint64_t dtn_tx_bytes = port_hw_stats[srv_rx_port].q_ibytes[srv_rx_queue];

        // DTN RX (Server→DTN) = Server TX = HW q_opackets[queue] on rx_server_port
        uint16_t srv_tx_port = entry->rx_server_port;
        uint16_t srv_tx_queue = entry->rx_server_queue;
        uint64_t dtn_rx_pkts = port_hw_stats[srv_tx_port].q_opackets[srv_tx_queue];
        uint64_t dtn_rx_bytes = port_hw_stats[srv_tx_port].q_obytes[srv_tx_queue];

        // Port 12 contribution: DTN 32 → DTN 0-7, 16-23 (each target equally across 4 DTN ports)
        for (uint16_t t = 0; t < port12->tx_target_count; t++) {
            if (port12_target_dest[t] == entry->rx_server_port) {
                dtn_rx_bytes += port12_target_tx_bytes[t] / 4;
                dtn_rx_pkts += port12_target_tx_pkts[t] / 4;
                break;
            }
        }

        // Gbps delta calculation
        uint64_t tx_delta = dtn_tx_bytes - dtn_prev_tx_bytes[dtn];
        uint64_t rx_delta = dtn_rx_bytes - dtn_prev_rx_bytes[dtn];
        double tx_gbps = to_gbps(tx_delta);
        double rx_gbps = to_gbps(rx_delta);

        // Update prev values
        dtn_prev_tx_bytes[dtn] = dtn_tx_bytes;
        dtn_prev_rx_bytes[dtn] = dtn_rx_bytes;

        // PRBS statistics (from dtn_stats)
        uint64_t good = rte_atomic64_read(&dtn_stats[dtn].good_pkts);
        uint64_t bad = rte_atomic64_read(&dtn_stats[dtn].bad_pkts);
        uint64_t lost = rte_atomic64_read(&dtn_stats[dtn].lost_pkts);
        uint64_t bit_errors_raw = rte_atomic64_read(&dtn_stats[dtn].bit_errors);

        // Include lost packets in bit_errors (each lost packet = all bits erroneous)
#if IMIX_ENABLED
        uint64_t lost_bits = lost * (uint64_t)IMIX_AVG_PACKET_SIZE * 8;
#else
        uint64_t lost_bits = lost * (uint64_t)PACKET_SIZE * 8;
#endif
        uint64_t bit_errors = bit_errors_raw + lost_bits;

        // BER calculation (lost packet bits added to total)
        double ber = 0.0;
        uint64_t total_bits = dtn_tx_bytes * 8 + lost_bits;
        if (total_bits > 0) {
            ber = (double)bit_errors / (double)total_bits;
        }

        printf("│  %2u  │ %19lu │ %19lu │ %23.2f │ %19lu │ %19lu │ %23.2f │ %19lu │ %19lu │ %19lu │ %19lu │ %11.2e │\n",
               dtn,
               dtn_tx_pkts, dtn_tx_bytes, tx_gbps,
               dtn_rx_pkts, dtn_rx_bytes, rx_gbps,
               good, bad, lost, bit_errors, ber);
    }

    // DTN Port 32 (Port 12 - 1G raw socket)
    // DTN TX = DTN→Server = dpdk_ext_rx_stats (server receives from this port)
    // DTN RX = Server→DTN = raw socket TX aggregate (server sends through this port)
    {
        struct raw_socket_port *port12 = &raw_ports[0];
        // DTN TX: What server received from Port 12 (DPDK External TX RX stats)
        pthread_spin_lock(&port12->dpdk_ext_rx_stats.lock);
        uint64_t dtn32_tx_pkts = port12->dpdk_ext_rx_stats.rx_packets;
        uint64_t dtn32_tx_bytes = port12->dpdk_ext_rx_stats.rx_bytes;
        uint64_t dtn32_good = port12->dpdk_ext_rx_stats.good_pkts;
        uint64_t dtn32_bad = port12->dpdk_ext_rx_stats.bad_pkts;
        uint64_t dtn32_bit_err = port12->dpdk_ext_rx_stats.bit_errors;
        pthread_spin_unlock(&port12->dpdk_ext_rx_stats.lock);

        // DTN RX: What server sent through Port 12 (raw socket TX aggregate)
        uint64_t dtn32_rx_pkts = 0, dtn32_rx_bytes = 0;
        for (uint16_t t = 0; t < port12->tx_target_count; t++) {
            pthread_spin_lock(&port12->tx_targets[t].stats.lock);
            dtn32_rx_pkts += port12->tx_targets[t].stats.tx_packets;
            dtn32_rx_bytes += port12->tx_targets[t].stats.tx_bytes;
            pthread_spin_unlock(&port12->tx_targets[t].stats.lock);
        }

        uint64_t tx_delta = dtn32_tx_bytes - dtn_prev_tx_bytes[DTN_RAW_PORT_12];
        uint64_t rx_delta = dtn32_rx_bytes - dtn_prev_rx_bytes[DTN_RAW_PORT_12];
        dtn_prev_tx_bytes[DTN_RAW_PORT_12] = dtn32_tx_bytes;
        dtn_prev_rx_bytes[DTN_RAW_PORT_12] = dtn32_rx_bytes;
        double tx_gbps = to_gbps(tx_delta);
        double rx_gbps = to_gbps(rx_delta);

        uint64_t dtn32_lost = get_global_sequence_lost();

        // Include lost packets in bit_errors
#if IMIX_ENABLED
        uint64_t dtn32_lost_bits = dtn32_lost * (uint64_t)RAW_IMIX_AVG_PACKET_SIZE * 8;
#else
        uint64_t dtn32_lost_bits = dtn32_lost * (uint64_t)RAW_PKT_TOTAL_SIZE * 8;
#endif
        uint64_t dtn32_bit_err_eff = dtn32_bit_err + dtn32_lost_bits;

        double ber = 0.0;
        uint64_t total_bits = dtn32_tx_bytes * 8 + dtn32_lost_bits;
        if (total_bits > 0) ber = (double)dtn32_bit_err_eff / (double)total_bits;

        printf("│  32  │ %19lu │ %19lu │ %23.2f │ %19lu │ %19lu │ %23.2f │ %19lu │ %19lu │ %19lu │ %19lu │ %11.2e │\n",
               dtn32_tx_pkts, dtn32_tx_bytes, tx_gbps,
               dtn32_rx_pkts, dtn32_rx_bytes, rx_gbps,
               dtn32_good, dtn32_bad, dtn32_lost, dtn32_bit_err_eff, ber);
    }

    // DTN Port 33 (Port 13 - 100M raw socket)
    {
        struct raw_socket_port *port13 = &raw_ports[1];
        // DTN TX: What server received from Port 13 (DPDK External TX RX stats)
        pthread_spin_lock(&port13->dpdk_ext_rx_stats.lock);
        uint64_t dtn33_tx_pkts = port13->dpdk_ext_rx_stats.rx_packets;
        uint64_t dtn33_tx_bytes = port13->dpdk_ext_rx_stats.rx_bytes;
        uint64_t dtn33_good = port13->dpdk_ext_rx_stats.good_pkts;
        uint64_t dtn33_bad = port13->dpdk_ext_rx_stats.bad_pkts;
        uint64_t dtn33_bit_err = port13->dpdk_ext_rx_stats.bit_errors;
        pthread_spin_unlock(&port13->dpdk_ext_rx_stats.lock);

        // DTN RX: What server sent through Port 13
        uint64_t dtn33_rx_pkts = 0, dtn33_rx_bytes = 0;
        for (uint16_t t = 0; t < port13->tx_target_count; t++) {
            pthread_spin_lock(&port13->tx_targets[t].stats.lock);
            dtn33_rx_pkts += port13->tx_targets[t].stats.tx_packets;
            dtn33_rx_bytes += port13->tx_targets[t].stats.tx_bytes;
            pthread_spin_unlock(&port13->tx_targets[t].stats.lock);
        }

        uint64_t tx_delta = dtn33_tx_bytes - dtn_prev_tx_bytes[DTN_RAW_PORT_13];
        uint64_t rx_delta = dtn33_rx_bytes - dtn_prev_rx_bytes[DTN_RAW_PORT_13];
        dtn_prev_tx_bytes[DTN_RAW_PORT_13] = dtn33_tx_bytes;
        dtn_prev_rx_bytes[DTN_RAW_PORT_13] = dtn33_rx_bytes;
        double tx_gbps = to_gbps(tx_delta);
        double rx_gbps = to_gbps(rx_delta);

        uint64_t dtn33_lost = get_global_sequence_lost_p13();

        // Include lost packets in bit_errors
#if IMIX_ENABLED
        uint64_t dtn33_lost_bits = dtn33_lost * (uint64_t)RAW_IMIX_AVG_PACKET_SIZE * 8;
#else
        uint64_t dtn33_lost_bits = dtn33_lost * (uint64_t)RAW_PKT_TOTAL_SIZE * 8;
#endif
        uint64_t dtn33_bit_err_eff = dtn33_bit_err + dtn33_lost_bits;

        double ber = 0.0;
        uint64_t total_bits = dtn33_tx_bytes * 8 + dtn33_lost_bits;
        if (total_bits > 0) ber = (double)dtn33_bit_err_eff / (double)total_bits;

        printf("│  33  │ %19lu │ %19lu │ %23.2f │ %19lu │ %19lu │ %23.2f │ %19lu │ %19lu │ %19lu │ %19lu │ %11.2e │\n",
               dtn33_tx_pkts, dtn33_tx_bytes, tx_gbps,
               dtn33_rx_pkts, dtn33_rx_bytes, rx_gbps,
               dtn33_good, dtn33_bad, dtn33_lost, dtn33_bit_err_eff, ber);
    }

    printf("└──────┴─────────────────────┴─────────────────────┴─────────────────────────┴─────────────────────┴─────────────────────┴─────────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┴─────────────┘\n");

    // DTN warnings
    bool has_warning = false;
    for (uint16_t dtn = 0; dtn < DTN_DPDK_PORT_COUNT; dtn++) {
        uint64_t bad = rte_atomic64_read(&dtn_stats[dtn].bad_pkts);
        uint64_t bit_err = rte_atomic64_read(&dtn_stats[dtn].bit_errors);
        uint64_t lost = rte_atomic64_read(&dtn_stats[dtn].lost_pkts);

        if (bad > 0 || bit_err > 0 || lost > 0) {
            if (!has_warning) {
                printf("\n  WARNINGS:\n");
                has_warning = true;
            }
            if (bad > 0)
                printf("      DTN Port %u: %lu bad packets!\n", dtn, bad);
            if (bit_err > 0)
                printf("      DTN Port %u: %lu bit errors!\n", dtn, bit_err);
            if (lost > 0)
                printf("      DTN Port %u: %lu lost packets!\n", dtn, lost);
        }
    }

    printf("\n  Press Ctrl+C to stop\n");
}
#endif /* STATS_MODE_DTN */

// ==========================================
// SERVER PORT-BASED STATISTICS TABLE (Legacy table)
// ==========================================
static void helper_print_server_stats(const struct ports_config *ports_config,
                                      const uint64_t prev_tx_bytes[],
                                      const uint64_t prev_rx_bytes[],
                                      bool warmup_complete, unsigned loop_count,
                                      unsigned test_time)
{
    // Clear screen (only in interactive mode, disabled in daemon mode for log files)
    if (!g_daemon_mode) {
        printf("\033[2J\033[H");
    } else {
        // Daemon mode: separator line between tables
        printf("\n========== [%s %u sec] ==========\n",
               warmup_complete ? "TEST" : "WARM-UP",
               warmup_complete ? test_time : loop_count);
    }

    // Header (240 characters wide)
    printf("╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
    if (!warmup_complete) {
        printf("║                                                                    WARM-UP PHASE (%3u/120 sec) - Statistics will be reset at 120 seconds                                                                                        ║\n", loop_count);
    } else {
        printf("║                                                                    TEST IN PROGRESS - Test Duration: %5u sec                                                                                                                   ║\n", test_time);
    }
    printf("╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝\n\n");

    // Main statistics table (240 characters)
    printf("┌──────┬─────────────────────────────────────────────────────────────────────┬─────────────────────────────────────────────────────────────────────┬───────────────────────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│ Port │                            TX (Transmitted)                          │                            RX (Received)                            │                                      PRBS Verification                                              │\n");
    printf("│      ├─────────────────────┬─────────────────────┬─────────────────────────┼─────────────────────┬─────────────────────┬─────────────────────────┼─────────────────────┬─────────────────────┬─────────────────────┬─────────────────────┬─────────────┤\n");
    printf("│      │       Packets       │        Bytes        │          Gbps           │       Packets       │        Bytes        │          Gbps           │        Good         │         Bad         │        Lost         │      Bit Error      │     BER     │\n");
    printf("├──────┼─────────────────────┼─────────────────────┼─────────────────────────┼─────────────────────┼─────────────────────┼─────────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┼─────────────┤\n");

    struct rte_eth_stats st;

    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        uint16_t port_id = ports_config->ports[i].port_id;

        if (rte_eth_stats_get(port_id, &st) != 0) {
            printf("│  %2u  │         N/A         │         N/A         │           N/A           │         N/A         │         N/A         │           N/A           │         N/A         │         N/A         │         N/A         │         N/A         │     N/A     │\n", port_id);
            continue;
        }

        // HW statistics
        uint64_t tx_pkts = st.opackets;
        uint64_t tx_bytes = st.obytes;
        uint64_t rx_pkts = st.ipackets;
        uint64_t rx_bytes = st.ibytes;

        // Per-second rate calculation
        uint64_t tx_bytes_delta = tx_bytes - prev_tx_bytes[port_id];
        uint64_t rx_bytes_delta = rx_bytes - prev_rx_bytes[port_id];
        double tx_gbps = to_gbps(tx_bytes_delta);
        double rx_gbps = to_gbps(rx_bytes_delta);

        // PRBS verification statistics
        uint64_t good = rte_atomic64_read(&rx_stats_per_port[port_id].good_pkts);
        uint64_t bad = rte_atomic64_read(&rx_stats_per_port[port_id].bad_pkts);
        uint64_t lost = rte_atomic64_read(&rx_stats_per_port[port_id].lost_pkts);
        uint64_t bit_errors_raw = rte_atomic64_read(&rx_stats_per_port[port_id].bit_errors);

        // Include lost packets in bit_errors (each lost packet = all bits erroneous)
#if IMIX_ENABLED
        uint64_t lost_bits = lost * (uint64_t)IMIX_AVG_PACKET_SIZE * 8;
#else
        uint64_t lost_bits = lost * (uint64_t)PACKET_SIZE * 8;
#endif
        uint64_t bit_errors = bit_errors_raw + lost_bits;

        // Bit Error Rate (BER) calculation (lost packet bits added to total)
        double ber = 0.0;
        uint64_t total_bits = rx_bytes * 8 + lost_bits;
        if (total_bits > 0) {
            ber = (double)bit_errors / (double)total_bits;
        }

        // Print table row
        printf("│  %2u  │ %19lu │ %19lu │ %23.2f │ %19lu │ %19lu │ %23.2f │ %19lu │ %19lu │ %19lu │ %19lu │ %11.2e │\n",
               port_id,
               tx_pkts, tx_bytes, tx_gbps,
               rx_pkts, rx_bytes, rx_gbps,
               good, bad, lost, bit_errors, ber);
    }

    printf("└──────┴─────────────────────┴─────────────────────┴─────────────────────────┴─────────────────────┴─────────────────────┴─────────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┴─────────────┘\n");

    // Warnings
    bool has_warning = false;
    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        uint16_t port_id = ports_config->ports[i].port_id;

        uint64_t bad_pkts = rte_atomic64_read(&rx_stats_per_port[port_id].bad_pkts);
        uint64_t bit_errors = rte_atomic64_read(&rx_stats_per_port[port_id].bit_errors);
        uint64_t lost_pkts = rte_atomic64_read(&rx_stats_per_port[port_id].lost_pkts);

        if (bad_pkts > 0 || bit_errors > 0 || lost_pkts > 0) {
            if (!has_warning) {
                printf("\n  WARNINGS:\n");
                has_warning = true;
            }
            if (bad_pkts > 0) {
                printf("      Port %u: %lu bad packets detected!\n", port_id, bad_pkts);
            }
            if (bit_errors > 0) {
                printf("      Port %u: %lu bit errors detected!\n", port_id, bit_errors);
            }
            if (lost_pkts > 0) {
                printf("      Port %u: %lu lost packets detected!\n", port_id, lost_pkts);
            }
        }

        // HW missed packets check
        struct rte_eth_stats st2;
        if (rte_eth_stats_get(port_id, &st2) == 0 && st2.imissed > 0) {
            if (!has_warning) {
                printf("\n  WARNINGS:\n");
                has_warning = true;
            }
            printf("      Port %u: %lu packets missed by hardware (imissed)!\n", port_id, st2.imissed);
        }
    }

    printf("\n  Press Ctrl+C to stop\n");
}

// ==========================================
// PUBLIC API: helper_print_stats
// ==========================================
// Draws DTN or Server table based on STATS_MODE_DTN flag

void helper_print_stats(const struct ports_config *ports_config,
                        const uint64_t prev_tx_bytes[], const uint64_t prev_rx_bytes[],
                        bool warmup_complete, unsigned loop_count, unsigned test_time)
{
#if STATS_MODE_DTN
    helper_print_dtn_stats(ports_config, warmup_complete, loop_count, test_time);
    (void)prev_tx_bytes;
    (void)prev_rx_bytes;
#else
    helper_print_server_stats(ports_config, prev_tx_bytes, prev_rx_bytes,
                              warmup_complete, loop_count, test_time);
#endif
}