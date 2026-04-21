#pragma once

#include <cstdlib>
#include <iostream>

#define panic(msg)                                                    \
    do {                                                              \
        std::cerr << __FILE__ << ":" << __LINE__ << " panic: " << msg \
                  << std::endl;                                       \
        std::abort();                                                 \
    } while (0)
