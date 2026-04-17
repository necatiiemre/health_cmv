#ifndef SERVER_H
#define SERVER_H

#include <string>
#include <chrono>

/**
 * @brief Server power state (retrieved from iDRAC)
 */
enum class PowerState {
    On,
    Off,
    Unknown
};

/**
 * @brief Server detailed state (retrieved from iDRAC getsysinfo)
 *
 * Boot process: PowerOff -> PowerOn -> BiosPost -> OSRunning
 */
enum class ServerState {
    PowerOff,       // Power Status = OFF
    PowerOn,        // Power Status = ON, but no OS info (in BIOS/POST stage)
    OSRunning,      // Power Status = ON and OS Name is populated (operating system running)
    Unknown         // Could not retrieve information
};

/**
 * @brief System information retrieved from iDRAC
 */
struct SystemInfo {
    PowerState power_state = PowerState::Unknown;
    ServerState server_state = ServerState::Unknown;
    std::string host_name;
    std::string os_name;
    std::string os_version;
    std::string bios_version;
    std::string system_model;

    // Helper methods
    bool isPowerOn() const { return power_state == PowerState::On; }
    bool isPowerOff() const { return power_state == PowerState::Off; }
    bool isOSRunning() const { return server_state == ServerState::OSRunning; }
};

/**
 * @brief Server management class
 *
 * Provides programmatic control of the server managed via terminal
 * server_on/server_off commands. Controls power state via SSH through iDRAC.
 */
class Server {
public:
    /**
     * @brief Singleton instance
     */
    static Server& getInstance();

    // Copy/move prohibited
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /**
     * @brief Powers on server and waits until fully ready
     *
     * 2-step boot process:
     * 1. Wait for Power ON (max 300s) - iDRAC powerstatus
     * 2. Wait for Ping (max 300s) - network reachability
     *
     * On timeout at any step, hard reset is performed and process restarts.
     * Returns false after 3 failed attempts.
     *
     * @param max_retries Maximum reset attempts (default: 3)
     * @return true Server started and ping successful
     * @return false Failed after 3 attempts
     */
    bool onWithWait(int max_retries = 3);

    /**
     * @brief Powers off server and waits for shutdown
     *
     * 1. Executes "server_off" command
     * 2. Checks power state from iDRAC
     * 3. Waits until Power OFF
     *
     * @param timeout_seconds Maximum wait time (seconds)
     * @return true Server powered off
     * @return false Timeout
     */
    bool offWithWait(int timeout_seconds = 300);

    /**
     * @brief Powers on server (non-blocking)
     * @return true Command successful
     */
    bool on();

    /**
     * @brief Powers off server (non-blocking)
     * @return true Command successful
     */
    bool off();

    /**
     * @brief Performs hard reset via iDRAC
     * @return true Command successful
     */
    bool hardReset();

    /**
     * @brief Gets power state from iDRAC
     * @return PowerState::On, PowerState::Off or PowerState::Unknown
     */
    PowerState getPowerState();

    /**
     * @brief Gets detailed system info from iDRAC (getsysinfo)
     * @return SystemInfo struct
     */
    SystemInfo getSystemInfo();

    /**
     * @brief Gets server state from iDRAC (PowerOff/PowerOn/OSRunning)
     * @return ServerState enum
     */
    ServerState getServerState();

    /**
     * @brief Is server powered on?
     * @return true If power state is ON
     */
    bool isOn();

    /**
     * @brief Is server powered off?
     * @return true If power state is OFF
     */
    bool isOff();

    /**
     * @brief Is OS running?
     * @return true If OS Name field is populated
     */
    bool isOSRunning();

    /**
     * @brief Checks if server is reachable (via ping)
     * @return true Ping successful
     */
    bool isReachable();

    // ==================== Configuration ====================

    /**
     * @brief Sets server IP address
     * @param ip IP address (default: 10.1.33.2)
     */
    void setServerIP(const std::string& ip);
    std::string getServerIP() const;

    /**
     * @brief Sets iDRAC connection credentials
     * @param ip iDRAC IP address
     * @param username Username
     * @param password Password
     */
    void setIdracCredentials(const std::string& ip,
                             const std::string& username,
                             const std::string& password);

    void setIdracIP(const std::string& ip);
    std::string getIdracIP() const;

    void setIdracUsername(const std::string& username);
    void setIdracPassword(const std::string& password);

    /**
     * @brief Sets check interval
     * @param ms Interval in milliseconds (default: 2000ms)
     */
    void setCheckIntervalMs(int ms);

private:
    Server();
    ~Server() = default;

    std::string m_server_ip;
    std::string m_idrac_ip;
    std::string m_idrac_username;
    std::string m_idrac_password;
    int m_check_interval_ms;

    // Boot process timeout values (seconds)
    static constexpr int POWER_ON_TIMEOUT_SEC = 200;    // Step 1: Wait for Power ON
    static constexpr int PING_TIMEOUT_SEC = 400;        // Step 2: Wait for Ping

    /**
     * @brief Step 1: Wait for power on
     * @param timeout_seconds Timeout duration
     * @return true Power ON achieved, false timeout
     */
    bool waitForPowerOn(int timeout_seconds);

    /**
     * @brief Step 2: Wait for ping response
     * @param timeout_seconds Timeout duration
     * @return true Ping successful, false timeout
     */
    bool waitForPing(int timeout_seconds);

    /**
     * @brief Connects to iDRAC via SSH and executes racadm command
     * @param command racadm command (e.g., "serveraction powerstatus")
     * @return Command output
     */
    std::string executeIdracCommand(const std::string& command);

    /**
     * @brief Parses getsysinfo output
     * @param output racadm getsysinfo output
     * @return SystemInfo struct
     */
    SystemInfo parseSystemInfo(const std::string& output);

    /**
     * @brief Sends a single ping to specified IP
     * @return true Ping successful (host reachable)
     */
    bool ping(const std::string& ip);

    /**
     * @brief Converts ServerState to string
     */
    static std::string serverStateToString(ServerState state);
};

// Global access
extern Server& g_Server;

#endif // SERVER_H
