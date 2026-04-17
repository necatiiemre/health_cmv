#include "Hsn.h"
#include "ReportManager.h"
#include "SafeShutdown.h"
#include "ErrorPrinter.h"
#include "Utils.h"
#include <iostream>
#include <unistd.h>
#include <iomanip>
// 28V 3.0A

Hsn g_hsn;

Hsn::Hsn()
{
}

Hsn::~Hsn()
{
}

bool Hsn::configureSequence()
{
    auto& shutdown = SafeShutdown::getInstance();

    if (!g_Server.onWithWait(3))
    {
        ErrorPrinter::error("SERVER", "HSN: Server could not be started!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerServerOn();

    // Create PSU G30 (30V, 56A)
    if (!g_DeviceManager.create(PSUG30))
    {
        ErrorPrinter::error("PSU", "HSN: Failed to create PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    // Connect to PSU G30
    if (!g_DeviceManager.connect(PSUG30))
    {
        ErrorPrinter::error("PSU", "HSN: Failed to connect to PSU G30!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerPsuConnected(PSUG30);

    if (!g_DeviceManager.setCurrent(PSUG30, 1.5))
    {
        ErrorPrinter::error("PSU", "HSN: Failed to set current on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    if (!g_DeviceManager.setVoltage(PSUG30, 20.0))
    {
        ErrorPrinter::error("PSU", "HSN: Failed to set voltage on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    if (!g_DeviceManager.enableOutput(PSUG30, true))
    {
        ErrorPrinter::error("PSU", "HSN: Failed to enable output on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.registerPsuOutputEnabled(PSUG30);

    // Record Unit Power On Time when PSU output is enabled
    g_ReportManager.recordUnitPowerOnTime();

    serial::sendSerialCommand("/dev/ttyACM0", "ID 1");

    for (int i = 0; i < 1000; i++)
    {
        if (i % 20 == 0)
        {
            double current = g_DeviceManager.measureCurrent(PSUG30);
            double voltage = g_DeviceManager.measureVoltage(PSUG30);
            double power = g_DeviceManager.measurePower(PSUG30);
            double get_current = g_DeviceManager.getCurrent(PSUG30);
            double get_voltage = g_DeviceManager.getVoltage(PSUG30);

#if DEBUG_PRINT
            {
                utils::FloatFormatGuard guard(std::cout, 2, true);
                std::cout << "Current: " << current << " " << "Voltage: " << voltage << " " << "Power: " << power << " " << "Get Current: " << get_current << " " << "Get Voltage:" << get_voltage << std::endl;
            }
#endif
        }
    }

    if (!g_DeviceManager.enableOutput(PSUG30, false))
    {
        ErrorPrinter::error("PSU", "HSN: Failed to disable output on PSU G30!");
        shutdown.executeShutdown();
        return false;
    }

    // Verify PSU output is actually off, retry if not
    for (int retry = 0; retry < 3; retry++)
    {
        usleep(500000); // 500ms wait before checking
        if (!g_DeviceManager.isOutputEnabled(PSUG30))
        {
            std::cout << "HSN: PSU G30 output verified OFF." << std::endl;
            break;
        }
        ErrorPrinter::warn("PSU", "HSN: PSU G30 still ON, retry " + std::to_string(retry + 1) + "/3...");
        g_DeviceManager.enableOutput(PSUG30, false);
    }
    shutdown.unregisterPsuOutputEnabled(PSUG30);

    // Record Power Off Time when PSU output is disabled
    g_ReportManager.recordPowerOffTime();

    if (!g_DeviceManager.disconnect(PSUG30))
    {
        ErrorPrinter::error("PSU", "HSN: Failed to disconnect PSU G30!");
        shutdown.executeShutdown();
        return false;
    }
    shutdown.unregisterPsuConnected(PSUG30);

    g_Server.offWithWait(300);
    shutdown.unregisterServerOn();

    std::cout << "HSN: PSU configured successfully." << std::endl;
    return true;
}
