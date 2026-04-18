/**
 * @file PsuTelemetryReceiver.h
 * @brief UDP listener that caches the latest PSU measurement packet pushed
 *        by MainSoftware, and prints it as a health-table row.
 *
 * Threading model:
 *   - One background pthread does blocking recvfrom() with a short SO_RCVTIMEO
 *     so stop() is responsive.
 *   - The cache is guarded by a pthread_spinlock; readers (print) take a
 *     snapshot under the lock.
 *
 * Lifecycle: init -> start -> print (repeatedly, from main loop) -> stop.
 */
#ifndef PSU_TELEMETRY_RECEIVER_H
#define PSU_TELEMETRY_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the receiver: bind a UDP socket on 0.0.0.0:port.
 * @param port UDP port (pass PSU_TELEM_PORT = 20050 for the wire default).
 * @return 0 on success, negative on failure.
 */
int psu_telem_init(uint16_t port);

/**
 * Start the listener thread. Must be called after psu_telem_init().
 * @param force_quit global shutdown flag; when it becomes true the thread
 *                   exits its loop cleanly.
 * @return 0 on success, negative on failure.
 */
int psu_telem_start(volatile bool *force_quit);

/**
 * Stop the listener thread and close the socket. Idempotent.
 */
void psu_telem_stop(void);

/**
 * Print the current PSU measurement as a [HEALTH]-prefixed table block to
 * stdout. Safe to call every print cycle. If no packet has been received
 * yet, prints a "NO DATA" row. If the cache is older than 3 seconds prints
 * a "STALE" row instead of the regular one.
 */
void psu_telem_print_table(void);

/**
 * Diagnostics.
 */
uint64_t psu_telem_packets_received(void);
uint64_t psu_telem_packets_dropped(void);
bool     psu_telem_is_running(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSU_TELEMETRY_RECEIVER_H */
