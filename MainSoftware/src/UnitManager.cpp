#include "UnitManager.h"
#include "Utils.h"
#include <algorithm>
#include <iostream>

UnitManager g_UnitManager;

UnitManager::UnitManager()
{
    // Constructor
}

UnitManager::~UnitManager()
{
    // Destructor - stop all devices
    for (const auto &deviceId : m_activeDevices)
    {
        stopDevice(deviceId);
    }
}

bool UnitManager::startDevice(const std::string &deviceId)
{
    // Return false if device is already running
    if (isDeviceRunning(deviceId))
    {
        DEBUG_LOG("Device already running: " << deviceId);
        return false;
    }

    // TODO: Actual device start operation will be implemented here

    m_activeDevices.push_back(deviceId);
    DEBUG_LOG("Device started: " << deviceId);
    return true;
}

bool UnitManager::stopDevice(const std::string &deviceId)
{
    // Return false if device is not running
    if (!isDeviceRunning(deviceId))
    {
        DEBUG_LOG("Device not running: " << deviceId);
        return false;
    }

    // TODO: Actual device stop operation will be implemented here

    auto it = std::find(m_activeDevices.begin(), m_activeDevices.end(), deviceId);
    if (it != m_activeDevices.end())
    {
        m_activeDevices.erase(it);
    }

    DEBUG_LOG("Device stopped: " << deviceId);
    return true;
}

bool UnitManager::isDeviceRunning(const std::string &deviceId) const
{
    return std::find(m_activeDevices.begin(), m_activeDevices.end(), deviceId) != m_activeDevices.end();
}

Unit UnitManager::unitSelector()
{
    int choice = 0;
    Unit unit;
    while (true)
    {
        std::cout << "Select Unit?\n";
        std::cout << "1) CMC\n";
        std::cout << "2) MMC\n";
        std::cout << "3) VMC\n";
        std::cout << "4) DTN\n";
        std::cout << "5) HSN\n";
        std::cout << "Enter choice: ";
        std::cin >> choice;

        // User entered letter, symbol, etc.
        if (std::cin.fail())
        {
            std::cin.clear();
            std::cin.ignore(1000, '\n');
            std::cout << "Invalid input!\n\n";
            continue;
        }

        // User entered valid number but wrong range
        if (choice < 1 || choice > 5)
        {
            std::cout << "Invalid option! Please select between 1 - 5.\n\n";
            continue;
        }

        // Valid input
        break;
    }

    unit = (Unit)choice;
    std::cout << "You selected: " << enumToString(unit) << std::endl;
    return unit;
}

bool UnitManager::configureDeviceForUnit(Unit unit)
{
    switch (unit)
    {
    case CMC:
        if (!g_cmc.configureSequence()) return false;
        break;
    case MMC:
        if (!g_mmc.configureSequence()) return false;
        break;
    case VMC:
        if (!g_vmc.configureSequence()) return false;
        break;
    case DTN:
        if (!g_dtn.configureSequence()) return false;
        break;
    case HSN:
        if (!g_hsn.configureSequence()) return false;
        break;
    default:
        std::cout << "Unknown unit type!" << std::endl;
        return false;
    }

    return true;
}

std::vector<std::string> UnitManager::getDeviceList() const
{
    return m_activeDevices;
}

std::string UnitManager::enumToString(Unit unit)
{
    switch (unit)
    {
    case Unit::CMC:
        return "CMC";
    case Unit::MMC:
        return "MMC";
    case Unit::VMC:
        return "VMC";
    case Unit::DTN:
        return "DTN";
    case Unit::HSN:
        return "HSN";
    }
    return "UNKNOWN";
}
