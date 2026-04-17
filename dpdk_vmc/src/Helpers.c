#include "Helpers.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <rte_ethdev.h>
#include <rte_atomic.h>

#include "Config.h"
#include "TxRxManager.h"  // for rx_stats_per_port
#include "AteMode.h"

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

#if STATS_MODE_VMC
    init_vmc_stats();
#endif
}

#if STATS_MODE_VMC
// ==========================================
// VMC PORT-BASED STATISTICS - 3-TABLE DISPLAY
// ==========================================
// Groups 16 VMC ports into VSCPU / FCPU / CROSS tables.
// VMC TX (VMC→Server) = Server RX = HW q_ipackets[queue]
// VMC RX (Server→VMC) = Server TX = HW q_opackets[queue]
// PRBS = vmc_stats[vmc_port] (from RX worker)

// Per-VMC-port prev bytes for delta calculation
static uint64_t vmc_prev_tx_bytes[VMC_PORT_COUNT];
static uint64_t vmc_prev_rx_bytes[VMC_PORT_COUNT];

static void print_vmc_table_group(const uint16_t *indices, uint16_t count,
                                  const struct rte_eth_stats port_hw_stats[])
{
    printf("  ┌─────────┬────────┬─────────────────────────────────────────────────────────────────────┬─────────────────────────────────────────────────────────────────────┬──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("  │ Server  │  VMC   │                          VMC TX (VMC→Server)                        │                          VMC RX (Server→VMC)                        │                                              Payload Verification                                                              │\n");
    printf("  │  Port   │  Port  ├─────────────────────┬─────────────────────┬─────────────────────────┼─────────────────────┬─────────────────────┬─────────────────────────┼─────────────────────┬─────────────────────┬─────────────────────┬─────────────────────┬─────────────────────┬─────────────────────┬─────────────┤\n");
    printf("  │         │        │       Packets       │        Bytes        │          Gbps           │       Packets       │        Bytes        │          Gbps           │        Good         │         Bad         │  SplitMix64 Fail    │    CRC32 Fail       │        Loss         │      Bit Error      │     BER     │\n");
    printf("  ├─────────┼────────┼─────────────────────┼─────────────────────┼─────────────────────────┼─────────────────────┼─────────────────────┼─────────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┼─────────────┤\n");

    for (uint16_t i = 0; i < count; i++) {
        uint16_t vmc = indices[i];
        const struct vmc_port_map_entry *entry = &vmc_port_map[vmc];

        // VMC TX (VMC→Server) = Server RX = HW q_ipackets[queue] on tx_server_port
        uint16_t srv_rx_port = entry->tx_server_port;
        uint16_t srv_rx_queue = entry->tx_server_queue;
        uint64_t vmc_tx_pkts = port_hw_stats[srv_rx_port].q_ipackets[srv_rx_queue];
        uint64_t vmc_tx_bytes = port_hw_stats[srv_rx_port].q_ibytes[srv_rx_queue];

        // VMC RX (Server→VMC) = Server TX = HW q_opackets[queue] on rx_server_port
        uint16_t srv_tx_port = entry->rx_server_port;
        uint16_t srv_tx_queue = entry->rx_server_queue;
        uint64_t vmc_rx_pkts = port_hw_stats[srv_tx_port].q_opackets[srv_tx_queue];
        uint64_t vmc_rx_bytes = port_hw_stats[srv_tx_port].q_obytes[srv_tx_queue];

        // Gbps delta calculation
        uint64_t tx_delta = vmc_tx_bytes - vmc_prev_tx_bytes[vmc];
        uint64_t rx_delta = vmc_rx_bytes - vmc_prev_rx_bytes[vmc];
        double tx_gbps = to_gbps(tx_delta);
        double rx_gbps = to_gbps(rx_delta);

        // Update prev values
        vmc_prev_tx_bytes[vmc] = vmc_tx_bytes;
        vmc_prev_rx_bytes[vmc] = vmc_rx_bytes;

        // Payload verification statistics
        uint64_t good = rte_atomic64_read(&vmc_stats[vmc].good_pkts);
        uint64_t bad = rte_atomic64_read(&vmc_stats[vmc].bad_pkts);
        uint64_t sm_fail = rte_atomic64_read(&vmc_stats[vmc].splitmix_fail);
        uint64_t crc_fail = rte_atomic64_read(&vmc_stats[vmc].crc32_fail);
        uint64_t lost = rte_atomic64_read(&vmc_stats[vmc].lost_pkts);
        uint64_t bit_errors_raw = rte_atomic64_read(&vmc_stats[vmc].bit_errors);

#if IMIX_ENABLED
        uint64_t lost_bits = lost * (uint64_t)IMIX_AVG_PACKET_SIZE * 8;
#else
        uint64_t lost_bits = lost * (uint64_t)PACKET_SIZE * 8;
#endif
        uint64_t bit_errors = bit_errors_raw + lost_bits;

        double ber = 0.0;
        uint64_t total_bits = vmc_tx_bytes * 8 + lost_bits;
        if (total_bits > 0) {
            ber = (double)bit_errors / (double)total_bits;
        }

        printf("  │    %u    │ %-6s │ %19lu │ %19lu │ %23.4f │ %19lu │ %19lu │ %23.4f │ %19lu │ %19lu │ %19lu │ %19lu │ %19lu │ %19lu │ %11.2e │\n",
               entry->rx_server_port,
               vmc_port_labels[vmc],
               vmc_tx_pkts, vmc_tx_bytes, tx_gbps,
               vmc_rx_pkts, vmc_rx_bytes, rx_gbps,
               good, bad, sm_fail, crc_fail, lost, bit_errors, ber);
    }

    printf("  └─────────┴────────┴─────────────────────┴─────────────────────┴─────────────────────────┴─────────────────────┴─────────────────────┴─────────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┴─────────────┘\n");
}

static void helper_print_vmc_stats(const struct ports_config *ports_config,
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
    printf("╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
    if (!warmup_complete) {
        printf("║                                                              VMC PORT STATS - WARM-UP (%3u/120 sec)                                                                                                                                  ║\n", loop_count);
    } else {
        printf("║                                                              VMC PORT STATS - TEST Duration: %5u sec                                                                                                                                 ║\n", test_time);
    }
    printf("╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝\n");

    // Fetch HW stats once (per port)
    struct rte_eth_stats port_hw_stats[MAX_PORTS];
    for (uint16_t i = 0; i < ports_config->nb_ports; i++) {
        uint16_t port_id = ports_config->ports[i].port_id;
        if (rte_eth_stats_get(port_id, &port_hw_stats[port_id]) != 0) {
            memset(&port_hw_stats[port_id], 0, sizeof(struct rte_eth_stats));
        }
    }

    // Table 1: VSCPU
    printf("\n  === VSCPU (Server Port 2: J4-1,J4-2 | Port 3: J5-1..J5-4 | Port 0: J6-3) ===\n");
    print_vmc_table_group(vscpu_vmc_indices, VSCPU_COUNT, port_hw_stats);

    // Table 2: FCPU
    printf("\n  === FCPU (Server Port 2: J4-3,J4-4 | Port 1: J7-1..J7-4 | Port 0: J6-1) ===\n");
    print_vmc_table_group(fcpu_vmc_indices, FCPU_COUNT, port_hw_stats);

    // Table 3: Cross
    printf("\n  === CROSS (Port 0: J6-2 TX->J6-4 RX, J6-4 TX->J6-2 RX) ===\n");
    print_vmc_table_group(cross_vmc_indices, CROSS_COUNT, port_hw_stats);

    // Warnings
    bool has_warning = false;
    for (uint16_t vmc = 0; vmc < VMC_DPDK_PORT_COUNT; vmc++) {
        uint64_t bad = rte_atomic64_read(&vmc_stats[vmc].bad_pkts);
        uint64_t sm_fail = rte_atomic64_read(&vmc_stats[vmc].splitmix_fail);
        uint64_t crc_fail = rte_atomic64_read(&vmc_stats[vmc].crc32_fail);
        uint64_t bit_err = rte_atomic64_read(&vmc_stats[vmc].bit_errors);
        uint64_t lost = rte_atomic64_read(&vmc_stats[vmc].lost_pkts);

        if (bad > 0 || bit_err > 0 || lost > 0) {
            if (!has_warning) {
                printf("\n  WARNINGS:\n");
                has_warning = true;
            }
            if (bad > 0)
                printf("      %s (VMC %u): %lu bad packets! (SM:%lu CRC:%lu)\n", vmc_port_labels[vmc], vmc, bad, sm_fail, crc_fail);
            if (bit_err > 0)
                printf("      %s (VMC %u): %lu bit errors!\n", vmc_port_labels[vmc], vmc, bit_err);
            if (lost > 0)
                printf("      %s (VMC %u): %lu lost packets!\n", vmc_port_labels[vmc], vmc, lost);
        }
    }

    printf("\n  Press Ctrl+C to stop\n");
}
#endif /* STATS_MODE_VMC */

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
        printf("│  %2u  │ %19lu │ %19lu │ %23.4f │ %19lu │ %19lu │ %23.4f │ %19lu │ %19lu │ %19lu │ %19lu │ %11.2e │\n",
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
// Normal mode: 3-table VMC display (VSCPU / FCPU / Cross)
// ATE mode: Server port table (loopback)

void helper_print_stats(const struct ports_config *ports_config,
                        const uint64_t prev_tx_bytes[], const uint64_t prev_rx_bytes[],
                        bool warmup_complete, unsigned loop_count, unsigned test_time)
{
#if STATS_MODE_VMC
    if (ate_mode_enabled()) {
        // ATE mode: server port table (loopback)
        helper_print_server_stats(ports_config, prev_tx_bytes, prev_rx_bytes,
                                  warmup_complete, loop_count, test_time);
    } else {
        // Normal mode: 3 tables (VSCPU / FCPU / Cross) with VMC port-level stats
        helper_print_vmc_stats(ports_config, warmup_complete, loop_count, test_time);
        (void)prev_tx_bytes;
        (void)prev_rx_bytes;
    }
#else
    helper_print_server_stats(ports_config, prev_tx_bytes, prev_rx_bytes,
                              warmup_complete, loop_count, test_time);
#endif
}