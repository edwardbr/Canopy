/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <connection_factory_components.h>
#include <json/config.h>
#include <json/json_dom.h>

namespace
{
    struct options
    {
        std::filesystem::path app_schema;
        std::filesystem::path generated_schema_dir;
        std::filesystem::path output_dir;
        std::string output_schema;
        std::string root_definition;
    };

    struct component_schema
    {
        std::string type;
        std::string schema_path;
        std::string definition;
    };

    [[nodiscard]] bool starts_with(
        std::string_view value,
        std::string_view prefix)
    {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    [[nodiscard]] auto parse_options(
        int argc,
        char** argv) -> options
    {
        options result;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg(argv[i]);
            auto require_value = [&]
            {
                if (i + 1 >= argc)
                    throw std::runtime_error("missing value for " + std::string(arg));
                return std::string(argv[++i]);
            };

            if (arg == "--app-schema")
                result.app_schema = require_value();
            else if (arg == "--generated-schema-dir")
                result.generated_schema_dir = require_value();
            else if (arg == "--output-dir")
                result.output_dir = require_value();
            else if (arg == "--output-schema")
                result.output_schema = require_value();
            else if (arg == "--root-definition")
                result.root_definition = require_value();
            else
                throw std::runtime_error("unknown argument " + std::string(arg));
        }

        if (result.app_schema.empty())
            throw std::runtime_error("--app-schema is required");
        if (result.generated_schema_dir.empty())
            throw std::runtime_error("--generated-schema-dir is required");
        if (result.output_dir.empty())
            throw std::runtime_error("--output-dir is required");
        if (result.output_schema.empty())
            throw std::runtime_error("--output-schema is required");
        return result;
    }

    [[nodiscard]] auto as_map(
        const json::v1::object& value,
        std::string_view name) -> json::v1::map
    {
        if (value.get_type() != json::v1::object::type::map_type)
            throw std::runtime_error(std::string(name) + " must be a JSON object");
        return value.as_map();
    }

    [[nodiscard]] auto as_array(
        const json::v1::object& value,
        std::string_view name) -> json::v1::array
    {
        if (value.get_type() != json::v1::object::type::array_type)
            throw std::runtime_error(std::string(name) + " must be a JSON array");
        return value.as_array();
    }

    [[nodiscard]] auto schema_path_from_id(const std::string& schema_id) -> std::string
    {
        const std::string prefix(CANOPY_SCHEMA_ID_BASE);
        if (!starts_with(schema_id, prefix))
        {
            throw std::runtime_error(
                "schema id '" + schema_id + "' does not start with configured CANOPY_SCHEMA_ID_BASE '" + prefix + "'");
        }
        return schema_id.substr(prefix.size());
    }

    [[nodiscard]] auto schema_ref(const component_schema& component) -> std::string
    {
        if (component.schema_path.empty())
            return component.definition;
        return component.schema_path + component.definition;
    }

    [[nodiscard]] auto component_from_descriptor(const rpc::connection_factory::detail::component_descriptor& descriptor)
        -> component_schema
    {
        return {descriptor.type, schema_path_from_id(descriptor.settings_schema_id), descriptor.settings_definition};
    }

    [[nodiscard]] auto unique_sorted(std::vector<component_schema> components) -> std::vector<component_schema>
    {
        std::sort(
            components.begin(), components.end(), [](const auto& lhs, const auto& rhs) { return lhs.type < rhs.type; });
        components.erase(
            std::unique(
                components.begin(),
                components.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.type == rhs.type; }),
            components.end());
        return components;
    }

    [[nodiscard]] auto descriptor_components(
        const std::vector<rpc::connection_factory::detail::component_descriptor>& descriptors)
        -> std::vector<component_schema>
    {
        std::vector<component_schema> result;
        result.reserve(descriptors.size());
        for (const auto& descriptor : descriptors)
        {
            if (descriptor.settings_schema_id.empty() || descriptor.settings_definition.empty())
                continue;
            result.push_back(component_from_descriptor(descriptor));
        }
        return unique_sorted(std::move(result));
    }

    [[nodiscard]] auto local_component(
        std::string type,
        std::string definition) -> component_schema
    {
        return {std::move(type), {}, std::move(definition)};
    }

    [[nodiscard]] auto type_enum(const std::vector<component_schema>& components) -> json::v1::array
    {
        json::v1::array values;
        values.reserve(components.size());
        for (const auto& component : components)
            values.push_back(json::v1::object(component.type));
        return values;
    }

    [[nodiscard]] auto const_type_condition(const std::string& type) -> json::v1::object
    {
        return json::v1::object(
            json::v1::map{{"properties", json::v1::map{{"type", json::v1::map{{"const", type}}}}},
                {"required", json::v1::array{"type"}}});
    }

    [[nodiscard]] auto settings_ref_branch(const component_schema& component) -> json::v1::object
    {
        return json::v1::object(
            json::v1::map{{"if", const_type_condition(component.type)},
                {"then",
                    json::v1::map{
                        {"properties", json::v1::map{{"settings", json::v1::map{{"$ref", schema_ref(component)}}}}}}}});
    }

    [[nodiscard]] auto nullable_ref(
        std::string ref,
        const json::v1::object& original_property) -> json::v1::object
    {
        auto result = as_map(original_property, "connection property schema");
        result["oneOf"] = json::v1::array{json::v1::map{{"$ref", std::move(ref)}}, json::v1::map{{"type", "null"}}};
        return json::v1::object(std::move(result));
    }

    void patch_typed_envelope(
        json::v1::map& definitions,
        const std::string& definition_name,
        const std::vector<component_schema>& components)
    {
        auto definition = as_map(definitions.at(definition_name), definition_name);
        auto properties = as_map(definition.at("properties"), definition_name + ".properties");
        auto type_property = as_map(properties.at("type"), definition_name + ".properties.type");
        type_property["enum"] = type_enum(components);
        properties["type"] = std::move(type_property);
        definition["properties"] = std::move(properties);

        json::v1::array all_of;
        const auto existing_all_of = definition.find("allOf");
        if (existing_all_of != definition.end())
            all_of = as_array(existing_all_of->second, definition_name + ".allOf");

        for (const auto& component : components)
            all_of.push_back(settings_ref_branch(component));

        if (!all_of.empty())
            definition["allOf"] = std::move(all_of);
        definitions[definition_name] = std::move(definition);
    }

    void patch_connection_settings(json::v1::map& definitions)
    {
        auto connection_settings = as_map(
            definitions.at("rpc_connection_factory_connection_settings"), "rpc_connection_factory_connection_settings");
        auto properties
            = as_map(connection_settings.at("properties"), "rpc_connection_factory_connection_settings.properties");

        properties["service"]
            = nullable_ref("#/definitions/rpc_connection_factory_service_typed_settings", properties.at("service"));
        properties["listener"]
            = nullable_ref("#/definitions/rpc_connection_factory_listener_typed_settings", properties.at("listener"));
        properties["transport"]
            = nullable_ref("#/definitions/rpc_connection_factory_transport_typed_settings", properties.at("transport"));

        connection_settings["properties"] = std::move(properties);
        definitions["rpc_connection_factory_connection_settings"] = std::move(connection_settings);
    }

    void add_editor_schema_property(
        json::v1::map& definitions,
        const std::string& root_definition)
    {
        auto root = as_map(definitions.at(root_definition), root_definition);
        auto properties = as_map(root.at("properties"), root_definition + ".properties");
        properties["$schema"] = json::v1::map{
            {"description", "JSON schema used by editors; ignored by the application at runtime."}, {"type", "string"}};
        root["properties"] = std::move(properties);
        definitions[root_definition] = std::move(root);
    }

    void ensure_service_settings_definition(json::v1::map& definitions)
    {
        if (definitions.find("rpc_connection_factory_service_settings") != definitions.end())
            return;

        definitions["rpc_connection_factory_service_settings"] = json::v1::map{{"type", "object"},
            {"properties",
                json::v1::map{{"name",
                    json::v1::map{
                        {"description",
                            "Root service name used only when the caller did not pass an existing service to the "
                            "factory."},
                        {"oneOf", json::v1::array{json::v1::map{{"type", "string"}}, json::v1::map{{"type", "null"}}}}}}}},
            {"additionalProperties", false}};
    }

    [[nodiscard]] bool imported_definition_name(std::string_view name)
    {
        return starts_with(name, "rpc_") || starts_with(name, "json_") || starts_with(name, "streaming_")
               || starts_with(name, "canopy_") || starts_with(name, "std_");
    }

    [[nodiscard]] auto infer_root_definition(
        const json::v1::map& definitions,
        const std::string& explicit_root) -> std::string
    {
        if (!explicit_root.empty())
        {
            if (definitions.find(explicit_root) == definitions.end())
                throw std::runtime_error("root definition '" + explicit_root + "' was not found in the app schema");
            return explicit_root;
        }

        std::vector<std::string> candidates;
        for (const auto& [name, definition] : definitions)
        {
            if (imported_definition_name(name))
                continue;
            if (definition.get_type() == json::v1::object::type::map_type)
                candidates.push_back(name);
        }
        std::sort(candidates.begin(), candidates.end());

        if (candidates.size() == 1)
            return candidates.front();

        std::string candidate_list;
        for (const auto& candidate : candidates)
        {
            if (!candidate_list.empty())
                candidate_list += ", ";
            candidate_list += candidate;
        }
        throw std::runtime_error(
            "could not infer a single application root definition; pass ROOT_DEFINITION explicitly"
            + (candidate_list.empty() ? std::string{} : " (candidates: " + candidate_list + ")"));
    }

    void copy_component_schemas(
        const std::filesystem::path& generated_schema_dir,
        const std::filesystem::path& output_dir,
        const std::vector<component_schema>& components)
    {
        std::unordered_set<std::string> copied;
        for (const auto& component : components)
        {
            if (component.schema_path.empty() || !copied.insert(component.schema_path).second)
                continue;

            const auto source = generated_schema_dir / component.schema_path;
            const auto destination = output_dir / component.schema_path;
            if (!std::filesystem::exists(source))
                throw std::runtime_error("component schema does not exist: " + source.string());

            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
        }
    }

    void write_schema(
        const std::filesystem::path& output_path,
        const json::v1::object& schema)
    {
        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output(output_path);
        if (!output)
            throw std::runtime_error("failed to open " + output_path.string());
        output << json::v1::dump(schema) << '\n';
    }

    [[nodiscard]] auto compose_schema(const options& opts) -> json::v1::object
    {
        auto schema = json::v1::parse_file(opts.app_schema);
        auto root = as_map(schema, "app schema");
        auto definitions = as_map(root.at("definitions"), "app schema definitions");

        const auto root_definition = infer_root_definition(definitions, opts.root_definition);
        root["$id"] = opts.output_schema;
        root["$ref"] = "#/definitions/" + root_definition;

        auto transport_components
            = descriptor_components(rpc::connection_factory::detail::built_in_transport_component_descriptors());
        auto listener_components = std::vector<component_schema>{component_schema{"stream_rpc",
            "stream_transport/stream_transport_config.json",
            "#/definitions/rpc_stream_transport_listener_settings"}};
        auto service_components = std::vector<component_schema>{
            local_component("root_service", "#/definitions/rpc_connection_factory_service_settings"),
            local_component("service", "#/definitions/rpc_connection_factory_service_settings")};
        ensure_service_settings_definition(definitions);

        auto generic_typed_components = transport_components;
        generic_typed_components.insert(
            generic_typed_components.end(), service_components.begin(), service_components.end());
        generic_typed_components = unique_sorted(std::move(generic_typed_components));

        auto stream_components
            = descriptor_components(rpc::connection_factory::detail::built_in_stream_component_descriptors());
        auto stream_layer_components
            = descriptor_components(rpc::connection_factory::detail::built_in_stream_layer_descriptors());
        stream_components.insert(stream_components.end(), stream_layer_components.begin(), stream_layer_components.end());
        stream_components = unique_sorted(std::move(stream_components));

        definitions["rpc_connection_factory_service_typed_settings"]
            = definitions.at("rpc_connection_factory_typed_settings");
        definitions["rpc_connection_factory_listener_typed_settings"]
            = definitions.at("rpc_connection_factory_typed_settings");
        definitions["rpc_connection_factory_transport_typed_settings"]
            = definitions.at("rpc_connection_factory_typed_settings");

        patch_typed_envelope(definitions, "rpc_connection_factory_typed_settings", generic_typed_components);
        patch_typed_envelope(definitions, "rpc_connection_factory_service_typed_settings", service_components);
        patch_typed_envelope(definitions, "rpc_connection_factory_listener_typed_settings", listener_components);
        patch_typed_envelope(definitions, "rpc_connection_factory_transport_typed_settings", transport_components);
        patch_typed_envelope(definitions, "rpc_stream_layers_stream_layer_settings", stream_components);
        patch_connection_settings(definitions);
        add_editor_schema_property(definitions, root_definition);

        root["definitions"] = std::move(definitions);
        copy_component_schemas(opts.generated_schema_dir, opts.output_dir, generic_typed_components);
        copy_component_schemas(opts.generated_schema_dir, opts.output_dir, stream_components);
        return json::v1::object(std::move(root));
    }
} // namespace

int main(
    int argc,
    char** argv)
{
    try
    {
        const auto opts = parse_options(argc, argv);
        const auto schema = compose_schema(opts);
        write_schema(opts.output_dir / opts.output_schema, schema);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "config_schema_compose failed: " << error.what() << '\n';
        return 1;
    }
}
