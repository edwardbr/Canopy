/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "test_globals.h"
#include <string>

// Global variable definitions
std::weak_ptr<rpc::service> current_host_service; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

#ifdef _WIN32                                                   // windows
std::string enclave_path = "./marshal_test_enclave.signed.dll"; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
#else                                                           // Linux
std::string enclave_path = "./libmarshal_test_enclave.signed.so"; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
#endif
