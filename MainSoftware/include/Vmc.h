#ifndef VMC_H
#define VMC_H

#include <string>
#include <vector>
#include "DeviceManager.h"
#include "Utils.h"
#include "SerialPort.h"
#include "Server.h"
#include "SystemCommand.h"

class Vmc
{
private:
    /**
     * @brief Ensure LOGS directory structure exists
     * @return true on success
     */
    bool ensureLogDirectories();

public:
    Vmc();
    ~Vmc();

    bool configureSequence();

    /**
     * @brief Deploy, build and run DPDK VMC interactively
     *
     * This function:
     * 1. Deploys DPDK VMC source to remote server
     * 2. Builds DPDK VMC on remote server
     * 3. Runs DPDK VMC interactively (user can answer y/n prompts for latency tests)
     * 4. After latency tests complete, DPDK VMC automatically forks to background
     * 5. SSH connection closes, main software continues
     *
     * @param eal_args EAL arguments (e.g., "-l 0-255 -n 16")
     * @param make_args Optional make arguments (e.g., "NUM_TX_CORES=4")
     * @return true on success
     */
    bool runDpdkVmcInteractive(const std::string& eal_args = "-l 0-255 -n 16",
                                const std::string& make_args = "");
};

extern Vmc g_vmc;

#endif // VMC_H
