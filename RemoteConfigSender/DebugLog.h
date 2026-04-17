#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <iostream>

// ========== DEBUG PRINT MODE ==========
// 1 = DEBUG ACIK, 0 = DEBUG KAPALI
#define DEBUG_PRINT 0

#if DEBUG_PRINT
  #define DEBUG_LOG(msg) std::cout << msg << std::endl
#else
  #define DEBUG_LOG(msg)
#endif
// ======================================

#endif // DEBUG_LOG_H
