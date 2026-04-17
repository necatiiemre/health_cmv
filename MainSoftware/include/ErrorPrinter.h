#ifndef ERROR_PRINTER_H
#define ERROR_PRINTER_H

#include <iostream>
#include <string>

/**
 * @brief Consistent, categorized error/warning/info messages.
 *
 * Categories: PSU, SERVER, SSH, CUMULUS, SERIAL, DPDK, SYSTEM
 *
 * Output examples:
 *   [ERROR][PSU]     PSU G30 connection failed!
 *   [WARN][SERVER]   Server ping timeout, retrying...
 *   [INFO][SYSTEM]   Safe shutdown initiated...
 */
class ErrorPrinter {
public:
    static void error(const std::string& category, const std::string& message) {
        std::cerr << "[ERROR][" << category << "] " << message << std::endl;
    }

    static void warn(const std::string& category, const std::string& message) {
        std::cout << "[WARN][" << category << "] " << message << std::endl;
    }

    static void info(const std::string& category, const std::string& message) {
        std::cout << "[INFO][" << category << "] " << message << std::endl;
    }
};

#endif // ERROR_PRINTER_H
