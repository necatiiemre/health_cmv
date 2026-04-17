/**
 * @file tdk_lambda_psu.h
 * @brief Generic TDK Lambda Power Supply Controller Base Class
 * @version 1.0.0
 * @date 2025-11-26
 *
 * Professional C++ interface for controlling TDK Lambda series power supplies.
 * This is a generic base class from which specific models (PSU30, PSU300, etc.)
 * can be derived.
 *
 * Supports voltage and current control via Ethernet (TCP/IP) communication using SCPI commands.
 *
 * @author Professional Power Supply Control Library
 * @copyright MIT License
 */

#ifndef TDK_LAMBDA_PSU_H
#define TDK_LAMBDA_PSU_H

#include "PowerSupplyInterface.h"
#include <string>
#include <memory>
#include <stdexcept>
#include <functional>
#include <vector>
#include <iomanip>

namespace TDKLambda {

// Import types from generic PowerSupply namespace
using PowerSupply::PowerSupplyStatus;
using PowerSupply::PowerSupplyCapabilities;
using PowerSupply::Vendor;

// Type alias for backward compatibility
using ConnectionType = PowerSupply::ConnectionType;

/**
 * @brief Custom exception class for TDK Lambda PSU errors
 */
class PSUException : public std::runtime_error {
public:
    explicit PSUException(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * @brief Communication interface for abstraction
 *
 * This allows for dependency injection and easier testing.
 * Supports Ethernet (TCP/IP) communication.
 */
class ICommunication {
public:
    virtual ~ICommunication() = default;

    /**
     * @brief Write data to communication port
     * @param data Data to write
     * @return Number of bytes written
     */
    virtual size_t write(const std::string& data) = 0;

    /**
     * @brief Read data from communication port
     * @param timeout_ms Timeout in milliseconds
     * @return Read data
     */
    virtual std::string read(int timeout_ms = 1000) = 0;

    /**
     * @brief Check if port is open
     * @return true if open, false otherwise
     */
    virtual bool isOpen() const = 0;

    /**
     * @brief Close the port
     */
    virtual void close() = 0;

    /**
     * @brief Open the port
     */
    virtual void open() = 0;
};

/**
 * @brief Configuration structure for TDK Lambda PSU
 */
struct PSUConfig {
    // Ethernet settings
    std::string ip_address;     ///< IP address (e.g., "192.168.1.100")
    int tcp_port;               ///< TCP port (default: 8003 for TDK Lambda)

    // Common settings
    int timeout_ms;             ///< Communication timeout in milliseconds

    PSUConfig()
        : ip_address(""),
          tcp_port(8003),  // TDK Lambda default TCP port
          timeout_ms(1000) {}
};

/**
 * @brief Model specifications for TDK Lambda power supplies
 */
struct PSUModelSpec {
    std::string model_name;      ///< Model name (e.g., "GENESYS+ 30-56")
    double max_voltage;          ///< Maximum voltage rating
    double max_current;          ///< Maximum current rating
    double max_power;            ///< Maximum power rating
    bool supports_remote_sensing; ///< Remote sensing support

    PSUModelSpec()
        : model_name(""),
          max_voltage(0),
          max_current(0),
          max_power(0),
          supports_remote_sensing(false) {}

    PSUModelSpec(const std::string& name, double voltage, double current, double power, bool remote_sensing = false)
        : model_name(name),
          max_voltage(voltage),
          max_current(current),
          max_power(power),
          supports_remote_sensing(remote_sensing) {}
};

/**
 * @brief Generic TDK Lambda Power Supply Controller Base Class
 *
 * This class provides a clean, modern C++ interface for controlling
 * TDK Lambda series programmable power supplies via Ethernet.
 *
 * Implements the generic IPowerSupply interface while providing
 * TDK Lambda specific features and optimizations.
 *
 * Features:
 * - RAII-compliant resource management
 * - Exception-based error handling
 * - Ethernet (TCP/IP) communication
 * - Thread-safe operations (when used with proper locking)
 * - Full SCPI command support
 * - Generic PowerSupply interface compliance
 * - Extensible through inheritance for specific models
 *
 * Example usage:
 * @code
 * // Use specific model classes
 * auto psu30 = TDKLambda::createPSU30("192.168.1.100", 8003);
 * psu30->connect();
 * psu30->setVoltage(12.5);
 * psu30->setCurrent(2.0);
 * psu30->enableOutput(true);
 *
 * auto psu300 = TDKLambda::createPSU300("192.168.1.101", 8003);
 * psu300->connect();
 * psu300->setVoltage(250.0);
 * @endcode
 */
class TDKLambdaPSU : public PowerSupply::IPowerSupply {
public:
    /**
     * @brief Construct a new TDKLambdaPSU object
     * @param config Configuration parameters
     * @param spec Model specifications
     */
    TDKLambdaPSU(const PSUConfig& config, const PSUModelSpec& spec);

    /**
     * @brief Construct with custom communication implementation
     * @param commPort Custom communication port implementation
     * @param config Configuration parameters
     * @param spec Model specifications
     */
    TDKLambdaPSU(std::unique_ptr<ICommunication> comm_port, const PSUConfig& config, const PSUModelSpec& spec);

    /**
     * @brief Destructor - ensures proper cleanup
     */
    virtual ~TDKLambdaPSU();

    // Delete copy operations (non-copyable due to unique resource)
    TDKLambdaPSU(const TDKLambdaPSU&) = delete;
    TDKLambdaPSU& operator=(const TDKLambdaPSU&) = delete;

    // Allow move operations
    TDKLambdaPSU(TDKLambdaPSU&&) noexcept;
    TDKLambdaPSU& operator=(TDKLambdaPSU&&) noexcept;

    /**
     * @brief Connect to the power supply
     * @throws PSUException if connection fails
     */
    void connect() override;

    /**
     * @brief Disconnect from the power supply
     */
    void disconnect() override;

    /**
     * @brief Check if connected
     * @return true if connected, false otherwise
     */
    bool isConnected() const override;

    // ==================== Basic Control ====================

    /**
     * @brief Enable or disable output
     * @param enable true to enable, false to disable
     * @throws PSUException on communication error
     */
    void enableOutput(bool enable) override;

    /**
     * @brief Get output state
     * @return true if enabled, false if disabled
     * @throws PSUException on communication error
     */
    bool isOutputEnabled() const override;

    /**
     * @brief Reset the power supply to default state
     * @throws PSUException on communication error
     */
    void reset() override;

    // ==================== Voltage Control ====================

    /**
     * @brief Set output voltage
     * @param voltage Voltage in volts
     * @param channel Channel number (ignored for single-channel models, default: 1)
     * @throws PSUException if voltage is out of range or communication fails
     */
    void setVoltage(double voltage, int channel = 1) override;

    /**
     * @brief Get set voltage value
     * @param channel Channel number (ignored for single-channel models, default: 1)
     * @return Voltage in volts
     * @throws PSUException on communication error
     */
    double getVoltage(int channel = 1) const override;

    /**
     * @brief Measure actual output voltage
     * @param channel Channel number (ignored for single-channel models, default: 1)
     * @return Measured voltage in volts
     * @throws PSUException on communication error
     */
    double measureVoltage(int channel = 1) const override;

    /**
     * @brief Set voltage with ramp rate
     * @param voltage Target voltage in volts
     * @param rampRate Ramp rate in V/s
     * @throws PSUException on error
     */
    virtual void setVoltageWithRamp(double voltage, double rampRate);

    // ==================== Current Control ====================

    /**
     * @brief Set current limit
     * @param current Current in amperes
     * @param channel Channel number (ignored for single-channel models, default: 1)
     * @throws PSUException if current is out of range or communication fails
     */
    void setCurrent(double current, int channel = 1) override;

    /**
     * @brief Get set current limit
     * @param channel Channel number (ignored for single-channel models, default: 1)
     * @return Current in amperes
     * @throws PSUException on communication error
     */
    double getCurrent(int channel = 1) const override;

    /**
     * @brief Measure actual output current
     * @param channel Channel number (ignored for single-channel models, default: 1)
     * @return Measured current in amperes
     * @throws PSUException on communication error
     */
    double measureCurrent(int channel = 1) const override;

    /**
     * @brief Set current with ramp rate
     * @param current Target current in amperes
     * @param rampRate Ramp rate in A/s
     * @throws PSUException on error
     */
    virtual void setCurrentWithRamp(double current, double rampRate);

    // ==================== Power and Limits ====================

    /**
     * @brief Measure output power
     * @param channel Channel number (ignored for single-channel models, default: 1)
     * @return Power in watts
     * @throws PSUException on communication error
     */
    double measurePower(int channel = 1) const override;

    /**
     * @brief Set over-voltage protection level
     * @param voltage OVP level in volts
     * @param channel Channel number (ignored for single-channel models, default: 1)
     * @throws PSUException on error
     */
    void setOverVoltageProtection(double voltage, int channel = 1) override;

    /**
     * @brief Get over-voltage protection level
     * @return OVP level in volts
     * @throws PSUException on error
     */
    double getOverVoltageProtection() const;

    /**
     * @brief Clear protection faults
     * @throws PSUException on error
     */
    void clearProtection() override;

    // ==================== Status and Information ====================

    /**
     * @brief Get device identification string
     * @return ID string (manufacturer, model, serial, firmware)
     * @throws PSUException on communication error
     */
    std::string getIdentification() const override;

    /**
     * @brief Get detailed status
     * @param channel Channel number (ignored for single-channel models, default: 1)
     * @return PowerSupplyStatus structure
     * @throws PSUException on communication error
     */
    PowerSupplyStatus getStatus(int channel = 1) const override;

    /**
     * @brief Get power supply capabilities
     * @return PowerSupplyCapabilities structure
     */
    PowerSupplyCapabilities getCapabilities() const override;

    /**
     * @brief Get vendor
     * @return Vendor enumeration
     */
    Vendor getVendor() const override;

    /**
     * @brief Get model name
     * @return Model string
     */
    std::string getModel() const override;

    /**
     * @brief Check for errors in error queue
     * @return Error message, empty if no errors
     * @throws PSUException on communication error
     */
    std::string checkError() const;

    /**
     * @brief Get maximum voltage rating
     * @return Maximum voltage in volts
     */
    double getMaxVoltage() const { return m_model_spec.max_voltage; }

    /**
     * @brief Get maximum current rating
     * @return Maximum current in amperes
     */
    double getMaxCurrent() const { return m_model_spec.max_current; }

    /**
     * @brief Get model specifications
     * @return PSUModelSpec structure
     */
    const PSUModelSpec& getModelSpec() const { return m_model_spec; }

    // ==================== Advanced Features ====================

    /**
     * @brief Send raw SCPI command
     * @param command SCPI command string
     * @return Response from device
     * @throws PSUException on communication error
     */
    std::string sendCommand(const std::string& command) override;

    /**
     * @brief Send raw SCPI query
     * @param query SCPI query string
     * @return Response from device
     * @throws PSUException on communication error
     */
    std::string sendQuery(const std::string& query) const override;

    /**
     * @brief Set custom error handler callback
     * @param handler Error handler function
     */
    void setErrorHandler(std::function<void(const std::string&)> handler);

    /**
     * @brief Lightweight health probe (sends "*IDN?" and expects any reply)
     * @return true if the PSU responds, false otherwise
     *
     * Never throws. Intended for periodic heartbeating during long idle
     * periods and for detecting stale connections before they are exercised.
     */
    bool ping();

    /**
     * @brief Tear down and re-establish the underlying transport
     * @return true on successful reconnect + *IDN? handshake
     *
     * Idempotent: safe to call on a PSU that is already disconnected.
     * Used as an automatic recovery step inside sendQuery()/sendCommand()
     * when the socket is detected as dead.
     */
    bool reconnect();

protected:
    std::unique_ptr<ICommunication> m_comm_port;
    PSUConfig m_config;
    PSUModelSpec m_model_spec;
    bool m_connected;
    mutable bool m_output_enabled;

    // Error handling
    std::function<void(const std::string&)> m_error_handler;

    /**
     * @brief Validate voltage is within limits
     * @param voltage Voltage to validate
     * @throws PSUException if out of range
     */
    virtual void validateVoltage(double voltage) const;

    /**
     * @brief Validate current is within limits
     * @param current Current to validate
     * @throws PSUException if out of range
     */
    virtual void validateCurrent(double current) const;

    /**
     * @brief Parse numeric response from device
     * @param response Response string
     * @return Parsed numeric value
     * @throws PSUException if parsing fails
     */
    double parseNumericResponse(const std::string& response) const;

    /**
     * @brief Trim whitespace from string
     * @param str String to trim
     * @return Trimmed string
     */
    std::string trim(const std::string& str) const;

    /**
     * @brief Default error handler
     * @param error Error message
     */
    void defaultErrorHandler(const std::string& error);

    /**
     * @brief Create default TCP communication port
     */
    void createDefaultCommPort();
};

// ==================== Specific Model Classes ====================

/**
 * @brief TDK Lambda PSU30 - 30V Model (e.g., GENESYS+ 30-56)
 *
 * Specifications:
 * - Max Voltage: 30V
 * - Max Current: 56A
 * - Max Power: 1680W
 */
class TDKLambdaPSU30 : public TDKLambdaPSU {
public:
    /**
     * @brief Construct PSU30 with configuration
     * @param config Configuration parameters
     */
    explicit TDKLambdaPSU30(const PSUConfig& config);

    /**
     * @brief Construct PSU30 with custom communication
     * @param commPort Custom communication port
     * @param config Configuration parameters
     */
    TDKLambdaPSU30(std::unique_ptr<ICommunication> comm_port, const PSUConfig& config);

    /**
     * @brief Get PSU30 default specifications
     * @return Model specifications for PSU30
     */
    static PSUModelSpec getDefaultSpec();
};

/**
 * @brief TDK Lambda PSU300 - 300V Model (e.g., GENESYS+ 300-5.6)
 *
 * Specifications:
 * - Max Voltage: 300V
 * - Max Current: 5.6A
 * - Max Power: 1680W
 */
class TDKLambdaPSU300 : public TDKLambdaPSU {
public:
    /**
     * @brief Construct PSU300 with configuration
     * @param config Configuration parameters
     */
    explicit TDKLambdaPSU300(const PSUConfig& config);

    /**
     * @brief Construct PSU300 with custom communication
     * @param commPort Custom communication port
     * @param config Configuration parameters
     */
    TDKLambdaPSU300(std::unique_ptr<ICommunication> comm_port, const PSUConfig& config);

    /**
     * @brief Get PSU300 default specifications
     * @return Model specifications for PSU300
     */
    static PSUModelSpec getDefaultSpec();
};

// ==================== Factory Functions ====================

/**
 * @brief Factory function to create TDKLambdaPSU30 instance with Ethernet
 * @param ipAddress IP address (e.g., "192.168.1.100")
 * @param tcpPort TCP port (default: 8003 for TDK Lambda)
 * @return Unique pointer to TDKLambdaPSU30 instance
 */
std::unique_ptr<TDKLambdaPSU30> createPSU30(const std::string& ip_address, int tcp_port = 8003);

/**
 * @brief Factory function to create TDKLambdaPSU300 instance with Ethernet
 * @param ip_address IP address (e.g., "192.168.1.100")
 * @param tcp_port TCP port (default: 8003 for TDK Lambda)
 * @return Unique pointer to TDKLambdaPSU300 instance
 */
std::unique_ptr<TDKLambdaPSU300> createPSU300(const std::string& ip_address, int tcp_port = 8003);

/**
 * @brief Factory function to create generic TDKLambdaPSU with custom specifications
 * @param ip_address IP address
 * @param spec Model specifications
 * @param tcp_port TCP port (default: 8003)
 * @return Unique pointer to TDKLambdaPSU instance
 */
std::unique_ptr<TDKLambdaPSU> createPSU(const std::string& ip_address, const PSUModelSpec& spec, int tcp_port = 8003);

// Backward compatibility aliases
using G30Exception = PSUException;
using G30Config = PSUConfig;

} // namespace TDKLambda

#endif // TDK_LAMBDA_PSU_H
