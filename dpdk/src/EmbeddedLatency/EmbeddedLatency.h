/**
 * @file embedded_latency.h
 * @brief Embedded HW Timestamp Latency Test for DPDK
 *
 * Runs BEFORE DPDK EAL starts (uses raw sockets).
 * Results are stored in a global struct, accessible from within DPDK.
 *
 * IMPORTANT: This test must run BEFORE DPDK takes over the NICs!
 */

#ifndef EMBEDDED_LATENCY_H
#define EMBEDDED_LATENCY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================
// CONFIGURATION
// ============================================
#define EMB_LAT_MAX_RESULTS     64      // Maximum VLAN results
#define EMB_LAT_MAX_PORT_PAIRS  10      // Maximum port pairs (8 fiber + 2 copper)
#define EMB_LAT_DEFAULT_SWITCH_US 14.0  // Default Mellanox switch latency (microseconds)

// Store-and-forward serialization delay
// DUT is store-and-forward (buffers entire frame before forwarding)
// Loopback adapter is passive (no buffering), so this delay only exists in unit test path
// Fiber (VLAN tagged): 1518 frame + 4 FCS = 1522 bytes → 1G: 12.176 µs
// Copper (untagged):   1514 frame + 4 FCS = 1518 bytes → 1G: 12.144 µs, 100M: 121.44 µs
// Using 12.176 for both 1G cases (0.032 µs difference is negligible)
#define EMB_LAT_STORE_FWD_DELAY_1G_US    12.176
#define EMB_LAT_STORE_FWD_DELAY_100M_US  121.44
#define EMB_LAT_STORE_FWD_DELAY_NIC_US   EMB_LAT_STORE_FWD_DELAY_1G_US  // Default (fiber)

// Unit (device) latency threshold - PASS if below, FAIL if above
#define EMB_LAT_UNIT_THRESHOLD_US 30.0

// Copper port configuration
#define EMB_LAT_COPPER_PORT_12_IFACE "eno12399"
#define EMB_LAT_COPPER_PORT_13_IFACE "eno12409"
#define EMB_LAT_COPPER_PORT_14_IFACE "eno12419"
#define EMB_LAT_COPPER_PORT_15_IFACE "eno12429"
#define EMB_LAT_COPPER_VLID_12_TO_13  4489
#define EMB_LAT_COPPER_VLID_13_TO_12  4490

// ============================================
// TEST TYPES
// ============================================
typedef enum {
    EMB_TEST_LOOPBACK,      // Loopback test (Mellanox switch latency)
    EMB_TEST_UNIT           // Unit test (total latency through device)
} emb_test_type_t;

// ============================================
// RESULT STRUCTURE (per VLAN)
// ============================================
struct emb_latency_result {
    uint16_t tx_port;           // TX port ID
    uint16_t rx_port;           // RX port ID
    uint16_t vlan_id;           // VLAN tag
    uint16_t vl_id;             // VL-ID

    uint32_t tx_count;          // Packets sent
    uint32_t rx_count;          // Packets received

    uint64_t min_latency_ns;    // Minimum latency (nanoseconds)
    uint64_t max_latency_ns;    // Maximum latency (nanoseconds)
    uint64_t avg_latency_ns;    // Average latency (nanoseconds)

    bool     valid;             // Valid result?
    bool     passed;            // Latency threshold passed?
    char     error_msg[64];     // Error message
};

// ============================================
// COMBINED LATENCY RESULT (per direction)
// ============================================
struct emb_combined_latency {
    uint16_t tx_port;                   // TX Port (e.g., 0)
    uint16_t rx_port;                   // RX Port (e.g., 1)

    // Switch latency (from loopback test or default)
    double   switch_latency_us;         // Mellanox switch latency (µs)
    bool     switch_measured;           // true = measured, false = default 14µs

    // Total latency (from unit test)
    double   total_latency_us;          // Total latency TX→RX (µs)
    bool     total_measured;            // true = measured

    // Unit (device) latency = total - loopback - store_fwd_delay
    double   unit_latency_us;           // Device latency (µs)
    bool     unit_valid;                // Calculation valid?

    bool     passed;                    // Within threshold?
    bool     is_copper;                 // Copper port (no switch, no VLAN)

    // Legacy aliases for backward compatibility
    #define port_a tx_port
    #define port_b rx_port
};

// ============================================
// GLOBAL STATE
// ============================================
struct emb_latency_state {
    // Loopback test state
    bool     loopback_completed;        // Loopback test ran?
    bool     loopback_passed;           // All loopback tests passed?
    bool     loopback_skipped;          // User skipped loopback test?
    uint32_t loopback_result_count;     // Number of loopback results
    struct emb_latency_result loopback_results[EMB_LAT_MAX_RESULTS];

    // Unit test state
    bool     unit_completed;            // Unit test ran?
    bool     unit_passed;               // All unit tests passed?
    uint32_t unit_result_count;         // Number of unit test results
    struct emb_latency_result unit_results[EMB_LAT_MAX_RESULTS];

    // Combined results (fiber: 0→1..7→6, copper: 12→13, 13→12)
    uint32_t combined_count;            // Number of combined results (8 fiber + 2 copper)
    struct emb_combined_latency combined[EMB_LAT_MAX_PORT_PAIRS];

    // Legacy fields for backward compatibility
    bool     test_completed;            // Any test ran?
    bool     test_passed;               // All tests passed?
    uint32_t result_count;              // Total results
    uint32_t passed_count;              // Passed tests
    uint32_t failed_count;              // Failed tests

    uint64_t overall_min_ns;            // Overall minimum
    uint64_t overall_max_ns;            // Overall maximum
    uint64_t overall_avg_ns;            // Overall average

    uint64_t test_duration_ns;          // Test duration

    struct emb_latency_result results[EMB_LAT_MAX_RESULTS];
};

// Global state - accessible from anywhere in DPDK
extern struct emb_latency_state g_emb_latency;

// ATE test mode flag - set when user selects ATE mode
extern bool g_ate_mode;

// ============================================
// API
// ============================================

/**
 * Run embedded latency test
 * MUST be called BEFORE rte_eal_init()!
 *
 * @param packet_count  Packets per VLAN (default: 1)
 * @param timeout_ms    RX timeout in ms (default: 100)
 * @param max_latency_us Maximum acceptable latency in us (default: 30)
 * @return              0 = all passed, >0 = fail count, <0 = error
 */
int emb_latency_run(int packet_count, int timeout_ms, int max_latency_us);

/**
 * Quick run with defaults
 * @return 0 = all passed, >0 = fail count, <0 = error
 */
int emb_latency_run_default(void);

/**
 * Interactive run with user prompts
 * Asks user if they want to run test and if loopback connectors are installed.
 * Same pattern as Dtn.cpp latencyTestSequence()
 *
 * @return 0 = all passed or skipped, >0 = fail count, <0 = error
 */
int emb_latency_run_interactive(void);

/**
 * Run loopback test (measures Mellanox switch latency)
 * Port pairs: 0→7, 1→6, 2→5, 3→4, etc.
 *
 * @param packet_count  Packets per VLAN
 * @param timeout_ms    RX timeout
 * @param max_latency_us Maximum acceptable latency
 * @return 0 = all passed, >0 = fail count
 */
int emb_latency_run_loopback(int packet_count, int timeout_ms, int max_latency_us);

/**
 * Run unit test (measures total latency through device)
 * Port pairs: 0↔1, 2↔3, 4↔5, 6↔7
 *
 * @param packet_count  Packets per VLAN
 * @param timeout_ms    RX timeout
 * @param max_latency_us Maximum acceptable latency
 * @return 0 = all passed, >0 = fail count
 */
int emb_latency_run_unit_test(int packet_count, int timeout_ms, int max_latency_us);

/**
 * Calculate combined latency (unit_latency = total - loopback - store_fwd_delay)
 * Both paths traverse switch twice, so switch cancels out.
 * Store-and-forward correction removes DUT serialization delay (not in loopback).
 * Must be called after both loopback and unit tests complete
 */
void emb_latency_calculate_combined(void);

/**
 * Full interactive test sequence:
 * 1. Ask for loopback test (or use default 14µs)
 * 2. Run unit test
 * 3. Calculate combined results
 *
 * @return 0 = success, >0 = fail count
 */
int emb_latency_full_sequence(void);

/**
 * Check if ATE test mode is enabled
 */
bool ate_mode_enabled(void);

/**
 * Check if test completed
 */
bool emb_latency_completed(void);

/**
 * Check if all tests passed
 */
bool emb_latency_all_passed(void);

/**
 * Get result count
 */
int emb_latency_get_count(void);

/**
 * Get result by index
 */
const struct emb_latency_result* emb_latency_get(int index);

/**
 * Get result by VLAN ID
 */
const struct emb_latency_result* emb_latency_get_by_vlan(uint16_t vlan_id);

/**
 * Get latency values for a VLAN (in microseconds)
 */
bool emb_latency_get_us(uint16_t vlan_id, double *min, double *avg, double *max);

/**
 * Print all results
 */
void emb_latency_print(void);

/**
 * Print loopback test results
 */
void emb_latency_print_loopback(void);

/**
 * Print unit test results
 */
void emb_latency_print_unit(void);

/**
 * Print summary only
 */
void emb_latency_print_summary(void);

/**
 * Print combined latency results
 */
void emb_latency_print_combined(void);

// ============================================
// COMBINED LATENCY ACCESSORS
// ============================================

/**
 * Get combined latency for a direction (tx_port → rx_port)
 * @param tx_port TX port (0-7)
 * @return Pointer to first matching combined latency struct, or NULL
 */
const struct emb_combined_latency* emb_latency_get_combined(uint16_t tx_port);

/**
 * Get combined latency for a specific direction
 * @param tx_port TX port (0-7)
 * @param rx_port RX port (0-7)
 * @return Pointer to combined latency struct, or NULL
 */
const struct emb_combined_latency* emb_latency_get_combined_direction(uint16_t tx_port, uint16_t rx_port);

/**
 * Get unit (device) latency for a direction in microseconds
 * @param tx_port TX port (0-7)
 * @param unit_latency_us Output: device latency in µs
 * @return true if valid, false otherwise
 */
bool emb_latency_get_unit_us(uint16_t tx_port, double *unit_latency_us);

/**
 * Get all latency components for a direction
 * @param tx_port TX port (0-7)
 * @param switch_us Output: switch latency (µs)
 * @param total_us Output: total latency (µs)
 * @param unit_us Output: device latency (µs)
 * @return true if valid, false otherwise
 */
bool emb_latency_get_all_us(uint16_t tx_port, double *switch_us, double *total_us, double *unit_us);

#ifdef __cplusplus
}
#endif

#endif // EMBEDDED_LATENCY_H