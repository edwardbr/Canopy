/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

// NOLINTNEXTLINE(bugprone-incorrect-enable-shared-from-this): class_entity is defined in the idlparser submodule.
class class_entity;

namespace rest_generator
{
    void write_files(
        const class_entity& lib,
        std::ostream& output,
        const std::vector<std::string>& namespaces,
        const std::string& header_filename,
        const std::filesystem::path& metadata_path);
}
