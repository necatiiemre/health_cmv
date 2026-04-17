#define _GNU_SOURCE  // For clock_gettime and CLOCK_REALTIME

/**
 * PTP State Machine
 *
 * Handles PTP slave state transitions and offset/delay calculations.
 *
 * State Flow:
 *   INIT → LISTENING → SYNC_RECEIVED → DELAY_REQ_SENT → SYNCED
 *                ↑                                         |
 *                └─────────────────────────────────────────┘
 *
 * Timing:
 *   - t1: Master's TX time (from Sync packet origin_timestamp)
 *   - t2: Our RX time (software timestamp via rte_rdtsc)
 *   - t3: Our TX time (software timestamp via rte_rdtsc)
 *   - t4: Master's RX time (from Delay_Resp receive_timestamp)
 *
 * Calculations:
 *   - Offset = ((t2 - t1) - (t4 - t3)) / 2
 *   - Delay  = ((t2 - t1) + (t4 - t3)) / 2
 */

#include <rte_cycles.h>
#include <rte_log.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "PtpTypes.h"
#include "PtpSlave.h"
#include "Config.h"

// Helper: Get current time in nanoseconds (CLOCK_REALTIME - same epoch as PTP)
static inline uint64_t get_realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Timeout values in TSC cycles (calculated at init)
static uint64_t sync_timeout_cycles;
static uint64_t delay_resp_timeout_cycles;
static uint64_t delay_req_interval_cycles;
static uint64_t tsc_hz;
static bool timeouts_initialized = false;

/**
 * Initialize timeout values
 */
static void init_timeouts(void)
{
    if (timeouts_initialized)
        return;

    tsc_hz = rte_get_tsc_hz();
    sync_timeout_cycles = tsc_hz * PTP_SYNC_TIMEOUT_SEC;
    delay_resp_timeout_cycles = tsc_hz * PTP_DELAY_RESP_TIMEOUT_SEC;
    delay_req_interval_cycles = (tsc_hz * PTP_DELAY_REQ_INTERVAL_MS) / 1000;
    timeouts_initialized = true;
}

/**
 * Handle Sync message received
 */
void ptp_handle_sync(ptp_session_t *session,
                     const ptp_header_t *header,
                     const ptp_timestamp_t *timestamp,
                     uint64_t rx_tsc)
{
    init_timeouts();

    // Store master information
    memcpy(&session->master_port_id, &header->source_port_id,
           sizeof(ptp_port_identity_t));
    session->master_domain = header->domain_number;

    // Store sequence ID
    session->sync_seq_id = rte_be_to_cpu_16(header->sequence_id);

    // Update timing (always track last sync for timeout detection)
    session->last_sync_tsc = rx_tsc;

    // Only store T1/T2 and transition if we're ready for a new sync cycle
    // Don't overwrite T1/T2 if we're waiting for Delay_Resp (DELAY_REQ_SENT)
    // or already processing a Sync (SYNC_RECEIVED)
    if (session->state == PTP_STATE_LISTENING ||
        session->state == PTP_STATE_SYNCED ||
        session->state == PTP_STATE_ERROR) {

        // Store timestamps
        // t1: Master's TX time (from Sync origin_timestamp) - PTP epoch
        session->t1_ns = ptp_timestamp_to_ns(timestamp);

        // t2: Our RX time (software timestamps)
        session->t2_tsc = rx_tsc;                    // TSC for delay calculation
        session->t2_realtime_ns = get_realtime_ns(); // Realtime for offset calculation

        session->state = PTP_STATE_SYNC_RECEIVED;
        session->last_state_change = rx_tsc;
    }
}

/**
 * Handle Delay_Resp message received
 */
void ptp_handle_delay_resp(ptp_session_t *session,
                           const ptp_header_t *header,
                           const ptp_timestamp_t *timestamp,
                           const ptp_port_identity_t *req_port_id)
{
    (void)req_port_id;

    init_timeouts();

    // Check sequence ID matches our last Delay_Req
    uint16_t resp_seq_id = rte_be_to_cpu_16(header->sequence_id);
    if (resp_seq_id != session->last_delay_req_seq_id) {
        // Stale or mismatched Delay_Resp, ignore
        return;
    }

    // Store t4: Master's RX time (from Delay_Resp receive_timestamp)
    session->t4_ns = ptp_timestamp_to_ns(timestamp);

    // State transition
    if (session->state == PTP_STATE_DELAY_REQ_SENT) {
        // Calculate offset and delay
        ptp_calculate_offset_delay(session);

        // Mark as synced
        session->state = PTP_STATE_SYNCED;
        session->is_synced = true;
        session->sync_count++;
        session->last_state_change = rte_rdtsc();
    }
}

/**
 * Calculate offset and delay from t1, t2, t3, t4
 *
 * Using CLOCK_REALTIME for T2/T3 (same epoch as PTP - 1970), we can now
 * calculate both offset and delay properly.
 *
 * PTP Formulas:
 *   - Offset = ((T2 - T1) - (T4 - T3)) / 2  (how much our clock is ahead of master)
 *   - Delay  = ((T2 - T1) + (T4 - T3)) / 2  (one-way network delay)
 *
 * Timestamps (all in nanoseconds from 1970 epoch):
 *   - T1: Master's Sync TX time (from Sync packet)
 *   - T2: Our Sync RX time (clock_gettime CLOCK_REALTIME)
 *   - T3: Our Delay_Req TX time (clock_gettime CLOCK_REALTIME)
 *   - T4: Master's Delay_Req RX time (from Delay_Resp packet)
 */
void ptp_calculate_offset_delay(ptp_session_t *session)
{
    init_timeouts();

    // All timestamps in nanoseconds from 1970 epoch
    uint64_t t1_ns = session->t1_ns;              // Master Sync TX (from packet)
    uint64_t t2_ns = session->t2_realtime_ns;     // Our Sync RX (clock_gettime)
    uint64_t t3_ns = session->t3_realtime_ns;     // Our Delay_Req TX (clock_gettime)
    uint64_t t4_ns = session->t4_ns;              // Master Delay_Req RX (from packet)

    // Also get TSC-based values for comparison
    uint64_t t2_tsc_ns = tsc_to_ns(session->t2_tsc, tsc_hz);
    uint64_t t3_tsc_ns = tsc_to_ns(session->t3_tsc, tsc_hz);

    // Debug: Print all timestamps (first 10 calculations)
    static uint64_t calc_print_count = 0;
    if (calc_print_count < 10) {
        printf("PTP Calc [Port%u VLAN%u]:\n", session->port_id, session->rx_vlan_id);
        printf("  T1 (Master Sync TX)     = %lu ns (PTP epoch)\n", t1_ns);
        printf("  T2 (Slave Sync RX)      = %lu ns (realtime)\n", t2_ns);
        printf("  T3 (Slave DelayReq TX)  = %lu ns (realtime)\n", t3_ns);
        printf("  T4 (Master DelayReq RX) = %lu ns (PTP epoch)%s\n", t4_ns, t4_ns == 0 ? " (EMPTY!)" : "");
        printf("  T3-T2 (our processing)  = %ld ns (%.2f us)\n",
               (int64_t)(t3_ns - t2_ns), (t3_ns - t2_ns) / 1000.0);
        printf("  T3-T2 (TSC based)       = %ld ns (%.2f us)\n",
               (int64_t)(t3_tsc_ns - t2_tsc_ns), (t3_tsc_ns - t2_tsc_ns) / 1000.0);
    }

    // Check if T4 is valid (DTN may not fill it in)
    if (t4_ns == 0) {
        // T4 not provided by DTN, cannot calculate
        session->delay_ns = 0;
        session->offset_ns = 0;
        if (calc_print_count < 10) {
            printf("  >>> T4 is EMPTY - cannot calculate offset/delay\n");
            calc_print_count++;
        }
    } else {
        // Calculate using PTP formulas
        // T2 - T1: Time from Master TX to Slave RX = delay + offset
        // T4 - T3: Time from Slave TX to Master RX = delay - offset
        int64_t t2_minus_t1 = (int64_t)t2_ns - (int64_t)t1_ns;
        int64_t t4_minus_t3 = (int64_t)t4_ns - (int64_t)t3_ns;

        // Offset = ((T2-T1) - (T4-T3)) / 2
        // Positive offset means our clock is ahead of master
        session->offset_ns = (t2_minus_t1 - t4_minus_t3) / 2;

        // Delay = ((T2-T1) + (T4-T3)) / 2
        // One-way network propagation delay
        session->delay_ns = (t2_minus_t1 + t4_minus_t3) / 2;

        if (calc_print_count < 10) {
            printf("  T2-T1 (Sync path)       = %ld ns (%.2f ms)\n",
                   t2_minus_t1, t2_minus_t1 / 1000000.0);
            printf("  T4-T3 (DelayReq path)   = %ld ns (%.2f ms)\n",
                   t4_minus_t3, t4_minus_t3 / 1000000.0);
            printf("  >>> Offset = (T2-T1 - T4+T3) / 2 = %ld ns (%.3f ms)\n",
                   session->offset_ns, session->offset_ns / 1000000.0);
            printf("  >>> Delay  = (T2-T1 + T4-T3) / 2 = %ld ns (%.2f us)\n",
                   session->delay_ns, session->delay_ns / 1000.0);
            calc_print_count++;
        }
    }

    session->tsc_hz = tsc_hz;
}

/**
 * Process state machine for a session
 */
void ptp_state_machine_tick(ptp_session_t *session,
                            ptp_port_t *port,
                            uint64_t current_tsc)
{
    init_timeouts();

    switch (session->state) {
    case PTP_STATE_INIT:
        // Transition to LISTENING
        session->state = PTP_STATE_LISTENING;
        session->last_state_change = current_tsc;
        break;

    case PTP_STATE_LISTENING:
        // Check for Sync timeout
        if (session->last_sync_tsc > 0 &&
            (current_tsc - session->last_sync_tsc) > sync_timeout_cycles) {
            session->sync_timeout_count++;
        }
        // Stay in LISTENING until Sync received (handled by ptp_handle_sync)
        break;

    case PTP_STATE_SYNC_RECEIVED:
        // Wait a short interval then send Delay_Req
        if ((current_tsc - session->last_state_change) >= delay_req_interval_cycles) {
            uint64_t tx_tsc;
            int ret;

            // Use raw socket or DPDK based on port type
            if (port->is_raw_socket) {
                ret = ptp_send_delay_req_raw(session, port, &tx_tsc);
            } else {
                ret = ptp_send_delay_req(session, port, &tx_tsc);
            }

            if (ret == 0) {
                session->state = PTP_STATE_DELAY_REQ_SENT;
                session->last_state_change = current_tsc;
            } else {
                // TX failed, go to error state
                session->state = PTP_STATE_ERROR;
                session->sync_errors++;
            }
        }
        break;

    case PTP_STATE_DELAY_REQ_SENT:
        // Check for Delay_Resp timeout
        if ((current_tsc - session->last_state_change) > delay_resp_timeout_cycles) {
            session->sync_timeout_count++;
            session->state = PTP_STATE_LISTENING;
            session->last_state_change = current_tsc;
        }
        // Stay here until Delay_Resp received (handled by ptp_handle_delay_resp)
        break;

    case PTP_STATE_SYNCED:
        // Successfully synchronized
        // Wait for next Sync message (will be handled by ptp_handle_sync)
        // Check for Sync timeout
        if ((current_tsc - session->last_sync_tsc) > sync_timeout_cycles) {
            session->sync_timeout_count++;
            session->is_synced = false;
            session->state = PTP_STATE_LISTENING;
            session->last_state_change = current_tsc;
        }
        break;

    case PTP_STATE_ERROR:
        // Error state, reset to LISTENING after a delay
        if ((current_tsc - session->last_state_change) > sync_timeout_cycles) {
            session->state = PTP_STATE_LISTENING;
            session->last_state_change = current_tsc;
        }
        break;
    }
}

/**
 * Initialize a PTP session (with split TX/RX port support)
 * rx_vl_idx is not configured - it will be read from Sync packet at runtime
 *
 * @param session     Session to initialize
 * @param rx_port_id  Port where Sync/Delay_Resp arrives (session lives here)
 * @param tx_port_id  Port for sending Delay_Req (may differ from rx_port_id)
 * @param session_idx Session index within port
 * @param rx_vlan_id  VLAN for receiving Sync/Delay_Resp
 * @param tx_vlan_id  VLAN for sending Delay_Req
 * @param tx_vl_idx   VL-IDX for Delay_Req packet (request)
 * @param rx_vl_idx   VL-IDX for Sync/Delay_Resp (response)
 */
void ptp_session_init(ptp_session_t *session,
                      uint16_t rx_port_id,
                      uint16_t tx_port_id,
                      uint8_t session_idx,
                      uint16_t rx_vlan_id,
                      uint16_t tx_vlan_id,
                      uint16_t tx_vl_idx,
                      uint16_t rx_vl_idx)
{
    init_timeouts();

    memset(session, 0, sizeof(ptp_session_t));

    session->port_id = rx_port_id;      // RX port (session owner)
    session->tx_port_id = tx_port_id;   // TX port for Delay_Req
    session->session_idx = session_idx;
    session->rx_vlan_id = rx_vlan_id;
    session->tx_vlan_id = tx_vlan_id;
    session->tx_vl_idx = tx_vl_idx;
    session->rx_vl_idx = rx_vl_idx;
    session->vlan_id = rx_vlan_id; // Primary VLAN ID for identification

    session->state = PTP_STATE_INIT;
    session->is_synced = false;
    session->tsc_hz = tsc_hz;

    // Initialize our port identity
    ptp_init_port_identity(session, rx_port_id);

    printf("PTP: Session init - RX Port %u (VLAN %u, RespVL %u), TX Port %u (VLAN %u, ReqVL %u)\n",
           rx_port_id, rx_vlan_id, rx_vl_idx, tx_port_id, tx_vlan_id, tx_vl_idx);
}

/**
 * Reset session statistics
 */
void ptp_session_reset_stats(ptp_session_t *session)
{
    session->sync_rx_count = 0;
    session->delay_req_tx_count = 0;
    session->delay_resp_rx_count = 0;
    session->sync_timeout_count = 0;
    session->sync_errors = 0;
    session->sync_count = 0;
}

/**
 * Get session statistics
 */
void ptp_session_get_stats(const ptp_session_t *session,
                           ptp_session_stats_t *stats)
{
    stats->port_id = session->port_id;
    stats->vlan_id = session->vlan_id;
    stats->state_str = ptp_state_to_str(session->state);
    stats->offset_ns = session->offset_ns;
    stats->delay_ns = session->delay_ns;
    stats->sync_rx_count = session->sync_rx_count;
    stats->delay_req_tx_count = session->delay_req_tx_count;
    stats->delay_resp_rx_count = session->delay_resp_rx_count;
    stats->is_synced = session->is_synced;
}