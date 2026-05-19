/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/types.h>

#include <openssl/crypto.h>

namespace canopy::security::attestation
{
    aead_key_material::~aead_key_material()
    {
        OPENSSL_cleanse(key.data(), key.size());
        OPENSSL_cleanse(nonce_prefix.data(), nonce_prefix.size());
    }
} // namespace canopy::security::attestation
