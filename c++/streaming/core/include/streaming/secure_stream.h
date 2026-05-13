// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#if defined(CANOPY_SECURE_STREAM_BACKEND_MBEDTLS)
#  include <streaming/mbedtls/stream.h>

namespace streaming::secure
{
    using client_context = ::streaming::mbedtls::client_context;
    using client_context_options = ::streaming::mbedtls::client_context_options;
    using context = ::streaming::mbedtls::context;
    using peer_verification = ::streaming::mbedtls::peer_verification;
    using pem_credentials = ::streaming::mbedtls::pem_credentials;
    using server_context_options = ::streaming::mbedtls::server_context_options;
    using stream = ::streaming::mbedtls::stream;
} // namespace streaming::secure

#elif defined(CANOPY_SECURE_STREAM_BACKEND_OPENSSL)
#  include <streaming/tls/stream.h>

namespace streaming::secure
{
    using client_context = ::streaming::tls::client_context;
    using client_context_options = ::streaming::tls::client_context_options;
    using context = ::streaming::tls::context;
    using peer_verification = ::streaming::tls::peer_verification;
    using pem_credentials = ::streaming::tls::pem_credentials;
    using server_context_options = ::streaming::tls::server_context_options;
    using stream = ::streaming::tls::stream;
} // namespace streaming::secure

#else
#  error "Select exactly one secure stream backend"
#endif
