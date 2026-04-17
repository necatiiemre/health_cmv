#include "Cmc.h"
#include "ReportManager.h"
#include "SafeShutdown.h"
#include "ErrorPrinter.h"
#include "Utils.h"
#include <iostream>
#include <unistd.h>
#include <iomanip>
// 28V 3.0A

Cmc g_cmc;

Cmc::Cmc()
{
}

Cmc::~Cmc()
{
}

bool Cmc::configureSequence()
{
    auto& shutdown = SafeShutdown::getInstance();

    if (!g_Server.onWithWait(3))
    {
        ErrorPrinter::error("SERVER", "CMC: Server could not be started!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerServerOn();

    // Create PSU G300 (300V, 5.6A)
    if (!g_DeviceManager.create(PSUG300))
    {
        ErrorPrinter::error("PSU", "CMC: Failed to create PSU G300!");
        shutdown.executeShutdown();
        return false;
    }

    // Connect to PSU G300
    if (!g_DeviceManager.connect(PSUG300))
    {
        ErrorPrinter::error("PSU", "CMC: Failed to connect to PSU G300!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerPsuConnected(PSUG300);

    if (!g_DeviceManager.setCurrent(PSUG300, 1.5))
    {
        ErrorPrinter::error("PSU", "CMC: Failed to set current on PSU G300!");
        shutdown.executeShutdown();
        return false;
    }

    if (!g_DeviceManager.setVoltage(PSUG300, 20.0))
    {
        ErrorPrinter::error("PSU", "CMC: Failed to set voltage on PSU G300!");
        shutdown.executeShutdown();
        return false;
    }

    if (!g_DeviceManager.enableOutput(PSUG300, true))
    {
        ErrorPrinter::error("PSU", "CMC: Failed to enable output on PSU G300!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerPsuOutputEnabled(PSUG300);

    // Record Unit Power On Time when PSU output is enabled
    g_ReportManager.recordUnitPowerOnTime();

    serial::sendSerialCommand("/dev/ttyACM0", "VMC_ID 1");

    for (int i = 0; i < 1000; i++)
    {
        if (i % 20 == 0)
        {
            double current = g_DeviceManager.measureCurrent(PSUG300);
            double voltage = g_DeviceManager.measureVoltage(PSUG300);
            double power = g_DeviceManager.measurePower(PSUG300);
            double get_current = g_DeviceManager.getCurrent(PSUG300);
            double get_voltage = g_DeviceManager.getVoltage(PSUG300);

            {
                utils::FloatFormatGuard guard(std::cout, 2, true);
                std::cout << "Current: " << current << " " << "Voltage: " << voltage << " " << "Power: " << power << " " << "Get Current: " << get_current << " " << "Get Voltage:" << get_voltage << std::endl;
            }
        }
    }

    if (!g_DeviceManager.enableOutput(PSUG300, false))
    {
        ErrorPrinter::error("PSU", "CMC: Failed to disable output on PSU G300!");
        shutdown.executeShutdown();
        return false;
    }

    // Verify PSU output is actually off, retry if not
    for (int retry = 0; retry < 3; retry++)
    {
        usleep(500000); // 500ms wait before checking
        if (!g_DeviceManager.isOutputEnabled(PSUG300))
        {
            std::cout << "CMC: PSU G300 output verified OFF." << std::endl;
            break;
        }
        ErrorPrinter::warn("PSU", "CMC: PSU G300 still ON, retry " + std::to_string(retry + 1) + "/3...");
        g_DeviceManager.enableOutput(PSUG300, false);
    }
    shutdown.unregisterPsuOutputEnabled(PSUG300);

    // Record Power Off Time when PSU output is disabled
    g_ReportManager.recordPowerOffTime();

    if (!g_DeviceManager.disconnect(PSUG300))
    {
        ErrorPrinter::error("PSU", "CMC: Failed to disconnect PSU G300!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.unregisterPsuConnected(PSUG300);

    g_Server.offWithWait(300);
    shutdown.unregisterServerOn();

    std::cout << "CMC: PSU configured successfully." << std::endl;
    return true;
}
