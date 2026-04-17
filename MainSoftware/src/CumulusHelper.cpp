#include "CumulusHelper.h"
#include "SSHDeployer.h"
#include "SystemCommand.h"
#include "ErrorPrinter.h"
#include "Utils.h"
#include <iostream>
#include <filesystem>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <regex>
#include <vector>

// Global instance
CumulusHelper g_cumulus;

// ====================================================================
// Static helpers - local to this translation unit.
// ====================================================================

namespace {

// Strip leading/trailing whitespace (incl. \r\n).
std::string rtrim(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    size_t start = 0;
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    return s.substr(start, end - start);
}

// Extract the first 32-hex-char token from a string. Used to pull the md5
// hash out of output that may be contaminated with SSH login banners,
// shell prompts, MOTDs etc. Returns empty if nothing hex-32-shaped found.
std::string extractMd5Hash(const std::string& s) {
    static const std::regex md5_re("\\b[0-9a-fA-F]{32}\\b");
    std::smatch m;
    if (std::regex_search(s, m, md5_re)) return m.str();
    return "";
}

// Compute MD5 of a local file by shelling out to md5sum - the binary is
// always present on Linux and we already rely on it on the remote side.
// Returns empty string on error.
std::string localFileMd5(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return "";
    }
    // Safely quote the path for the shell.
    std::ostringstream cmd;
    cmd << "md5sum \"" << path << "\" 2>/dev/null | awk '{print $1}'";
    auto result = g_systemCommand.execute(cmd.str());
    if (!result.success) {
        return "";
    }
    // Use the same hex-extraction as the remote side - popen shouldn't pick
    // up banners locally, but being consistent costs nothing and guards
    // against future surprises (custom PROMPT_COMMAND etc).
    std::string hash = extractMd5Hash(result.output);
    return hash.empty() ? rtrim(result.output) : hash;
}

// Parse the JSON produced by `bridge -j vlan show` into a flat entry list.
// The format is small and predictable enough that a hand-rolled scanner
// is cheaper than pulling in a JSON dependency. One entry per
// (interface, vlan) pair. Returns false only on obviously malformed input.
bool parseBridgeVlanJson(const std::string& json,
                         std::vector<BridgeVlanEntry>& out)
{
    size_t pos = 0;
    const std::string k_ifname = "\"ifname\":\"";
    const std::string k_vlans  = "\"vlans\":[";
    const std::string k_vlan   = "\"vlan\":";
    const std::string k_untag  = "\"Egress Untagged\"";
    const std::string k_pvid   = "\"PVID\"";

    while (pos < json.size()) {
        size_t name_key = json.find(k_ifname, pos);
        if (name_key == std::string::npos) break;

        size_t name_start = name_key + k_ifname.size();
        size_t name_end = json.find('"', name_start);
        if (name_end == std::string::npos) break;
        std::string iface = json.substr(name_start, name_end - name_start);

        size_t vlans_key = json.find(k_vlans, name_end);
        if (vlans_key == std::string::npos) { pos = name_end + 1; continue; }
        size_t vlans_start = vlans_key + k_vlans.size();

        // Find matching ']' for this vlans array, respecting nested brackets
        // (the "flags":[...] inside each vlan object also uses []).
        int depth = 1;
        size_t p = vlans_start;
        while (p < json.size() && depth > 0) {
            char c = json[p];
            if (c == '[') depth++;
            else if (c == ']') { depth--; if (depth == 0) break; }
            ++p;
        }
        if (depth != 0) break;
        size_t vlans_end = p;

        // Walk the objects inside vlans_block one by one.
        size_t vp = vlans_start;
        while (vp < vlans_end) {
            size_t obj_start = json.find('{', vp);
            if (obj_start == std::string::npos || obj_start >= vlans_end) break;

            int obj_depth = 1;
            size_t op = obj_start + 1;
            while (op < vlans_end && obj_depth > 0) {
                char c = json[op];
                if (c == '{') obj_depth++;
                else if (c == '}') { obj_depth--; if (obj_depth == 0) break; }
                ++op;
            }
            if (obj_depth != 0) break;
            size_t obj_end = op;

            // Extract "vlan":N from this object.
            size_t vlan_key = json.find(k_vlan, obj_start);
            if (vlan_key == std::string::npos || vlan_key > obj_end) {
                vp = obj_end + 1;
                continue;
            }
            size_t num_start = vlan_key + k_vlan.size();
            size_t num_end = num_start;
            while (num_end < obj_end &&
                   std::isdigit(static_cast<unsigned char>(json[num_end]))) {
                ++num_end;
            }
            if (num_end == num_start) { vp = obj_end + 1; continue; }
            int vid = std::stoi(json.substr(num_start, num_end - num_start));

            // Look for the flags inside this object only.
            std::string obj_slice = json.substr(obj_start, obj_end - obj_start + 1);
            bool untagged = obj_slice.find(k_untag) != std::string::npos;
            bool pvid     = obj_slice.find(k_pvid)  != std::string::npos;

            out.emplace_back(iface, vid, untagged, pvid);
            vp = obj_end + 1;
        }

        pos = vlans_end + 1;
    }

    return !out.empty();
}

// Build expected VLAN table for a unit, given a list of port groups.
// Each group is a base name ("swp25") paired with the VLAN id that its
// s0 sub-port should own; the following three sub-ports get id+1..id+3.
// All entries are egress-untagged, matching what bridgeVLANAdd(..,untagged)
// produces.
std::vector<BridgeVlanEntry> expandGroups(
    const std::vector<std::pair<std::string, int>>& groups)
{
    std::vector<BridgeVlanEntry> out;
    out.reserve(groups.size() * 4);
    for (const auto& g : groups) {
        for (int i = 0; i < 4; ++i) {
            out.emplace_back(
                g.first + "s" + std::to_string(i),
                g.second + i,
                /*egress_untagged=*/true,
                /*pvid=*/false);
        }
    }
    return out;
}

} // namespace

CumulusHelper::CumulusHelper()
{
}

std::string CumulusHelper::getLogPrefix() const
{
    return "[Cumulus]";
}

// ==================== Connection ====================

bool CumulusHelper::connect()
{
    return g_ssh_deployer_cumulus.testConnection();
}

bool CumulusHelper::configureSwp1325()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // for (int vlan_id = 97; vlan_id <= 100; vlan_id++)
    // {
    //     if (!addVLAN("swp13", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp13" << std::endl;
    //         return false;
    //     }
    // }

    // for (int vlan_id = 225; vlan_id <= 228; vlan_id++)
    // {
    //     if (!addVLAN("swp13", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp13" << std::endl;
    //         return false;
    //     }
    // }

    // if (!setUntaggedVLAN("swp13", 49))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 1 on swp13" << std::endl;
    //     return false;
    // }

    // if (!apply())
    //     return false;

    // if (!addVLAN("swp25s0", 97))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "97" << " to swp25s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp25s0", 97, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 97 untagged to swp25s0  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // // 4. nv set interface swp25s3 bridge domain br_default untagged 4
    // if (!setUntaggedVLAN("swp25s0", 225))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 225 on swp25s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    /********************************** */

    // if (!addVLAN("swp25s1", 98))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "98" << " to swp25s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp25s1", 98, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 98 untagged to swp25s1  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp25s1", 226))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 226 on swp25s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    /********************************** */

    // if (!addVLAN("swp25s2", 99))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "99" << " to swp25s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp25s2", 99, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 99 untagged to swp25s2  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp25s2", 227))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 227 on swp25s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    /********************************** */

    // if (!addVLAN("swp25s3", 100))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "100" << " to swp25s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp25s3", 100, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 100 untagged to swp25s3  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp25s3", 228))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 228 on swp25s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " VLAN Configuration Sw13 and Swp25 Completed Successfully!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool CumulusHelper::configureSwp1426()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // for (int vlan_id = 101; vlan_id <= 104; vlan_id++)
    // {
    //     if (!addVLAN("swp14", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp14" << std::endl;
    //         return false;
    //     }
    // }

    // for (int vlan_id = 229; vlan_id <= 232; vlan_id++)
    // {
    //     if (!addVLAN("swp14", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp14" << std::endl;
    //         return false;
    //     }
    // }

    // if (!setUntaggedVLAN("swp14", 50))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 2 on swp14" << std::endl;
    //     return false;
    // }

    // if (!apply())
    //     return false;

    // if (!addVLAN("swp26s0", 101))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "101" << " to swp26s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp26s0", 101, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 101 untagged to swp26s0  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // // 4. nv set interface swp25s3 bridge domain br_default untagged 4
    // if (!setUntaggedVLAN("swp26s0", 229))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 229 on swp26s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp26s1", 102))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "102" << " to swp26s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp26s1", 102, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 102 untagged to swp26s1  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp26s1", 230))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 230 on swp26s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    /********************************** */

    // if (!addVLAN("swp26s2", 103))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "103" << " to swp26s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp26s2", 103, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 103 untagged to swp26s2  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp26s2", 231))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 231 on swp26s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp26s3", 104))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "104" << " to swp26s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp26s3", 104, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 100 untagged to swp26s3  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp26s3", 232))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 232 on swp26s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " VLAN Configuration Sw14 and Swp26 Completed Successfully!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool CumulusHelper::configureSwp1527()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // for (int vlan_id = 105; vlan_id <= 108; vlan_id++)
    // {
    //     if (!addVLAN("swp15", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp15" << std::endl;
    //         return false;
    //     }
    // }

    // for (int vlan_id = 233; vlan_id <= 236; vlan_id++)
    // {
    //     if (!addVLAN("swp15", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp15" << std::endl;
    //         return false;
    //     }
    // }

    // if (!setUntaggedVLAN("swp15", 51))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 3 on swp15" << std::endl;
    //     return false;
    // }

    // if (!apply())
    //     return false;

    // if (!addVLAN("swp27s0", 105))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "105" << " to swp27s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp27s0", 105, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 105 untagged to swp27s0  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // // 4. nv set interface swp25s3 bridge domain br_default untagged 4
    // if (!setUntaggedVLAN("swp27s0", 233))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 233 on swp27s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp27s1", 106))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "106" << " to swp27s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp27s1", 106, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 106 untagged to swp27s1  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp27s1", 234))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 234 on swp27s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp27s2", 107))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "107" << " to swp27s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp27s2", 107, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 107 untagged to swp27s2  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp27s2", 235))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 235 on swp27s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp27s3", 108))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "108" << " to swp27s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp27s3", 108, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 108 untagged to swp27s3  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp27s3", 236))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 236 on swp27s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " VLAN Configuration Sw15 and Swp27 Completed Successfully!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool CumulusHelper::configureSwp1628()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // for (int vlan_id = 109; vlan_id <= 112; vlan_id++)
    // {
    //     if (!addVLAN("swp16", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp16" << std::endl;
    //         return false;
    //     }
    // }

    // for (int vlan_id = 237; vlan_id <= 240; vlan_id++)
    // {
    //     if (!addVLAN("swp16", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp16" << std::endl;
    //         return false;
    //     }
    // }

    // if (!setUntaggedVLAN("swp16", 52))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 4 on swp16" << std::endl;
    //     return false;
    // }

    // if (!apply())
    //     return false;

    // if (!addVLAN("swp28s0", 109))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "109" << " to swp28s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp28s0", 109, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 109 untagged to swp28s0  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // // 4. nv set interface swp25s3 bridge domain br_default untagged 4
    // if (!setUntaggedVLAN("swp28s0", 237))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 237 on swp28s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp28s1", 110))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "110" << " to swp28s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp28s1", 110, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 110 untagged to swp28s1  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp28s1", 238))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 238 on swp28s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp28s2", 111))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "111" << " to swp28s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp28s2", 111, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 111 untagged to swp28s2  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp28s2", 239))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 239 on swp28s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp28s3", 112))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "112" << " to swp28s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp28s3", 112, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 112 untagged to swp28s3  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp28s3", 240))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 240 on swp28s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " VLAN Configuration Sw16 and Swp28 Completed Successfully!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool CumulusHelper::configureSwp1729()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // for (int vlan_id = 113; vlan_id <= 116; vlan_id++)
    // {
    //     if (!addVLAN("swp17", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp17" << std::endl;
    //         return false;
    //     }
    // }

    // for (int vlan_id = 241; vlan_id <= 244; vlan_id++)
    // {
    //     if (!addVLAN("swp17", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp17" << std::endl;
    //         return false;
    //     }
    // }

    // if (!setUntaggedVLAN("swp17", 53))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 5 on swp17" << std::endl;
    //     return false;
    // }

    // if (!apply())
    //     return false;

    // if (!addVLAN("swp29s0", 113))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "113" << " to swp29s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp29s0", 113, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 113 untagged to swp29s0  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // // 4. nv set interface swp25s3 bridge domain br_default untagged 4
    // if (!setUntaggedVLAN("swp29s0", 241))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 241 on swp29s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp29s1", 114))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "114" << " to swp29s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp29s1", 114, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 114 untagged to swp29s1  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp29s1", 242))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 242 on swp28s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp29s2", 115))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "115" << " to swp29s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp29s2", 115, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 115 untagged to swp29s2  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp29s2", 243))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 234 on swp29s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp29s3", 116))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "116" << " to swp29s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp29s3", 116, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 116 untagged to swp29s3  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp29s3", 244))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 244 on swp29s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " VLAN Configuration Sw17 and Swp29 Completed Successfully!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool CumulusHelper::configureSwp1830()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // for (int vlan_id = 117; vlan_id <= 120; vlan_id++)
    // {
    //     if (!addVLAN("swp18", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp18" << std::endl;
    //         return false;
    //     }
    // }

    // for (int vlan_id = 245; vlan_id <= 248; vlan_id++)
    // {
    //     if (!addVLAN("swp18", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp18" << std::endl;
    //         return false;
    //     }
    // }

    // if (!setUntaggedVLAN("swp18", 54))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 6 on swp18" << std::endl;
    //     return false;
    // }

    // if (!apply())
    //     return false;

    // if (!addVLAN("swp30s0", 117))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "117" << " to swp30s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp30s0", 117, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 117 untagged to swp30s0  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // // 4. nv set interface swp25s3 bridge domain br_default untagged 4
    // if (!setUntaggedVLAN("swp30s0", 245))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 245 on swp30s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp30s1", 118))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "118" << " to swp30s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp30s1", 118, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 118 untagged to swp30s1  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp30s1", 246))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 246 on swp30s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp30s2", 119))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "119" << " to swp30s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp30s2", 119, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 119 untagged to swp30s2  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp30s2", 247))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 247 on swp30s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp30s3", 120))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "120" << " to swp30s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp30s3", 120, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 120 untagged to swp30s3  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp30s3", 248))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 248 on swp30s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " VLAN Configuration Sw18 and Swp30 Completed Successfully!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool CumulusHelper::configureSwp1931()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // for (int vlan_id = 121; vlan_id <= 124; vlan_id++)
    // {
    //     if (!addVLAN("swp19", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp19" << std::endl;
    //         return false;
    //     }
    // }

    // for (int vlan_id = 249; vlan_id <= 252; vlan_id++)
    // {
    //     if (!addVLAN("swp19", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp19" << std::endl;
    //         return false;
    //     }
    // }

    // if (!setUntaggedVLAN("swp19", 55))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 7 on swp19" << std::endl;
    //     return false;
    // }

    // if (!apply())
    //     return false;

    // if (!addVLAN("swp31s0", 121))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "121" << " to swp31s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp31s0", 121, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 121 untagged to swp31s0  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // // 4. nv set interface swp25s3 bridge domain br_default untagged 4
    // if (!setUntaggedVLAN("swp31s0", 249))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 249 on swp31s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp31s1", 122))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "122" << " to swp31s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp31s1", 122, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 122 untagged to swp31s1  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp31s1", 250))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 250 on swp31s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp31s2", 123))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "123" << " to swp31s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp31s2", 123, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 123 untagged to swp31s2  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp31s2", 251))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 251 on swp31s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp31s3", 124))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "124" << " to swp31s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp31s3", 124, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 124 untagged to swp31s3  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp31s3", 252))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 252 on swp31s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " VLAN Configuration Swp19 and Swp31 Completed Successfully!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool CumulusHelper::configureSwp2032()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // for (int vlan_id = 125; vlan_id <= 128; vlan_id++)
    // {
    //     if (!addVLAN("swp20", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp20" << std::endl;
    //         return false;
    //     }
    // }

    // for (int vlan_id = 253; vlan_id <= 256; vlan_id++)
    // {
    //     if (!addVLAN("swp20", vlan_id))
    //     {
    //         std::cerr << getLogPrefix() << " Failed to add VLAN " << vlan_id << " to swp20" << std::endl;
    //         return false;
    //     }
    // }

    // if (!setUntaggedVLAN("swp20", 56))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 8 on swp20" << std::endl;
    //     return false;
    // }

    // if (!apply())
    //     return false;

    // if (!addVLAN("swp32s0", 125))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "125" << " to swp32s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp32s0", 125, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 125 untagged to swp32s0  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // // 4. nv set interface swp25s3 bridge domain br_default untagged 4
    // if (!setUntaggedVLAN("swp32s0", 253))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 253 on swp32s0" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp32s1", 126))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "126" << " to swp32s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp32s1", 126, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 126 untagged to swp32s1  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp32s1", 254))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 254 on swp32s1" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp32s2", 127))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "127" << " to swp32s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp32s2", 127, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 127 untagged to swp32s2  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp32s2", 255))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 255 on swp32s2" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // /********************************** */

    // if (!addVLAN("swp32s3", 128))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN " << "128" << " to swp32s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    if (!egressUntagged("swp32s3", 128, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 128 untagged to swp32s3  " << std::endl;
        return false;
    }
    // if (!apply())
    //     return false;

    // if (!setUntaggedVLAN("swp32s3", 256))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 256 on swp32s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " VLAN Configuration Swp20 and Swp32 Completed Successfully!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool CumulusHelper::configureSequence()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " Starting VLAN Configuration Sequence (DTN)" << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // 2. Idempotent skip: if the switch already has every bridge-vlan entry
    //    we'd be setting up, do nothing. Saves ~40 SSH round-trips on every
    //    repeat run where the operator hasn't rebooted the switch or
    //    redeployed interfaces.
    const auto expected = expectedVlansDtn();
    std::vector<BridgeVlanEntry> actual;
    if (fetchBridgeVlanState(actual) &&
        hasAllExpectedVlans(expected, actual))
    {
        ErrorPrinter::info("CUMULUS",
            "DTN VLAN state already matches expected (" +
            std::to_string(expected.size()) + " entries), skipping configuration");
        return true;
    }

    // 3. Need to (re)apply. Build all 32 'bridge vlan add ... untagged'
    //    commands and send them in a single SSH session instead of 40+
    //    separate logins.
    std::vector<std::string> batch;
    batch.reserve(expected.size());
    for (const auto& e : expected) {
        std::string cmd = "bridge vlan add dev " + e.interface +
                          " vid " + std::to_string(e.vlan_id);
        if (e.egress_untagged) cmd += " untagged";
        batch.push_back(cmd);
    }

    if (!executeBatch(batch, /*use_sudo=*/true)) {
        std::cerr << getLogPrefix() << " DTN batch VLAN apply failed" << std::endl;
        return false;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " DTN VLAN batch applied ("
              << expected.size() << " entries)" << std::endl;
    std::cout << "========================================\n" << std::endl;
    // Legacy per-port helpers remain available for standalone use:
    // configureSwp1325() ... configureSwp2032()

    // // 2. nv set interface swp25s3 bridge domain br_default vlan 100
    // if (!addVLAN("swp25s3", 100))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN 100 to swp25s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // // 3. sudo bridge vlan add dev swp25s3 vid 100 untagged
    // if (!bridgeVLANAdd("swp25s3", 100, true))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add bridge VLAN 100 untagged to swp25s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // // 4. nv set interface swp25s3 bridge domain br_default untagged 4
    // if (!setUntaggedVLAN("swp25s3", 4))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 4 on swp25s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // // 5. nv set interface swp29s3 bridge domain br_default untagged 244
    // if (!setUntaggedVLAN("swp29s3", 244))
    // {
    //     std::cerr << getLogPrefix() << " Failed to set untagged VLAN 244 on swp29s3" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // // 6. nv set interface swp17 bridge domain br_default vlan 244
    // if (!addVLAN("swp17", 244))
    // {
    //     std::cerr << getLogPrefix() << " Failed to add VLAN 244 to swp17" << std::endl;
    //     return false;
    // }
    // if (!apply())
    //     return false;

    // std::cout << "\n========================================" << std::endl;
    // std::cout << getLogPrefix() << " VLAN Configuration Completed Successfully!" << std::endl;
    // std::cout << "========================================\n"
    //           << std::endl;

    return true;
}

// ==================== VMC VLAN Configuration ====================

bool CumulusHelper::configureSwp1309()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }


    if (!egressUntagged("swp9s0", 97, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 97 untagged to swp25s0  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp9s1", 98, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 98 untagged to swp25s1  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp9s2", 99, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 99 untagged to swp25s2  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp9s3", 100, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 100 untagged to swp25s3  " << std::endl;
        return false;
    }


    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " VLAN Configuration Sw13 and Swp25 Completed Successfully!" << std::endl;
    std::cout << "========================================\n"
              << std::endl;

    return true;
}

bool CumulusHelper::configureSwp1410()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    if (!egressUntagged("swp10s0", 101, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 101 untagged to swp26s0  " << std::endl;
        return false;
    }

    if (!egressUntagged("swp10s1", 102, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 102 untagged to swp26s1  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp10s2", 103, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 103 untagged to swp26s2  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp10s3", 104, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 100 untagged to swp26s3  " << std::endl;
        return false;
    }


    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " VLAN Configuration Sw14 and Swp26 Completed Successfully!" << std::endl;
    std::cout << "========================================\n"
              << std::endl;

    return true;
}

bool CumulusHelper::configureSwp1511()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }


    if (!egressUntagged("swp11s0", 105, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 105 untagged to swp27s0  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp11s1", 106, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 106 untagged to swp27s1  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp11s2", 107, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 107 untagged to swp27s2  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp11s3", 108, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 108 untagged to swp27s3  " << std::endl;
        return false;
    }


    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " VLAN Configuration Sw15 and Swp27 Completed Successfully!" << std::endl;
    std::cout << "========================================\n"
              << std::endl;

    return true;
}

bool CumulusHelper::configureSwp1612()
{

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }


    if (!egressUntagged("swp12s0", 109, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 105 untagged to swp27s0  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp12s1", 110, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 106 untagged to swp27s1  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp12s2", 111, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 107 untagged to swp27s2  " << std::endl;
        return false;
    }


    if (!egressUntagged("swp12s3", 112, true))
    {
        std::cerr << getLogPrefix() << " Failed to add bridge VLAN 108 untagged to swp27s3  " << std::endl;
        return false;
    }


    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " VLAN Configuration Sw15 and Swp27 Completed Successfully!" << std::endl;
    std::cout << "========================================\n"
              << std::endl;

    return true;
}

bool CumulusHelper::configureSequenceVmc()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " Starting VLAN Configuration Sequence (VMC)" << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. Test connection
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Configuration failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // 2. Idempotent skip - same pattern as configureSequence() but for the
    //    VMC port range (swp9-swp12, 16 entries).
    const auto expected = expectedVlansVmc();
    std::vector<BridgeVlanEntry> actual;
    if (fetchBridgeVlanState(actual) &&
        hasAllExpectedVlans(expected, actual))
    {
        ErrorPrinter::info("CUMULUS",
            "VMC VLAN state already matches expected (" +
            std::to_string(expected.size()) + " entries), skipping configuration");
        return true;
    }

    // 3. Apply all 16 entries in a single SSH batch.
    std::vector<std::string> batch;
    batch.reserve(expected.size());
    for (const auto& e : expected) {
        std::string cmd = "bridge vlan add dev " + e.interface +
                          " vid " + std::to_string(e.vlan_id);
        if (e.egress_untagged) cmd += " untagged";
        batch.push_back(cmd);
    }

    if (!executeBatch(batch, /*use_sudo=*/true)) {
        std::cerr << getLogPrefix() << " VMC batch VLAN apply failed" << std::endl;
        return false;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " VMC VLAN batch applied ("
              << expected.size() << " entries)" << std::endl;
    std::cout << "========================================\n" << std::endl;
    // Legacy per-port helpers remain available:
    // configureSwp1309() ... configureSwp1612()
    return true;
}

// ==================== NVUE VLAN Commands ====================

bool CumulusHelper::addVLAN(const std::string &interface, int vlan_id, const std::string &bridge)
{
    DEBUG_LOG(getLogPrefix() << " Adding VLAN " << vlan_id << " to " << interface);
    // nv set interface swp25s3 bridge domain br_default vlan 100
    std::string cmd = "nv set interface " + interface + " bridge domain " + bridge + " vlan " + std::to_string(vlan_id);
    return g_ssh_deployer_cumulus.execute(cmd);
}

bool CumulusHelper::removeVLAN(const std::string &interface, int vlan_id, const std::string &bridge)
{
    DEBUG_LOG(getLogPrefix() << " Removing VLAN " << vlan_id << " from " << interface);
    std::string cmd = "nv unset interface " + interface + " bridge domain " + bridge + " vlan " + std::to_string(vlan_id);
    return g_ssh_deployer_cumulus.execute(cmd);
}

bool CumulusHelper::setUntaggedVLAN(const std::string &interface, int vlan_id, const std::string &bridge)
{
    DEBUG_LOG(getLogPrefix() << " Setting untagged VLAN " << vlan_id << " on " << interface);
    // nv set interface swp25s3 bridge domain br_default untagged 4
    std::string cmd = "nv set interface " + interface + " bridge domain " + bridge + " untagged " + std::to_string(vlan_id);
    return g_ssh_deployer_cumulus.execute(cmd);
}

// ==================== Bridge Commands (Direct) ====================

bool CumulusHelper::egressUntagged(const std::string &interface, int vlan_id, bool untagged)
{
    DEBUG_LOG(getLogPrefix() << " Bridge: Adding VLAN " << vlan_id << " to " << interface << (untagged ? " (untagged)" : ""));

    // sudo bridge vlan add dev swp25s3 vid 100 untagged
    std::string cmd = "bridge vlan add dev " + interface + " vid " + std::to_string(vlan_id);
    if (untagged)
    {
        cmd += " untagged";
    }
    return g_ssh_deployer_cumulus.execute(cmd, nullptr, true);
}

bool CumulusHelper::bridgeVLANRemove(const std::string &interface, int vlan_id)
{
    DEBUG_LOG(getLogPrefix() << " Bridge: Removing VLAN " << vlan_id << " from " << interface);
    std::string cmd = "bridge vlan del dev " + interface + " vid " + std::to_string(vlan_id);
    return g_ssh_deployer_cumulus.execute(cmd, nullptr, true);
}

// ==================== Configuration ====================

bool CumulusHelper::apply()
{
    DEBUG_LOG(getLogPrefix() << " Applying configuration...");
    // Use sudo for password and 'yes' pipe for y/n confirmation
    return g_ssh_deployer_cumulus.execute("yes | nv config apply", nullptr, true);
}

bool CumulusHelper::save()
{
    DEBUG_LOG(getLogPrefix() << " Saving configuration...");
    return g_ssh_deployer_cumulus.execute("nv config save");
}

// ==================== Network Interfaces Deployment ====================

bool CumulusHelper::deployNetworkInterfaces(const std::string& local_interfaces_path)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " Deploying Network Interfaces" << std::endl;
    std::cout << "========================================" << std::endl;

    // Resolve file path - search in multiple locations for relative paths
    std::string resolved_path;
    std::filesystem::path input_path(local_interfaces_path);

    if (input_path.is_absolute())
    {
        resolved_path = local_interfaces_path;
    }
    else
    {
        // Search locations: current dir, parent dir, grandparent dir (project root from build/bin/)
        std::vector<std::filesystem::path> search_paths = {
            std::filesystem::current_path() / input_path,
            std::filesystem::current_path().parent_path() / input_path,
            std::filesystem::current_path().parent_path().parent_path() / input_path
        };

        for (const auto& path : search_paths)
        {
            if (std::filesystem::exists(path))
            {
                resolved_path = path.string();
                break;
            }
        }

        if (resolved_path.empty())
        {
            std::cerr << getLogPrefix() << " Deploy failed: Cannot find file '" << local_interfaces_path << "'" << std::endl;
            std::cerr << getLogPrefix() << " Searched in:" << std::endl;
            for (const auto& path : search_paths)
            {
                std::cerr << "  - " << path.string() << std::endl;
            }
            return false;
        }
    }

    DEBUG_LOG(getLogPrefix() << " Using interfaces file: " << resolved_path);

    // 1. Test connection
    DEBUG_LOG(getLogPrefix() << " [Step 1/4] Testing connection...");
    if (!connect())
    {
        std::cerr << getLogPrefix() << " Deploy failed: Cannot connect to switch" << std::endl;
        return false;
    }

    // 2. Idempotent skip: if /etc/network/interfaces on the switch already
    //    matches the local file byte-for-byte (md5), we don't need to
    //    scp+ifreload (the expensive step - ifreload alone is 5-10s and
    //    wipes bridge-vlan state, which would force the caller's
    //    configureSequence() to re-apply afterwards).
    DEBUG_LOG(getLogPrefix() << " [Step 2/4] Checking remote interfaces file...");
    if (isRemoteInterfacesFileUpToDate(resolved_path)) {
        ErrorPrinter::info("CUMULUS",
            "interfaces file unchanged on switch, skipping scp+ifreload");
        std::cout << "========================================\n" << std::endl;
        return true;
    }

    // 3. Copy interfaces file to /etc/network/interfaces (with sudo)
    DEBUG_LOG(getLogPrefix() << " [Step 3/4] Copying interfaces file...");
    if (!g_ssh_deployer_cumulus.copyFileToPath(resolved_path, "/etc/network/interfaces", true))
    {
        std::cerr << getLogPrefix() << " Deploy failed: Cannot copy interfaces file" << std::endl;
        return false;
    }

    // 4. Run ifreload -a to apply configuration
    DEBUG_LOG(getLogPrefix() << " [Step 4/4] Reloading network interfaces (ifreload -a)...");
    if (!g_ssh_deployer_cumulus.execute("ifreload -a", nullptr, true))
    {
        // ifreload may return non-zero exit code but still apply changes
        std::cerr << getLogPrefix() << " Warning: ifreload -a returned non-zero exit code (changes may still be applied)" << std::endl;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << getLogPrefix() << " Network Interfaces Deployed Successfully!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return true;
}

bool CumulusHelper::showPending()
{
    DEBUG_LOG(getLogPrefix() << " Showing pending changes...");
    return g_ssh_deployer_cumulus.execute("nv config diff");
}

// ==================== Show Commands ====================

bool CumulusHelper::showInterface(const std::string &interface)
{
    if (interface.empty())
    {
        DEBUG_LOG(getLogPrefix() << " Showing all interfaces...");
        return g_ssh_deployer_cumulus.execute("nv show interface");
    }
    else
    {
        DEBUG_LOG(getLogPrefix() << " Showing interface " << interface << "...");
        return g_ssh_deployer_cumulus.execute("nv show interface " + interface);
    }
}

bool CumulusHelper::showVLAN()
{
    DEBUG_LOG(getLogPrefix() << " Showing VLAN configuration...");
    return g_ssh_deployer_cumulus.execute("nv show bridge domain br_default vlan");
}

bool CumulusHelper::showBridgeVLAN()
{
    DEBUG_LOG(getLogPrefix() << " Showing bridge VLAN table...");
    return g_ssh_deployer_cumulus.execute("bridge vlan show");
}

// ==================== Raw Commands ====================

bool CumulusHelper::nv(const std::string &nv_command, std::string *output)
{
    DEBUG_LOG(getLogPrefix() << " nv " << nv_command);
    return g_ssh_deployer_cumulus.execute("nv " + nv_command, output);
}

bool CumulusHelper::execute(const std::string &command, std::string *output, bool use_sudo)
{
    return g_ssh_deployer_cumulus.execute(command, output, use_sudo);
}

// ========================================================================
// Idempotent state checks
// ========================================================================

std::vector<BridgeVlanEntry> CumulusHelper::expectedVlansDtn()
{
    // Matches the old configureSwp1325..2032 sequence:
    //   swp25s[0-3] -> vid  97..100 untagged
    //   swp26s[0-3] -> vid 101..104 untagged
    //   swp27s[0-3] -> vid 105..108 untagged
    //   swp28s[0-3] -> vid 109..112 untagged
    //   swp29s[0-3] -> vid 113..116 untagged
    //   swp30s[0-3] -> vid 117..120 untagged
    //   swp31s[0-3] -> vid 121..124 untagged
    //   swp32s[0-3] -> vid 125..128 untagged
    static const std::vector<std::pair<std::string, int>> groups = {
        {"swp25",  97}, {"swp26", 101}, {"swp27", 105}, {"swp28", 109},
        {"swp29", 113}, {"swp30", 117}, {"swp31", 121}, {"swp32", 125},
    };
    return expandGroups(groups);
}

std::vector<BridgeVlanEntry> CumulusHelper::expectedVlansVmc()
{
    // Matches configureSwp1309..1612:
    //   swp9s[0-3]  -> vid  97..100 untagged
    //   swp10s[0-3] -> vid 101..104 untagged
    //   swp11s[0-3] -> vid 105..108 untagged
    //   swp12s[0-3] -> vid 109..112 untagged
    static const std::vector<std::pair<std::string, int>> groups = {
        {"swp9",   97}, {"swp10", 101}, {"swp11", 105}, {"swp12", 109},
    };
    return expandGroups(groups);
}

bool CumulusHelper::isRemoteInterfacesFileUpToDate(const std::string& local_interfaces_path)
{
    std::string local_md5 = localFileMd5(local_interfaces_path);
    if (local_md5.empty()) {
        ErrorPrinter::warn("CUMULUS",
            "md5 check: cannot hash local file '" + local_interfaces_path + "'");
        return false;
    }

    std::string remote_output;
    if (!g_ssh_deployer_cumulus.execute(
            "md5sum /etc/network/interfaces | awk '{print $1}'",
            &remote_output, /*use_sudo=*/false, /*silent=*/true))
    {
        ErrorPrinter::warn("CUMULUS",
            "md5 check: SSH failed to read remote md5");
        return false;
    }
    // Cumulus SSH prints a login banner ("Debian GNU/Linux 10") before the
    // command output, so rtrim() alone leaves us with banner + hash + path
    // and breaks string comparison. Pull just the 32-hex token out.
    std::string remote_md5 = extractMd5Hash(remote_output);
    if (remote_md5.empty()) {
        ErrorPrinter::warn("CUMULUS",
            "md5 check: could not extract hash from remote output: '" +
            rtrim(remote_output) + "'");
        return false;
    }

    bool match = (local_md5 == remote_md5);
    // Always visible so the operator can see exactly what each side hashes to
    // when the skip path doesn't engage. Includes the resolved local path
    // because a lot of grief has come from multiple copies of the file at
    // different roots (dev vs portable layout).
    ErrorPrinter::info("CUMULUS",
        std::string("md5 check: ") +
        (match ? "MATCH (skip deploy)" : "MISMATCH (will deploy)") +
        " local=" + local_md5 +
        " remote=" + remote_md5 +
        " path=" + local_interfaces_path);
    return match;
}

bool CumulusHelper::fetchBridgeVlanState(std::vector<BridgeVlanEntry>& out)
{
    out.clear();
    std::string output;
    // sshpass/non-login shells on Cumulus don't include /sbin in PATH, so
    // 'bridge' isn't found by name. Inline PATH prefix fixes this for
    // just this one command without touching the user's profile.
    if (!g_ssh_deployer_cumulus.execute(
            "PATH=/sbin:/usr/sbin:$PATH bridge -j vlan show",
            &output, /*use_sudo=*/false, /*silent=*/true))
    {
        DEBUG_LOG(getLogPrefix() << " bridge -j vlan show: SSH failed");
        return false;
    }
    if (output.empty()) {
        DEBUG_LOG(getLogPrefix() << " bridge -j vlan show: empty output");
        return false;
    }
    if (!parseBridgeVlanJson(output, out)) {
        DEBUG_LOG(getLogPrefix() << " bridge -j vlan show: parse produced 0 entries");
        return false;
    }
    DEBUG_LOG(getLogPrefix() << " bridge -j vlan show: parsed "
                             << out.size() << " entries");
    return true;
}

bool CumulusHelper::hasAllExpectedVlans(
    const std::vector<BridgeVlanEntry>& expected,
    const std::vector<BridgeVlanEntry>& actual)
{
    // Simple O(N*M) linear scan - both lists are small (<=50 entries).
    for (const auto& exp : expected) {
        bool found = false;
        for (const auto& act : actual) {
            if (act.interface == exp.interface &&
                act.vlan_id == exp.vlan_id &&
                act.egress_untagged == exp.egress_untagged)
            {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

bool CumulusHelper::executeBatch(const std::vector<std::string>& commands,
                                 bool use_sudo)
{
    if (commands.empty()) return true;

    // Wrap the whole batch in `sh -c '...'` for two reasons:
    //  1) If use_sudo=true, SSHDeployer will turn this into
    //     `echo <pw> | sudo -S sh -c '...'`. Sudo sees exactly ONE exec
    //     target (sh); without this wrapping sudo tries to run `set -e`
    //     as a binary and fails with "sudo: set: command not found"
    //     (set is a shell builtin, not a program).
    //  2) `set -e` then runs *inside* that shell, properly aborting the
    //     batch on the first failed command.
    //
    // Also prepend PATH=/sbin:/usr/sbin so tools that live outside the
    // interactive PATH (bridge, ip on Cumulus) resolve. The inline
    // `;` separator keeps the whole script on a single line, which is
    // what ssh's double-quoted argument handles cleanly.
    //
    // Safety: we build single-quoted `'...'` around the body, so nothing
    // inside the body can be re-interpreted by the outer shell. Callers
    // must avoid raw single-quote characters in their commands; our
    // generated `bridge vlan add ...` strings never contain them.
    std::ostringstream body;
    body << "export PATH=/sbin:/usr/sbin:/bin:/usr/bin:$PATH";
    body << "; set -e";
    for (const auto& cmd : commands) {
        body << "; " << cmd;
    }
    std::string wrapped = "sh -c '" + body.str() + "'";

    return g_ssh_deployer_cumulus.execute(wrapped, nullptr,
                                          use_sudo, /*silent=*/true);
}