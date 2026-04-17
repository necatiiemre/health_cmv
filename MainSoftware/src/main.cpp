#include "ReportManager.h"
#include "UnitManager.h"
#include "Server.h"
#include "SSHDeployer.h"
#include "SafeShutdown.h"
#include "ErrorPrinter.h"
#include <iostream>
#include <csignal>

int main(int argc, char const *argv[])
{
    // Ignore SIGPIPE so that writing to a closed TCP socket (e.g. PSU after
    // long idle or network glitch) surfaces as send()==-1/EPIPE instead of
    // silently terminating the process. MSG_NOSIGNAL is used on send()s too,
    // but this is a cheap belt-and-suspenders that also protects any library
    // code we don't control (scp pipes, SSH, etc.).
    std::signal(SIGPIPE, SIG_IGN);

    // Install signal handlers for safe shutdown (Ctrl+C, SIGTERM)
    SafeShutdown::getInstance().installSignalHandlers();

    // Record Software Start Time before collecting test info
    g_ReportManager.recordSoftwareStartTime();

    if (!g_ReportManager.collectTestInfo())
    {
        ErrorPrinter::error("SYSTEM", "Failed to collect report information!");
        return -1;
    }

    Unit unit;
    unit = g_UnitManager.unitSelector();

    g_ReportManager.setUnitName(g_UnitManager.enumToString(unit));

    if (!g_UnitManager.configureDeviceForUnit(unit))
    {
        // SafeShutdown has already been called inside configureSequence()
        ErrorPrinter::error("SYSTEM", "Device configuration failed! System has been safely shut down.");
        return -1;
    }

    // Record Software End Time after configure sequence completes
    g_ReportManager.recordSoftwareEndTime();

    g_ReportManager.writeReportHeader();

    // Create PDF report after test
    if (!g_ReportManager.createPdfReport())
    {
        ErrorPrinter::warn("SYSTEM", "Failed to create PDF report!");
    }

    return 0;
}
