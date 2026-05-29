#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

# Patch the build-local OpenSSL source prepared by Intel SGXSSL. The SampleAttestedTLS project in Intel's
# confidential-computing.sgx tree builds SGXSSL against OpenSSL 3.0.13, which does not pull a plain sprintf reference
# into the enclave libssl archive. The bundled SGXSSL 3.1.6 flow does, via ssl/ssl_lib.c's keylog formatter. The SGX SDK
# trusted C library provides snprintf but not sprintf, so adjust only that generated source tree before libssl.a is
# rebuilt and copied into the SGXSSL package.

if(NOT DEFINED CANOPY_SGXSSL_OPENSSL_SOURCE_DIR)
  message(FATAL_ERROR "CANOPY_SGXSSL_OPENSSL_SOURCE_DIR must point at the prepared OpenSSL source directory.")
endif()

set(_canopy_ssl_lib_c "${CANOPY_SGXSSL_OPENSSL_SOURCE_DIR}/ssl/ssl_lib.c")
if(NOT EXISTS "${_canopy_ssl_lib_c}")
  message(FATAL_ERROR "Cannot patch SGXSSL OpenSSL source: ${_canopy_ssl_lib_c} was not found.")
endif()

file(READ "${_canopy_ssl_lib_c}" _canopy_ssl_lib_contents)

set(_canopy_ssl_sprintf_1 "sprintf(cursor, \"%02x\", parameter_1[i]);")
set(_canopy_ssl_sprintf_2 "sprintf(cursor, \"%02x\", parameter_2[i]);")
set(_canopy_ssl_snprintf_1 "snprintf(cursor, 3, \"%02x\", parameter_1[i]);")
set(_canopy_ssl_snprintf_2 "snprintf(cursor, 3, \"%02x\", parameter_2[i]);")

if(_canopy_ssl_lib_contents MATCHES "sprintf\\(cursor, \"%02x\", parameter_[12]\\[i\\]\\);")
  string(REPLACE "${_canopy_ssl_sprintf_1}" "${_canopy_ssl_snprintf_1}" _canopy_ssl_lib_contents
                 "${_canopy_ssl_lib_contents}")
  string(REPLACE "${_canopy_ssl_sprintf_2}" "${_canopy_ssl_snprintf_2}" _canopy_ssl_lib_contents
                 "${_canopy_ssl_lib_contents}")
  file(WRITE "${_canopy_ssl_lib_c}" "${_canopy_ssl_lib_contents}")
  message(STATUS "Patched SGXSSL OpenSSL ssl_lib.c to avoid enclave sprintf dependency.")
elseif(_canopy_ssl_lib_contents MATCHES "snprintf\\(cursor, 3, \"%02x\", parameter_[12]\\[i\\]\\);")
  message(STATUS "SGXSSL OpenSSL ssl_lib.c sprintf patch already applied.")
else()
  message(FATAL_ERROR "Cannot patch SGXSSL OpenSSL source: expected keylog sprintf formatter was not found.")
endif()
