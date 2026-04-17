#ifndef HSN_H
#define HSN_H

#include <string>
#include <vector>
#include "DeviceManager.h"
#include "Utils.h"
#include "SerialPort.h"
#include "Server.h"
#include "SystemCommand.h"

class Hsn
{
private:
    /* data */
public:
    Hsn();
    ~Hsn();

    bool configureSequence();
};

extern Hsn g_hsn;

#endif // HSN_H
