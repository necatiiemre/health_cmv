#pragma once
#include "Port.h"

static volatile bool force_quit = false;

#define MAX_SOCKET 8
#define MAX_LCORE_PER_SOCKET 32

// Declare variables as extern (declarations only)
extern struct ports_config ports_config;
extern uint16_t socket_to_lcore[MAX_SOCKET][MAX_LCORE_PER_SOCKET];
extern uint16_t unused_socket_to_lcore[MAX_SOCKET][MAX_LCORE_PER_SOCKET];

// Number of TX/RX cores per port - can be overridden from Makefile
// Number of queues = Number of cores
#ifndef NUM_TX_CORES
#define NUM_TX_CORES 2
#endif

#ifndef NUM_RX_CORES
#define NUM_RX_CORES 4
#endif
