/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <filesystem>

class class_entity;

namespace javascript_generator
{
    // Generates a UMD JavaScript proxy/stub file from the IDL AST.
    // Output: output_path/<base_filename>.js  (output_path is created if absent)
    // Returns the path of the written (or unchanged) file.
    std::filesystem::path write_files(
        const class_entity& lib,
        const std::filesystem::path& output_path,
        const std::filesystem::path& base_filename);
}
