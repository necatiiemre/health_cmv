/**
 * PTP Flow Rules
 *
 * Installs rte_flow rules to direct PTP packets (EtherType 0x88F7) to the
 * dedicated PTP RX queue (Queue 5).
 *
 * Flow Rule Pattern:
 *   - Match Ethernet EtherType = 0x88F7 (PTP)
 *   - Or match VLAN-tagged packets with inner EtherType = 0x88F7
 *
 * Flow Rule Action:
 *   - Queue: Direct to PTP_RX_QUEUE (Queue 5)
 */

#include <rte_flow.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <stdio.h>
#include <string.h>

#include "PtpTypes.h"
#include "PtpSlave.h"
#include "Config.h"

// Store flow handles for cleanup
static struct rte_flow *ptp_flow_handles[PTP_MAX_PORTS] = {NULL};

/**
 * Install rte_flow rule for PTP packets on a port
 *
 * Creates a flow rule that matches:
 *   - VLAN-tagged packets with inner EtherType 0x88F7 (PTP)
 *
 * Action:
 *   - Direct to PTP_RX_QUEUE (Queue 5)
 */
int ptp_flow_rule_install(uint16_t port_id)
{
    if (port_id >= PTP_MAX_PORTS) {
        fprintf(stderr, "PTP: Invalid port_id %u\n", port_id);
        return -1;
    }

    // Remove existing rule if any
    if (ptp_flow_handles[port_id]) {
        ptp_flow_rule_remove(port_id);
    }

    struct rte_flow_error error;
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[4];  // ETH -> VLAN -> END
    struct rte_flow_action action[2]; // QUEUE -> END

    // Initialize pattern and action arrays
    memset(pattern, 0, sizeof(pattern));
    memset(action, 0, sizeof(action));
    memset(&attr, 0, sizeof(attr));

    // Flow attributes
    attr.ingress = 1;
    attr.priority = 0; // High priority for PTP

    // Action 0: Queue to PTP RX queue
    struct rte_flow_action_queue queue_action;
    queue_action.index = PTP_RX_QUEUE_ID;

    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[0].conf = &queue_action;

    // Action 1: End of actions
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    // ========================================================
    // Attempt 1: Match ETH (any) + VLAN (inner_type=0x88F7)
    // ========================================================
    printf("PTP Port %u: Trying VLAN-tagged pattern (inner_type=0x88F7)...\n", port_id);

    struct rte_flow_item_eth eth_spec;
    struct rte_flow_item_eth eth_mask;
    memset(&eth_spec, 0, sizeof(eth_spec));
    memset(&eth_mask, 0, sizeof(eth_mask));

    // Don't match on outer EtherType - let VLAN item handle that
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = NULL;  // Match any ETH
    pattern[0].mask = NULL;

    // Pattern 1: Match VLAN header with inner EtherType = 0x88F7 (PTP)
    struct rte_flow_item_vlan vlan_spec;
    struct rte_flow_item_vlan vlan_mask;
    memset(&vlan_spec, 0, sizeof(vlan_spec));
    memset(&vlan_mask, 0, sizeof(vlan_mask));

    vlan_spec.inner_type = rte_cpu_to_be_16(PTP_ETHERTYPE);
    vlan_mask.inner_type = 0xFFFF;
    vlan_mask.tci = 0;  // Accept all VLAN IDs

    pattern[1].type = RTE_FLOW_ITEM_TYPE_VLAN;
    pattern[1].spec = &vlan_spec;
    pattern[1].mask = &vlan_mask;

    pattern[2].type = RTE_FLOW_ITEM_TYPE_END;

    int ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
    if (ret == 0) {
        struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, action, &error);
        if (flow) {
            ptp_flow_handles[port_id] = flow;
            printf("PTP: ✓ Port %u: VLAN-tagged PTP → Queue %u\n",
                   port_id, PTP_RX_QUEUE_ID);
            return 0;
        }
        printf("PTP Port %u: VLAN flow create failed: %s\n",
               port_id, error.message ? error.message : "unknown");
    } else {
        printf("PTP Port %u: VLAN flow validate failed: %s\n",
               port_id, error.message ? error.message : "unknown");
    }

    // ========================================================
    // Attempt 2: Match ETH (type=0x8100) + VLAN (inner_type=0x88F7)
    // ========================================================
    printf("PTP Port %u: Trying explicit VLAN EtherType pattern...\n", port_id);

    memset(&eth_spec, 0, sizeof(eth_spec));
    memset(&eth_mask, 0, sizeof(eth_mask));
    eth_spec.type = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
    eth_mask.type = 0xFFFF;

    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = &eth_spec;
    pattern[0].mask = &eth_mask;

    ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
    if (ret == 0) {
        struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, action, &error);
        if (flow) {
            ptp_flow_handles[port_id] = flow;
            printf("PTP: ✓ Port %u: VLAN-tagged PTP (explicit) → Queue %u\n",
                   port_id, PTP_RX_QUEUE_ID);
            return 0;
        }
        printf("PTP Port %u: Explicit VLAN flow create failed: %s\n",
               port_id, error.message ? error.message : "unknown");
    } else {
        printf("PTP Port %u: Explicit VLAN flow validate failed: %s\n",
               port_id, error.message ? error.message : "unknown");
    }

    // ========================================================
    // Attempt 3: Untagged PTP (EtherType 0x88F7 directly)
    // ========================================================
    printf("PTP Port %u: Trying untagged PTP pattern...\n", port_id);

    memset(pattern, 0, sizeof(pattern));
    memset(&eth_spec, 0, sizeof(eth_spec));
    memset(&eth_mask, 0, sizeof(eth_mask));

    eth_spec.type = rte_cpu_to_be_16(PTP_ETHERTYPE);
    eth_mask.type = 0xFFFF;

    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = &eth_spec;
    pattern[0].mask = &eth_mask;

    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
    if (ret == 0) {
        struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, action, &error);
        if (flow) {
            ptp_flow_handles[port_id] = flow;
            printf("PTP: ⚠ Port %u: Untagged PTP → Queue %u (VLAN-tagged won't work!)\n",
                   port_id, PTP_RX_QUEUE_ID);
            return 0;
        }
        printf("PTP Port %u: Untagged flow create failed: %s\n",
               port_id, error.message ? error.message : "unknown");
    } else {
        printf("PTP Port %u: Untagged flow validate failed: %s\n",
               port_id, error.message ? error.message : "unknown");
    }

    // ========================================================
    // All attempts failed - no flow rule installed
    // ========================================================
    fprintf(stderr, "PTP: ✗ Port %u: ALL flow patterns failed! PTP packets won't reach Queue %u\n",
            port_id, PTP_RX_QUEUE_ID);
    return -1;
}

/**
 * Remove PTP flow rule from port
 */
void ptp_flow_rule_remove(uint16_t port_id)
{
    if (port_id >= PTP_MAX_PORTS)
        return;

    if (ptp_flow_handles[port_id]) {
        struct rte_flow_error error;
        int ret = rte_flow_destroy(port_id, ptp_flow_handles[port_id], &error);
        if (ret != 0) {
            fprintf(stderr, "PTP: Flow destroy failed on port %u: %s\n",
                    port_id, error.message ? error.message : "unknown");
        } else {
            printf("PTP: Removed flow rule from port %u\n", port_id);
        }
        ptp_flow_handles[port_id] = NULL;
    }
}

/**
 * Install flow rules on all PTP ports
 */
int ptp_flow_rules_install_all(void)
{
    int success = 0;
    int failed = 0;

    ptp_context_t *ctx = ptp_get_context();
    if (!ctx) {
        fprintf(stderr, "PTP: Context not available for flow rules\n");
        return -1;
    }

    printf("\n========== PTP Flow Rule Installation ==========\n");

    // Iterate through all possible ports and install flow rules on enabled ones
    for (int i = 0; i < PTP_MAX_PORTS; i++) {
        ptp_port_t *port = &ctx->ports[i];
        if (!port->enabled)
            continue;

        // Skip raw socket ports - they don't use rte_flow
        if (port->is_raw_socket) {
            printf("PTP: Skipping flow rule for raw socket port %u\n", port->port_id);
            continue;
        }

        if (ptp_flow_rule_install(port->port_id) == 0) {
            success++;
        } else {
            failed++;
        }
    }

    printf("=================================================\n");
    printf("PTP: Flow rules installed on %d ports, failed on %d ports\n",
           success, failed);
    printf("=================================================\n\n");

    return (failed > 0) ? -1 : 0;
}

/**
 * Remove flow rules from all PTP ports
 */
void ptp_flow_rules_remove_all(void)
{
    ptp_context_t *ctx = ptp_get_context();
    if (!ctx)
        return;

    // Iterate through all possible ports and remove flow rules from enabled ones
    for (int i = 0; i < PTP_MAX_PORTS; i++) {
        ptp_port_t *port = &ctx->ports[i];
        if (!port->enabled || port->is_raw_socket)
            continue;

        ptp_flow_rule_remove(port->port_id);
    }
}