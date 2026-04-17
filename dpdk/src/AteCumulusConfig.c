/**
 * @file ate_cumulus_config.c
 * @brief ATE Test Mode - Cumulus Switch Configuration via SSH
 *
 * C equivalent of the C++ CumulusHelper + SSHDeployer mechanism.
 * Sends ATE config from DPDK server (10.1.33.2) directly to
 * Cumulus switch (10.1.33.3) via SSH.
 *
 * Mechanism:
 *   1. Send interfaces file via sshpass + scp
 *   2. Run sudo ifreload -a via sshpass + ssh
 *   3. Run sudo bridge vlan add commands via sshpass + ssh
 */

#include "AteCumulusConfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>

// ============================================
// INTERNAL CONSTANTS
// ============================================

// Maximum command buffer size
#define ATE_CMD_BUF_SIZE 1024

// Log prefix
#define ATE_LOG_PREFIX "[ATE-Cumulus]"

// ============================================
// LOW-LEVEL SSH/SCP HELPERS
// ============================================

bool ate_cumulus_ssh_execute(const char *command, bool use_sudo)
{
    char cmd[ATE_CMD_BUF_SIZE];

    if (use_sudo) {
        // echo 'password' | sudo -S command
        snprintf(cmd, sizeof(cmd),
                 "sshpass -p '%s' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "
                 "%s@%s \"echo '%s' | sudo -S %s\"",
                 ATE_CUMULUS_PASSWORD,
                 ATE_CUMULUS_USER, ATE_CUMULUS_HOST,
                 ATE_CUMULUS_PASSWORD, command);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "sshpass -p '%s' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "
                 "%s@%s \"%s\"",
                 ATE_CUMULUS_PASSWORD,
                 ATE_CUMULUS_USER, ATE_CUMULUS_HOST,
                 command);
    }

    printf("%s Executing: %s%s\n", ATE_LOG_PREFIX, command,
           use_sudo ? " (sudo)" : "");

    int ret = system(cmd);
    if (ret != 0) {
        printf("%s Command failed (exit code: %d)\n", ATE_LOG_PREFIX, ret);
        return false;
    }

    return true;
}

bool ate_cumulus_scp_copy(const char *local_path, const char *remote_path)
{
    char cmd[ATE_CMD_BUF_SIZE];

    printf("%s SCP: %s -> %s@%s:%s\n", ATE_LOG_PREFIX,
           local_path, ATE_CUMULUS_USER, ATE_CUMULUS_HOST, remote_path);

    snprintf(cmd, sizeof(cmd),
             "sshpass -p '%s' scp -o StrictHostKeyChecking=no -o ConnectTimeout=10 "
             "%s %s@%s:%s",
             ATE_CUMULUS_PASSWORD,
             local_path,
             ATE_CUMULUS_USER, ATE_CUMULUS_HOST, remote_path);

    int ret = system(cmd);
    if (ret != 0) {
        printf("%s SCP failed (exit code: %d)\n", ATE_LOG_PREFIX, ret);
        return false;
    }

    printf("%s SCP completed successfully\n", ATE_LOG_PREFIX);
    return true;
}

// ============================================
// CONNECTION TEST
// ============================================

bool ate_cumulus_test_connection(void)
{
    printf("%s Testing connection to %s...\n", ATE_LOG_PREFIX, ATE_CUMULUS_HOST);

    bool result = ate_cumulus_ssh_execute("echo 'Connection OK'", false);
    if (result) {
        printf("%s Connection successful!\n", ATE_LOG_PREFIX);
    } else {
        printf("%s Connection FAILED!\n", ATE_LOG_PREFIX);
    }

    return result;
}

// ============================================
// INTERFACES DEPLOYMENT
// ============================================

bool ate_cumulus_deploy_interfaces(void)
{
    printf("\n========================================\n");
    printf("%s Deploying ATE Network Interfaces\n", ATE_LOG_PREFIX);
    printf("========================================\n");

    // Check if local file exists
    struct stat st;
    if (stat(ATE_INTERFACES_LOCAL_PATH, &st) != 0) {
        printf("%s ERROR: ATE interfaces file not found: %s\n",
               ATE_LOG_PREFIX, ATE_INTERFACES_LOCAL_PATH);
        return false;
    }

    printf("%s Using interfaces file: %s\n", ATE_LOG_PREFIX, ATE_INTERFACES_LOCAL_PATH);

    // Step 1: Test connection
    printf("%s [Step 1/3] Testing connection...\n", ATE_LOG_PREFIX);
    if (!ate_cumulus_test_connection()) {
        printf("%s Deploy failed: Cannot connect to switch\n", ATE_LOG_PREFIX);
        return false;
    }

    // Step 2: Copy interfaces file via SCP to /tmp first, then sudo mv
    printf("%s [Step 2/3] Copying interfaces file...\n", ATE_LOG_PREFIX);

    // SCP to /tmp/interfaces (no sudo needed for /tmp)
    if (!ate_cumulus_scp_copy(ATE_INTERFACES_LOCAL_PATH, "/tmp/interfaces")) {
        printf("%s Deploy failed: Cannot copy interfaces file\n", ATE_LOG_PREFIX);
        return false;
    }

    // sudo mv /tmp/interfaces /etc/network/interfaces
    if (!ate_cumulus_ssh_execute("mv /tmp/interfaces /etc/network/interfaces", true)) {
        printf("%s Deploy failed: Cannot move interfaces file\n", ATE_LOG_PREFIX);
        return false;
    }

    // Step 3: Run ifreload -a to apply configuration
    printf("%s [Step 3/3] Reloading network interfaces (ifreload -a)...\n", ATE_LOG_PREFIX);
    if (!ate_cumulus_ssh_execute("ifreload -a", true)) {
        // ifreload may return non-zero but still apply changes
        printf("%s Warning: ifreload -a returned non-zero (changes may still be applied)\n",
               ATE_LOG_PREFIX);
    }

    printf("\n========================================\n");
    printf("%s ATE Network Interfaces Deployed Successfully!\n", ATE_LOG_PREFIX);
    printf("========================================\n\n");

    return true;
}

// ============================================
// BRIDGE VLAN HELPER
// ============================================

bool ate_cumulus_egress_untagged(const char *interface, int vlan_id)
{
    char cmd[ATE_CMD_BUF_SIZE];

    printf("%s Bridge: Adding VLAN %d to %s (untagged)\n",
           ATE_LOG_PREFIX, vlan_id, interface);

    // sudo bridge vlan add dev swpXXsY vid VV untagged
    snprintf(cmd, sizeof(cmd),
             "bridge vlan add dev %s vid %d untagged",
             interface, vlan_id);

    return ate_cumulus_ssh_execute(cmd, true);
}

// ============================================
// ATE VLAN CONFIGURATION SEQUENCE
// ============================================
// Same logic as CumulusHelper::configureSequence().
// Runs egressUntagged commands for each swp port group.
// NOTE: Content will be modified by user!

static bool ate_configure_swp1325(void)
{
    printf("%s Configuring swp13/swp25...\n", ATE_LOG_PREFIX);

    if (!ate_cumulus_egress_untagged("swp25s0", 97))  return false;
    if (!ate_cumulus_egress_untagged("swp25s1", 98))  return false;
    if (!ate_cumulus_egress_untagged("swp25s2", 99))  return false;
    if (!ate_cumulus_egress_untagged("swp25s3", 100)) return false;

    printf("%s swp13/swp25 VLAN configuration completed\n", ATE_LOG_PREFIX);
    return true;
}

static bool ate_configure_swp1426(void)
{
    printf("%s Configuring swp14/swp26...\n", ATE_LOG_PREFIX);

    if (!ate_cumulus_egress_untagged("swp26s0", 101)) return false;
    if (!ate_cumulus_egress_untagged("swp26s1", 102)) return false;
    if (!ate_cumulus_egress_untagged("swp26s2", 103)) return false;
    if (!ate_cumulus_egress_untagged("swp26s3", 104)) return false;

    printf("%s swp14/swp26 VLAN configuration completed\n", ATE_LOG_PREFIX);
    return true;
}

static bool ate_configure_swp1527(void)
{
    printf("%s Configuring swp15/swp27...\n", ATE_LOG_PREFIX);

    if (!ate_cumulus_egress_untagged("swp27s0", 105)) return false;
    if (!ate_cumulus_egress_untagged("swp27s1", 106)) return false;
    if (!ate_cumulus_egress_untagged("swp27s2", 107)) return false;
    if (!ate_cumulus_egress_untagged("swp27s3", 108)) return false;

    printf("%s swp15/swp27 VLAN configuration completed\n", ATE_LOG_PREFIX);
    return true;
}

static bool ate_configure_swp1628(void)
{
    printf("%s Configuring swp16/swp28...\n", ATE_LOG_PREFIX);

    if (!ate_cumulus_egress_untagged("swp28s0", 109)) return false;
    if (!ate_cumulus_egress_untagged("swp28s1", 110)) return false;
    if (!ate_cumulus_egress_untagged("swp28s2", 111)) return false;
    if (!ate_cumulus_egress_untagged("swp28s3", 112)) return false;

    printf("%s swp16/swp28 VLAN configuration completed\n", ATE_LOG_PREFIX);
    return true;
}

static bool ate_configure_swp1729(void)
{
    printf("%s Configuring swp17/swp29...\n", ATE_LOG_PREFIX);

    if (!ate_cumulus_egress_untagged("swp29s0", 113)) return false;
    if (!ate_cumulus_egress_untagged("swp29s1", 114)) return false;
    if (!ate_cumulus_egress_untagged("swp29s2", 115)) return false;
    if (!ate_cumulus_egress_untagged("swp29s3", 116)) return false;

    printf("%s swp17/swp29 VLAN configuration completed\n", ATE_LOG_PREFIX);
    return true;
}

static bool ate_configure_swp1830(void)
{
    printf("%s Configuring swp18/swp30...\n", ATE_LOG_PREFIX);

    if (!ate_cumulus_egress_untagged("swp30s0", 117)) return false;
    if (!ate_cumulus_egress_untagged("swp30s1", 118)) return false;
    if (!ate_cumulus_egress_untagged("swp30s2", 119)) return false;
    if (!ate_cumulus_egress_untagged("swp30s3", 120)) return false;

    printf("%s swp18/swp30 VLAN configuration completed\n", ATE_LOG_PREFIX);
    return true;
}

static bool ate_configure_swp1931(void)
{
    printf("%s Configuring swp19/swp31...\n", ATE_LOG_PREFIX);

    if (!ate_cumulus_egress_untagged("swp31s0", 121)) return false;
    if (!ate_cumulus_egress_untagged("swp31s1", 122)) return false;
    if (!ate_cumulus_egress_untagged("swp31s2", 123)) return false;
    if (!ate_cumulus_egress_untagged("swp31s3", 124)) return false;

    printf("%s swp19/swp31 VLAN configuration completed\n", ATE_LOG_PREFIX);
    return true;
}

static bool ate_configure_swp2032(void)
{
    printf("%s Configuring swp20/swp32...\n", ATE_LOG_PREFIX);

    if (!ate_cumulus_egress_untagged("swp32s0", 125)) return false;
    if (!ate_cumulus_egress_untagged("swp32s1", 126)) return false;
    if (!ate_cumulus_egress_untagged("swp32s2", 127)) return false;
    if (!ate_cumulus_egress_untagged("swp32s3", 128)) return false;

    printf("%s swp20/swp32 VLAN configuration completed\n", ATE_LOG_PREFIX);
    return true;
}

bool ate_cumulus_configure_sequence(void)
{
    printf("\n========================================\n");
    printf("%s Starting ATE VLAN Configuration Sequence\n", ATE_LOG_PREFIX);
    printf("========================================\n");

    // Test connection first
    if (!ate_cumulus_test_connection()) {
        printf("%s Configuration failed: Cannot connect to switch\n", ATE_LOG_PREFIX);
        return false;
    }

    // Configure all port groups
    // NOTE: These values are placeholders, user will modify for ATE
    if (!ate_configure_swp1325()) return false;
    if (!ate_configure_swp1426()) return false;
    if (!ate_configure_swp1527()) return false;
    if (!ate_configure_swp1628()) return false;
    if (!ate_configure_swp1729()) return false;
    if (!ate_configure_swp1830()) return false;
    if (!ate_configure_swp1931()) return false;
    if (!ate_configure_swp2032()) return false;

    printf("\n========================================\n");
    printf("%s ATE VLAN Configuration Completed Successfully!\n", ATE_LOG_PREFIX);
    printf("========================================\n\n");

    return true;
}

// ============================================
// MAIN ENTRY POINT
// ============================================

int ate_configure_cumulus(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║         ATE CUMULUS SWITCH CONFIGURATION                        ║\n");
    printf("║  1. Deploy ATE interfaces file                                  ║\n");
    printf("║  2. Configure bridge VLAN settings                              ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    // Step 1: Deploy ATE interfaces file
    if (!ate_cumulus_deploy_interfaces()) {
        printf("%s FAILED: Could not deploy ATE interfaces\n", ATE_LOG_PREFIX);
        return -1;
    }

    // Wait for network reload to settle
    printf("%s Waiting for network configuration to settle...\n", ATE_LOG_PREFIX);
    sleep(2);

    // Step 2: Configure bridge VLAN sequence
    if (!ate_cumulus_configure_sequence()) {
        printf("%s FAILED: Could not configure ATE VLANs\n", ATE_LOG_PREFIX);
        return -1;
    }

    printf("\n========================================\n");
    printf("%s ATE Cumulus Configuration Complete!\n", ATE_LOG_PREFIX);
    printf("========================================\n\n");

    return 0;
}