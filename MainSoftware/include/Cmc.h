#ifndef CMC_H
#define CMC_H

#include <string>
#include <vector>
#include "DeviceManager.h"
#include "Utils.h"
#include "SerialPort.h"
#include "Server.h"
#include "SystemCommand.h"

class Cmc
{
private:
    /* data */
public:
    Cmc();
    ~Cmc();

    bool configureSequence();
};

extern Cmc g_cmc;

#endif // CMC_H
