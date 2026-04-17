#include "SafeShutdown.h"
#include "ErrorPrinter.h"
#include "DeviceManager.h"
#include "SSHDeployer.h"
#include "Server.h"
#include <csignal>
#include <algorithm>
#include <unistd.h>

SafeShutdown& SafeShutdown::getInstance() {
    static SafeShutdown instance;
    return instance;
}

SafeShutdown::SafeShutdown() {}

// ==================== Signal Handling ====================

void SafeShutdown::signalHandler(int sig) {
    (void)sig;
    ErrorPrinter::info("SYSTEM", "Signal received, initiating safe shutdown...");
    SafeShutdown::getInstance().executeShutdown();
    _exit(1);
}

void SafeShutdown::installSignalHandlers() {
    struct sigaction sa;
    sa.sa_handler = SafeShutdown::signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// ==================== Resource Registration ====================

void SafeShutdown::registerPsuOutputEnabled(Device device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::find(m_psu_output_enabled.begin(), m_psu_output_enabled.end(), device) == m_psu_output_enabled.end()) {
        m_psu_output_enabled.push_back(device);
    }
}

void SafeShutdown::unregisterPsuOutputEnabled(Device device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find(m_psu_output_enabled.begin(), m_psu_output_enabled.end(), device);
    if (it != m_psu_output_enabled.end()) {
        m_psu_output_enabled.erase(it);
    }
}

void SafeShutdown::registerPsuConnected(Device device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::find(m_psu_connected.begin(), m_psu_connected.end(), device) == m_psu_connected.end()) {
        m_psu_connected.push_back(device);
    }
}

void SafeShutdown::unregisterPsuConnected(Device device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find(m_psu_connected.begin(), m_psu_connected.end(), device);
    if (it != m_psu_connected.end()) {
        m_psu_connected.erase(it);
    }
}

void SafeShutdown::registerServerOn() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_server_on = true;
}

void SafeShutdown::unregisterServerOn() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_server_on = false;
}

void SafeShutdown::registerDpdkRunning() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dpdk_running = true;
}

void SafeShutdown::unregisterDpdkRunning() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dpdk_running = false;
}

void SafeShutdown::registerRemoteApp(const std::string& app_name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::find(m_remote_apps.begin(), m_remote_apps.end(), app_name) == m_remote_apps.end()) {
        m_remote_apps.push_back(app_name);
    }
}

void SafeShutdown::unregisterRemoteApp(const std::string& app_name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find(m_remote_apps.begin(), m_remote_apps.end(), app_name);
    if (it != m_remote_apps.end()) {
        m_remote_apps.erase(it);
    }
}

// ==================== Shutdown ====================

bool SafeShutdown::isShutdownExecuted() const {
    return m_shutdown_executed.load();
}

void SafeShutdown::executeShutdown() {
    // Idempotent: only execute once
    bool expected = false;
    if (!m_shutdown_executed.compare_exchange_strong(expected, true)) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    ErrorPrinter::info("SYSTEM", "Safe shutdown initiated...");

    // PRIORITY 1: Disable PSU outputs (ELECTRICAL SAFETY - MOST CRITICAL)
    for (const auto& device : m_psu_output_enabled) {
        std::string name = (device == PSUG30) ? "PSU G30" : "PSU G300";
        ErrorPrinter::info("PSU", name + " output is being disabled...");
        try {
            g_DeviceManager.enableOutput(device, false);
            ErrorPrinter::info("PSU", name + " output disabled.");
        } catch (...) {
            ErrorPrinter::error("PSU", name + " output could not be disabled!");
        }
    }
    m_psu_output_enabled.clear();

    // PRIORITY 2: Disconnect PSUs
    for (const auto& device : m_psu_connected) {
        std::string name = (device == PSUG30) ? "PSU G30" : "PSU G300";
        ErrorPrinter::info("PSU", name + " disconnecting...");
        try {
            g_DeviceManager.disconnect(device);
            ErrorPrinter::info("PSU", name + " disconnected.");
        } catch (...) {
            ErrorPrinter::error("PSU", name + " could not be disconnected!");
        }
    }
    m_psu_connected.clear();

    // PRIORITY 3: Stop DPDK on server (only if still running)
    if (m_dpdk_running) {
        try {
            if (g_ssh_deployer_server.isApplicationRunning("dpdk_app")) {
                ErrorPrinter::info("DPDK", "Stopping DPDK on server...");
                g_ssh_deployer_server.stopApplication("dpdk_app", true);
                ErrorPrinter::info("DPDK", "DPDK stopped.");
            } else {
                ErrorPrinter::info("DPDK", "DPDK already exited, skipping.");
            }
        } catch (...) {
            ErrorPrinter::error("DPDK", "DPDK could not be stopped!");
        }
        m_dpdk_running = false;
    }

    // PRIORITY 4: Stop remote applications (only if still running)
    for (const auto& app : m_remote_apps) {
        try {
            if (g_ssh_deployer_server.isApplicationRunning(app)) {
                ErrorPrinter::info("SSH", "Stopping remote application: " + app);
                g_ssh_deployer_server.stopApplication(app, true);
                ErrorPrinter::info("SSH", app + " stopped.");
            } else {
                ErrorPrinter::info("SSH", app + " already exited, skipping.");
            }
        } catch (...) {
            ErrorPrinter::error("SSH", app + " could not be stopped!");
        }
    }
    m_remote_apps.clear();

    // NOTE: Server power-off is NOT automatic.
    // It can take a long time and may not be desired in all error scenarios.
    // The server state flag is cleared but no power-off command is sent.
    m_server_on = false;

    ErrorPrinter::info("SYSTEM", "Safe shutdown completed.");
}
