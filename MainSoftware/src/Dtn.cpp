#include "Dtn.h"
#include "SSHDeployer.h"
#include "CumulusHelper.h"
#include "SerialTimeForwarder.h"
#include "ReportManager.h"
#include "SafeShutdown.h"
#include "ErrorPrinter.h"
#include "PsuTelemetry.h"
#include "PsuTelemetryPublisher.h"
#include <iostream>
#include <unistd.h>
#include <iomanip>
#include <filesystem>
#include <limits>
#include <csignal>
#include <atomic>
#include "Utils.h"

// LogPaths::rootDir() - runtime root detection (replaces compile-time PROJECT_ROOT)
std::string LogPaths::rootDir() {
    return SSHDeployer::getSourceRoot();
}

// Global flag for Ctrl+C handling in DPDK monitoring
static std::atomic<bool> g_dpdk_monitoring_running{true};

static void dpdk_monitor_signal_handler(int sig)
{
    (void)sig;
    g_dpdk_monitoring_running = false;
}

// PSU Configuration: 28V 3.0A

Dtn g_dtn;

Dtn::Dtn()
{
}

Dtn::~Dtn()
{
}

bool Dtn::latencyTestSequence()
{
    bool valid_test = false;

    if (askQuestion("Do you want to run HW Timestamp Latency Test (Default measured latency : 14us)"))
    {
        while (!valid_test)
        {
            bool answer = askQuestion("You need to install the LoopBack connectors for this test. Check before starting the test. Should I start the test?");
            if (answer)
            {
                valid_test = true;
                runLatencyTest("-n 1 -vvv");
            }
            else
            {
                if (askQuestion("Do you want to skip the test?"))
                {
                    valid_test = true;
                    return true;
                }
            }
        }
    }
    return 0;
}

bool Dtn::askQuestion(const std::string &question)
{
    char response;
    while (true)
    {
        std::cout << question << " [y/n]: ";
        std::cin >> response;

        if (std::cin.fail())
        {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "Invalid input! Please enter 'y' or 'n'.\n";
            continue;
        }

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        if (response == 'y' || response == 'Y')
        {
            return true;
        }
        else if (response == 'n' || response == 'N')
        {
            return false;
        }
        else
        {
            std::cout << "Invalid input! Please enter 'y' or 'n'.\n";
        }
    }
}

uint8_t Dtn::ask28VoltStatus()
{
    int choice = 0;
    while (true)
    {
        std::cout << std::endl;
        std::cout << "======================================" << std::endl;
        std::cout << "  28V Power Status Selection" << std::endl;
        std::cout << "======================================" << std::endl;
        std::cout << "  1. Primary SUCCESS, Secondary FAIL" << std::endl;
        std::cout << "  2. Primary FAIL, Secondary SUCCESS" << std::endl;
        std::cout << "  3. Both SUCCESS" << std::endl;
        std::cout << "======================================" << std::endl;
        std::cout << "Select option [1-3]: ";
        std::cin >> choice;

        if (std::cin.fail())
        {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "Invalid input! Please enter 1, 2, or 3." << std::endl;
            continue;
        }

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        switch (choice)
        {
            case 1:
                // Primary SUCCESS (bit0=0), Secondary FAIL (bit1=1) → 0x02
                std::cout << "Selected: Primary=SUCCESS, Secondary=FAIL (0x02)" << std::endl;
                return 0x02;
            case 2:
                // Primary FAIL (bit0=1), Secondary SUCCESS (bit1=0) → 0x01
                std::cout << "Selected: Primary=FAIL, Secondary=SUCCESS (0x01)" << std::endl;
                return 0x01;
            case 3:
                // Both SUCCESS → 0x00
                std::cout << "Selected: Primary=SUCCESS, Secondary=SUCCESS (0x00)" << std::endl;
                return 0x00;
            default:
                std::cout << "Invalid option! Please enter 1, 2, or 3." << std::endl;
                break;
        }
    }
}

bool Dtn::ensureLogDirectories()
{
    try
    {
        std::filesystem::create_directories(LogPaths::CMC());
        std::filesystem::create_directories(LogPaths::VMC());
        std::filesystem::create_directories(LogPaths::MMC());
        std::filesystem::create_directories(LogPaths::DTN());
        std::filesystem::create_directories(LogPaths::HSN());
        DEBUG_LOG("DTN: Log directories created/verified at " << LogPaths::baseDir());
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "DTN: Failed to create log directories: " << e.what() << std::endl;
        return false;
    }
}

bool Dtn::runLatencyTest(const std::string &run_args, int timeout_seconds)
{
    std::cout << "======================================" << std::endl;
    std::cout << "DTN: HW Timestamp Latency Test" << std::endl;
    std::cout << "======================================" << std::endl;

    // Ensure log directories exist
    if (!ensureLogDirectories())
    {
        std::cerr << "DTN: Failed to create log directories!" << std::endl;
        return false;
    }

    // Build local log path
    std::string local_log_path = LogPaths::DTN() + "/latency_test.log";

    DEBUG_LOG("DTN: Run arguments: " << (run_args.empty() ? "(default)" : run_args));
    DEBUG_LOG("DTN: Timeout: " << timeout_seconds << " seconds");
    DEBUG_LOG("DTN: Log output: " << local_log_path);

    // Run the test using SSHDeployer (prebuilt binary, no compilation)
    bool result = g_ssh_deployer_server.deployPrebuiltRunAndFetchLog(
        "latency_test", // Prebuilt folder (inside prebuilt/)
        "latency_test", // Executable name
        run_args,       // Arguments (e.g., "-n 10 -v")
        local_log_path, // Local log path
        timeout_seconds // Timeout
    );

    if (result)
    {
        std::cout << "======================================" << std::endl;
        std::cout << "DTN: Latency Test COMPLETED" << std::endl;
        std::cout << "DTN: Log saved to: " << local_log_path << std::endl;
        std::cout << "======================================" << std::endl;
    }
    else
    {
        std::cerr << "DTN: Latency Test FAILED!" << std::endl;
    }

    return result;
}

bool Dtn::runDpdkInteractive(const std::string &eal_args, const std::string &make_args)
{
    std::cout << "======================================" << std::endl;
    std::cout << "DTN: DPDK Interactive Deployment" << std::endl;
    std::cout << "======================================" << std::endl;

    // Step 1: Test connection
    if (!g_ssh_deployer_server.testConnection())
    {
        std::cerr << "DTN: Cannot connect to server!" << std::endl;
        return false;
    }

    // Step 2: Deploy prebuilt DPDK binary (no compilation on server)
    DEBUG_LOG("DTN: Deploying prebuilt DPDK binary...");
    if (!g_ssh_deployer_server.deployPrebuilt(
            "dpdk",  // prebuilt folder (inside prebuilt/)
            "",      // app name (auto-detect: dpdk_app)
            false,   // DON'T run after deploy (we'll run interactively)
            false,   // no sudo for deploy
            "",      // no run args (not running yet)
            false    // not background
            ))
    {
        std::cerr << "DTN: DPDK prebuilt deploy failed!" << std::endl;
        return false;
    }

    // Step 3: Run DPDK interactively
    // User can answer y/n prompts for latency tests
    // After tests complete, DPDK will fork to background automatically
    std::cout << std::endl;
    std::cout << "======================================" << std::endl;
    DEBUG_LOG("DTN: Starting DPDK Interactive Mode");
    DEBUG_LOG("DTN: You can answer latency test prompts (y/n)");
    DEBUG_LOG("DTN: After tests, DPDK will continue in background");
    std::cout << "======================================" << std::endl;
    std::cout << std::endl;

    std::string remote_dir = g_ssh_deployer_server.getRemoteDirectory();

    // IMPORTANT: Don't pipe password to sudo, it breaks stdin for interactive input!
    // Instead: First authenticate sudo (caches credentials), then run DPDK
    // sudo -v = validate/refresh sudo timestamp without running a command
    // sudo -S = read password from stdin (only for the -v part)
    // After -v succeeds, subsequent sudo commands don't need password (within timeout)
    // --daemon flag: tells DPDK to fork to background after latency tests
    std::string dpdk_command = "cd " + remote_dir + "/dpdk && "
                                                    "echo 'q' | sudo -S -v && " // Authenticate sudo first
                                                    "sudo ./dpdk_app --daemon " +
                               eal_args; // --daemon for background mode

    bool result = g_ssh_deployer_server.executeInteractive(dpdk_command, false);

    if (result)
    {
        std::cout << std::endl;
        std::cout << "======================================" << std::endl;
        std::cout << "DTN: DPDK started successfully!" << std::endl;
        std::cout << "DTN: Running in background on server" << std::endl;
        std::cout << "DTN: Log file: /tmp/dpdk_app.log" << std::endl;
        std::cout << "======================================" << std::endl;
    }
    else
    {
        std::cerr << "DTN: DPDK interactive execution failed!" << std::endl;
    }

    return result;
}

bool Dtn::configureSequence()
{
    auto& shutdown = SafeShutdown::getInstance();

    // Server power on
    if (!g_Server.onWithWait(3))
    {
        ErrorPrinter::error("SERVER", "DTN: Server could not be started!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerServerOn();

    // Send serial command
    g_systemCommand.execute("echo \"VMC_ID 38\" > /dev/ttyACM0");
    sleep(2);

    // Create PSU G30 (30V, 56A)
    if (!g_DeviceManager.create(PSUG30))
    {
        ErrorPrinter::error("PSU", "DTN: Failed to create PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    // Connect to PSU G30
    if (!g_DeviceManager.connect(PSUG30))
    {
        ErrorPrinter::error("PSU", "DTN: Failed to connect to PSU G30!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerPsuConnected(PSUG30);

    if (!g_DeviceManager.setCurrent(PSUG30, 3.0))
    {
        ErrorPrinter::error("PSU", "DTN: Failed to set current on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    if (!g_DeviceManager.setVoltage(PSUG30, 28.0))
    {
        ErrorPrinter::error("PSU", "DTN: Failed to set voltage on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    // Ask user for expected 28V power status BEFORE enabling power output
    // This way the user declares expected state before power sources are turned on
    uint8_t expected_power_status = ask28VoltStatus();
    std::string power_status_arg = "--expected-power-status " + std::to_string(expected_power_status);

    if (!g_DeviceManager.enableOutput(PSUG30, true))
    {
        ErrorPrinter::error("PSU", "DTN: Failed to enable output on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerPsuOutputEnabled(PSUG30);

    // Record Unit Power On Time when PSU output is enabled
    g_ReportManager.recordUnitPowerOnTime();

    // Start PSU telemetry publisher. Sends V/I/W to the DPDK process every
    // second over UDP so DPDK can print a power-supply row in its health
    // table. Also acts as a heartbeat that keeps the PSU TCP connection
    // alive during long idle periods - fixes the "after 2h the PSU doesn't
    // turn off" failure mode. RAII: destructor stops it on any return path.
    PsuTelemetryPublisher psu_publisher(
        PSUG30,
        g_ssh_deployer_server.getHost(),
        PSU_TELEM_PORT);
    if (!psu_publisher.start()) {
        ErrorPrinter::warn("PSU-TELEM",
            "DTN: Failed to start PSU telemetry publisher - "
            "DPDK will not receive power-supply readings, "
            "but the test will continue.");
    }

    sleep(2);
    if (!g_cumulus.deployNetworkInterfaces(SSHDeployer::getPrebuiltRoot() + "/CumulusInterfaces/DTNIRSW/interfaces"))
    {
        ErrorPrinter::error("CUMULUS", "DTN: Failed to deploy network configuration!");
        shutdown.executeShutdown();
        return false;
    }
    DEBUG_LOG("DTN: Network configuration deployed successfully.");

    sleep(1);

    // Configure Cumulus switch VLANs
    if (!g_cumulus.configureSequence())
    {
        ErrorPrinter::error("CUMULUS", "DTN: Cumulus configuration failed!");
        shutdown.executeShutdown();
        return false;
    }

    sleep(1);

    // Deploy prebuilt RemoteConfigSender binary and run with sudo (required for raw sockets)
    // NOTE: RemoteConfigSender is a fire-and-forget app (runs, completes its task, exits).
    // Do NOT register it with SafeShutdown - it will have already exited by the time
    // shutdown runs, and trying to stop a non-existent process can cause hangs.
    if (!g_ssh_deployer_server.deployPrebuilt("RemoteConfigSender", "", true, true))
    {
        ErrorPrinter::error("SSH", "DTN: RemoteConfigSender deployment unsuccessful!");
        shutdown.executeShutdown();
        return false;
    }

    sleep(2);
    // Start SerialTimeForwarder after remote_config_sender is running
    // Reads time from MicroChip SyncServer (USB0), forwards to USB1, verifies on USB2
    serial::SerialTimeForwarder timeForwarder("/dev/ttyUSB0", "/dev/ttyUSB1");
    if (timeForwarder.start())
    {
        DEBUG_LOG("DTN: SerialTimeForwarder started successfully.");
    }
    else
    {
        ErrorPrinter::warn("SERIAL", "DTN: Failed to start SerialTimeForwarder: "
                  + timeForwarder.getLastError());
    }

    // DPDK - Interactive mode with embedded latency test
    // Note: expected_power_status was already asked before PSU output was enabled
    if (!runDpdkInteractive(power_status_arg + " -l 0-255 -n 16"))
    {
        ErrorPrinter::error("DPDK", "DTN: DPDK deployment unsuccessful!");
        if (timeForwarder.isRunning()) timeForwarder.stop();
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerDpdkRunning();

    // DPDK is now running in background on server
    std::cout << "DTN: DPDK is running in background, continuing..." << std::endl;

    // Monitor DPDK stats every 10 seconds until Ctrl+C
    std::cout << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "DTN: Monitoring DPDK (every 10 seconds)" << std::endl;
    std::cout << "DTN: Press Ctrl+C to stop" << std::endl;
    std::cout << "======================================" << std::endl;

    // Setup signal handler for Ctrl+C (DPDK monitoring only)
    g_dpdk_monitoring_running = true;
    struct sigaction sa;
    sa.sa_handler = dpdk_monitor_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    while (g_dpdk_monitoring_running)
    {
        // Wait 10 seconds (check flag each second)
        for (int i = 0; i < 10 && g_dpdk_monitoring_running; i++)
        {
            sleep(1);
        }

        if (!g_dpdk_monitoring_running)
            break;

        std::string raw;
        g_ssh_deployer_server.execute(
            "tail -n 300 /tmp/dpdk_app.log", &raw, false, true);

        if (!raw.empty())
        {
            // Check if DPDK has exited or is shutting down by looking for markers in the log
            if (raw.find("Application exited cleanly") != std::string::npos ||
                raw.find("=== Shutting down ===") != std::string::npos)
            {
                std::cout << "\033[2J\033[H";
                std::cout << "======================================" << std::endl;
                std::cout << "DTN: DPDK has exited on server." << std::endl;
                std::cout << "DTN: Auto-exiting monitoring loop..." << std::endl;
                std::cout << "======================================" << std::endl;
                g_dpdk_monitoring_running = false;
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
            std::cout << "=== DPDK Live Stats (Press Ctrl+C to stop) ===" << std::endl;
            std::cout << block << std::endl;
        }
        else
        {
            DEBUG_LOG("(No log output yet - DPDK might still be starting)");
        }
    }

    // Re-install SafeShutdown signal handlers after DPDK monitoring
    shutdown.installSignalHandlers();

    // Stop SerialTimeForwarder and show stats
    if (timeForwarder.isRunning())
    {
        DEBUG_LOG("DTN: SerialTimeForwarder stats:");
        DEBUG_LOG("  Packets sent: " << timeForwarder.getPacketsSent());
        DEBUG_LOG("  Last timestamp: " << timeForwarder.getLastTimestamp());
        DEBUG_LOG("  Last time string: " << timeForwarder.getLastTimeString());
        timeForwarder.stop();
        DEBUG_LOG("DTN: SerialTimeForwarder stopped.");
    }

    // Stop the PSU telemetry publisher BEFORE stopping DPDK so DPDK's final
    // log lines aren't interleaved with PSU telem warnings. Idempotent, so
    // the RAII destructor later is a harmless no-op.
    psu_publisher.stop();

    // Stop DPDK on server
    std::cout << "DTN: Stopping DPDK on server..." << std::endl;
    if (g_ssh_deployer_server.isApplicationRunning("dpdk_app"))
    {
        g_ssh_deployer_server.stopApplication("dpdk_app", true);
        std::cout << "DTN: DPDK stopped." << std::endl;
    }
    else
    {
        DEBUG_LOG("DTN: DPDK was not running.");
    }
    shutdown.unregisterDpdkRunning();

    // Fetch DPDK log from server to local PC
    DEBUG_LOG("DTN: Fetching DPDK log from server...");
    ensureLogDirectories();
    std::string local_dpdk_log = LogPaths::DTN() + "/dpdk_app.log";
    if (g_ssh_deployer_server.fetchFile("/tmp/dpdk_app.log", local_dpdk_log))
    {
        std::cout << "DTN: DPDK log saved to: " << local_dpdk_log << std::endl;
    }
    else
    {
        ErrorPrinter::warn("SSH", "DTN: Failed to fetch DPDK log (file may not exist)");
    }

    // Disable PSU output
    if (!g_DeviceManager.enableOutput(PSUG30, false))
    {
        ErrorPrinter::error("PSU", "DTN: Failed to disable output on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    // Verify PSU output is actually off, retry if not
    for (int retry = 0; retry < 3; retry++)
    {
        usleep(500000);
        if (!g_DeviceManager.isOutputEnabled(PSUG30))
        {
            std::cout << "DTN: PSU G30 output verified OFF." << std::endl;
            break;
        }
        ErrorPrinter::warn("PSU", "DTN: PSU G30 still ON, retry " + std::to_string(retry + 1) + "/3...");
        g_DeviceManager.enableOutput(PSUG30, false);
    }
    shutdown.unregisterPsuOutputEnabled(PSUG30);

    // Record Power Off Time when PSU output is disabled
    g_ReportManager.recordPowerOffTime();

    if (!g_DeviceManager.disconnect(PSUG30))
    {
        ErrorPrinter::error("PSU", "DTN: Failed to disconnect PSU G30!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.unregisterPsuConnected(PSUG30);

    // g_Server.offWithWait(300);

    std::cout << "DTN: PSU configured successfully." << std::endl;
    return true;
}