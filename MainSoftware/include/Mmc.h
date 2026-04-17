#ifndef MMC_H
#define MMC_H

#include <string>
#include <vector>
#include "DeviceManager.h"
#include "Utils.h"
#include "SerialPort.h"
#include "Server.h"
#include "SystemCommand.h"

class Mmc
{
private:
    /* data */
public:
    Mmc();
    ~Mmc();

    bool configureSequence();
};

extern Mmc g_mmc;

#endif // MMC_H
