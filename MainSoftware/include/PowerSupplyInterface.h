/**
 * @file power_supply_interface.h
 * @brief Generic Power Supply Interface
 * @version 1.0.0
 * @date 2025-11-24
 *
 * Generic interface for programmable power supplies.
 * Supports multiple vendors/models through common abstraction.
 *
 * @author Professional Power Supply Control Library
 * @copyright MIT License
 */

#ifndef POWER_SUPPLY_INTERFACE_H
#define POWER_SUPPLY_INTERFACE_H

#include <string>
#include <memory>
#include <stdexcept>
#include <functional>

namespace PowerSupply {

/**
 * @brief Power supply vendor enumeration
 */
enum class Vendor {
    TDK_LAMBDA,     ///< TDK Lambda
    KEYSIGHT,       ///< Keysight / Agilent
    ROHDE_SCHWARZ,  ///< Rohde & Schwarz
    RIGOL,          ///< Rigol
    SIGLENT,        ///< Siglent
    TTI,            ///< Thurlby Thandar Instruments
    BK_PRECISION,   ///< B&K Precision
    TENMA,          ///< Tenma
    CUSTOM          ///< Custom/Other vendors
};

/**
 * @brief Connection type enumeration
 */
enum class ConnectionType {
    SERIAL,      ///< Serial port connection (RS232/USB)
    ETHERNET,    ///< Ethernet TCP/IP connection
    USB,         ///< Direct USB (USBTMC)
    GPIB         ///< GPIB/IEEE-488
};

/**
 * @brief Power supply status information
 */
struct PowerSupplyStatus {
    bool output_enabled;          ///< Output state
    bool over_voltage_protection; ///< OVP triggered
    bool over_current_protection; ///< OCP triggered
    bool over_power_protection;   ///< OPP triggered
    bool over_temperature;        ///< Over temperature
    bool remote_sensing;          ///< Remote sensing enabled
    bool cc_mode;                 ///< Constant current mode
    bool cv_mode;                 ///< Constant voltage mode

    PowerSupplyStatus()
        : output_enabled(false),
          over_voltage_protection(false),
          over_current_protection(false),
          over_power_protection(false),
          over_temperature(false),
          remote_sensing(false),
          cc_mode(false),
          cv_mode(false) {}
};

/**
 * @brief Power supply capabilities
 */
struct PowerSupplyCapabilities {
    double max_voltage;           ///< Maximum output voltage
    double max_current;           ///< Maximum output current
    double max_power;             ///< Maximum output power
    int number_of_channels;       ///< Number of output channels
    bool supports_remote_sensing; ///< Supports remote sensing
    bool supports_ovp;            ///< Supports OVP
    bool supports_ocp;            ///< Supports OCP
    bool supports_opp;            ///< Supports OPP
    bool supports_sequencing;     ///< Supports voltage/current sequencing

    PowerSupplyCapabilities()
        : max_voltage(0),
          max_current(0),
          max_power(0),
          number_of_channels(1),
          supports_remote_sensing(false),
          supports_ovp(false),
          supports_ocp(false),
          supports_opp(false),
          supports_sequencing(false) {}
};

/**
 * @brief Generic Power Supply Interface
 *
 * Abstract base class for all programmable power supplies.
 * Defines common operations that all power supplies should support.
 */
class IPowerSupply {
public:
    virtual ~IPowerSupply() = default;

    // ==================== Connection Management ====================

    /**
     * @brief Connect to the power supply
     * @throws std::runtime_error if connection fails
     */
    virtual void connect() = 0;

    /**
     * @brief Disconnect from the power supply
     */
    virtual void disconnect() = 0;

    /**
     * @brief Check if connected
     * @return true if connected, false otherwise
     */
    virtual bool isConnected() const = 0;

    // ==================== Basic Control ====================

    /**
     * @brief Enable or disable output
     * @param enable true to enable, false to disable
     * @throws std::runtime_error on error
     */
    virtual void enableOutput(bool enable) = 0;

    /**
     * @brief Get output state
     * @return true if enabled, false if disabled
     * @throws std::runtime_error on error
     */
    virtual bool isOutputEnabled() const = 0;

    /**
     * @brief Reset the power supply to default state
     * @throws std::runtime_error on error
     */
    virtual void reset() = 0;

    // ==================== Voltage Control ====================

    /**
     * @brief Set output voltage
     * @param voltage Voltage in volts
     * @param channel Channel number (default: 1)
     * @throws std::runtime_error if out of range or communication fails
     */
    virtual void setVoltage(double voltage, int channel = 1) = 0;

    /**
     * @brief Get set voltage value
     * @param channel Channel number (default: 1)
     * @return Voltage in volts
     * @throws std::runtime_error on error
     */
    virtual double getVoltage(int channel = 1) const = 0;

    /**
     * @brief Measure actual output voltage
     * @param channel Channel number (default: 1)
     * @return Measured voltage in volts
     * @throws std::runtime_error on error
     */
    virtual double measureVoltage(int channel = 1) const = 0;

    // ==================== Current Control ====================

    /**
     * @brief Set current limit
     * @param current Current in amperes
     * @param channel Channel number (default: 1)
     * @throws std::runtime_error if out of range or communication fails
     */
    virtual void setCurrent(double current, int channel = 1) = 0;

    /**
     * @brief Get set current limit
     * @param channel Channel number (default: 1)
     * @return Current in amperes
     * @throws std::runtime_error on error
     */
    virtual double getCurrent(int channel = 1) const = 0;

    /**
     * @brief Measure actual output current
     * @param channel Channel number (default: 1)
     * @return Measured current in amperes
     * @throws std::runtime_error on error
     */
    virtual double measureCurrent(int channel = 1) const = 0;

    // ==================== Power Measurement ====================

    /**
     * @brief Measure output power
     * @param channel Channel number (default: 1)
     * @return Power in watts
     * @throws std::runtime_error on error
     */
    virtual double measurePower(int channel = 1) const = 0;

    // ==================== Status and Information ====================

    /**
     * @brief Get device identification string
     * @return ID string (vendor, model, serial, firmware)
     * @throws std::runtime_error on error
     */
    virtual std::string getIdentification() const = 0;

    /**
     * @brief Get detailed status
     * @param channel Channel number (default: 1)
     * @return PowerSupplyStatus structure
     * @throws std::runtime_error on error
     */
    virtual PowerSupplyStatus getStatus(int channel = 1) const = 0;

    /**
     * @brief Get power supply capabilities
     * @return PowerSupplyCapabilities structure
     */
    virtual PowerSupplyCapabilities getCapabilities() const = 0;

    /**
     * @brief Get vendor
     * @return Vendor enumeration
     */
    virtual Vendor getVendor() const = 0;

    /**
     * @brief Get model name
     * @return Model string
     */
    virtual std::string getModel() const = 0;

    // ==================== Optional Advanced Features ====================

    /**
     * @brief Set over-voltage protection level (if supported)
     * @param voltage OVP level in volts
     * @param channel Channel number (default: 1)
     * @throws std::runtime_error if not supported or on error
     */
    virtual void setOverVoltageProtection(double voltage, int channel = 1) {
        (void)voltage; (void)channel;
        throw std::runtime_error("OVP not supported by this power supply");
    }

    /**
     * @brief Clear protection faults (if supported)
     * @throws std::runtime_error if not supported or on error
     */
    virtual void clearProtection() {
        throw std::runtime_error("Protection clear not supported by this power supply");
    }

    /**
     * @brief Send raw command (vendor-specific)
     * @param command Command string
     * @return Response (may be empty for commands)
     * @throws std::runtime_error on error
     */
    virtual std::string sendCommand(const std::string& command) = 0;

    /**
     * @brief Send raw query (vendor-specific)
     * @param query Query string
     * @return Response from device
     * @throws std::runtime_error on error
     */
    virtual std::string sendQuery(const std::string& query) const = 0;
};

/**
 * @brief Power Supply Factory
 *
 * Factory class for creating power supply instances based on vendor/model
 */
class PowerSupplyFactory {
public:
    /**
     * @brief Create a power supply instance
     * @param vendor Vendor enumeration
     * @param model Model string
     * @param connectionType Connection type
     * @param connectionString Connection string (port, IP, etc.)
     * @return Unique pointer to IPowerSupply instance
     * @throws std::runtime_error if vendor/model not supported
     */
    static std::unique_ptr<IPowerSupply> create(
        Vendor vendor,
        const std::string& model,
        ConnectionType connection_type,
        const std::string& connection_string
    );

    /**
     * @brief Create a power supply instance from identification string
     * @param idn_string *IDN? response string
     * @param connection_type Connection type
     * @param connection_string Connection string
     * @return Unique pointer to IPowerSupply instance
     * @throws std::runtime_error if cannot parse or vendor not supported
     */
    static std::unique_ptr<IPowerSupply> createFromIDN(
        const std::string& idn_string,
        ConnectionType connection_type,
        const std::string& connection_string
    );
};

} // namespace PowerSupply

#endif // POWER_SUPPLY_INTERFACE_H
