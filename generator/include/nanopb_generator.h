/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

class class_entity;

namespace nanopb_generator
{
    void write_cpp_files(
        const class_entity& lib,
        std::ostream& cpp_stream,
        const std::vector<std::string>& namespaces,
        const std::filesystem::path& header_filename,
        const std::filesystem::path& nanopb_include_path,
        const std::vector<std::string>& additional_stub_headers);
}
