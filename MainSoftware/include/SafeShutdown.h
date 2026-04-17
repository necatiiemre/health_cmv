#ifndef SAFE_SHUTDOWN_H
#define SAFE_SHUTDOWN_H

#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include "Device.h"

/**
 * @brief Tracks active resources and performs safe shutdown on error or signal.
 *
 * When a resource is activated (PSU output enabled, server powered on, etc.),
 * register it here. On error, call executeShutdown() to safely release all
 * resources in reverse priority order.
 *
 * Shutdown priority (most critical first):
 *   1. PSU output disable   (electrical safety)
 *   2. PSU disconnect        (resource cleanup)
 *   3. Stop DPDK on server   (free server resources)
 *   4. Stop remote apps       (RemoteConfigSender etc.)
 *   5. (Server power off is optional - not automatic)
 */
class SafeShutdown {
public:
    static SafeShutdown& getInstance();

    // Prohibited
    SafeShutdown(const SafeShutdown&) = delete;
    SafeShutdown& operator=(const SafeShutdown&) = delete;

    // ==================== Signal Handling ====================

    /**
     * @brief Install SIGINT and SIGTERM handlers
     * When signal is received, executeShutdown() is called automatically.
     */
    void installSignalHandlers();

    // ==================== Resource Registration ====================

    void registerPsuOutputEnabled(Device device);
    void unregisterPsuOutputEnabled(Device device);

    void registerPsuConnected(Device device);
    void unregisterPsuConnected(Device device);

    void registerServerOn();
    void unregisterServerOn();

    void registerDpdkRunning();
    void unregisterDpdkRunning();

    void registerRemoteApp(const std::string& app_name);
    void unregisterRemoteApp(const std::string& app_name);

    // ==================== Shutdown ====================

    /**
     * @brief Safely shutdown all registered resources.
     *
     * - Idempotent: can be called multiple times safely
     * - Best-effort: if one step fails, continues with the rest
     * - Thread-safe: protected by mutex
     */
    void executeShutdown();

    /**
     * @brief Check if shutdown has already been executed
     */
    bool isShutdownExecuted() const;

private:
    SafeShutdown();
    ~SafeShutdown() = default;

    static void signalHandler(int sig);

    mutable std::mutex m_mutex;
    std::atomic<bool> m_shutdown_executed{false};

    // Tracked resources
    std::vector<Device> m_psu_output_enabled;
    std::vector<Device> m_psu_connected;
    bool m_server_on = false;
    bool m_dpdk_running = false;
    std::vector<std::string> m_remote_apps;
};

#endif // SAFE_SHUTDOWN_H
