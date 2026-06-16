/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <security/attestation/types.h>

namespace canopy::security::attestation
{
    class security_context_source
    {
    public:
        virtual ~security_context_source() = default;

        [[nodiscard]] virtual auto security_context() const -> security_context = 0;
    };
} // namespace canopy::security::attestation
