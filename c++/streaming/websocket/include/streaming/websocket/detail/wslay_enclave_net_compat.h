// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <stdint.h>

// wslay normally gets htons/ntohs/htonl/ntohl from platform socket headers.
// Enclave builds do not have those Linux networking headers, and the websocket
// framing code only needs fixed-width byte-order conversion. Keep that
// compatibility local to the enclave-built wslay target.
#if defined(FOR_SGX)
static inline uint16_t canopy_wslay_host_to_be16(uint16_t value)
{
#  if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return value;
#  else
    return __builtin_bswap16(value);
#  endif
}

static inline uint32_t canopy_wslay_host_to_be32(uint32_t value)
{
#  if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return value;
#  else
    return __builtin_bswap32(value);
#  endif
}

#  ifndef htons
#    define htons(value) canopy_wslay_host_to_be16((uint16_t)(value))
#  endif

#  ifndef ntohs
#    define ntohs(value) canopy_wslay_host_to_be16((uint16_t)(value))
#  endif

#  ifndef htonl
#    define htonl(value) canopy_wslay_host_to_be32((uint32_t)(value))
#  endif

#  ifndef ntohl
#    define ntohl(value) canopy_wslay_host_to_be32((uint32_t)(value))
#  endif
#endif
