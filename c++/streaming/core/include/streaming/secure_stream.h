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
#  include <streaming/openssl_tls/stream.h>

namespace streaming::secure
{
    using client_context = ::streaming::openssl_tls::client_context;
    using client_context_options = ::streaming::openssl_tls::client_context_options;
    using context = ::streaming::openssl_tls::context;
    using peer_verification = ::streaming::openssl_tls::peer_verification;
    using pem_credentials = ::streaming::openssl_tls::pem_credentials;
    using server_context_options = ::streaming::openssl_tls::server_context_options;
    using stream = ::streaming::openssl_tls::stream;
} // namespace streaming::secure

#else
// No secure-stream backend selected. Provide opaque forward declarations
// so headers that pass shared_ptr<streaming::secure::context> through
// (e.g. canopy_http_server) can include this header without a backend
// being chosen. Any attempt to actually construct or call into these
// types will fail at link time, not compile time. Use this configuration
// only when the secure-stream feature is not used at runtime (e.g. plain
// HTTP in blocking-mode builds before TLS dual-mode lands).
namespace streaming::secure
{
    class context;
    class stream;
    struct client_context_options;
    struct server_context_options;
    struct pem_credentials;
    enum class peer_verification;
} // namespace streaming::secure
#endif
