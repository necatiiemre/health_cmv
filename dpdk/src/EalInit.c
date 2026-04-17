#include "EalInit.h"
#include "Common.h"

int initialize_eal(int argc, char const *argv[])
{
    int return_value;

    printf("Initializing DPDK EAL...\n");

    return_value = rte_eal_init(argc, (char **)argv);
    if (return_value < 0)
    {
        fprintf(stderr, "Error with EAL initialization\n");
        rte_exit(EXIT_FAILURE, "EAL init failed\n");
    }

    printf("EAL initialized successfully!\n");
    return return_value;
}

void print_eal_info(void)
{
    printf("\n=== DPDK EAL Information ===\n");
    printf("DPDK Version: %s\n", rte_version());
    printf("Main lcore ID: %u\n", rte_get_main_lcore());
    printf("Total available lcores: %u\n", rte_lcore_count());

    // Print socket to lcore mapping
    printf("Socket to Lcore mapping:\n");

    unsigned max_socket = 0;
    unsigned lcore_id;

    // Find max socket
    RTE_LCORE_FOREACH(lcore_id)
    {
        unsigned socket = rte_lcore_to_socket_id(lcore_id);
        if (socket > max_socket)
        {
            max_socket = socket;
        }
    }

    // Print lcores for each socket
    for (unsigned socket = 0; socket <= max_socket; socket++)
    {
        printf("  Socket %u -> [", socket);
        bool first = true;
        RTE_LCORE_FOREACH(lcore_id)
        {
            if (rte_lcore_to_socket_id(lcore_id) == socket)
            {
                if (!first)
                    printf(", ");
                printf("%u", lcore_id);
                first = false;
            }
        }
        printf("]\n");
    }
}

void print_lcore_info(void)
{
    unsigned lcore_id;

    printf("\nLcore Information:\n");
    printf("Enabled lcores: ");
    RTE_LCORE_FOREACH(lcore_id)
    {
        printf("%u ", lcore_id);
    }
    printf("\n");

    printf("Worker lcores: ");
    RTE_LCORE_FOREACH_WORKER(lcore_id)
    {
        printf("%u ", lcore_id);
    }
    printf("\n");
}

void printSocketToLcoreList()
{

    for (uint16_t socket = 0; socket < MAX_SOCKET; socket++)
    {
        printf("  Socket %u -> [", socket);
        for (uint16_t lcore_id = 0; lcore_id < MAX_LCORE; lcore_id++)
        {
            printf("%u ", socket_to_lcore[socket][lcore_id]);
        }
        printf("]\n");
    }

    for (uint16_t socket = 0; socket < MAX_SOCKET; socket++)
    {
        printf(" Unused  Socket %u -> [", socket);
        for (uint16_t lcore_id = 0; lcore_id < MAX_LCORE; lcore_id++)
        {
            printf("%u ", unused_socket_to_lcore[socket][lcore_id]);
        }
        printf("]\n");
    }
}

void cleanup_eal(void)
{
    printf("Cleaning up EAL resources...\n");
    rte_eal_cleanup();
}
