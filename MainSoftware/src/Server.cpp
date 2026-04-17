#include "Server.h"
#include "SystemCommand.h"
#include "Utils.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <sstream>

// Global instance
Server& g_Server = Server::getInstance();

Server& Server::getInstance() {
    static Server instance;
    return instance;
}

Server::Server()
    : m_server_ip("10.1.33.2")
    , m_idrac_ip("10.1.33.254")
    , m_idrac_username("power")
    , m_idrac_password("mmuBilgem2025")
    , m_check_interval_ms(2000) {
}

// ==================== Basic Commands ====================

bool Server::on() {
    // Skip if already powered on
    if (getPowerState() == PowerState::On) {
        DEBUG_LOG("[Server] Server already powered on, skipping.");
        return true;
    }

    auto result = g_systemCommand.execute("server_on");
    return result.success;
}

bool Server::off() {
    // Skip if already powered off
    if (getPowerState() == PowerState::Off) {
        DEBUG_LOG("[Server] Server already powered off, skipping.");
        return true;
    }

    auto result = g_systemCommand.execute("server_off");
    return result.success;
}

bool Server::hardReset() {
    std::cout << "[Server] Performing hard reset..." << std::endl;
    std::string output = executeIdracCommand("serveraction hardreset");
    return !output.empty();
}

std::string Server::executeIdracCommand(const std::string& command) {
    if (m_idrac_ip.empty() || m_idrac_username.empty() || m_idrac_password.empty()) {
        std::cerr << "[Server] iDRAC credentials not configured!" << std::endl;
        return "";
    }

    std::string ssh_cmd = "sshpass -p '" + m_idrac_password + "' "
                          "ssh -o StrictHostKeyChecking=no "
                          "-o ConnectTimeout=10 "
                          + m_idrac_username + "@" + m_idrac_ip + " "
                          "\"racadm " + command + "\"";

    auto result = g_systemCommand.execute(ssh_cmd);
    return result.output;
}

// ==================== Helper Functions ====================

std::string Server::serverStateToString(ServerState state) {
    switch (state) {
        case ServerState::PowerOff:   return "PowerOff";
        case ServerState::PowerOn:    return "PowerOn (BIOS/POST)";
        case ServerState::OSRunning:  return "OSRunning";
        case ServerState::Unknown:    return "Unknown";
        default:                      return "Unknown";
    }
}

SystemInfo Server::parseSystemInfo(const std::string& output) {
    SystemInfo info;

    if (output.empty()) {
        return info;
    }

    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = (eq_pos + 1 < line.length()) ? line.substr(eq_pos + 1) : "";

        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t");
            size_t end = s.find_last_not_of(" \t");
            if (start == std::string::npos) {
                s = "";
            } else {
                s = s.substr(start, end - start + 1);
            }
        };

        trim(key);
        trim(value);

        if (key == "Power Status") {
            std::string lower_value = value;
            std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
            if (lower_value == "on") {
                info.power_state = PowerState::On;
            } else if (lower_value == "off") {
                info.power_state = PowerState::Off;
            }
        }
        else if (key == "Host Name") {
            info.host_name = value;
        }
        else if (key == "OS Name") {
            info.os_name = value;
        }
        else if (key == "OS Version") {
            info.os_version = value;
        }
        else if (key == "System BIOS Version") {
            info.bios_version = value;
        }
        else if (key == "System Model") {
            info.system_model = value;
        }
    }

    if (info.power_state == PowerState::Off) {
        info.server_state = ServerState::PowerOff;
    }
    else if (info.power_state == PowerState::On) {
        if (!info.os_name.empty()) {
            info.server_state = ServerState::OSRunning;
        } else {
            info.server_state = ServerState::PowerOn;
        }
    }
    else {
        info.server_state = ServerState::Unknown;
    }

    return info;
}

// ==================== State Queries ====================

PowerState Server::getPowerState() {
    std::string output = executeIdracCommand("serveraction powerstatus");

    if (output.empty()) {
        return PowerState::Unknown;
    }

    std::string lower_output = output;
    std::transform(lower_output.begin(), lower_output.end(), lower_output.begin(), ::tolower);

    if (lower_output.find("on") != std::string::npos) {
        return PowerState::On;
    } else if (lower_output.find("off") != std::string::npos) {
        return PowerState::Off;
    }

    return PowerState::Unknown;
}

SystemInfo Server::getSystemInfo() {
    std::string output = executeIdracCommand("getsysinfo");
    return parseSystemInfo(output);
}

ServerState Server::getServerState() {
    return getSystemInfo().server_state;
}

bool Server::isOn() {
    return getPowerState() == PowerState::On;
}

bool Server::isOff() {
    return getPowerState() == PowerState::Off;
}

bool Server::isOSRunning() {
    return getServerState() == ServerState::OSRunning;
}

bool Server::ping(const std::string& ip) {
    std::string cmd = "ping -c 1 -W 1 " + ip + " > /dev/null 2>&1";
    auto result = g_systemCommand.execute(cmd);
    return result.success;
}

bool Server::isReachable() {
    return ping(m_server_ip);
}

// ==================== Boot Steps ====================

bool Server::waitForPowerOn(int timeout_seconds) {
    std::cout << "[Server] STEP 1: Waiting for Power ON (max " << timeout_seconds << "s)..." << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        PowerState state = getPowerState();

        if (state == PowerState::On) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            std::cout << "[Server] Power ON! (" << elapsed_sec << " seconds)" << std::endl;
            return true;
        }

        // Timeout check
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        if (elapsed_sec >= timeout_seconds) {
            std::cerr << "[Server] TIMEOUT! Power ON not achieved within " << timeout_seconds
                      << " seconds" << std::endl;
            return false;
        }

        DEBUG_LOG("[Server] Power Status: OFF - waiting... ("
                  << elapsed_sec << "/" << timeout_seconds << "s)");

        std::this_thread::sleep_for(std::chrono::milliseconds(m_check_interval_ms));
    }
}

bool Server::waitForPing(int timeout_seconds) {
    std::cout << "[Server] STEP 2: Waiting for Ping (max " << timeout_seconds << "s) - "
              << m_server_ip << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        if (ping(m_server_ip)) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            std::cout << "[Server] Ping successful! (" << elapsed_sec << " seconds)" << std::endl;
            return true;
        }

        // Timeout check
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        if (elapsed_sec >= timeout_seconds) {
            std::cerr << "[Server] TIMEOUT! No ping response within " << timeout_seconds
                      << " seconds" << std::endl;
            return false;
        }

        DEBUG_LOG("[Server] Ping: failed - waiting... ("
                  << elapsed_sec << "/" << timeout_seconds << "s)");

        std::this_thread::sleep_for(std::chrono::milliseconds(m_check_interval_ms));
    }
}

// ==================== Main Boot Function ====================

bool Server::onWithWait(int max_retries) {
    // Check if already on and reachable
    if (getPowerState() == PowerState::On && isReachable()) {
        std::cout << "[Server] Server already on and reachable, skipping." << std::endl;
        return true;
    }

    int reset_count = 0;

    while (reset_count < max_retries) {
        std::cout << "\n========================================" << std::endl;
        std::cout << "[Server] Starting... (Attempt " << (reset_count + 1)
                  << "/" << max_retries << ")" << std::endl;
        std::cout << "========================================" << std::endl;

        // Send server_on command
        if (!on()) {
            std::cerr << "[Server] server_on command failed!" << std::endl;
            reset_count++;
            continue;
        }

        // STEP 1: Wait for Power ON (300 seconds)
        if (!waitForPowerOn(POWER_ON_TIMEOUT_SEC)) {
            std::cerr << "[Server] Step 1 failed - Performing hard reset..." << std::endl;
            hardReset();
            reset_count++;
            std::this_thread::sleep_for(std::chrono::seconds(5)); // Short wait after reset
            continue;
        }

        // STEP 2: Wait for Ping (300 seconds)
        if (!waitForPing(PING_TIMEOUT_SEC)) {
            std::cerr << "[Server] Step 2 failed - Performing hard reset..." << std::endl;
            hardReset();
            reset_count++;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // All steps successful!
        std::cout << "\n========================================" << std::endl;
        std::cout << "[Server] SERVER STARTED SUCCESSFULLY!" << std::endl;
        std::cout << "========================================\n" << std::endl;
        return true;
    }

    // All attempts failed
    std::cerr << "\n========================================" << std::endl;
    std::cerr << "[Server] ERROR! " << max_retries << " attempts failed." << std::endl;
    std::cerr << "[Server] Server could not be started - terminating program!" << std::endl;
    std::cerr << "========================================\n" << std::endl;
    return false;
}

bool Server::offWithWait(int timeout_seconds) {
    // Check if already powered off
    if (getPowerState() == PowerState::Off) {
        DEBUG_LOG("[Server] Server already powered off, skipping.");
        return true;
    }

    std::cout << "[Server] Shutting down..." << std::endl;

    if (!off()) {
        std::cerr << "[Server] server_off command failed!" << std::endl;
        return false;
    }

    std::cout << "[Server] Command sent, waiting for shutdown (max "
              << timeout_seconds << "s)..." << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        PowerState state = getPowerState();

        if (state == PowerState::Off) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            std::cout << "[Server] Power OFF! (" << elapsed_sec << " seconds)" << std::endl;
            return true;
        }

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        if (elapsed_sec >= timeout_seconds) {
            std::cerr << "[Server] TIMEOUT! Could not shutdown within " << timeout_seconds
                      << " seconds" << std::endl;
            return false;
        }

        DEBUG_LOG("[Server] Power Status: ON - waiting... ("
                  << elapsed_sec << "/" << timeout_seconds << "s)");

        std::this_thread::sleep_for(std::chrono::milliseconds(m_check_interval_ms));
    }
}

// ==================== Configuration ====================

void Server::setServerIP(const std::string& ip) {
    m_server_ip = ip;
}

std::string Server::getServerIP() const {
    return m_server_ip;
}

void Server::setIdracCredentials(const std::string& ip,
                                  const std::string& username,
                                  const std::string& password) {
    m_idrac_ip = ip;
    m_idrac_username = username;
    m_idrac_password = password;
}

void Server::setIdracIP(const std::string& ip) {
    m_idrac_ip = ip;
}

std::string Server::getIdracIP() const {
    return m_idrac_ip;
}

void Server::setIdracUsername(const std::string& username) {
    m_idrac_username = username;
}

void Server::setIdracPassword(const std::string& password) {
    m_idrac_password = password;
}

void Server::setCheckIntervalMs(int ms) {
    m_check_interval_ms = ms;
}
