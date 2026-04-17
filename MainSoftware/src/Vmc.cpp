#include "Vmc.h"
#include "ReportManager.h"
#include "SafeShutdown.h"
#include "ErrorPrinter.h"
#include "CumulusHelper.h"
#include "SSHDeployer.h"
#include "Dtn.h"
#include "Utils.h"
#include <iostream>
#include <unistd.h>
#include <iomanip>
#include <filesystem>
#include <csignal>
#include <atomic>
// 28V 3.0A

// Global flag for Ctrl+C handling in DPDK VMC monitoring
static std::atomic<bool> g_dpdk_vmc_monitoring_running{true};

static void dpdk_vmc_monitor_signal_handler(int sig)
{
    (void)sig;
    g_dpdk_vmc_monitoring_running = false;
}

Vmc g_vmc;

Vmc::Vmc()
{
}

Vmc::~Vmc()
{
}

bool Vmc::ensureLogDirectories()
{
    try
    {
        std::filesystem::create_directories(LogPaths::VMC());
        DEBUG_LOG("VMC: Log directories created/verified at " << LogPaths::VMC());
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "VMC: Failed to create log directories: " << e.what() << std::endl;
        return false;
    }
}

bool Vmc::runDpdkVmcInteractive(const std::string &eal_args, const std::string &make_args)
{
    std::cout << "======================================" << std::endl;
    std::cout << "VMC: DPDK VMC Interactive Deployment" << std::endl;
    std::cout << "======================================" << std::endl;

    // Step 1: Test connection
    if (!g_ssh_deployer_server.testConnection())
    {
        std::cerr << "VMC: Cannot connect to server!" << std::endl;
        return false;
    }

    // Step 2: Deploy prebuilt DPDK VMC binary (no compilation on server)
    DEBUG_LOG("VMC: Deploying prebuilt DPDK VMC binary...");
    if (!g_ssh_deployer_server.deployPrebuilt(
            "dpdk_vmc",  // prebuilt folder (inside prebuilt/)
            "",          // app name (auto-detect: dpdk_app)
            false,       // DON'T run after deploy (we'll run interactively)
            false,       // no sudo for deploy
            "",          // no run args (not running yet)
            false        // not background
            ))
    {
        std::cerr << "VMC: DPDK VMC prebuilt deploy failed!" << std::endl;
        return false;
    }

    // Step 3: Run DPDK VMC interactively
    // User can answer y/n prompts for latency tests
    // After tests complete, DPDK VMC will fork to background automatically
    std::cout << std::endl;
    std::cout << "======================================" << std::endl;
    DEBUG_LOG("VMC: Starting DPDK VMC Interactive Mode");
    DEBUG_LOG("VMC: You can answer latency test prompts (y/n)");
    DEBUG_LOG("VMC: After tests, DPDK VMC will continue in background");
    std::cout << "======================================" << std::endl;
    std::cout << std::endl;

    std::string remote_dir = g_ssh_deployer_server.getRemoteDirectory();

    std::string dpdk_command = "cd " + remote_dir + "/dpdk_vmc && "
                                                    "echo 'q' | sudo -S -v && "
                                                    "sudo ./dpdk_app --daemon " +
                               eal_args;

    bool result = g_ssh_deployer_server.executeInteractive(dpdk_command, false);

    if (result)
    {
        std::cout << std::endl;
        std::cout << "======================================" << std::endl;
        std::cout << "VMC: DPDK VMC started successfully!" << std::endl;
        std::cout << "VMC: Running in background on server" << std::endl;
        std::cout << "VMC: Log file: /tmp/dpdk_app.log" << std::endl;
        std::cout << "======================================" << std::endl;
    }
    else
    {
        std::cerr << "VMC: DPDK VMC interactive execution failed!" << std::endl;
    }

    return result;
}

bool Vmc::configureSequence()
{
    auto& shutdown = SafeShutdown::getInstance();

    if (!g_Server.onWithWait(3))
    {
        ErrorPrinter::error("SERVER", "VMC: Server could not be started!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerServerOn();

    // Create PSU G30 (30V, 56A)
    if (!g_DeviceManager.create(PSUG30))
    {
        ErrorPrinter::error("PSU", "VMC: Failed to create PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    // Connect to PSU G30
    if (!g_DeviceManager.connect(PSUG30))
    {
        ErrorPrinter::error("PSU", "VMC: Failed to connect to PSU G30!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerPsuConnected(PSUG30);

    if (!g_DeviceManager.setCurrent(PSUG30, 5.0))
    {
        ErrorPrinter::error("PSU", "VMC: Failed to set current on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    if (!g_DeviceManager.setVoltage(PSUG30, 28.0))
    {
        ErrorPrinter::error("PSU", "VMC: Failed to set voltage on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    serial::sendSerialCommand("/dev/ttyACM0", "VMC_ID 27");

    if (!g_DeviceManager.enableOutput(PSUG30, true))
    {
        ErrorPrinter::error("PSU", "VMC: Failed to enable output on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerPsuOutputEnabled(PSUG30);

    // Record Unit Power On Time when PSU output is enabled
    g_ReportManager.recordUnitPowerOnTime();

    sleep(30);

    sleep(2);
    if (!g_cumulus.deployNetworkInterfaces(SSHDeployer::getPrebuiltRoot() + "/CumulusInterfaces/VMC/interfaces"))
    {
        ErrorPrinter::error("CUMULUS", "VMC: Failed to deploy network configuration!");
        shutdown.executeShutdown();
        return false;
    }
    DEBUG_LOG("VMC: Network configuration deployed successfully.");

    sleep(1);

    // Configure Cumulus switch VLANs (VMC-specific)
    if (!g_cumulus.configureSequenceVmc())
    {
        ErrorPrinter::error("CUMULUS", "VMC: Cumulus configuration failed!");
        shutdown.executeShutdown();
        return false;
    }

    sleep(1);

    // DPDK VMC - Interactive mode with embedded latency test
    if (!runDpdkVmcInteractive("-l 0-255 -n 16"))
    {
        ErrorPrinter::error("DPDK", "VMC: DPDK VMC deployment unsuccessful!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerDpdkRunning();

    // DPDK VMC is now running in background on server
    std::cout << "VMC: DPDK VMC is running in background, continuing..." << std::endl;

    // Monitor DPDK VMC stats every 10 seconds until Ctrl+C
    std::cout << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "VMC: Monitoring DPDK VMC (every 10 seconds)" << std::endl;
    std::cout << "VMC: Press Ctrl+C to stop" << std::endl;
    std::cout << "======================================" << std::endl;

    // Setup signal handler for Ctrl+C (DPDK VMC monitoring only)
    g_dpdk_vmc_monitoring_running = true;
    struct sigaction sa;
    sa.sa_handler = dpdk_vmc_monitor_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    while (g_dpdk_vmc_monitoring_running)
    {
        // Wait 10 seconds (check flag each second)
        for (int i = 0; i < 10 && g_dpdk_vmc_monitoring_running; i++)
        {
            sleep(1);
        }

        if (!g_dpdk_vmc_monitoring_running)
            break;

        std::string raw;
        g_ssh_deployer_server.execute(
            "tail -n 300 /tmp/dpdk_app.log", &raw, false, true);

        if (!raw.empty())
        {
            // Check if DPDK VMC has exited or is shutting down
            if (raw.find("Application exited cleanly") != std::string::npos ||
                raw.find("=== Shutting down ===") != std::string::npos)
            {
                std::cout << "\033[2J\033[H";
                std::cout << "======================================" << std::endl;
                std::cout << "VMC: DPDK VMC has exited on server." << std::endl;
                std::cout << "VMC: Auto-exiting monitoring loop..." << std::endl;
                std::cout << "======================================" << std::endl;
                g_dpdk_vmc_monitoring_running = false;
                break;
            }

            const std::string sep = "========== [";
            size_t last = raw.rfind(sep);
            size_t prev = (last != std::string::npos && last > 0)
                              ? raw.rfind(sep, last - 1)
                              : std::string::npos;

            std::string block;
            if (prev != std::string::npos && last != std::string::npos)
                block = raw.substr(prev, last - prev);
            else if (last != std::string::npos)
                block = raw.substr(last);
            else
                block = raw;

            std::cout << "\033[2J\033[H";
            std::cout << "=== DPDK VMC Live Stats (Press Ctrl+C to stop) ===" << std::endl;
            std::cout << block << std::endl;
        }
        else
        {
            DEBUG_LOG("(No log output yet - DPDK VMC might still be starting)");
        }
    }

    // Re-install SafeShutdown signal handlers after DPDK VMC monitoring
    shutdown.installSignalHandlers();

    // Stop DPDK VMC on server
    std::cout << "VMC: Stopping DPDK VMC on server..." << std::endl;
    if (g_ssh_deployer_server.isApplicationRunning("dpdk_app"))
    {
        g_ssh_deployer_server.stopApplication("dpdk_app", true);
        std::cout << "VMC: DPDK VMC stopped." << std::endl;
    }
    else
    {
        DEBUG_LOG("VMC: DPDK VMC was not running.");
    }
    shutdown.unregisterDpdkRunning();

    // Fetch DPDK VMC log from server to local PC
    DEBUG_LOG("VMC: Fetching DPDK VMC log from server...");
    ensureLogDirectories();
    std::string local_dpdk_log = LogPaths::VMC() + "/dpdk_app.log";
    if (g_ssh_deployer_server.fetchFile("/tmp/dpdk_app.log", local_dpdk_log))
    {
        std::cout << "VMC: DPDK VMC log saved to: " << local_dpdk_log << std::endl;
    }
    else
    {
        ErrorPrinter::warn("SSH", "VMC: Failed to fetch DPDK VMC log (file may not exist)");
    }

    if (!g_DeviceManager.enableOutput(PSUG30, false))
    {
        ErrorPrinter::error("PSU", "VMC: Failed to disable output on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    // Verify PSU output is actually off, retry if not
    for (int retry = 0; retry < 3; retry++)
    {
        usleep(500000); // 500ms wait before checking
        if (!g_DeviceManager.isOutputEnabled(PSUG30))
        {
            std::cout << "VMC: PSU G30 output verified OFF." << std::endl;
            break;
        }
        ErrorPrinter::warn("PSU", "VMC: PSU G30 still ON, retry " + std::to_string(retry + 1) + "/3...");
        g_DeviceManager.enableOutput(PSUG30, false);
    }
    shutdown.unregisterPsuOutputEnabled(PSUG30);

    // Record Power Off Time when PSU output is disabled
    g_ReportManager.recordPowerOffTime();

    if (!g_DeviceManager.disconnect(PSUG30))
    {
        ErrorPrinter::error("PSU", "VMC: Failed to disconnect PSU G30!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.unregisterPsuConnected(PSUG30);

    // g_Server.offWithWait(300);
    // shutdown.unregisterServerOn();

    std::cout << "VMC: PSU configured successfully." << std::endl;
    return true;
}
