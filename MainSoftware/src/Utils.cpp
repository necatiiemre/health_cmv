#include "Utils.h"

#include <iostream>   // std::cout
#include <iomanip>    // std::setprecision, std::fixed
#include <sstream>    // std::ostringstream
#include <stdio.h>
#include <csignal>
#include <atomic>

namespace utils {

void set_global_float_format(std::ostream& os, int precision, bool fixed)
{
    if (fixed) {
        os.setf(std::ios::fixed, std::ios::floatfield);
    } else {
        os.setf(std::ios::fmtflags(0), std::ios::floatfield);
    }

    os << std::setprecision(precision);
}

void reset_float_format(std::ostream& os)
{
    // Reset floatfield (fixed / scientific etc.)
    os.unsetf(std::ios::floatfield);

    // Default precision is usually 6, adjust for your project if needed
    os << std::setprecision(6);
}

std::string format_float(double value, int precision, bool fixed)
{
    std::ostringstream oss;

    if (fixed) {
        oss.setf(std::ios::fixed, std::ios::floatfield);
    }

    oss << std::setprecision(precision) << value;
    return oss.str();
}

// ---------------- RAII Guard ----------------

FloatFormatGuard::FloatFormatGuard(std::ostream& os,
                                   int precision,
                                   bool fixed)
    : m_os(os),
      m_old_flags(os.flags()),          // Save old flags
      m_old_precision(os.precision())   // Save old precision
{
    if (fixed) {
        m_os.setf(std::ios::fixed, std::ios::floatfield);
    } else {
        m_os.setf(std::ios::fmtflags(0), std::ios::floatfield);
    }

    m_os << std::setprecision(precision);
}

FloatFormatGuard::~FloatFormatGuard()
{
    // Restore old format settings
    m_os.flags(m_old_flags);
    m_os << std::setprecision(m_old_precision);
}

void pressEnterForDebug(){
    std::cout << "Press enter for continue...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

// Ctrl+C sinyali için global flag
static std::atomic<bool> g_ctrlc_received(false);

// SIGINT handler
static void ctrlc_handler(int signum) {
    (void)signum; // unused warning'i önle
    g_ctrlc_received.store(true);
}

void waitForCtrlC() {
    // Flag'i sıfırla
    g_ctrlc_received.store(false);

    // Eski handler'ı sakla
    struct sigaction old_action;
    struct sigaction new_action;

    new_action.sa_handler = ctrlc_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;

    // Yeni handler'ı kaydet
    sigaction(SIGINT, &new_action, &old_action);

    std::cout << "Waiting for Ctrl+C to continue..." << std::endl;

    // Ctrl+C gelene kadar bekle
    while (!g_ctrlc_received.load()) {
        usleep(100000); // 100ms bekle (CPU kullanımını azaltmak için)
    }

    std::cout << "\nCtrl+C received, continuing..." << std::endl;

    // Eski handler'ı geri yükle
    sigaction(SIGINT, &old_action, nullptr);
}

} // namespace utils
