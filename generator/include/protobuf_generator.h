/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <ostream>
#include <map>

// Forward declarations
class class_entity;
class attributes;

namespace protobuf_generator
{
    // entry point - generates multiple .proto files for nested namespaces
    // Returns a vector of generated .proto file paths (relative to output_path/src)
    std::vector<std::string> write_files(
        const class_entity& lib,
        const std::filesystem::path& output_path,
        const std::filesystem::path& sub_directory,
        const std::filesystem::path& base_filename);

    // entry point - generates protobuf C++ serialization implementation
    void write_cpp_files(
        const class_entity& lib,
        std::ostream& cpp_stream,
        const std::vector<std::string>& namespaces,
        const std::filesystem::path& header_filename,
        const std::filesystem::path& protobuf_include_path,
        const std::vector<std::string>& additional_stub_headers);
}
