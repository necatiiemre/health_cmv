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
