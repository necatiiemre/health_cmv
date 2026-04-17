#ifndef PTP_SLAVE_H
#define PTP_SLAVE_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include "PtpTypes.h"
#include "Config.h"  // For struct ptp_session_config

// ==========================================
// PTP INITIALIZATION API
// ==========================================

/**
 * Initialize PTP slave subsystem
 * @return 0 on success, negative on error
 */
int ptp_init(void);

/**
 * Configure PTP sessions with split TX/RX port support (NEW)
 * Use this for asymmetric routing where TX and RX go through different ports
 * @param sessions Array of session configurations (from PTP_SESSIONS_CONFIG_INIT)
 * @param session_count Number of sessions
 * @return 0 on success, negative on error
 */
int ptp_configure_split_sessions(const struct ptp_session_config *sessions,
                                  uint16_t session_count);

/**
 * Configure PTP for a specific port using full session config (legacy)
 * Note: For split TX/RX ports, use ptp_configure_split_sessions instead
 * @param port_id DPDK port ID
 * @param sessions Array of session configurations
 * @param session_count Number of sessions (should be 4)
 * @return 0 on success, negative on error
 */
int ptp_port_configure_sessions(uint16_t port_id,
                                const struct ptp_session_config *sessions,
                                uint16_t session_count);

/**
 * Configure PTP for a specific port (legacy interface)
 * @param port_id DPDK port ID
 * @param rx_vlans Array of RX VLAN IDs (for receiving Sync/Delay_Resp)
 * @param tx_vlans Array of TX VLAN IDs (for sending Delay_Req)
 * @param vlan_count Number of VLANs (should be PTP_SESSIONS_PER_PORT)
 * @return 0 on success, negative on error
 */
int ptp_port_configure(uint16_t port_id,
                       const uint16_t *rx_vlans,
                       const uint16_t *tx_vlans,
                       uint16_t vlan_count);

/**
 * Start PTP workers on all configured ports
 * @return 0 on success, negative on error
 */
int ptp_start(void);

/**
 * Stop PTP workers
 */
void ptp_stop(void);

/**
 * Cleanup PTP subsystem
 */
void ptp_cleanup(void);

// ==========================================
// PTP FLOW RULES API
// ==========================================

/**
 * Install rte_flow rule to direct PTP packets (EtherType 0x88F7) to PTP RX queue
 * @param port_id DPDK port ID
 * @return 0 on success, negative on error
 */
int ptp_flow_rule_install(uint16_t port_id);

/**
 * Remove PTP flow rules from port
 * @param port_id DPDK port ID
 */
void ptp_flow_rule_remove(uint16_t port_id);

/**
 * Install flow rules on all PTP ports
 * @return 0 on success, negative on error
 */
int ptp_flow_rules_install_all(void);

/**
 * Remove flow rules from all PTP ports
 */
void ptp_flow_rules_remove_all(void);

// ==========================================
// PTP WORKER API
// ==========================================

/**
 * PTP worker main loop (run on dedicated lcore)
 * @param arg Pointer to ptp_port_t
 * @return 0 on exit
 */
int ptp_worker_main(void *arg);

/**
 * Assign lcore to PTP port
 * @param port_id DPDK port ID
 * @param lcore_id Lcore to assign
 * @return 0 on success, negative on error
 */
int ptp_assign_lcore(uint16_t port_id, uint16_t lcore_id);

// ==========================================
// PTP PACKET API
// ==========================================

/**
 * Parse received PTP packet and update session state
 * @param session PTP session to update
 * @param mbuf Received mbuf containing PTP packet
 * @param rx_tsc TSC timestamp when packet was received
 * @return 0 on success, negative on error
 */
int ptp_packet_process(ptp_session_t *session,
                       struct rte_mbuf *mbuf,
                       uint64_t rx_tsc);

/**
 * Build and send Delay_Req packet
 * @param session PTP session
 * @param port Pointer to ptp_port_t for TX
 * @param tx_tsc Output: TSC timestamp when packet was sent
 * @return 0 on success, negative on error
 */
int ptp_send_delay_req(ptp_session_t *session,
                       ptp_port_t *port,
                       uint64_t *tx_tsc);

/**
 * Check if mbuf contains a PTP packet (EtherType 0x88F7)
 * @param mbuf Packet mbuf
 * @return true if PTP packet
 */
bool ptp_is_ptp_packet(struct rte_mbuf *mbuf);

/**
 * Get PTP message type from packet
 * @param mbuf Packet mbuf
 * @return PTP message type or -1 on error
 */
int ptp_get_msg_type(struct rte_mbuf *mbuf);

/**
 * Get VLAN ID from packet
 * @param mbuf Packet mbuf
 * @return VLAN ID or 0 if untagged
 */
uint16_t ptp_get_vlan_id(struct rte_mbuf *mbuf);

/**
 * Initialize session's port identity from MAC address
 * @param session PTP session
 * @param port_id DPDK port ID
 */
void ptp_init_port_identity(ptp_session_t *session, uint16_t port_id);

// ==========================================
// PTP STATE MACHINE API
// ==========================================

/**
 * Process state machine for a session
 * @param session PTP session
 * @param port PTP port (for TX operations)
 * @param current_tsc Current TSC value
 */
void ptp_state_machine_tick(ptp_session_t *session,
                            ptp_port_t *port,
                            uint64_t current_tsc);

/**
 * Handle Sync message received
 * @param session PTP session
 * @param header PTP header
 * @param timestamp Origin timestamp from Sync
 * @param rx_tsc RX timestamp (TSC)
 */
void ptp_handle_sync(ptp_session_t *session,
                     const ptp_header_t *header,
                     const ptp_timestamp_t *timestamp,
                     uint64_t rx_tsc);

/**
 * Handle Delay_Resp message received
 * @param session PTP session
 * @param header PTP header
 * @param timestamp Receive timestamp from Delay_Resp
 * @param req_port_id Requesting port identity from Delay_Resp
 */
void ptp_handle_delay_resp(ptp_session_t *session,
                           const ptp_header_t *header,
                           const ptp_timestamp_t *timestamp,
                           const ptp_port_identity_t *req_port_id);

/**
 * Calculate offset and delay from t1,t2,t3,t4
 * @param session PTP session (must have valid t1,t2,t3,t4)
 */
void ptp_calculate_offset_delay(ptp_session_t *session);

/**
 * Initialize a PTP session (with split TX/RX port support)
 * @param session PTP session to initialize
 * @param rx_port_id RX port ID - where Sync/Delay_Resp arrives (session owner)
 * @param tx_port_id TX port ID - for sending Delay_Req (may differ from rx_port_id)
 * @param session_idx Session index within port (0-3)
 * @param rx_vlan_id RX VLAN ID (for Sync/Delay_Resp)
 * @param tx_vlan_id TX VLAN ID (for Delay_Req)
 * @param tx_vl_idx TX VL-IDX (for Delay_Req packet) - request
 * @param rx_vl_idx RX VL-IDX (for Sync/Delay_Resp) - response
 */
void ptp_session_init(ptp_session_t *session,
                      uint16_t rx_port_id,
                      uint16_t tx_port_id,
                      uint8_t session_idx,
                      uint16_t rx_vlan_id,
                      uint16_t tx_vlan_id,
                      uint16_t tx_vl_idx,
                      uint16_t rx_vl_idx);

/**
 * Reset session statistics
 * @param session PTP session
 */
void ptp_session_reset_stats(ptp_session_t *session);

/**
 * Get session statistics
 * @param session PTP session
 * @param stats Output stats structure
 */
void ptp_session_get_stats(const ptp_session_t *session,
                           ptp_session_stats_t *stats);

// ==========================================
// PTP STATISTICS API
// ==========================================

/**
 * Get statistics for all PTP sessions
 * @param stats Output array (must be PTP_MAX_SESSIONS size)
 * @param count Output: number of sessions
 */
void ptp_get_stats(ptp_session_stats_t *stats, uint8_t *count);

/**
 * Get statistics for a specific port
 * @param port_id DPDK port ID
 * @param stats Output array (must be PTP_SESSIONS_PER_PORT size)
 * @param count Output: number of sessions
 */
void ptp_get_port_stats(uint16_t port_id,
                        ptp_session_stats_t *stats,
                        uint8_t *count);

/**
 * Print PTP statistics to stdout
 */
void ptp_print_stats(void);

/**
 * Reset all PTP statistics
 */
void ptp_reset_stats(void);

// ==========================================
// PTP GLOBAL CONTEXT ACCESS
// ==========================================

/**
 * Get global PTP context
 * @return Pointer to global ptp_context_t
 */
ptp_context_t *ptp_get_context(void);

/**
 * Get PTP port by port ID
 * @param port_id DPDK port ID
 * @return Pointer to ptp_port_t or NULL
 */
ptp_port_t *ptp_get_port(uint16_t port_id);

/**
 * Get PTP session by port and VLAN
 * @param port_id DPDK port ID
 * @param vlan_id VLAN ID
 * @return Pointer to ptp_session_t or NULL
 */
ptp_session_t *ptp_get_session(uint16_t port_id, uint16_t vlan_id);

// ==========================================
// PTP RAW SOCKET API (Port 12/13)
// ==========================================

/**
 * Initialize raw socket PTP port
 * Creates AF_PACKET sockets for PTP RX/TX on the given interface
 * @param port PTP port structure
 * @param iface_name Kernel interface name (e.g. "eno12399")
 * @return 0 on success, negative on error
 */
int ptp_raw_socket_init(ptp_port_t *port, const char *iface_name);

/**
 * Raw socket PTP worker thread function
 * @param arg Pointer to ptp_port_t
 * @return NULL
 */
void *ptp_raw_worker_main(void *arg);

/**
 * Build and send Delay_Req via raw socket (no VLAN)
 * @param session PTP session
 * @param port PTP port with raw socket
 * @param tx_tsc Output: TSC timestamp
 * @return 0 on success, negative on error
 */
int ptp_send_delay_req_raw(ptp_session_t *session,
                            ptp_port_t *port,
                            uint64_t *tx_tsc);

/**
 * Process raw PTP packet buffer (non-DPDK)
 * @param session PTP session
 * @param pkt_data Raw packet data
 * @param pkt_len Packet length
 * @param rx_tsc RX TSC timestamp
 * @return 0 on success, negative on error
 */
int ptp_raw_packet_process(ptp_session_t *session,
                            const uint8_t *pkt_data,
                            uint32_t pkt_len,
                            uint64_t rx_tsc);

// ==========================================
// PTP QUEUE CONFIGURATION
// ==========================================

/**
 * Get PTP TX queue ID
 * @return TX queue ID for PTP (5)
 */
static inline uint16_t ptp_get_tx_queue(void) {
    return PTP_TX_QUEUE_ID;
}

/**
 * Get PTP RX queue ID
 * @return RX queue ID for PTP (5)
 */
static inline uint16_t ptp_get_rx_queue(void) {
    return PTP_RX_QUEUE_ID;
}

#endif /* PTP_SLAVE_H */