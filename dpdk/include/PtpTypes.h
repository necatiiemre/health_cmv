#ifndef PTP_TYPES_H
#define PTP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <rte_ether.h>

// ==========================================
// PTP IEEE 1588v2 CONSTANTS
// ==========================================

// PTP EtherType
#define PTP_ETHERTYPE           0x88F7

// PTP Message Types
#define PTP_MSG_SYNC            0x00
#define PTP_MSG_DELAY_REQ       0x01
#define PTP_MSG_PDELAY_REQ      0x02
#define PTP_MSG_PDELAY_RESP     0x03
#define PTP_MSG_FOLLOW_UP       0x08
#define PTP_MSG_DELAY_RESP      0x09
#define PTP_MSG_PDELAY_RESP_FU  0x0A
#define PTP_MSG_ANNOUNCE        0x0B
#define PTP_MSG_SIGNALING       0x0C
#define PTP_MSG_MANAGEMENT      0x0D

// PTP Control Field values (for IEEE 1588v1 compatibility)
#define PTP_CTRL_SYNC           0x00
#define PTP_CTRL_DELAY_REQ      0x01
#define PTP_CTRL_FOLLOW_UP      0x02
#define PTP_CTRL_DELAY_RESP     0x03
#define PTP_CTRL_MANAGEMENT     0x04
#define PTP_CTRL_OTHER          0x05

// PTP Header Lengths
#define PTP_HEADER_LEN          34
#define PTP_SYNC_LEN            44    // Header (34) + Origin timestamp (10)
#define PTP_DELAY_REQ_LEN       44    // Header (34) + Origin timestamp (10)
#define PTP_DELAY_RESP_LEN      54    // Header (34) + Receive timestamp (10) + Requesting port ID (10)

// PTP Multicast MAC addresses (Layer 2)
#define PTP_MAC_PDELAY          {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E}  // Peer delay
#define PTP_MAC_DEFAULT         {0x01, 0x1B, 0x19, 0x00, 0x00, 0x00}  // Default (E2E)

// PTP Flags
#define PTP_FLAG_TWO_STEP       0x0200
#define PTP_FLAG_UNICAST        0x0400
#define PTP_FLAG_ALTERNATE_MASTER 0x0100

// PTP Version
#define PTP_VERSION             2

// Default values
#define PTP_DEFAULT_DOMAIN      10   // Domain number (matches DTN switch)
#define PTP_LOG_MSG_INTERVAL    0    // 2^0 = 1 second (for Sync)
#define PTP_LOG_DELAY_REQ_INT   -1   // 2^-1 = 0.5 second (for Delay_Req)

// PTP Flags (as seen in Wireshark capture)
#define PTP_FLAGS_DEFAULT       0x0002  // Flags from DTN switch (0x0102 in big-endian = 0x0201)

// ==========================================
// PTP SESSION CONFIGURATION
// ==========================================

#define PTP_MAX_PORTS           14   // 0-7 DPDK, 12-13 raw socket
#define PTP_SESSIONS_PER_PORT   4
#define PTP_MAX_SESSIONS        (PTP_MAX_PORTS * PTP_SESSIONS_PER_PORT)

// Raw socket port detection
#define PTP_RAW_SOCKET_PORT_START  12
#define PTP_IS_RAW_SOCKET_PORT(port_id)  ((port_id) >= PTP_RAW_SOCKET_PORT_START)

// PTP Queue assignments
#define PTP_TX_QUEUE_ID         5
#define PTP_RX_QUEUE_ID         5

// VL-ID range for PTP (starts at 4500+)
#define PTP_VL_ID_BASE          4500

// ==========================================
// PTP STATE MACHINE
// ==========================================

typedef enum {
    PTP_STATE_INIT = 0,          // Initial state, waiting for configuration
    PTP_STATE_LISTENING,         // Waiting for Sync message from master
    PTP_STATE_SYNC_RECEIVED,     // Sync received, t1 and t2 available
    PTP_STATE_DELAY_REQ_SENT,    // Delay_Req sent, waiting for Delay_Resp
    PTP_STATE_SYNCED,            // Fully synchronized (t1,t2,t3,t4 valid)
    PTP_STATE_ERROR              // Error state
} ptp_state_t;

// ==========================================
// PTP TIMESTAMP (IEEE 1588 format)
// ==========================================

// 80-bit timestamp: 48-bit seconds + 32-bit nanoseconds
typedef struct __attribute__((packed)) {
    uint16_t seconds_msb;        // Upper 16 bits of seconds
    uint32_t seconds_lsb;        // Lower 32 bits of seconds
    uint32_t nanoseconds;        // Nanoseconds (0 - 999,999,999)
} ptp_timestamp_t;

// ==========================================
// PTP PORT IDENTITY
// ==========================================

typedef struct __attribute__((packed)) {
    uint8_t  clock_identity[8];  // 64-bit clock identity (usually MAC-based)
    uint16_t port_number;        // Port number
} ptp_port_identity_t;

// ==========================================
// PTP MESSAGE HEADER (Common Header)
// ==========================================

typedef struct __attribute__((packed)) {
    uint8_t  msg_type : 4;       // Message type (0-15)
    uint8_t  transport : 4;      // Transport specific (upper nibble)
    uint8_t  version;            // PTP version (2)
    uint16_t msg_length;         // Total message length
    uint8_t  domain_number;      // Domain number
    uint8_t  reserved1;          // Reserved
    uint16_t flags;              // Flags
    int64_t  correction;         // Correction field (ns * 2^16)
    uint32_t reserved2;          // Reserved
    ptp_port_identity_t source_port_id;  // Source port identity
    uint16_t sequence_id;        // Sequence ID
    uint8_t  control;            // Control field (deprecated in v2)
    int8_t   log_msg_interval;   // Log message interval
} ptp_header_t;

// ==========================================
// PTP MESSAGE BODIES
// ==========================================

// Sync message (also used for Delay_Req)
typedef struct __attribute__((packed)) {
    ptp_header_t    header;
    ptp_timestamp_t origin_timestamp;  // t1 (master's TX time for Sync)
} ptp_sync_msg_t;

// Delay_Resp message
typedef struct __attribute__((packed)) {
    ptp_header_t        header;
    ptp_timestamp_t     receive_timestamp;   // t4 (master's RX time for Delay_Req)
    ptp_port_identity_t requesting_port_id;  // Slave's port identity
} ptp_delay_resp_msg_t;

// ==========================================
// PTP SESSION DATA (Per VLAN)
// ==========================================

typedef struct {
    // Session identification (Split TX/RX port support)
    uint16_t port_id;            // RX port ID - session lives here (receives Sync/Delay_Resp)
    uint16_t tx_port_id;         // TX port ID - for sending Delay_Req (may differ from port_id)
    uint16_t vlan_id;            // VLAN ID for this session (primary, usually rx_vlan)
    uint16_t rx_vlan_id;         // RX VLAN ID (for Sync/Delay_Resp)
    uint16_t tx_vlan_id;         // TX VLAN ID (for Delay_Req)
    uint16_t tx_vl_idx;          // TX VL-IDX (for Delay_Req) - configured
    uint16_t rx_vl_idx;          // RX VL-IDX (read from Sync packet) - runtime
    uint8_t  session_idx;        // Session index within port (0-3)

    // State machine
    ptp_state_t state;
    uint64_t last_state_change;  // TSC at last state change

    // Master information (from received Sync)
    ptp_port_identity_t master_port_id;
    uint8_t  master_domain;

    // Our clock identity
    ptp_port_identity_t our_port_id;

    // Sequence tracking
    uint16_t sync_seq_id;        // Last received Sync sequence ID
    uint16_t delay_req_seq_id;   // Our Delay_Req sequence ID counter
    uint16_t last_delay_req_seq_id;  // Sequence ID of last sent Delay_Req

    // Timestamps
    // t1: Master's TX timestamp for Sync (from Sync packet) - PTP epoch (1970)
    // t2: Our RX timestamp for Sync - both TSC and realtime
    // t3: Our TX timestamp for Delay_Req - both TSC and realtime
    // t4: Master's RX timestamp for Delay_Req (from Delay_Resp) - PTP epoch (1970)

    uint64_t t1_ns;              // Master TX time (from Sync) - PTP epoch
    uint64_t t2_tsc;             // Our RX time (rte_rdtsc) - for delay calc
    uint64_t t2_realtime_ns;     // Our RX time (clock_gettime) - for offset calc
    uint64_t t3_tsc;             // Our TX time (rte_rdtsc) - for delay calc
    uint64_t t3_realtime_ns;     // Our TX time (clock_gettime) - for offset calc
    uint64_t t4_ns;              // Master RX time (from Delay_Resp) - PTP epoch

    // Calculated values (in nanoseconds)
    int64_t  offset_ns;          // Clock offset = ((t2-t1) - (t4-t3)) / 2
    int64_t  delay_ns;           // Path delay = ((t2-t1) + (t4-t3)) / 2

    // TSC to nanoseconds conversion
    uint64_t tsc_hz;             // TSC frequency (from rte_get_tsc_hz)

    // Statistics
    uint64_t sync_rx_count;      // Total Sync messages received
    uint64_t delay_req_tx_count; // Total Delay_Req messages sent
    uint64_t delay_resp_rx_count;// Total Delay_Resp messages received
    uint64_t sync_timeout_count; // Sync timeout count
    uint64_t sync_errors;        // Sequence errors, etc.

    // Timing
    uint64_t last_sync_tsc;      // TSC of last Sync received
    uint64_t last_delay_req_tsc; // TSC of last Delay_Req sent

    // Sync status
    bool     is_synced;          // True if sync is established
    uint32_t sync_count;         // Successful sync count
} ptp_session_t;

// ==========================================
// PTP PORT DATA (Per DPDK Port)
// ==========================================

typedef struct {
    uint16_t        port_id;                        // DPDK port ID
    uint16_t        session_count;                  // Number of sessions (4)
    ptp_session_t   sessions[PTP_SESSIONS_PER_PORT]; // 4 sessions per port
    bool            enabled;                        // PTP enabled on this port
    struct rte_mempool *tx_mbuf_pool;              // TX mbuf pool for PTP packets (DPDK only)
    uint16_t        ptp_lcore_id;                  // Assigned lcore for PTP (DPDK only)

    // Raw socket support (Port 12/13)
    bool            is_raw_socket;                  // true for raw socket ports
    int             raw_rx_fd;                      // AF_PACKET socket for PTP RX
    int             raw_tx_fd;                      // AF_PACKET socket for PTP TX
    int             raw_if_index;                   // Interface index
    char            raw_iface_name[32];             // Interface name (e.g. "eno12399")
    pthread_t       raw_worker_thread;              // Worker thread for raw socket PTP
    volatile bool   raw_worker_running;             // Worker thread running flag
} ptp_port_t;

// ==========================================
// PTP GLOBAL CONTEXT
// ==========================================

typedef struct {
    ptp_port_t      ports[PTP_MAX_PORTS];           // 8 ports
    uint8_t         port_count;                     // Active port count
    bool            initialized;                    // Initialization complete
    bool            running;                        // PTP workers running
    uint64_t        tsc_hz;                         // TSC frequency
    struct rte_ether_addr local_mac;                // Local MAC for clock ID
} ptp_context_t;

// ==========================================
// PTP STATISTICS (Per Session, for display)
// ==========================================

typedef struct {
    uint16_t port_id;
    uint16_t vlan_id;
    const char *state_str;
    int64_t  offset_ns;
    int64_t  delay_ns;
    uint64_t sync_rx_count;
    uint64_t delay_req_tx_count;
    uint64_t delay_resp_rx_count;
    bool     is_synced;
} ptp_session_stats_t;

// ==========================================
// HELPER MACROS
// ==========================================

// Convert PTP timestamp to nanoseconds
// NOTE: DTN uses non-standard format where seconds_msb is a fixed constant.
// We only use seconds_lsb (4 bytes) for the actual seconds value.
static inline uint64_t ptp_timestamp_to_ns(const ptp_timestamp_t *ts) {
    // Only use seconds_lsb (last 4 bytes), ignore seconds_msb (fixed in DTN)
    uint64_t seconds = rte_be_to_cpu_32(ts->seconds_lsb);
    uint32_t nanos = rte_be_to_cpu_32(ts->nanoseconds);
    return seconds * 1000000000ULL + nanos;
}

// Convert nanoseconds to PTP timestamp
// NOTE: Sets seconds_msb to 0 since DTN ignores it
static inline void ns_to_ptp_timestamp(uint64_t ns, ptp_timestamp_t *ts) {
    uint64_t seconds = ns / 1000000000ULL;
    uint32_t nanos = ns % 1000000000ULL;
    ts->seconds_msb = 0;  // DTN uses fixed value, we set to 0
    ts->seconds_lsb = rte_cpu_to_be_32((uint32_t)seconds);
    ts->nanoseconds = rte_cpu_to_be_32(nanos);
}

// Convert TSC to nanoseconds
static inline uint64_t tsc_to_ns(uint64_t tsc, uint64_t tsc_hz) {
    return (tsc * 1000000000ULL) / tsc_hz;
}

// Get state name string
static inline const char *ptp_state_to_str(ptp_state_t state) {
    switch (state) {
        case PTP_STATE_INIT:           return "INIT";
        case PTP_STATE_LISTENING:      return "LISTENING";
        case PTP_STATE_SYNC_RECEIVED:  return "SYNC_RECV";
        case PTP_STATE_DELAY_REQ_SENT: return "DELAY_SENT";
        case PTP_STATE_SYNCED:         return "SYNCED";
        case PTP_STATE_ERROR:          return "ERROR";
        default:                       return "UNKNOWN";
    }
}

// Get message type name string
static inline const char *ptp_msg_type_to_str(uint8_t msg_type) {
    switch (msg_type) {
        case PTP_MSG_SYNC:        return "SYNC";
        case PTP_MSG_DELAY_REQ:   return "DELAY_REQ";
        case PTP_MSG_DELAY_RESP:  return "DELAY_RESP";
        case PTP_MSG_FOLLOW_UP:   return "FOLLOW_UP";
        case PTP_MSG_ANNOUNCE:    return "ANNOUNCE";
        default:                  return "UNKNOWN";
    }
}

#endif /* PTP_TYPES_H */