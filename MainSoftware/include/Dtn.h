#ifndef DTN_H
#define DTN_H

#include <string>
#include <vector>
#include "DeviceManager.h"
#include "Utils.h"
#include "SerialPort.h"
#include "Server.h"
#include "SystemCommand.h"

/**
 * @brief Log directory paths for different subsystems
 * @note Paths are resolved at runtime relative to the executable location
 */

namespace LogPaths {
    /// Returns the root directory (detected at runtime via executable path)
    std::string rootDir();
    inline std::string baseDir()  { return rootDir() + "/MainSoftware/LOGS"; }
    inline std::string CMC()      { return baseDir() + "/CMC"; }
    inline std::string VMC()      { return baseDir() + "/VMC"; }
    inline std::string MMC()      { return baseDir() + "/MMC"; }
    inline std::string DTN()      { return baseDir() + "/DTN"; }
    inline std::string HSN()      { return baseDir() + "/HSN"; }
}

class Dtn
{
private:
    /**
     * @brief Ensure LOGS directory structure exists
     * @return true on success
     */
    bool ensureLogDirectories();

    /**
     * @brief Ask user a yes/no question
     * @param question The question to ask
     * @return true if user answered yes, false otherwise
     */
    bool askQuestion(const std::string& question);

    /**
     * @brief Ask user for expected 28V power status
     * @return expected input_power_status value (0x00, 0x01, or 0x02)
     */
    uint8_t ask28VoltStatus();

public:
    Dtn();
    ~Dtn();

    /**
     * @brief Configure power supply sequence
     * @return true on success
     */
    bool configureSequence();

    bool latencyTestSequence();

    /**
     * @brief Run HW timestamp latency test on remote server
     *
     * Deploys latency_test to server, runs it, and fetches the log back.
     * Log is saved to LOGS/DTN/latency_test.log
     *
     * @param run_args Optional arguments for the test (e.g., "-n 10 -v")
     * @param timeout_seconds Timeout for test execution (default: 120 seconds)
     * @return true on success
     */
    bool runLatencyTest(const std::string& run_args = "",
                        int timeout_seconds = 120);

    /**
     * @brief Deploy, build and run DPDK interactively
     *
     * This function:
     * 1. Deploys DPDK source to remote server
     * 2. Builds DPDK on remote server
     * 3. Runs DPDK interactively (user can answer y/n prompts for latency tests)
     * 4. After latency tests complete, DPDK automatically forks to background
     * 5. SSH connection closes, main software continues
     *
     * @param eal_args EAL arguments (e.g., "-l 0-255 -n 16")
     * @param make_args Optional make arguments (e.g., "NUM_TX_CORES=4")
     * @return true on success
     */
    bool runDpdkInteractive(const std::string& eal_args = "-l 0-255 -n 16",
                            const std::string& make_args = "");
};

extern Dtn g_dtn;

#endif // DTN_H
