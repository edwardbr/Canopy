/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

struct yas_serialization_options
{
    bool binary = false;
    bool compressed_binary = false;
    bool json = false;

    [[nodiscard]] bool any() const { return binary || compressed_binary || json; }
};
