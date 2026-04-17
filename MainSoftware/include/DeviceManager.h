#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <string>
#include <vector>
#include <memory>
#include "Device.h"
#include "TdkLambdaPsu.h"

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    // Creates device
    bool create(Device device);

    // Connects to device
    bool connect(Device device);

    // Disconnects from device
    bool disconnect(Device device);

    // Checks if device is connected
    bool isConnected(Device device) const;

    // ==================== PSU Control Methods ====================

    // Set voltage (volts) - for specified PSU
    bool setVoltage(Device device, double voltage);

    // Set current limit (amps) - for specified PSU
    bool setCurrent(Device device, double current);

    double getCurrent(Device device);

    double getVoltage(Device device);

    // Enable/disable output - for specified PSU
    bool enableOutput(Device device, bool enable);

    // Check if PSU output is currently enabled
    bool isOutputEnabled(Device device);

    // Read measured voltage - for specified PSU
    double measureVoltage(Device device);

    // Read measured current - for specified PSU
    double measureCurrent(Device device);

    // Read measured power - for specified PSU
    double measurePower(Device device);

    // Get PSU status
    PowerSupply::PowerSupplyStatus getStatus(Device device);

    // Get PSU identification
    std::string getIdentification(Device device);

    // Lightweight health probe for the PSU - true if PSU responds to *IDN?
    // Never throws; used as a heartbeat during long test runs.
    bool ping(Device device);

private:
    std::vector<Device> m_devices;
    std::vector<Device> m_connectedDevices;

    // PSU - TDK Lambda PSU30 (30V, 56A)
    std::unique_ptr<TDKLambda::TDKLambdaPSU30> m_psu_30;
    // PSU - TDK Lambda PSU300 (300V, 5.6A)
    std::unique_ptr<TDKLambda::TDKLambdaPSU300> m_psu_300;

    // Returns PSU pointer for specified device
    TDKLambda::TDKLambdaPSU* getPSU(Device device) const;
};

// Global singleton declaration
extern DeviceManager g_DeviceManager;

#endif // DEVICE_MANAGER_H
