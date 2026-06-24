/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <iostream>
#include <string>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <algorithm>
#include <map>
#include <vector>

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wsuggest-override"
#  pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wsuggest-override"
#endif

#include <args.hxx>

#ifdef __clang__
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#include "commonfuncs.h"
#include "macro_parser.h"

#include <coreclasses.h>

#include "synchronous_generator.h"
#include "synchronous_mock_generator.h"
#include "yas_generator.h"
#include "protobuf_generator.h"
#include "nanopb_generator.h"
#include "rust_generator.h"
#include "rust_protobuf_generator.h"
#include "javascript_generator.h"
#include "rest_generator.h"
#include "component_checksum.h"

#include "json_schema/generator.h"

using namespace std;

namespace javascript_json
{
    namespace json
    {
        extern string namespace_name; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    }
}

void get_imports(
    const class_entity& object,
    std::list<std::string>& imports,
    std::set<std::string>& imports_cache)
{
    for (auto& cls : object.get_classes())
    {
        if (!cls->get_import_lib().empty())
        {
            if (imports_cache.find(cls->get_import_lib()) == imports_cache.end())
            {
                imports_cache.insert(cls->get_import_lib());
                imports.push_back(cls->get_import_lib());
            }
        }
        get_imports(*cls, imports, imports_cache);
    }
}

bool is_different(
    const std::stringstream& stream,
    const std::string& data)
{
    auto stream_str = stream.str();
    if (stream_str.empty())
    {
        if (data.empty())
            return true;
        return false;
    }
    stream_str = stream_str.substr(0, stream_str.length() - 1);
    return stream_str != data;
}

std::string sanitise_cpp_identifier(std::string name)
{
    for (auto& ch : name)
    {
        const auto value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_')
            ch = '_';
    }

    if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())))
        name.insert(name.begin(), '_');

    return name;
}

std::string make_json_schema_header(
    const std::string& module_name,
    const class_entity& root_entity,
    const std::string& generated_header_include,
    const std::list<std::string>& imports,
    const json_schema::schema_profile& profile)
{
    const auto symbol_name = sanitise_cpp_identifier(module_name) + "_json_schema";

    std::stringstream header;
    header << "#pragma once\n\n";
    header << "#include <stdexcept>\n";
    header << "#include <string>\n\n";
    header << "#include <rpc/rpc_types.h>\n";
    header << "#include <json/convert.h>\n";
    // Pull in the schema headers of every imported IDL so generated
    // from_json_object/to_json_object overloads for cross-IDL types are
    // visible to ADL when this header is included alone.
    for (const auto& import : imports)
    {
        auto schema_include = import;
        const auto dot = schema_include.rfind('.');
        if (dot != std::string::npos)
            schema_include.erase(dot);
        schema_include += "_schema.h";
        header << "#include \"" << schema_include << "\"\n";
    }
    header << "#include \"" << generated_header_include << "\"\n\n";
    json_schema::write_cpp_schema_accessors(root_entity, header, module_name, symbol_name, profile);
    json_schema::write_cpp_convert_accessors(root_entity, header);
    return header.str();
}

std::string compact_type_for_policy(std::string type)
{
    type.erase(
        std::remove_if(type.begin(), type.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }), type.end());
    return type;
}

bool contains_forbidden_std_idl_sum_type(
    const std::string& type,
    const std::string& type_name)
{
    const auto compact = compact_type_for_policy(type);
    auto pos = compact.find(type_name + "<");
    while (pos != std::string::npos)
    {
        if (pos == 0 || compact[pos - 1] == ':')
            return true;
        pos = compact.find(type_name + "<", pos + 1);
    }
    return false;
}

void validate_idl_type_policy_type(
    const std::string& type,
    const std::string& location,
    std::vector<std::string>& errors)
{
    if (contains_forbidden_std_idl_sum_type(type, "std::optional"))
    {
        errors.push_back(location + " uses std::optional in IDL type '" + type + "'. Use rpc::optional instead.");
    }
    if (contains_forbidden_std_idl_sum_type(type, "std::variant"))
    {
        errors.push_back(location + " uses std::variant in IDL type '" + type + "'. Use rpc::variant instead.");
    }
}

void validate_idl_type_policy_entity(
    const class_entity& object,
    const std::string& scope,
    std::vector<std::string>& errors)
{
    const auto elements = object.get_elements(entity_type::NAMESPACE_MEMBERS | entity_type::STRUCTURE_MEMBERS);
    for (const auto& element : elements)
    {
        if (!element)
            continue;

        const auto name = element->get_name();
        const auto location = scope.empty() || name.empty() ? name : scope + "::" + name;

        if (const auto class_element = std::dynamic_pointer_cast<class_entity>(element))
        {
            if (class_element->get_entity_type() == entity_type::TYPEDEF)
                validate_idl_type_policy_type(class_element->get_alias_name(), location, errors);
            validate_idl_type_policy_entity(*class_element, location, errors);
        }
        else if (const auto function_element = std::dynamic_pointer_cast<function_entity>(element))
        {
            validate_idl_type_policy_type(function_element->get_return_type(), location, errors);
            for (const auto& parameter : function_element->get_parameters())
            {
                const auto parameter_location
                    = location + "(" + (parameter.get_name().empty() ? "<unnamed>" : parameter.get_name()) + ")";
                validate_idl_type_policy_type(parameter.get_type(), parameter_location, errors);
            }
        }
        else if (const auto parameter_element = std::dynamic_pointer_cast<parameter_entity>(element))
        {
            validate_idl_type_policy_type(parameter_element->get_type(), location, errors);
        }
    }
}

std::vector<std::string> validate_idl_type_policy(const class_entity& objects)
{
    std::vector<std::string> errors;
    validate_idl_type_policy_entity(objects, "", errors);
    return errors;
}

bool is_concrete_idl_symbol(entity_type type)
{
    return type == entity_type::STRUCT || type == entity_type::ENUM || type == entity_type::ERROR
           || type == entity_type::INTERFACE || type == entity_type::TYPEDEF || type == entity_type::CLASS;
}

std::string idl_symbol_source(const class_entity& object)
{
    if (!object.get_import_lib().empty())
        return object.get_import_lib();
    return "<current idl>";
}

void validate_duplicate_idl_symbols_entity(
    const class_entity& object,
    const std::string& scope,
    std::map<
        std::string,
        std::string>& symbols,
    std::vector<std::string>& errors)
{
    for (const auto& element : object.get_elements(entity_type::NAMESPACE_MEMBERS | entity_type::STRUCTURE_MEMBERS))
    {
        if (!element)
            continue;

        const auto class_element = std::dynamic_pointer_cast<class_entity>(element);
        if (!class_element)
            continue;

        const auto name = class_element->get_name();
        const auto location = scope.empty() || name.empty() ? name : scope + "::" + name;
        const auto entity_type = class_element->get_entity_type();

        if (is_concrete_idl_symbol(entity_type) && !location.empty())
        {
            const auto source = idl_symbol_source(*class_element);
            const auto existing = symbols.find(location);
            if (existing == symbols.end())
            {
                symbols.emplace(location, source);
            }
            else if (existing->second != source || source == "<current idl>")
            {
                errors.push_back("duplicate IDL symbol '" + location + "' from " + existing->second + " and " + source);
            }
        }

        validate_duplicate_idl_symbols_entity(*class_element, location, symbols, errors);
    }
}

std::vector<std::string> validate_duplicate_idl_symbols(const class_entity& objects)
{
    std::vector<std::string> errors;
    std::map<std::string, std::string> symbols;
    validate_duplicate_idl_symbols_entity(objects, "", symbols, errors);
    return errors;
}

int main(
    const int argc,
    char* argv[])
{
    try
    {
        args::ArgumentParser args_parser("Generate C++ headers and source from idl files");
        args::HelpFlag h(args_parser, "help", "help", {"help"});

        args::ValueFlag<std::string> name_arg(
            args_parser,
            "name",
            "the base name for generated files (e.g., 'example_shared' or 'rpc_types')",
            {'n', "name"},
            args::Options::Required);
        args::ValueFlag<std::string> root_idl_arg(
            args_parser, "path", "the idl to be parsed", {'i', "idl"}, args::Options::Required);
        args::ValueFlag<std::string> output_path_arg(
            args_parser, "path", "the base output path", {'p', "output_path"}, args::Options::Required);
        args::ValueFlag<std::string> mock_path_arg(
            args_parser, "path", "the generated mock relative filename", {'m', "mock"});
        args::ValueFlag<std::string> schema_id_base_arg(
            args_parser,
            "uri",
            "base URI prepended to generated JSON Schema $id paths",
            {"schema_id_base"},
            "https://schemas.canopy.dev/");
        args::Flag yas_arg(args_parser, "yas", "enable YAS serialization generation", {'y', "yas"});
        args::Flag yas_binary_arg(args_parser, "yas_binary", "enable YAS binary serialization generation", {"yas_binary"});
        args::Flag yas_compressed_binary_arg(
            args_parser,
            "yas_compressed_binary",
            "enable YAS compressed binary serialization generation",
            {"yas_compressed_binary"});
        args::Flag yas_json_arg(args_parser, "yas_json", "enable YAS JSON serialization generation", {"yas_json"});
        args::Flag protobuf_arg(
            args_parser, "protobuf", "enable Protocol Buffers serialization generation", {'b', "protobuf"});
        args::Flag nanopb_arg(args_parser, "nanopb", "enable Nanopb serialization generation", {"nanopb"});
        args::Flag canonical_crypto_arg(
            args_parser,
            "canonical_crypto",
            "enable deterministic canonical_crypto serialization generation",
            {"canonical_crypto"});
        args::Flag javascript_arg(
            args_parser, "javascript", "enable JavaScript proxy/stub generation", {'j', "javascript"});
        args::Flag rust_arg(args_parser, "rust", "enable initial Rust constants generation", {'R', "rust"});
        args::ValueFlag<std::string> rest_metadata_arg(
            args_parser, "path", "enable generated REST caller using REST metadata", {"rest_client"});
        args::Flag suppress_catch_stub_exceptions_arg(
            args_parser, "suppress_catch_stub_exceptions", "catch stub exceptions", {'c', "suppress_catch_stub_exceptions"});
        args::ValueFlagList<std::string> include_paths_arg(
            args_parser, "name", "locations of include files used by the idl", {'P', "path"});
        args::ValueFlagList<std::string> namespaces_arg(
            args_parser, "namespace", "namespace of the generated interface", {'N', "namespace"});
        args::Flag dump_preprocessor_output_and_die_arg(
            args_parser, "dump_preprocessor", "dump preprocessor", {'d', "dump_preprocessor"});
        args::ValueFlagList<std::string> defines_arg(args_parser, "define", "macro define", {'D'});
        args::ValueFlagList<std::string> additional_headers_arg(
            args_parser, "header", "additional header to be added to the idl generated header", {'H', "additional_headers"});
        args::ValueFlagList<std::string> rethrow_exceptions_arg(
            args_parser, "exception", "exceptions that should be rethrown", {'r', "rethrow_stub_exception"});
        args::ValueFlagList<std::string> additional_stub_headers_arg(
            args_parser, "header", "additional stub headers", {'A', "additional_stub_header"});
        args::Flag no_include_rpc_headers_arg(
            args_parser, "include rpc headers", "include rpc headers", {"no_include_rpc_headers"});

        try
        {
            args_parser.ParseCLI(argc, argv);
        }
        catch (const args::Help&)
        {
            std::cout << args_parser;
            return 0;
        }
        catch (const args::ParseError& e)
        {
            std::cerr << e.what() << std::endl;
            std::cerr << args_parser;
            return 1;
        }

        string name = args::get(name_arg);
        std::filesystem::path root_idl = args::get(root_idl_arg);
        std::filesystem::path output_path = args::get(output_path_arg);
        std::filesystem::path mock_path = args::get(mock_path_arg);
        std::string schema_id_base = args::get(schema_id_base_arg);
        bool enable_yas = args::get(yas_arg);
        yas_serialization_options yas_options;
        yas_options.binary = enable_yas || args::get(yas_binary_arg);
        yas_options.compressed_binary = enable_yas || args::get(yas_compressed_binary_arg);
        yas_options.json = enable_yas || args::get(yas_json_arg);
        enable_yas = yas_options.any();
        bool enable_protobuf = args::get(protobuf_arg);
        bool enable_nanopb = args::get(nanopb_arg);
        bool enable_canonical_crypto = args::get(canonical_crypto_arg);
        bool enable_javascript = args::get(javascript_arg);
        bool enable_rust = args::get(rust_arg);
        std::filesystem::path rest_metadata_path = args::get(rest_metadata_arg);
        bool enable_rest_client = !rest_metadata_path.empty();
        std::vector<std::string> namespaces = args::get(namespaces_arg);
        std::vector<std::string> include_paths = args::get(include_paths_arg);
        std::vector<std::string> defines = args::get(defines_arg);
        bool suppress_catch_stub_exceptions = args::get(suppress_catch_stub_exceptions_arg);
        std::vector<std::string> rethrow_exceptions = args::get(rethrow_exceptions_arg);
        std::vector<std::string> additional_headers = args::get(additional_headers_arg);
        std::vector<std::string> additional_stub_headers = args::get(additional_stub_headers_arg);
        bool dump_preprocessor_output_and_die = args::get(dump_preprocessor_output_and_die_arg);

        // Normalize path separators
        root_idl = root_idl.lexically_normal();
        mock_path = mock_path.lexically_normal();
        output_path = output_path.lexically_normal();
        rest_metadata_path = rest_metadata_path.lexically_normal();
        // Extract immediate parent directory from IDL path using filesystem::path
        // root_idl could be (absolute paths):
        //   - "/path/to/example_shared/example_shared.idl" -> directory = "example_shared"
        //   - "/path/to/rpc/rpc_types.idl" -> directory = "rpc"
        //   - "/path/to/example.idl" -> directory = "" (empty)
        string directory;
        auto parent_path = root_idl.parent_path();
        if (!parent_path.empty() && parent_path.has_filename())
        {
            // Get just the immediate parent directory name, not the full path
            directory = parent_path.filename().string();
        }
        else
        {
            directory = ""; // No directory - IDL in root
        }

        // Auto-generate module_name by appending "_idl" to name parameter
        std::string module_name = name + "_idl";

        // Construct file paths - if directory is empty, don't prepend it
        string path_prefix = directory.empty() ? name : directory + "/" + name;
        string header_path = path_prefix + ".h";
        string proxy_path = path_prefix + "_proxy.cpp";
        string stub_path = path_prefix + "_stub.cpp";
        string stub_header_path = path_prefix + "_stub.h";

        std::unique_ptr<macro_parser> parser = std::unique_ptr<macro_parser>(new macro_parser());

        for (auto& define : defines)
        {
            auto elems = split(define, '=');
            {
                macro_parser::definition def;
                const std::string& defName = elems[0];
                if (elems.size() > 1)
                {
                    def.m_substitutionString = elems[1];
                }
                parser->AddDefine(defName, def);
            }
        }

        std::string pre_parsed_data;

        {
            macro_parser::definition def;
            def.m_substitutionString = "1";
            parser->AddDefine("GENERATOR", def);
        }

        std::error_code ec;

        auto idl = std::filesystem::absolute(root_idl, ec);
        if (!std::filesystem::exists(idl))
        {
            cout << "Error file " << idl << " does not exist";
            return -1;
        }

        std::vector<std::filesystem::path> parsed_paths;
        for (auto& path : include_paths)
        {
            parsed_paths.emplace_back(std::filesystem::canonical(path, ec).make_preferred());
        }

        std::vector<std::string> loaded_includes;

        std::stringstream output_stream;
        auto r = parser->load(output_stream, root_idl.string(), parsed_paths, loaded_includes);
        if (!r)
        {
            std::cerr << "unable to load " << root_idl << '\n';
            return -1;
        }
        pre_parsed_data = output_stream.str();
        if (dump_preprocessor_output_and_die)
        {
            std::cout << pre_parsed_data << '\n';
            return 0;
        }

        // load the idl file
        auto objects = std::make_shared<class_entity>(nullptr);
        const auto* ppdata = pre_parsed_data.data();
        objects->parse_structure(ppdata, true, false);
        const auto idl_policy_errors = validate_idl_type_policy(*objects);
        const auto duplicate_symbol_errors = validate_duplicate_idl_symbols(*objects);
        if (!idl_policy_errors.empty() || !duplicate_symbol_errors.empty())
        {
            std::cerr << "IDL validation failed:\n";
            for (const auto& error : idl_policy_errors)
                std::cerr << "  " << error << '\n';
            for (const auto& error : duplicate_symbol_errors)
                std::cerr << "  " << error << '\n';
            return -1;
        }

        std::list<std::string> imports;
        {
            std::set<std::string> imports_cache;
            if (!objects->get_import_lib().empty())
            {
                std::cout << "root object has a non empty import lib\n";
                return -1;
            }

            get_imports(*objects, imports, imports_cache);
        }

        stub_header_path = stub_header_path.size() ? stub_header_path : (stub_path + ".h");

        // do the generation of the checksums, in a directory that matches the main header one
        auto checksums_path = output_path / "check_sums";
        std::filesystem::create_directories(checksums_path);
        component_checksum::write_namespace(*objects, checksums_path);

        // do the generation of the proxy and stubs
        {
            auto header_fs_path = output_path / "include" / header_path;
            auto proxy_fs_path = output_path / "src" / proxy_path;
            auto stub_fs_path = output_path / "src" / stub_path;
            auto stub_header_fs_path = output_path / "include" / stub_header_path;
            auto mock_fs_path = output_path / "include" / mock_path;

            std::filesystem::create_directories(header_fs_path.parent_path());
            std::filesystem::create_directories(proxy_fs_path.parent_path());
            std::filesystem::create_directories(stub_fs_path.parent_path());
            std::filesystem::create_directories(stub_header_fs_path.parent_path());
            if (!mock_path.empty())
                std::filesystem::create_directories(mock_fs_path.parent_path());

            // read the original data and close the files afterwards
            string interfaces_h_data;
            string interfaces_proxy_data;
            string interfaces_proxy_header_data;
            string interfaces_stub_data;
            string interfaces_stub_header_data;
            string interfaces_mock_data;

            {
                ifstream hfs(header_fs_path);
                std::getline(hfs, interfaces_h_data, '\0');

                ifstream proxy_fs(proxy_fs_path);
                std::getline(proxy_fs, interfaces_proxy_data, '\0');

                ifstream stub_fs(stub_fs_path);
                std::getline(stub_fs, interfaces_stub_data, '\0');

                ifstream stub_header_fs(stub_header_fs_path);
                std::getline(stub_header_fs, interfaces_stub_header_data, '\0');

                if (!mock_path.empty())
                {
                    ifstream mock_fs(mock_fs_path);
                    std::getline(mock_fs, interfaces_mock_data, '\0');
                }
            }

            std::stringstream header_stream;
            std::stringstream proxy_stream;
            std::stringstream stub_stream;
            std::stringstream stub_header_stream;
            std::stringstream mock_stream;

            synchronous_generator::write_files(
                true,
                *objects,
                header_stream,
                proxy_stream,
                stub_stream,
                stub_header_stream,
                namespaces,
                header_path,
                stub_header_path,
                imports,
                additional_headers,
                !suppress_catch_stub_exceptions,
                rethrow_exceptions,
                additional_stub_headers,
                !no_include_rpc_headers_arg,
                yas_options,
                enable_protobuf,
                enable_nanopb,
                enable_canonical_crypto,
                enable_rest_client);

            header_stream << ends;
            proxy_stream << ends;
            stub_stream << ends;
            stub_header_stream << ends;
            if (!mock_path.empty())
            {
                synchronous_mock_generator::write_files(true, *objects, mock_stream, namespaces, header_path);
                mock_stream << ends;
            }

            // compare and write if different
            if (is_different(header_stream, interfaces_h_data))
            {
                ofstream file(header_fs_path);
                file << header_stream.str();
            }
            if (is_different(proxy_stream, interfaces_proxy_data))
            {
                ofstream file(proxy_fs_path);
                file << proxy_stream.str();
            }
            if (is_different(stub_stream, interfaces_stub_data))
            {
                ofstream file(stub_fs_path);
                file << stub_stream.str();
            }
            if (is_different(stub_header_stream, interfaces_stub_header_data))
            {
                ofstream file(stub_header_fs_path);
                file << stub_header_stream.str();
            }
            if (!mock_path.empty())
            {
                if (interfaces_mock_data != mock_stream.str())
                {
                    ofstream file(mock_fs_path);
                    file << mock_stream.str();
                }
            }
        }

        // do the generation of the yas serialisation
        if (enable_yas)
        {
            // get default paths
            auto pos = header_path.rfind(".h");
            if (pos == std::string::npos)
            {
                std::cerr << "failed looking for a .h suffix " << header_path << '\n';
                return -1;
            }

            auto file_path = header_path.substr(0, pos) + ".cpp";
            auto tmp_header_path = output_path / "src" / file_path;

            // then generate yas subdirectories
            auto header_fs_path = tmp_header_path.parent_path() / "yas" / tmp_header_path.filename();

            std::filesystem::create_directories(header_fs_path.parent_path());

            // read the original data and close the files afterwards
            string interfaces_h_data;

            {
                std::error_code ec;
                ifstream hfs(header_fs_path);
                std::getline(hfs, interfaces_h_data, '\0');
            }

            std::stringstream header_stream;

            yas_generator::write_files(
                true,
                *objects,
                header_stream,
                namespaces,
                header_path,
                yas_options,
                !suppress_catch_stub_exceptions,
                rethrow_exceptions,
                additional_stub_headers);

            header_stream << ends;

            // compare and write if different
            if (is_different(header_stream, interfaces_h_data))
            {
                ofstream file(header_fs_path);
                file << header_stream.str();
            }
        }

        if (enable_rest_client)
        {
            auto rest_cpp_path = output_path / "src" / (path_prefix + "_rest.cpp");
            std::filesystem::create_directories(rest_cpp_path.parent_path());

            std::string existing_rest_cpp_data;
            {
                std::ifstream rest_cpp_fs(rest_cpp_path);
                std::getline(rest_cpp_fs, existing_rest_cpp_data, '\0');
            }

            std::stringstream rest_cpp_stream;
            rest_generator::write_files(*objects, rest_cpp_stream, namespaces, header_path, rest_metadata_path);
            rest_cpp_stream << ends;

            if (is_different(rest_cpp_stream, existing_rest_cpp_data))
            {
                std::ofstream file(rest_cpp_path);
                file << rest_cpp_stream.str();
            }
        }

        // do the generation of the protobuf definitions
        if (enable_protobuf || enable_nanopb)
        {
            // Construct protobuf path from proxy_path
            // proxy_path is like "example/example_proxy.cpp"
            // we want "example/protobuf" as subdirectory
            std::filesystem::path proxy_path_obj(proxy_path);
            std::string parent_directory = proxy_path_obj.parent_path().string();
            std::string base_filename = proxy_path_obj.stem().string();

            // Remove "_proxy" suffix from base filename if present
            if (base_filename.size() > 6 && base_filename.substr(base_filename.size() - 6) == "_proxy")
            {
                base_filename = base_filename.substr(0, base_filename.size() - 6);
            }

            // Construct subdirectory as "parent_directory/protobuf"
            std::filesystem::path sub_directory = parent_directory.empty()
                                                      ? std::filesystem::path("protobuf")
                                                      : std::filesystem::path(parent_directory) / "protobuf";

            auto generated_proto_files = protobuf_generator::write_files(
                *objects, output_path, sub_directory, std::filesystem::path(base_filename));

            if (enable_rust)
            {
                rust_protobuf_generator::write_file(
                    *objects, output_path, std::filesystem::path(path_prefix), sub_directory, base_filename, generated_proto_files);
            }

            // Generate the protobuf C++ serialization file
            std::string protobuf_cpp_filename = base_filename + ".cpp";
            auto protobuf_cpp_path = output_path / "src" / sub_directory / protobuf_cpp_filename;
            std::string nanopb_cpp_filename = base_filename + "_nanopb.cpp";
            auto nanopb_cpp_path = output_path / "src" / sub_directory / nanopb_cpp_filename;
            // Include path for the aggregator .pb.h file with full module path
            auto protobuf_include_path = sub_directory / (base_filename + "_all.pb.h");
            auto nanopb_include_path = sub_directory / (base_filename + "_all.pb.h");

            if (enable_protobuf)
            {
                std::stringstream protobuf_cpp_stream;
                protobuf_generator::write_cpp_files(
                    *objects,
                    protobuf_cpp_stream,
                    namespaces,
                    std::filesystem::path(header_path),
                    protobuf_include_path,
                    additional_stub_headers);

                // Append fingerprint data as comments to ensure type changes trigger rebuild
                // even if the serialization code itself doesn't change
                protobuf_cpp_stream
                    << "\n// Type Fingerprints - DO NOT EDIT (auto-generated for dependency tracking)\n";
                {
                    auto checksums_path_for_embed = output_path / "check_sums";
                    if (std::filesystem::exists(checksums_path_for_embed))
                    {
                        // Iterate through status subdirectories
                        for (const auto& status_dir : std::filesystem::directory_iterator(checksums_path_for_embed))
                        {
                            if (status_dir.is_directory())
                            {
                                std::string status = status_dir.path().filename().string();
                                for (const auto& type_file : std::filesystem::directory_iterator(status_dir))
                                {
                                    if (type_file.is_regular_file())
                                    {
                                        std::string full_name = type_file.path().filename().string();
                                        std::ifstream fingerprint_file(type_file.path());
                                        std::string fingerprint;
                                        std::getline(fingerprint_file, fingerprint);
                                        protobuf_cpp_stream << "// " << full_name << "," << status << "," << fingerprint
                                                            << "\n";
                                    }
                                }
                            }
                        }
                    }
                }

                protobuf_cpp_stream << ends;

                // Read existing file if it exists
                std::string existing_protobuf_cpp_data;
                if (std::filesystem::exists(protobuf_cpp_path))
                {
                    std::ifstream existing_file(protobuf_cpp_path);
                    std::getline(existing_file, existing_protobuf_cpp_data, '\0');
                }

                // Write if different or doesn't exist
                if (is_different(protobuf_cpp_stream, existing_protobuf_cpp_data))
                {
                    std::ofstream protobuf_cpp_file(protobuf_cpp_path);
                    protobuf_cpp_file << protobuf_cpp_stream.str();
                }
            }

            if (enable_nanopb)
            {
                std::stringstream nanopb_cpp_stream;
                nanopb_generator::write_cpp_files(
                    *objects,
                    nanopb_cpp_stream,
                    namespaces,
                    std::filesystem::path(header_path),
                    nanopb_include_path,
                    additional_stub_headers);
                nanopb_cpp_stream << ends;

                std::string existing_nanopb_cpp_data;
                if (std::filesystem::exists(nanopb_cpp_path))
                {
                    std::ifstream existing_file(nanopb_cpp_path);
                    std::getline(existing_file, existing_nanopb_cpp_data, '\0');
                }

                if (is_different(nanopb_cpp_stream, existing_nanopb_cpp_data))
                {
                    std::ofstream nanopb_cpp_file(nanopb_cpp_path);
                    nanopb_cpp_file << nanopb_cpp_stream.str();
                }
            }
        }

        // Generate JavaScript proxy/stub file
        if (enable_javascript)
        {
            javascript_generator::write_files(*objects, output_path, std::filesystem::path(name));
        }

        if (enable_rust)
        {
            rust_generator::write_file(*objects, output_path, std::filesystem::path(path_prefix));
        }

        {
            auto pos = header_path.rfind(".h");
            if (pos == std::string::npos)
            {
                std::cerr << "failed looking for a .h suffix " << header_path << '\n';
                return -1;
            }

            auto file_path = header_path.substr(0, pos) + ".json";

            auto json_schema_fs_path = output_path / "json_schema" / file_path;
            auto json_schema_header_fs_path = output_path / "include" / (header_path.substr(0, pos) + "_schema.h");

            std::filesystem::create_directories(json_schema_fs_path.parent_path());
            std::filesystem::create_directories(json_schema_header_fs_path.parent_path());

            // read the original data and close the files afterwards
            string json_schema_data;
            string json_schema_header_data;

            {
                ifstream hfs(json_schema_fs_path);
                std::getline(hfs, json_schema_data, '\0');
            }
            {
                ifstream hfs(json_schema_header_fs_path);
                std::getline(hfs, json_schema_header_data, '\0');
            }

            std::stringstream json_schema_stream;

            // The standalone .json document carries a deterministic $id
            // (= file_path) so the composed topology schema can cross-file $ref
            // it. The embedded _schema.h schema is self-contained (runtime
            // validation, intra-doc refs only) and emits no $id: it would
            // otherwise stamp the same $id on every per-struct/per-interface
            // document in the header, which is invalid (duplicate $ids).
            auto json_document_profile = json_schema::config_strict_profile();
            json_document_profile.id_base = schema_id_base;
            json_document_profile.id_path = file_path;

            // Generate the JSON Schema
            json_schema::write_json_schema(*objects, json_schema_stream, module_name, json_document_profile);

            const auto json_schema_string = json_schema_stream.str();
            if (is_different(json_schema_stream, json_schema_data))
            {
                ofstream file(json_schema_fs_path);
                file << json_schema_string;
            }

            const auto json_schema_header = make_json_schema_header(
                module_name, *objects, header_path, imports, json_schema::config_strict_profile());
            if (json_schema_header != json_schema_header_data)
            {
                ofstream file(json_schema_header_fs_path);
                file << json_schema_header;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}
