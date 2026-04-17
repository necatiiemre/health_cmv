#include "DeviceManager.h"
#include "Utils.h"
#include <algorithm>
#include <iostream>

DeviceManager g_DeviceManager;

DeviceManager::DeviceManager()
{
    // Constructor
}

DeviceManager::~DeviceManager()
{
    // Destructor - disconnect all devices
    for (const auto &device : m_connectedDevices)
    {
        disconnect(device);
    }
}

TDKLambda::TDKLambdaPSU *DeviceManager::getPSU(Device device) const
{
    switch (device)
    {
    case PSUG30:
        return m_psu_30.get();
    case PSUG300:
        return m_psu_300.get();
    default:
        return nullptr;
    }
}

bool DeviceManager::create(Device device)
{
    switch (device)
    {
    case PSUG30:
    {
        // Create TDK Lambda PSU30 (30V, 56A)
        TDKLambda::PSUConfig config;
        config.ip_address = "10.1.33.5";
        config.tcp_port = 8003;
        config.timeout_ms = 1000;

        m_psu_30 = std::make_unique<TDKLambda::TDKLambdaPSU30>(config);
        m_devices.push_back(device);
        DEBUG_LOG("PSU (TDK Lambda PSU30 - 30V/56A) created.");
        return true;
    }

    case PSUG300:
    {
        // Create TDK Lambda PSU300 (300V, 5.6A)
        TDKLambda::PSUConfig config;
        config.ip_address = "10.1.33.6"; // Different IP for PSU300
        config.tcp_port = 8003;
        config.timeout_ms = 1000;

        m_psu_300 = std::make_unique<TDKLambda::TDKLambdaPSU300>(config);
        m_devices.push_back(device);
        DEBUG_LOG("PSU (TDK Lambda PSU300 - 300V/5.6A) created.");
        return true;
    }

    default:
        std::cout << "Unknown device type!" << std::endl;
        return false;
    }
}

bool DeviceManager::connect(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu)
    {
        std::cout << "PSU not created! Call create() first. Device: " << device << std::endl;
        return false;
    }

    try
    {
        psu->connect();
        m_connectedDevices.push_back(device);

        std::string device_name = (device == PSUG30) ? "PSU30" : "PSU300";
        std::cout << "PSU (TDK Lambda " << device_name << ") connection successful." << std::endl;
        return true;
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU connection error: " << e.what() << std::endl;
        return false;
    }
}

bool DeviceManager::disconnect(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu)
    {
        std::cout << "PSU not created!" << std::endl;
        return false;
    }

    try
    {
        psu->disconnect();
        auto it = std::find(m_connectedDevices.begin(), m_connectedDevices.end(), device);
        if (it != m_connectedDevices.end())
        {
            m_connectedDevices.erase(it);
        }

        std::string device_name = (device == PSUG30) ? "PSU30" : "PSU300";
        DEBUG_LOG("PSU (TDK Lambda " << device_name << ") disconnected.");
        return true;
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU disconnect error: " << e.what() << std::endl;
        return false;
    }
}

bool DeviceManager::isConnected(Device device) const
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu)
    {
        return false;
    }

    return psu->isConnected();
}

// ==================== PSU Control Methods ====================

bool DeviceManager::setVoltage(Device device, double voltage)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return false;
    }

    try
    {
        psu->setVoltage(voltage);
        DEBUG_LOG("PSU voltage set: " << voltage << "V (Device: " << device << ")");
        return true;
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU voltage set error: " << e.what() << std::endl;
        return false;
    }
}

bool DeviceManager::setCurrent(Device device, double current)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return false;
    }

    try
    {
        psu->setCurrent(current);
        DEBUG_LOG("PSU current limit set: " << current << "A (Device: " << device << ")");
        return true;
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU current set error: " << e.what() << std::endl;
        return false;
    }
}

double DeviceManager::getVoltage(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return -1.0;
    }

    try
    {
        return psu->getVoltage();
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU voltage read error: " << e.what() << std::endl;
        return -1.0;
    }
}

double DeviceManager::getCurrent(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return -1.0;
    }

    try
    {
        return psu->getCurrent();
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU current read error: " << e.what() << std::endl;
        return -1.0;
    }
}

bool DeviceManager::enableOutput(Device device, bool enable)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return false;
    }

    try
    {
        psu->enableOutput(enable);
        std::cout << "PSU output " << (enable ? "enabled" : "disabled")
                  << " (Device: " << device << ")" << std::endl;
        return true;
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU output control error: " << e.what() << std::endl;
        return false;
    }
}

bool DeviceManager::isOutputEnabled(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return false;
    }

    try
    {
        return psu->isOutputEnabled();
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU output state query error: " << e.what() << std::endl;
        return false;
    }
}

double DeviceManager::measureVoltage(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return -1.0;
    }

    try
    {
        return psu->measureVoltage();
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU voltage measure error: " << e.what() << std::endl;
        return -1.0;
    }
}

double DeviceManager::measureCurrent(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return -1.0;
    }

    try
    {
        return psu->measureCurrent();
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU current measure error: " << e.what() << std::endl;
        return -1.0;
    }
}

double DeviceManager::measurePower(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return -1.0;
    }

    try
    {
        return psu->measurePower();
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU power measure error: " << e.what() << std::endl;
        return -1.0;
    }
}

PowerSupply::PowerSupplyStatus DeviceManager::getStatus(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return PowerSupply::PowerSupplyStatus();
    }

    try
    {
        return psu->getStatus();
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU status read error: " << e.what() << std::endl;
        return PowerSupply::PowerSupplyStatus();
    }
}

std::string DeviceManager::getIdentification(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);

    if (!psu || !psu->isConnected())
    {
        std::cout << "PSU not connected! Device: " << device << std::endl;
        return "";
    }

    try
    {
        return psu->getIdentification();
    }
    catch (const TDKLambda::PSUException &e)
    {
        std::cout << "PSU identification read error: " << e.what() << std::endl;
        return "";
    }
}

bool DeviceManager::ping(Device device)
{
    TDKLambda::TDKLambdaPSU *psu = getPSU(device);
    if (!psu) {
        return false;
    }
    // ping() in TDKLambdaPSU already never throws and attempts an internal
    // reconnect through sendQuery() if the transport is broken.
    return psu->ping();
}
