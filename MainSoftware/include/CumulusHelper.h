#ifndef CUMULUS_HELPER_H
#define CUMULUS_HELPER_H

#include <string>
#include <vector>

/**
 * @brief One row of "bridge vlan show" state.
 *
 * Used both as "expected" (what configureSequence* wants the switch to look
 * like) and "actual" (what we parse back from the switch) so the two can be
 * diffed cheaply before issuing any configuration commands.
 */
struct BridgeVlanEntry {
    std::string interface;
    int         vlan_id;
    bool        egress_untagged;   // "Egress Untagged" flag set
    bool        pvid;              // "PVID" flag set (ingress untagged)

    BridgeVlanEntry()
        : vlan_id(0), egress_untagged(false), pvid(false) {}
    BridgeVlanEntry(std::string iface, int vid, bool untagged, bool is_pvid = false)
        : interface(std::move(iface)), vlan_id(vid),
          egress_untagged(untagged), pvid(is_pvid) {}
};

/**
 * @brief Helper class for Cumulus switch configuration via SSH (NVUE commands)
 *
 * Usage:
 * @code
 *   if (g_cumulus.connect()) {
 *       // Add tagged VLAN to interface
 *       g_cumulus.addVLAN("swp25s3", 100);
 *       g_cumulus.apply();
 *
 *       // Set untagged VLAN (native)
 *       g_cumulus.setUntaggedVLAN("swp25s3", 4);
 *       g_cumulus.apply();
 *
 *       // Or use bridge command directly
 *       g_cumulus.bridgeVLANAdd("swp25s3", 100, true);
 *   }
 * @endcode
 */
class CumulusHelper {
public:
    CumulusHelper();
    ~CumulusHelper() = default;

    // ==================== Connection ====================

    /**
     * @brief Test SSH connection to Cumulus switch
     */
    bool connect();

    /**
     * @brief Run full VLAN configuration sequence
     * Configures VLANs on swp25s3, swp29s3, swp17
     */

    bool configureSwp1325();
    bool configureSwp1426();
    bool configureSwp1527();
    bool configureSwp1628();
    bool configureSwp1729();
    bool configureSwp1830();
    bool configureSwp1931();
    bool configureSwp2032();


    bool configureSequence();

    // VMC-specific VLAN configuration
    bool configureSwp1309();
    bool configureSwp1410();
    bool configureSwp1511();
    bool configureSwp1612();
    bool configureSequenceVmc();

    // ==================== NVUE VLAN Commands ====================

    /**
     * @brief Add tagged VLAN to interface (nv set interface X bridge domain br_default vlan Y)
     * @param interface Interface name (e.g., "swp25s3", "swp17")
     * @param vlan_id VLAN ID
     * @param bridge Bridge domain (default: "br_default")
     */
    bool addVLAN(const std::string& interface, int vlan_id, const std::string& bridge = "br_default");

    /**
     * @brief Remove VLAN from interface
     * @param interface Interface name
     * @param vlan_id VLAN ID
     * @param bridge Bridge domain
     */
    bool removeVLAN(const std::string& interface, int vlan_id, const std::string& bridge = "br_default");

    /**
     * @brief Set untagged (native) VLAN on interface (nv set interface X bridge domain br_default untagged Y)
     * @param interface Interface name
     * @param vlan_id VLAN ID for untagged traffic
     * @param bridge Bridge domain
     */
    bool setUntaggedVLAN(const std::string& interface, int vlan_id, const std::string& bridge = "br_default");

    // ==================== Bridge Commands (Direct) ====================

    /**
     * @brief Add VLAN using bridge command (sudo bridge vlan add dev X vid Y [untagged])
     * @param interface Interface name
     * @param vlan_id VLAN ID
     * @param untagged Set as untagged/PVID
     */
    bool egressUntagged(const std::string& interface, int vlan_id, bool untagged = false);

    /**
     * @brief Remove VLAN using bridge command
     * @param interface Interface name
     * @param vlan_id VLAN ID
     */
    bool bridgeVLANRemove(const std::string& interface, int vlan_id);

    // ==================== Configuration ====================

    /**
     * @brief Apply pending configuration (nv config apply)
     */
    bool apply();

    /**
     * @brief Save configuration (nv config save)
     */
    bool save();

    // ==================== Network Interfaces Deployment ====================

    /**
     * @brief Deploy interfaces file to Cumulus switch and reload network
     * @param local_interfaces_path Path to local interfaces file
     * @return true on success
     *
     * This function:
     * 1. Copies the interfaces file to /etc/network/interfaces (with sudo)
     * 2. Runs 'sudo ifreload -a' to apply the configuration
     */
    bool deployNetworkInterfaces(const std::string& local_interfaces_path);

    /**
     * @brief Show pending changes (nv config diff)
     */
    bool showPending();

    // ==================== Show Commands ====================

    /**
     * @brief Show interface configuration
     * @param interface Interface name (empty for all)
     */
    bool showInterface(const std::string& interface = "");

    /**
     * @brief Show VLAN configuration
     */
    bool showVLAN();

    /**
     * @brief Show bridge VLAN table
     */
    bool showBridgeVLAN();

    // ==================== Raw Commands ====================

    /**
     * @brief Execute NVUE command (nv ...)
     * @param nv_command Command without 'nv' prefix
     * @param output Command output (optional)
     */
    bool nv(const std::string& nv_command, std::string* output = nullptr);

    /**
     * @brief Execute raw command on Cumulus
     * @param command Command to execute
     * @param output Command output (optional)
     * @param use_sudo Run with sudo
     */
    bool execute(const std::string& command, std::string* output = nullptr, bool use_sudo = false);

    // ==================== Idempotent State Checks ====================

    /**
     * @brief Return the 32-entry expected bridge-vlan state for the DTN unit.
     * These are the (interface, vlan, egress-untagged) rows that
     * configureSequence() would normally set up.
     */
    static std::vector<BridgeVlanEntry> expectedVlansDtn();

    /**
     * @brief Return the 16-entry expected bridge-vlan state for the VMC unit.
     */
    static std::vector<BridgeVlanEntry> expectedVlansVmc();

    /**
     * @brief Compare the local interfaces file's MD5 against /etc/network/interfaces
     *        on the switch. Used to decide whether scp+ifreload can be skipped.
     * @return true if they match (no deploy needed).
     */
    bool isRemoteInterfacesFileUpToDate(const std::string& local_interfaces_path);

    /**
     * @brief Fetch the full 'bridge -j vlan show' state from the switch and
     *        parse it into a flat list. One SSH round-trip.
     * @return false on SSH error or parse failure.
     */
    bool fetchBridgeVlanState(std::vector<BridgeVlanEntry>& out);

    /**
     * @brief True iff every entry in 'expected' is present in 'actual'
     *        (with matching egress_untagged flag). Extra entries in 'actual'
     *        are ignored - we don't want to fight whatever else has been
     *        learned on the bridge.
     */
    static bool hasAllExpectedVlans(const std::vector<BridgeVlanEntry>& expected,
                                    const std::vector<BridgeVlanEntry>& actual);

    /**
     * @brief Send many shell commands in a single SSH session via heredoc.
     *        Uses 'set -e' so the first failure aborts the batch.
     * @return true if the overall session exited with status 0.
     */
    bool executeBatch(const std::vector<std::string>& commands, bool use_sudo = false);

private:
    std::string getLogPrefix() const;
};

// Global instance
extern CumulusHelper g_cumulus;

#endif // CUMULUS_HELPER_H