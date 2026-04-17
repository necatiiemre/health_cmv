#include "Common.h"

// Define the global variables here (only once)
struct ports_config ports_config;

uint16_t socket_to_lcore[MAX_SOCKET][MAX_LCORE_PER_SOCKET];
uint16_t unused_socket_to_lcore[MAX_SOCKET][MAX_LCORE_PER_SOCKET];
