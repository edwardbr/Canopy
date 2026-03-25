/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <string_view>
#include <iostream>

extern "C"
{
    void rpc_log(
        int level,
        const char* str,
        size_t sz)
    {
        std::string_view message(str, sz);
        switch (level)
        {
        case 0:
            std::cout << "[TRACE] " << message << std::endl;
            break;
        case 1:
            std::cout << "[DEBUG] " << message << std::endl;
            break;
        case 2:
            std::cout << "[INFO] " << message << std::endl;
            break;
        case 3:
            std::cout << "[WARN] " << message << std::endl;
            break;
        case 4:
            std::cout << "[ERROR] " << message << std::endl;
            break;
        case 5:
            std::cout << "[CRITICAL] " << message << std::endl;
            break;
        default:
            std::cout << "[LOG " << level << "] " << message << std::endl;
            break;
        }
    }
}
