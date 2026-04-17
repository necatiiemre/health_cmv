#ifndef SOCKET_H
#define SOCKET_H

#include "Common.h"

/**
 * Build socket-to-lcore mapping arrays
 * Populates socket_to_lcore and unused_socket_to_lcore arrays
 * based on system topology
 */
void socketToLcore(void);

/**
 * Get unused lcores for non-DPDK threads (e.g., raw socket)
 * Marks the returned cores as used in unused_socket_to_lcore
 *
 * @param count Number of cores requested
 * @param cores Output array to store core IDs (must be at least 'count' size)
 * @return Number of cores actually found and allocated
 */
int get_unused_cores(int count, uint16_t *cores);

#endif /* SOCKET_H */
