/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

class class_entity;

namespace rust_protobuf_generator
{
    // Generate Rust-facing metadata for the canonical protobuf schema files emitted by
    // protobuf_generator. This keeps Rust bound to the same .proto layout as C++ without
    // committing generated Rust protobuf code into the repository.
    std::filesystem::path write_file(
        const class_entity& lib,
        const std::filesystem::path& output_path,
        const std::filesystem::path& relative_path,
        const std::filesystem::path& sub_directory,
        const std::string& base_filename,
        const std::vector<std::string>& generated_proto_files);
}
