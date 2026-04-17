#pragma once
#include <rte_ethdev.h>

#define MAX_PORTS 8
#define MAX_LCORE 32
#define PCI_ADDR_LEN 32

// These will be defined in common.h but we need forward declaration
#ifndef NUM_TX_CORES
#define NUM_TX_CORES 2
#endif

#ifndef NUM_RX_CORES
#define NUM_RX_CORES 4
#endif

/**
 * Structure to hold port information
 */
struct port {
    uint16_t port_id;
    uint16_t numa_node;                  /* NUMA socket ID */
    char pci_addr[PCI_ADDR_LEN];         /* PCI address (e.g. 0000:01:00.0) */
    char driver_name[32];                /* Driver name */
    bool is_valid;                       /* Port is valid and usable */
    struct rte_ether_addr mac_addr;      /* MAC address */
    uint16_t used_tx_cores[NUM_TX_CORES]; /* Lcores assigned to TX queues */
    uint16_t used_rx_cores[NUM_RX_CORES]; /* Lcores assigned to RX queues */
    uint16_t used_ext_tx_core;           /* Lcore for external TX (ports 0-3) */
    uint16_t used_ptp_core;              /* Lcore for PTP worker */
};

/**
 * Structure to hold all ports information
 */
struct ports_config {
    uint16_t nb_ports;                   /* Number of available ports */
    struct port ports[MAX_PORTS];        /* Array of port information */
};
