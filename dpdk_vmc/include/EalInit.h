#ifndef EAL_INIT_H
#define EAL_INIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <rte_lcore.h>
#include <rte_version.h>
#include <rte_debug.h>

/**
 * Initialize DPDK EAL (Environment Abstraction Layer)
 * 
 * @param argc Number of command line arguments
 * @param argv Array of command line arguments
 * @return Number of arguments consumed by EAL on success, exits on failure
 */
int initialize_eal(int argc, char const *argv[]);

/**
 * Print EAL information after successful initialization
 */
void print_eal_info(void);

/**
 * Print available lcores information
 */
void print_lcore_info(void);

/**
 * Cleanup EAL resources
 */
void cleanup_eal(void);

/**
 * Print socket to lcore list array
 */
void printSocketToLcoreList();

#endif /* EAL_INIT_H */
