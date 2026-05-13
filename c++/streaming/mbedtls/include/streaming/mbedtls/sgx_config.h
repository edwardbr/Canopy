// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

// Mbed TLS is built for raw SGX enclaves without host filesystem, sockets,
// pthreads, or platform entropy. Entropy is supplied by the stream wrapper via
// sgx_read_rand.
#if defined(FOR_SGX)
#  define MBEDTLS_TEST_PLATFORM_IS_NOT_UNIXLIKE
#  undef MBEDTLS_HAVE_TIME
#  undef MBEDTLS_HAVE_TIME_DATE
#  undef MBEDTLS_FS_IO
#  undef MBEDTLS_NET_C
#  undef MBEDTLS_TIMING_C
#  undef MBEDTLS_DEBUG_C
#  undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#  undef MBEDTLS_PSA_ITS_FILE_C
#  undef MBEDTLS_SELF_TEST
#  undef MBEDTLS_THREADING_C
#  undef MBEDTLS_THREADING_PTHREAD
#  undef MBEDTLS_THREADING_ALT
#  define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#  define MBEDTLS_NO_PLATFORM_ENTROPY
#endif
