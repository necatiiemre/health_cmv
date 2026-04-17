#ifndef SOCKET_H
#define SOCKET_H

#include "Common.h"

/**
 * Build socket-to-lcore mapping arrays
 * Populates socket_to_lcore and unused_socket_to_lcore arrays
 * based on system topology
 */
void socketToLcore(void);

#endif /* SOCKET_H */
