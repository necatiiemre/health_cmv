#include "Socket.h"
#include <stdio.h>

void socketToLcore()
{
    unsigned lcore_id;
    for (unsigned socket = 0; socket <= MAX_SOCKET; socket++)
    {
        uint16_t lcore_index = 0;
        RTE_LCORE_FOREACH(lcore_id)
        {
            if (rte_lcore_to_socket_id(lcore_id) == socket)
            {
                socket_to_lcore[socket][lcore_index++] = lcore_id;
            }
        }
    }

   for (unsigned socket = 0; socket < MAX_SOCKET; socket++)
    {
        for (unsigned lcore_idx = 0; lcore_idx < MAX_LCORE_PER_SOCKET; lcore_idx++)
        {
            unused_socket_to_lcore[socket][lcore_idx] = socket_to_lcore[socket][lcore_idx];
        }
    }
}

int get_unused_cores(int count, uint16_t *cores)
{
    if (count <= 0 || cores == NULL) {
        return 0;
    }

    int found = 0;

    // Iterate through all sockets and find unused cores
    // Start from HIGH index to leave first cores unused (for other purposes)
    // This matches DPDK port core assignment which also starts from high cores
    for (int socket = 0; socket < MAX_SOCKET && found < count; socket++) {
        for (int idx = MAX_LCORE_PER_SOCKET - 1; idx >= 0 && found < count; idx--) {
            uint16_t lcore = unused_socket_to_lcore[socket][idx];
            if (lcore != 0) {
                cores[found] = lcore;
                unused_socket_to_lcore[socket][idx] = 0;  // Mark as used
                found++;
                printf("  Allocated unused core %u for raw socket (socket %d, idx %d)\n",
                       lcore, socket, idx);
            }
        }
    }

    if (found < count) {
        printf("  Warning: Requested %d cores but only %d available\n", count, found);
    }

    return found;
}
