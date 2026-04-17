#pragma once
#ifndef PORT_MANAGER_H
#define PORT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "Port.h"

/**
 * Initialize and discover all available DPDK ports
 */
int initialize_ports(struct ports_config *config);

/**
 * Print port information
 */
void print_ports_info(const struct ports_config *config);

/**
 * Print ports grouped by pairs (assuming 2 ports per NIC)
 */
void print_ports_by_card(const struct ports_config *config);

/**
 * Configure basic settings for a port
 */
int configure_port(uint16_t port_id);

/**
 * Set manual PCI addresses for ports (if auto-detection fails)
 */
void set_manual_pci_addresses(struct ports_config *config);

/**
 * Cleanup ports
 */
void cleanup_ports(struct ports_config *config);

/**
 * Check available ports
 */
void check_avail_ports(struct ports_config *config);

/**
 * Match ports to their NUMA nodes
 */
void portNumaNodesMatch(struct ports_config *config);

/**
 * Assign lcores to port TX/RX queues based on NUMA affinity
 */
void lcorePortAssign(struct ports_config *config);

#endif /* PORT_MANAGER_H */
