/**
 * @file tdk_lambda_psu.cpp
 * @brief Implementation of Generic TDK Lambda Power Supply Controller
 * @version 1.0.0
 * @date 2025-11-26
 */

#include "TdkLambdaPsu.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>
#include <chrono>

// Linux/POSIX includes
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>   // TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT, TCP_NODELAY
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <poll.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace TDKLambda {

// ==================== TCP/IP Port Implementation ====================

/**
 * @brief TCP/IP port implementation for Ethernet communication
 */
class TcpPort : public ICommunication {
public:
    TcpPort(const PSUConfig& config)
        : m_config(config), m_is_open(false), m_sockfd(-1) {
    }

    ~TcpPort() override {
        close();
    }

    void open() override {
        if (m_is_open) {
            return;
        }

        if (m_config.ip_address.empty()) {
            throw PSUException("IP address is empty");
        }

        // Create socket
        m_sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_sockfd < 0) {
            throw PSUException("Failed to create socket");
        }

        // Set socket timeout
        struct timeval timeout;
        timeout.tv_sec = m_config.timeout_ms / 1000;
        timeout.tv_usec = (m_config.timeout_ms % 1000) * 1000;
        setsockopt(m_sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(m_sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        // Setup server address
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(m_config.tcp_port);

        // Convert IP address
        if (inet_pton(AF_INET, m_config.ip_address.c_str(), &server_addr.sin_addr) <= 0) {
            ::close(m_sockfd);
            throw PSUException("Invalid IP address: " + m_config.ip_address);
        }

        // Connect to server
        if (connect(m_sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            ::close(m_sockfd);
            throw PSUException("Failed to connect to " + m_config.ip_address + ":" +
                             std::to_string(m_config.tcp_port));
        }

        // Enable TCP keepalive so silent peer drops surface as a real error
        // within ~60 seconds instead of leaving us with a half-open socket.
        // Critical for long tests (2h+) where the PSU sits idle.
        {
            int yes = 1;
            setsockopt(m_sockfd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));

            int keepidle  = 30;  // Start probing after 30s of idle
            int keepintvl = 10;  // Probe every 10s
            int keepcnt   = 3;   // 3 failed probes -> socket errored
            setsockopt(m_sockfd, IPPROTO_TCP, TCP_KEEPIDLE,  &keepidle,  sizeof(keepidle));
            setsockopt(m_sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
            setsockopt(m_sockfd, IPPROTO_TCP, TCP_KEEPCNT,   &keepcnt,   sizeof(keepcnt));

            // Disable Nagle: SCPI is a small request/response protocol.
            int nodelay = 1;
            setsockopt(m_sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        }

        m_is_open = true;
    }

    size_t write(const std::string& data) override {
        if (!m_is_open) {
            throw PSUException("TCP port is not open");
        }

        // MSG_NOSIGNAL prevents SIGPIPE when the peer has silently dropped the
        // connection (e.g. after long idle). Without it, writing to a closed
        // socket would terminate the entire process with no error message.
        ssize_t result = send(m_sockfd, data.c_str(), data.length(), MSG_NOSIGNAL);
        if (result < 0) {
            int err = errno;
            // Mark the port as broken so upper layers can reconnect
            m_is_open = false;
            if (m_sockfd >= 0) {
                ::close(m_sockfd);
                m_sockfd = -1;
            }
            throw PSUException(std::string("TCP send failed: ") + std::strerror(err));
        }

        return static_cast<size_t>(result);
    }

    std::string read(int timeout_ms = 1000) override {
        if (!m_is_open) {
            throw PSUException("TCP port is not open");
        }

        std::string result;
        char buffer[256];
        auto start = std::chrono::steady_clock::now();

        while (true) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

            if (elapsed >= timeout_ms) {
                break;
            }

            // Use poll to check for data
            struct pollfd pfd;
            pfd.fd = m_sockfd;
            pfd.events = POLLIN;

            int poll_result = poll(&pfd, 1, 10); // 10ms timeout
            if (poll_result > 0 && (pfd.revents & POLLIN)) {
                ssize_t bytes_read = recv(m_sockfd, buffer, sizeof(buffer) - 1, 0);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    result += buffer;
                    if (result.find('\n') != std::string::npos) {
                        break;
                    }
                } else if (bytes_read == 0) {
                    // Connection closed cleanly by peer
                    m_is_open = false;
                    if (m_sockfd >= 0) { ::close(m_sockfd); m_sockfd = -1; }
                    throw PSUException("TCP connection closed by remote host");
                } else {
                    // recv error (ECONNRESET, ETIMEDOUT, etc.) - socket is dead
                    int err = errno;
                    if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
                        m_is_open = false;
                        if (m_sockfd >= 0) { ::close(m_sockfd); m_sockfd = -1; }
                        throw PSUException(std::string("TCP recv failed: ") + std::strerror(err));
                    }
                }
            } else if (poll_result < 0 && errno != EINTR) {
                int err = errno;
                m_is_open = false;
                if (m_sockfd >= 0) { ::close(m_sockfd); m_sockfd = -1; }
                throw PSUException(std::string("TCP poll failed: ") + std::strerror(err));
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return result;
    }

    bool isOpen() const override {
        return m_is_open;
    }

    void close() override {
        if (!m_is_open) {
            return;
        }

        if (m_sockfd >= 0) {
            ::close(m_sockfd);
            m_sockfd = -1;
        }
        m_is_open = false;
    }

private:
    PSUConfig m_config;
    bool m_is_open;
    int m_sockfd;
};

// ==================== TDKLambdaPSU Base Class Implementation ====================

TDKLambdaPSU::TDKLambdaPSU(const PSUConfig& config, const PSUModelSpec& spec)
    : m_config(config),
      m_model_spec(spec),
      m_connected(false),
      m_output_enabled(false),
      m_error_handler(nullptr) {
    createDefaultCommPort();
}

TDKLambdaPSU::TDKLambdaPSU(std::unique_ptr<ICommunication> comm_port, const PSUConfig& config, const PSUModelSpec& spec)
    : m_comm_port(std::move(comm_port)),
      m_config(config),
      m_model_spec(spec),
      m_connected(false),
      m_output_enabled(false),
      m_error_handler(nullptr) {
}

TDKLambdaPSU::~TDKLambdaPSU() {
    try {
        if (m_connected) {
            enableOutput(false);
            disconnect();
        }
    } catch (...) {
        // Suppress exceptions in destructor
    }
}

TDKLambdaPSU::TDKLambdaPSU(TDKLambdaPSU&& other) noexcept
    : m_comm_port(std::move(other.m_comm_port)),
      m_config(std::move(other.m_config)),
      m_model_spec(std::move(other.m_model_spec)),
      m_connected(other.m_connected),
      m_output_enabled(other.m_output_enabled),
      m_error_handler(std::move(other.m_error_handler)) {
    other.m_connected = false;
}

TDKLambdaPSU& TDKLambdaPSU::operator=(TDKLambdaPSU&& other) noexcept {
    if (this != &other) {
        m_comm_port = std::move(other.m_comm_port);
        m_config = std::move(other.m_config);
        m_model_spec = std::move(other.m_model_spec);
        m_connected = other.m_connected;
        m_output_enabled = other.m_output_enabled;
        m_error_handler = std::move(other.m_error_handler);
        other.m_connected = false;
    }
    return *this;
}

void TDKLambdaPSU::createDefaultCommPort() {
    m_comm_port = std::make_unique<TcpPort>(m_config);
}

void TDKLambdaPSU::connect() {
    if (m_connected) {
        return;
    }

    try {
        // Open the communication port
        m_comm_port->open();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::string id = getIdentification();
        if (id.empty()) {
            throw PSUException("Failed to communicate with device");
        }

        m_connected = true;

        reset();
        clearProtection();

    } catch (const std::exception& e) {
        disconnect();
        throw PSUException("Connection failed: " + std::string(e.what()));
    }
}

void TDKLambdaPSU::disconnect() {
    if (m_comm_port) {
        m_comm_port->close();
    }
    m_connected = false;
}

bool TDKLambdaPSU::isConnected() const {
    return m_connected && m_comm_port && m_comm_port->isOpen();
}

void TDKLambdaPSU::enableOutput(bool enable) {
    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::string command = enable ? "OUTP ON\n" : "OUTP OFF\n";
    m_comm_port->write(command);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    m_output_enabled = enable;
}

bool TDKLambdaPSU::isOutputEnabled() const {
    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::string response = sendQuery("OUTP?");
    return (trim(response) == "1" || trim(response) == "ON");
}

void TDKLambdaPSU::reset() {
    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    m_comm_port->write("*RST\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    m_output_enabled = false;
}

void TDKLambdaPSU::setVoltage(double voltage, int channel) {
    (void)channel; // Single-channel device

    validateVoltage(voltage);

    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::ostringstream oss;
    oss.precision(3);
    oss << std::fixed << "VOLT " << voltage << "\n";

    m_comm_port->write(oss.str());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

double TDKLambdaPSU::getVoltage(int channel) const {
    (void)channel; // Single-channel device

    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::string response = sendQuery("VOLT?");
    return parseNumericResponse(response);
}

double TDKLambdaPSU::measureVoltage(int channel) const {
    (void)channel; // Single-channel device

    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::string response = sendQuery("MEAS:VOLT?");
    return parseNumericResponse(response);
}

void TDKLambdaPSU::setVoltageWithRamp(double voltage, double ramp_rate) {
    validateVoltage(voltage);

    if (ramp_rate <= 0) {
        throw PSUException("Ramp rate must be positive");
    }

    double current_voltage = getVoltage();
    double difference = std::abs(voltage - current_voltage);
    double steps = difference / ramp_rate * 10;
    double step_voltage = (voltage - current_voltage) / steps;

    for (int i = 0; i < static_cast<int>(steps); ++i) {
        current_voltage += step_voltage;
        setVoltage(current_voltage);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    setVoltage(voltage);
}

void TDKLambdaPSU::setCurrent(double current, int channel) {
    (void)channel; // Single-channel device

    validateCurrent(current);

    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::ostringstream oss;
    oss.precision(3);
    oss << std::fixed << "CURR " << current << "\n";

    m_comm_port->write(oss.str());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

double TDKLambdaPSU::getCurrent(int channel) const {
    (void)channel; // Single-channel device

    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::string response = sendQuery("CURR?");
    return parseNumericResponse(response);
}

double TDKLambdaPSU::measureCurrent(int channel) const {
    (void)channel; // Single-channel device

    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::string response = sendQuery("MEAS:CURR?");
    return parseNumericResponse(response);
}

void TDKLambdaPSU::setCurrentWithRamp(double current, double ramp_rate) {
    validateCurrent(current);

    if (ramp_rate <= 0) {
        throw PSUException("Ramp rate must be positive");
    }

    double current_current = getCurrent();
    double difference = std::abs(current - current_current);
    double steps = difference / ramp_rate * 10;
    double step_current = (current - current_current) / steps;

    for (int i = 0; i < static_cast<int>(steps); ++i) {
        current_current += step_current;
        setCurrent(current_current);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    setCurrent(current);
}

double TDKLambdaPSU::measurePower(int channel) const {
    (void)channel; // Single-channel device

    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    double voltage = measureVoltage();
    double current = measureCurrent();
    return voltage * current;
}

void TDKLambdaPSU::setOverVoltageProtection(double voltage, int channel) {
    (void)channel; // Single-channel device

    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::ostringstream oss;
    oss.precision(3);
    oss << std::fixed << "VOLT:PROT " << voltage << "\n";

    m_comm_port->write(oss.str());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

double TDKLambdaPSU::getOverVoltageProtection() const {
    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::string response = sendQuery("VOLT:PROT?");
    return parseNumericResponse(response);
}

void TDKLambdaPSU::clearProtection() {
    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    m_comm_port->write("*CLS\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

std::string TDKLambdaPSU::getIdentification() const {
    if (!isConnected() && !m_comm_port->isOpen()) {
        throw PSUException("Not connected to device");
    }

    return sendQuery("*IDN?");
}

PowerSupplyStatus TDKLambdaPSU::getStatus(int channel) const {
    (void)channel; // Single-channel device

    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    PowerSupplyStatus status;

    try {
        status.output_enabled = isOutputEnabled();

        std::string stat_query = sendQuery("STAT:QUES?");
        int stat_value = static_cast<int>(parseNumericResponse(stat_query));

        status.over_voltage_protection = (stat_value & 0x01) != 0;
        status.over_current_protection = (stat_value & 0x02) != 0;
        status.over_temperature = (stat_value & 0x10) != 0;

    } catch (const std::exception& e) {
        if (m_error_handler) {
            m_error_handler("Failed to get complete status: " + std::string(e.what()));
        }
    }

    return status;
}

std::string TDKLambdaPSU::checkError() const {
    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    return sendQuery("SYST:ERR?");
}

PowerSupplyCapabilities TDKLambdaPSU::getCapabilities() const {
    PowerSupplyCapabilities caps;
    caps.max_voltage = m_model_spec.max_voltage;
    caps.max_current = m_model_spec.max_current;
    caps.max_power = m_model_spec.max_power;
    caps.number_of_channels = 1;  // TDK Lambda Genesys is single channel
    caps.supports_remote_sensing = m_model_spec.supports_remote_sensing;
    caps.supports_ovp = true;
    caps.supports_ocp = true;
    caps.supports_opp = false;
    caps.supports_sequencing = false;
    return caps;
}

Vendor TDKLambdaPSU::getVendor() const {
    return Vendor::TDK_LAMBDA;
}

std::string TDKLambdaPSU::getModel() const {
    return m_model_spec.model_name;
}

std::string TDKLambdaPSU::sendCommand(const std::string& command) {
    if (!isConnected()) {
        throw PSUException("Not connected to device");
    }

    std::string cmd = command;
    if (cmd.back() != '\n') {
        cmd += '\n';
    }

    // First attempt; on transport failure try one transparent reconnect+retry.
    try {
        m_comm_port->write(cmd);
    } catch (const PSUException& first) {
        if (!reconnect()) {
            throw;
        }
        try {
            m_comm_port->write(cmd);
        } catch (const PSUException&) {
            // Fall through with the original error - caller sees a single failure
            throw first;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    return "OK";
}

std::string TDKLambdaPSU::sendQuery(const std::string& query) const {
    if (!isConnected() && !m_comm_port->isOpen()) {
        throw PSUException("Not connected to device");
    }

    std::string cmd = query;
    if (cmd.back() != '\n') {
        cmd += '\n';
    }

    // First attempt; on transport failure try one transparent reconnect+retry.
    // sendQuery is const by contract but reconnect()/write() mutate transport
    // state - that's an internal implementation detail, not observable state.
    auto* self = const_cast<TDKLambdaPSU*>(this);
    try {
        m_comm_port->write(cmd);
    } catch (const PSUException& first) {
        if (!self->reconnect()) {
            throw;
        }
        try {
            m_comm_port->write(cmd);
        } catch (const PSUException&) {
            throw first;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string response = m_comm_port->read(m_config.timeout_ms);
    return trim(response);
}

bool TDKLambdaPSU::ping() {
    try {
        std::string id = sendQuery("*IDN?");
        return !id.empty();
    } catch (...) {
        return false;
    }
}

bool TDKLambdaPSU::reconnect() {
    // Tear down current transport (idempotent).
    try {
        if (m_comm_port) {
            m_comm_port->close();
        }
    } catch (...) {
        // Ignore close errors - we're about to open a fresh socket.
    }
    m_connected = false;

    // Re-open and handshake. A fresh TcpPort was created in the constructor;
    // open() on it creates a new socket, so this works even after close().
    try {
        m_comm_port->open();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Validate with *IDN? - go directly through the transport to avoid
        // the reconnect wrapping in sendQuery().
        std::string cmd = "*IDN?\n";
        m_comm_port->write(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::string resp = m_comm_port->read(m_config.timeout_ms);
        if (trim(resp).empty()) {
            return false;
        }

        m_connected = true;
        return true;
    } catch (const std::exception&) {
        m_connected = false;
        return false;
    }
}

void TDKLambdaPSU::setErrorHandler(std::function<void(const std::string&)> handler) {
    m_error_handler = handler;
}

void TDKLambdaPSU::validateVoltage(double voltage) const {
    if (voltage < 0) {
        throw PSUException("Voltage cannot be negative");
    }
    if (voltage > m_model_spec.max_voltage) {
        throw PSUException("Voltage " + std::to_string(voltage) +
                         "V exceeds maximum limit of " + std::to_string(m_model_spec.max_voltage) + "V");
    }
}

void TDKLambdaPSU::validateCurrent(double current) const {
    if (current < 0) {
        throw PSUException("Current cannot be negative");
    }
    if (current > m_model_spec.max_current) {
        throw PSUException("Current " + std::to_string(current) +
                         "A exceeds maximum limit of " + std::to_string(m_model_spec.max_current) + "A");
    }
}

double TDKLambdaPSU::parseNumericResponse(const std::string& response) const {
    try {
        std::string cleaned = trim(response);
        return std::stod(cleaned);
    } catch (const std::exception& e) {
        throw PSUException("Failed to parse numeric response: '" + response + "'");
    }
}

std::string TDKLambdaPSU::trim(const std::string& str) const {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}

void TDKLambdaPSU::defaultErrorHandler(const std::string& error) {
    std::cerr << "TDK Lambda PSU Error: " << error << std::endl;
}

// ==================== TDKLambdaPSU30 Implementation ====================

PSUModelSpec TDKLambdaPSU30::getDefaultSpec() {
    return PSUModelSpec("GENESYS+ 30-56", 30.0, 56.0, 1680.0, false);
}

TDKLambdaPSU30::TDKLambdaPSU30(const PSUConfig& config)
    : TDKLambdaPSU(config, getDefaultSpec()) {
}

TDKLambdaPSU30::TDKLambdaPSU30(std::unique_ptr<ICommunication> comm_port, const PSUConfig& config)
    : TDKLambdaPSU(std::move(comm_port), config, getDefaultSpec()) {
}

// ==================== TDKLambdaPSU300 Implementation ====================

PSUModelSpec TDKLambdaPSU300::getDefaultSpec() {
    return PSUModelSpec("GENESYS+ 300-5.6", 300.0, 5.6, 1680.0, false);
}

TDKLambdaPSU300::TDKLambdaPSU300(const PSUConfig& config)
    : TDKLambdaPSU(config, getDefaultSpec()) {
}

TDKLambdaPSU300::TDKLambdaPSU300(std::unique_ptr<ICommunication> comm_port, const PSUConfig& config)
    : TDKLambdaPSU(std::move(comm_port), config, getDefaultSpec()) {
}

// ==================== Factory Functions ====================

std::unique_ptr<TDKLambdaPSU30> createPSU30(const std::string& ip_address, int tcp_port) {
    PSUConfig config;
    config.ip_address = ip_address;
    config.tcp_port = tcp_port;
    return std::make_unique<TDKLambdaPSU30>(config);
}

std::unique_ptr<TDKLambdaPSU300> createPSU300(const std::string& ip_address, int tcp_port) {
    PSUConfig config;
    config.ip_address = ip_address;
    config.tcp_port = tcp_port;
    return std::make_unique<TDKLambdaPSU300>(config);
}

std::unique_ptr<TDKLambdaPSU> createPSU(const std::string& ip_address, const PSUModelSpec& spec, int tcp_port) {
    PSUConfig config;
    config.ip_address = ip_address;
    config.tcp_port = tcp_port;
    return std::make_unique<TDKLambdaPSU>(config, spec);
}

} // namespace TDKLambda
