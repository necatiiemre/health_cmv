#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#include "Helpers.h" // helper_reset_stats, helper_print_stats, signal_handler / force_quit
#include "PortManager.h"
#include "EalInit.h"
#include "Socket.h"
#include "Packet.h"
#include "TxRxManager.h"
#include "AteMode.h"         // ATE test mode selection

// Check if --daemon flag is present and remove it from argv
// Returns true if --daemon was found, also updates argc
static bool check_and_remove_daemon_flag(int *argc, char const *argv[]) {
    bool found = false;
    int new_argc = 0;

    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0 || strcmp(argv[i], "-d") == 0) {
            found = true;
            // Skip this argument (don't copy to new position)
        } else {
            // Keep this argument
            argv[new_argc] = argv[i];
            new_argc++;
        }
    }

    *argc = new_argc;
    return found;
}

// force_quit and signal_handler are typically declared/defined in helpers.h.
// If not present in your helpers.h, you can uncomment these lines:
// volatile bool force_quit = false;
// static void signal_handler(int sig) { (void)sig; force_quit = true; }

int main(int argc, char const *argv[])
{
    // Check for --daemon flag BEFORE anything else, and remove it from argv
    // so it doesn't confuse DPDK EAL argument parser
    bool daemon_mode = check_and_remove_daemon_flag(&argc, argv);

    // Set daemon mode flag for helper functions (disables ANSI escape codes in logs)
    helper_set_daemon_mode(daemon_mode);

    printf("=== DPDK TX/RX Application with PRBS-31 & Sequence Validation ===\n");
    if (daemon_mode) {
        printf("Mode: DAEMON (will fork to background)\n");
    } else {
        printf("Mode: FOREGROUND (use --daemon for background mode)\n");
    }
    printf("TX Cores: %d | RX Cores: %d | VLAN: %s\n",
           NUM_TX_CORES, NUM_RX_CORES,
#if VLAN_ENABLED
           "Enabled"
#else
           "Disabled"
#endif
    );
    printf("PRBS Method: Sequence-based with ~268MB cache per port\n");
    printf("Payload format: [8-byte sequence][PRBS-31 data]\n");
    printf("WARM-UP: First 60 seconds (stats will reset at 60s)\n");
    printf("Sequence Validation: Enabled (Lost/Out-of-Order/Duplicate detection)\n");
    printf("\n");

    // ATE test mode selection (interactive, before daemon fork)
    ate_mode_selection();

    // Load appropriate VLAN config based on ATE mode selection
    port_vlans_load_config(ate_mode_enabled());

    // Daemon mode: fork to background for remote execution from main PC
    if (daemon_mode) {
        printf("=== Switching to background mode for DPDK operation ===\n\n");
        fflush(stdout);
        fflush(stderr);

        // Fork to background: Parent exits (SSH closes), Child continues
        pid_t pid = fork();
        if (pid < 0) {
            // Fork failed
            perror("fork failed");
            printf("Continuing in foreground mode...\n");
        } else if (pid > 0) {
            // Parent process - exit so SSH connection closes
            printf("DPDK VMC continuing in background (PID: %d)\n", pid);
            printf("Log file: /tmp/dpdk_app.log\n");
            printf("To monitor: ssh user@server 'tail -f /tmp/dpdk_app.log'\n");
            printf("To stop: ssh user@server 'sudo pkill -f dpdk_app'\n");
            fflush(stdout);
            _exit(0);  // Use _exit to avoid flushing stdio buffers twice
        } else {
            // Child process - continue running DPDK in background
            // Create new session to detach from terminal
            setsid();

            // Redirect stdout/stderr to log file
            int log_fd = open("/tmp/dpdk_app.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }

            // Close stdin
            close(STDIN_FILENO);
            open("/dev/null", O_RDONLY);

            printf("\n=== DPDK VMC Background Mode Started (PID: %d) ===\n", getpid());
            printf("Initializing DPDK EAL...\n\n");
            fflush(stdout);
        }
    } else {
        // Foreground mode - continue normally
        printf("=== Continuing in foreground mode ===\n\n");
    }

    // Initialize DPDK EAL
    initialize_eal(argc, argv);

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Print basic EAL info
    print_eal_info();

    // Initialize ports
    int nb_ports = initialize_ports(&ports_config);
    if (nb_ports < 0)
    {
        printf("Error: Failed to initialize ports\n");
        cleanup_eal();
        return -1;
    }

    printf("Found %d ports\n", nb_ports);

    // Setup port configuration
    set_manual_pci_addresses(&ports_config);
    portNumaNodesMatch(&ports_config);

    // Setup socket to lcore mapping
    socketToLcore();

    // Assign lcores to ports
    lcorePortAssign(&ports_config);

    // Initialize VLAN configuration + print
    init_vlan_config();
    print_vlan_config();

    // Initialize RX verification stats (PRBS good/bad/bit_errors + sequence stats)
    init_rx_stats();

#if STATS_MODE_VMC
    // Initialize VMC port mapping and statistics
    init_vmc_port_map();
    init_vmc_stats();
#endif

    // *** PRBS-31 CACHE INITIALIZATION ***
    printf("\n=== Initializing PRBS-31 Cache ===\n");
    printf("This will take a few minutes as we generate ~%u MB per port...\n",
           (unsigned)(PRBS_CACHE_SIZE / (1024 * 1024)));

    init_prbs_cache_for_all_ports((uint16_t)nb_ports, &ports_config);

    printf("PRBS-31 cache initialization complete!\n\n");

    // Configure TX/RX for each port
    printf("\n=== Configuring Ports ===\n");
    struct txrx_config txrx_configs[MAX_PORTS];

    for (uint16_t i = 0; i < (uint16_t)nb_ports; i++)
    {
        uint16_t port_id = ports_config.ports[i].port_id;
        uint16_t socket_id = ports_config.ports[i].numa_node;

        // Create mbuf pool
        struct rte_mempool *mbuf_pool = create_mbuf_pool(socket_id, port_id);
        if (mbuf_pool == NULL)
        {
            printf("Failed to create mbuf pool for port %u\n", port_id);
            cleanup_prbs_cache();
            cleanup_ports(&ports_config);
            cleanup_eal();
            return -1;
        }

        // Setup TX/RX configuration
        txrx_configs[i].port_id = port_id;

        // Calculate number of TX queues needed
        // Base: NUM_TX_CORES (0 to NUM_TX_CORES-1)
        // External TX: +1 queue (queue 4)
        uint16_t num_tx_queues = NUM_TX_CORES;

        txrx_configs[i].nb_tx_queues = num_tx_queues;

        // Calculate number of RX queues needed
        // Base: NUM_RX_CORES (0 to NUM_RX_CORES-1)
        uint16_t num_rx_queues = NUM_RX_CORES;

        txrx_configs[i].nb_rx_queues = num_rx_queues;
        txrx_configs[i].mbuf_pool = mbuf_pool;

        // Initialize port TX/RX
        int ret = init_port_txrx(port_id, &txrx_configs[i]);
        if (ret < 0)
        {
            printf("Failed to initialize TX/RX for port %u\n", port_id);
            cleanup_prbs_cache();
            cleanup_ports(&ports_config);
            cleanup_eal();
            return -1;
        }
    }

    print_ports_info(&ports_config);

    printf("All ports configured\n");

    // Start TX/RX workers
    printf("\n=== Starting Workers ===\n");
    printf("Configuration Check:\n");
    printf("  Ports detected: %d\n", nb_ports);
    printf("  TX cores per port: %d\n", NUM_TX_CORES);
    printf("  RX cores per port: %d\n", NUM_RX_CORES);
    printf("  Expected TX workers: %d\n", nb_ports * NUM_TX_CORES);
    printf("  Expected RX workers: %d\n", nb_ports * NUM_RX_CORES);
    printf("  PRBS-31 cache: Ready (~%.2f GB total)\n",
           (nb_ports * PRBS_CACHE_SIZE) / (1024.0 * 1024.0 * 1024.0));
    printf("  Payload per packet: %u bytes (SEQ: %u + PRBS: %u)\n",
           PAYLOAD_SIZE, SEQ_BYTES, NUM_PRBS_BYTES);
    printf("  Sequence Validation: ENABLED\n");
#if LATENCY_TEST_ENABLED
    printf("  Latency Test: ENABLED (will run before normal mode)\n");
#endif
    printf("\n");

#if LATENCY_TEST_ENABLED
    // *** LATENCY TEST - RUNS BEFORE NORMAL MODE ***
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║            LATENCY TEST MODE ENABLED                             ║\n");
    printf("║  1 packet will be sent from each VLAN, latency will be measured   ║\n");
    printf("║  Normal mode will resume after test                              ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    int latency_ret = start_latency_test(&ports_config, &force_quit);
    if (latency_ret < 0) {
        printf("Warning: Latency test failed, continuing with normal mode\n");
    }

    // Check if user pressed Ctrl+C during latency test
    if (force_quit) {
        printf("User interrupted during latency test, exiting...\n");
        cleanup_prbs_cache();
        cleanup_ports(&ports_config);
        cleanup_eal();
        return 0;
    }

    printf("\n=== Latency test complete, starting normal TX/RX workers ===\n\n");
#endif

    int start_ret = start_txrx_workers(&ports_config, &force_quit);
    if (start_ret < 0)
    {
        printf("Failed to start TX/RX workers\n");
        cleanup_prbs_cache();
        cleanup_ports(&ports_config);
        cleanup_eal();
        return -1;
    }

    printf("\n=== Running (Press Ctrl+C to stop) ===\n");
    printf("  WARM-UP PHASE: First 60 seconds (stats will reset)\n\n");

    // Previous TX/RX bytes for per-second rate calculation
    static uint64_t prev_tx_bytes[MAX_PORTS] = {0};
    static uint64_t prev_rx_bytes[MAX_PORTS] = {0};

    // Main loop - print stats table every second
    uint32_t loop_count = 0;
    bool warmup_complete = false;
    uint32_t test_time = 0;

    while (!force_quit)
    {
        sleep(1);
        loop_count++;

        // Reset when warm-up is complete
        if (loop_count == 120 && !warmup_complete)
        {
            printf("\n");
            printf("═══════════════════════════════════════════════════════════════\n");
            printf("   WARM-UP COMPLETE - RESETTING STATS - TEST STARTING NOW\n");
            printf("═══════════════════════════════════════════════════════════════\n");
            printf("\n");

            // Log Test Start Time for PDF report
            {
                time_t now_t = time(NULL);
                struct tm *tm_info = localtime(&now_t);
                char time_buf[64];
                strftime(time_buf, sizeof(time_buf), "%B %d, %Y %H:%M:%S", tm_info);
                printf("Test Start Time : %s\n", time_buf);
            }

            helper_reset_stats(&ports_config, prev_tx_bytes, prev_rx_bytes);

            warmup_complete = true;
            test_time = 0;

            // Short wait for visibility
            sleep(2);
            continue;
        }

        // Increment test time after warm-up
        if (warmup_complete)
        {
            test_time++;
        }

        // Full table + queue distributions (includes DPDK External TX stats)
        helper_print_stats(&ports_config, prev_tx_bytes, prev_rx_bytes,
                           warmup_complete, loop_count, test_time);

        fflush(stdout);  // Ensure output is visible on remote/main computer

        // Update prev_* for the NEXT second: (cumulative HW byte counters)
        // helper_print_stats calculates per-second rates based on prev_* difference.
        for (uint16_t i = 0; i < (uint16_t)nb_ports; i++)
        {
            uint16_t port_id = ports_config.ports[i].port_id;
            struct rte_eth_stats st;
            if (rte_eth_stats_get(port_id, &st) == 0)
            {
                prev_tx_bytes[port_id] = st.obytes;
                prev_rx_bytes[port_id] = st.ibytes;
            }
        }
    }

    // Log Test Finish Time for PDF report
    {
        time_t now_t = time(NULL);
        struct tm *tm_info = localtime(&now_t);
        char time_buf[64];
        strftime(time_buf, sizeof(time_buf), "%B %d, %Y %H:%M:%S", tm_info);
        printf("Test Finish Time : %s\n", time_buf);
    }

    printf("\n=== Shutting down ===\n");

    printf("Waiting 5 seconds for RX counters to flush...\n");
    sleep(15);

    // Wait for all DPDK workers to stop
    rte_eal_mp_wait_lcore();

    // Cleanup
    cleanup_prbs_cache();
    cleanup_ports(&ports_config);
    cleanup_eal();

    printf("Application exited cleanly\n");

    if (warmup_complete)
    {
        printf("\n Total test duration: %u seconds (after warm-up)\n", test_time);
    }

    return 0;
}