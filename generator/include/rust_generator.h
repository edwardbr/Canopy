/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <filesystem>

class class_entity;

namespace rust_generator
{
    // Rust generator: currently emits interface ordinals, method ordinals,
    // protocol-version fingerprints, generated interface-binding metadata,
    // and raw #rust_quote blocks.
    std::filesystem::path write_file(
        const class_entity& lib,
        const std::filesystem::path& output_path,
        const std::filesystem::path& relative_path);
}
