#ifndef UNIT_MANAGER_H
#define UNIT_MANAGER_H

#include <string>
#include <vector>
#include <string>
#include "DeviceManager.h"
#include "Dtn.h"
#include "Hsn.h"
#include "Cmc.h"
#include "Mmc.h"
#include "Vmc.h"

enum Unit
{
    CMC = 1,
    MMC = 2,
    VMC = 3,
    DTN = 4,
    HSN = 5
};

class UnitManager
{
public:
    UnitManager();
    ~UnitManager();

    // Starts device
    bool startDevice(const std::string &deviceId);

    // Stops device
    bool stopDevice(const std::string &deviceId);

    // Checks if device is running
    bool isDeviceRunning(const std::string &deviceId) const;

    // Lists all devices
    std::vector<std::string> getDeviceList() const;

    Unit unitSelector();

    bool configureDeviceForUnit(Unit unit);

    std::string enumToString(Unit unit);

private:
    std::vector<std::string> m_activeDevices;
};

// Global singleton declaration
extern UnitManager g_UnitManager;

#endif // UNIT_MANAGER_H
