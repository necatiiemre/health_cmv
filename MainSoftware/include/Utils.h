#pragma once

#include <iosfwd>   // std::ostream
#include <string>
#include <unistd.h>
#include <iostream>
#include <limits>
#include <csignal>

// ========== DEBUG PRINT MODE ==========
// 1 = DEBUG ACIK, 0 = DEBUG KAPALI
#define DEBUG_PRINT 0

#if DEBUG_PRINT
  #define DEBUG_LOG(msg) std::cout << msg << std::endl
#else
  #define DEBUG_LOG(msg)
#endif
// ======================================

namespace utils {

    // Set float format for global stream (e.g., std::cout)
    void set_global_float_format(std::ostream& os = std::cout,
                                 int precision = 2,
                                 bool fixed = true);

    // Reset global stream format to reasonable default
    void reset_float_format(std::ostream& os = std::cout);

    // Generate formatted string from a number
    std::string format_float(double value,
                             int precision = 2,
                             bool fixed = true);

    void pressEnterForDebug();

    // Waits until Ctrl+C (SIGINT) signal is received, then code continues
    void waitForCtrlC();

    // RAII guard to change format only within a specific scope
    class FloatFormatGuard {
    public:
        FloatFormatGuard(std::ostream& os,
                         int precision,
                         bool fixed = true);

        ~FloatFormatGuard();

    private:
        std::ostream&      m_os;
        std::ios::fmtflags m_old_flags;
        std::streamsize    m_old_precision;
    };

} // namespace utils