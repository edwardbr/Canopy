/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_set>

// Other headers
extern "C"
{
#include "sha3.h"
}
#include "attributes.h"
#include "coreclasses.h"
#include "cpp_parser.h"
#include "fingerprint_generator.h"
#include "helpers.h"
#include "interface_declaration_generator.h"
#include "json_schema/generator.h"
#include "json_schema/per_function_generator.h"
#include "json_schema/writer.h"
#include "../../rpc/include/rpc/internal/build_modifiers.h"
#include "rpc_attributes.h"
#include "synchronous_generator.h"
#include "writer.h"
#include "type_utils.h"
#include <map>

namespace synchronous_generator
{

    struct protocol_version_descriptor
    {
        const char* macro;
        const char* symbol;
        uint64_t value;
    };

    constexpr protocol_version_descriptor protocol_versions[] = {
        {FLD(macro) "RPC_V3", FLD(symbol) "rpc::VERSION_3", FLD(value) 3},
        {FLD(macro) "RPC_V2", FLD(symbol) "rpc::VERSION_2", FLD(value) 2},
    };

    bool is_serialized_pointer_field(const function_entity& field)
    {
        const auto type = field.get_return_type();
        return is_pointer(type) || is_pointer_reference(type) || is_pointer_to_pointer(type);
    }

    std::string pointer_cast_type(std::string type)
    {
        std::string modifiers;
        generator::strip_reference_modifiers(type, modifiers);
        modifiers.erase(std::remove(modifiers.begin(), modifiers.end(), '&'), modifiers.end());
        return type + modifiers;
    }

    enum print_type
    {
        PROXY_PREPARE_IN,
        PROXY_PREPARE_IN_INTERFACE_ID,
        PROXY_MARSHALL_IN,
        PROXY_OUT_DECLARATION,
        PROXY_MARSHALL_OUT,
        PROXY_VALUE_RETURN,
        PROXY_CLEAN_IN,

        STUB_DEMARSHALL_DECLARATION,
        STUB_MARSHALL_IN,
        STUB_PARAM_WRAP,
        STUB_PARAM_CAST,
        STUB_CLEAN_IN,
        STUB_ADD_REF_OUT_PREDECLARE,
        STUB_ADD_REF_OUT,
        STUB_MARSHALL_OUT,

        LOCAL_OPTIMISTIC_PTR_CALL
    };

    // Polymorphic renderer adapter that implements base_renderer interface
    class calling_renderer : public generator::base_renderer
    {
    public:
        calling_renderer() = default;

        // Implement pure virtual functions from base_renderer
        std::string render_by_value(
            int option,
            bool from_host,
            const class_entity& lib,
            const std::string& name,
            bool is_in,
            bool is_out,
            bool is_const,
            const std::string& type_name,
            uint64_t& count) override;

        std::string render_reference(
            int option,
            bool from_host,
            const class_entity& lib,
            const std::string& name,
            bool is_in,
            bool is_out,
            bool is_const,
            const std::string& type_name,
            uint64_t& count) override;

        std::string render_move(
            int option,
            bool from_host,
            const class_entity& lib,
            const std::string& name,
            bool is_in,
            bool is_out,
            bool is_const,
            const std::string& type_name,
            uint64_t& count) override;

        std::string render_pointer(
            int option,
            bool from_host,
            const class_entity& lib,
            const std::string& name,
            bool is_in,
            bool is_out,
            bool is_const,
            const std::string& type_name,
            uint64_t& count) override;

        std::string render_pointer_reference(
            int option,
            bool from_host,
            const class_entity& lib,
            const std::string& name,
            bool is_in,
            bool is_out,
            bool is_const,
            const std::string& type_name,
            uint64_t& count) override;

        std::string render_pointer_pointer(
            int option,
            bool from_host,
            const class_entity& lib,
            const std::string& name,
            bool is_in,
            bool is_out,
            bool is_const,
            const std::string& type_name,
            uint64_t& count) override;

        std::string render_interface(
            int option,
            bool from_host,
            const class_entity& lib,
            const std::string& name,
            bool is_in,
            bool is_out,
            bool is_const,
            const std::string& type_name,
            uint64_t& count) override;

        std::string render_interface_reference(
            int option,
            bool from_host,
            const class_entity& lib,
            const std::string& name,
            bool is_in,
            bool is_out,
            bool is_const,
            const std::string& type_name,
            uint64_t& count) override;
    };

    // Implementation functions for calling_renderer
    std::string calling_renderer::render_by_value(
        int option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        bool is_in,
        bool is_out,
        bool is_const,
        const std::string& object_type,
        uint64_t& count)
    {
        std::ignore = from_host;
        std::ignore = lib;
        std::ignore = is_in;
        std::ignore = is_out;
        std::ignore = is_const;
        std::ignore = count;

        auto pt = static_cast<print_type>(option);
        switch (pt)
        {
        case PROXY_MARSHALL_IN:
            return fmt::format("{0}, ", name);
        case PROXY_MARSHALL_OUT:
            return fmt::format("{0}, ", name);
        case STUB_DEMARSHALL_DECLARATION:
            return fmt::format("{} {}_{{}}", object_type, name);
        case STUB_MARSHALL_IN:
            return fmt::format("{}_, ", name);
        case STUB_PARAM_CAST:
            return fmt::format("{}_", name);
        case STUB_MARSHALL_OUT:
            return fmt::format("{0}_, ", name);

        case LOCAL_OPTIMISTIC_PTR_CALL:
            return fmt::format("{}", name);
        default:
            return "";
        }
    }

    std::string calling_renderer::render_reference(
        int option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        bool is_in,
        bool is_out,
        bool is_const,
        const std::string& object_type,
        uint64_t& count)
    {
        std::ignore = from_host;
        std::ignore = lib;
        std::ignore = is_in;
        std::ignore = is_out;
        std::ignore = is_const;
        std::ignore = count;

        auto pt = static_cast<print_type>(option);
        switch (pt)
        {
        case PROXY_MARSHALL_IN:
            return fmt::format("{0}, ", name);
        case PROXY_MARSHALL_OUT:
            return fmt::format("{0}, ", name);
        case STUB_DEMARSHALL_DECLARATION:
            return fmt::format("{} {}_{{}}", object_type, name);
        case STUB_MARSHALL_IN:
            return fmt::format("{}_, ", name);
        case STUB_PARAM_CAST:
            return fmt::format("{}_", name);
        case STUB_MARSHALL_OUT:
            return fmt::format("{0}_, ", name);

        case LOCAL_OPTIMISTIC_PTR_CALL:
            return fmt::format("{}", name);
        default:
            return "";
        }
    }

    std::string calling_renderer::render_move(
        int option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        bool is_in,
        bool is_out,
        bool is_const,
        const std::string& object_type,
        uint64_t& count)
    {
        std::ignore = from_host;
        std::ignore = lib;
        std::ignore = is_in;
        std::ignore = count;

        if (is_out)
        {
            throw std::runtime_error("MOVE does not support out vals");
        }
        if (is_const)
        {
            throw std::runtime_error("MOVE does not support const vals");
        }

        auto pt = static_cast<print_type>(option);
        switch (pt)
        {
        case PROXY_MARSHALL_IN:
            return fmt::format("{0}, ", name);
        case PROXY_MARSHALL_OUT:
            return fmt::format("{0}, ", name);
        case STUB_DEMARSHALL_DECLARATION:
            return fmt::format("{} {}_{{}}", object_type, name);
        case STUB_MARSHALL_IN:
            return fmt::format("{}_, ", name);
        case STUB_PARAM_CAST:
            return fmt::format("std::forward<{}>({}_)", object_type, name);
        case STUB_MARSHALL_OUT:
            return fmt::format("{0}_, ", name);
        case LOCAL_OPTIMISTIC_PTR_CALL:
            return fmt::format("std::forward<{}>({})", object_type, name);
        default:
            return "";
        }
    }

    std::string calling_renderer::render_pointer(
        int option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        bool is_in,
        bool is_out,
        bool is_const,
        const std::string& object_type,
        uint64_t& count)
    {
        std::ignore = from_host;
        std::ignore = lib;
        std::ignore = is_in;
        std::ignore = is_const;
        std::ignore = count;
        if (is_out)
        {
            throw std::runtime_error("POINTER does not support out vals");
        }

        auto pt = static_cast<print_type>(option);
        switch (pt)
        {
        case PROXY_MARSHALL_IN:
            return fmt::format("reinterpret_cast<uint64_t>({}), ", name);
        case PROXY_MARSHALL_OUT:
            return fmt::format("reinterpret_cast<uint64_t>({}), ", count);
        case STUB_DEMARSHALL_DECLARATION:
            return fmt::format("//boo;\nuint64_t {}_{{}}", name);
        case STUB_MARSHALL_IN:
            return fmt::format("{}_, ", name);
        case STUB_PARAM_CAST:
            return fmt::format("reinterpret_cast<{}*>({}_)", object_type, name);
        case LOCAL_OPTIMISTIC_PTR_CALL:
            return fmt::format("{}", name);
        default:
            return "";
        }
    }

    std::string calling_renderer::render_pointer_reference(
        int option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        bool is_in,
        bool is_out,
        bool is_const,
        const std::string& object_type,
        uint64_t& count)
    {
        std::ignore = from_host;
        std::ignore = lib;
        std::ignore = is_in;
        std::ignore = count;

        if (is_const && is_out)
        {
            throw std::runtime_error("POINTER_REFERENCE does not support const out vals");
        }
        auto pt = static_cast<print_type>(option);
        switch (pt)
        {
        case PROXY_MARSHALL_IN:
            return fmt::format("{0}_, ", name);
        case PROXY_MARSHALL_OUT:
            return fmt::format("{0}_, ", name);
        case STUB_DEMARSHALL_DECLARATION:
            return fmt::format("{}* {}_ = nullptr", object_type, name);
        case STUB_PARAM_CAST:
            return fmt::format("{}_", name);
        case PROXY_OUT_DECLARATION:
            return fmt::format("//bar\nuint64_t {}_ = 0;", name);
        case STUB_MARSHALL_OUT:
            return fmt::format("reinterpret_cast<uint64_t>({}_), ", name);
        case PROXY_VALUE_RETURN:
            return fmt::format("{} = reinterpret_cast<{}*>({}_);", name, object_type, name);
        case LOCAL_OPTIMISTIC_PTR_CALL:
            return fmt::format("{}", name);
        default:
            return "";
        }
    }

    std::string calling_renderer::render_pointer_pointer(
        int option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        bool is_in,
        bool is_out,
        bool is_const,
        const std::string& object_type,
        uint64_t& count)
    {
        std::ignore = from_host;
        std::ignore = lib;
        std::ignore = is_in;
        std::ignore = is_out;
        std::ignore = is_const;
        std::ignore = count;

        auto pt = static_cast<print_type>(option);
        switch (pt)
        {
        case PROXY_MARSHALL_IN:
            return fmt::format("{0}_, ", name);
        case PROXY_MARSHALL_OUT:
            return fmt::format("{0}_, ", name);
        case STUB_DEMARSHALL_DECLARATION:
            return fmt::format("{}* {}_ = nullptr", object_type, name);
        case STUB_PARAM_CAST:
            return fmt::format("&{}_", name);
        case PROXY_VALUE_RETURN:
            return fmt::format("*{} = reinterpret_cast<{}*>({}_);", name, object_type, name);
        case PROXY_OUT_DECLARATION:
            return fmt::format("//hi\nuint64_t {}_ = 0;", name);
        case STUB_MARSHALL_OUT:
            return fmt::format("reinterpret_cast<uint64_t>({}_), ", name);
        case LOCAL_OPTIMISTIC_PTR_CALL:
            return fmt::format("{}", name);
        default:
            return "";
        }
    }

    std::string calling_renderer::render_interface(
        int option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        bool is_in,
        bool is_out,
        bool is_const,
        const std::string& object_type,
        uint64_t& count)
    {
        std::ignore = from_host;
        std::ignore = lib;
        std::ignore = is_in;
        std::ignore = is_const;
        std::ignore = count;

        if (is_out)
        {
            throw std::runtime_error("INTERFACE does not support out vals");
        }

        bool is_optimistic = object_type.find("rpc::optimistic_ptr") != std::string::npos;
        auto template_start = object_type.find('<');
        auto template_end = object_type.rfind('>');
        auto iface_type = object_type.substr(template_start + 1, template_end - template_start - 1);
        auto bind_template_args
            = fmt::format("{}, {}", iface_type, is_optimistic ? "rpc::optimistic_ptr" : "rpc::shared_ptr");

        auto pt = static_cast<print_type>(option);
        switch (pt)
        {
        case PROXY_PREPARE_IN:
            return fmt::format("std::shared_ptr<rpc::object_stub> {}_stub_;", name);
        case PROXY_PREPARE_IN_INTERFACE_ID:
            if (is_optimistic)
            {
                return fmt::format(
                    "rpc::remote_object {0}_stub_id_;\n"
                    "\t\t\tif(!rpc::error::is_error(__rpc_ret))\n"
                    "\t\t\t{{{{\n"
                    "\t\t\t\tauto {0}_bind_result = CO_AWAIT rpc::proxy_bind_in_param(__rpc_get_object_proxy(), "
                    "__rpc_sp->get_remote_rpc_version(), "
                    "{0});\n"
                    "\t\t\t\t__rpc_ret = {0}_bind_result.error_code;\n"
                    "\t\t\t\t{0}_stub_ = std::move({0}_bind_result.stub);\n"
                    "\t\t\t\t{0}_stub_id_ = {0}_bind_result.descriptor;\n"
                    "\t\t\t}}}}",
                    name);
            }
            return fmt::format(
                "RPC_ASSERT(rpc::are_in_same_zone(this, {0}.get()));\n"
                "\t\t\trpc::remote_object {0}_stub_id_;\n"
                "\t\t\tif(!rpc::error::is_error(__rpc_ret))\n"
                "\t\t\t{{{{\n"
                "\t\t\t\tauto {0}_bind_result = CO_AWAIT rpc::proxy_bind_in_param(__rpc_get_object_proxy(), "
                "__rpc_sp->get_remote_rpc_version(), "
                "{0});\n"
                "\t\t\t\t__rpc_ret = {0}_bind_result.error_code;\n"
                "\t\t\t\t{0}_stub_ = std::move({0}_bind_result.stub);\n"
                "\t\t\t\t{0}_stub_id_ = {0}_bind_result.descriptor;\n"
                "\t\t\t}}}}",
                name);
        case PROXY_MARSHALL_IN:
        {
            auto ret = fmt::format("{0}_stub_id_, ", name);
            count++;
            return ret;
        }
        case PROXY_MARSHALL_OUT:
            return fmt::format("{0}_, ", name);

        case PROXY_CLEAN_IN:
            return fmt::format(
                "if({0}_stub_) CO_AWAIT "
                "{0}_stub_->release_from_service(__rpc_sp->get_destination_zone_id());",
                name);

        case STUB_DEMARSHALL_DECLARATION:
            return fmt::format(R"__(rpc::remote_object {0}_object_{{}};)__", name);
        case STUB_MARSHALL_IN:
        {
            auto ret = fmt::format("{}_object_, ", name);
            count++;
            return ret;
        }
        case STUB_PARAM_WRAP:
            return fmt::format(
                R"__(
                {0} {1};
                if(!rpc::error::is_error(__rpc_ret) && {1}_object_.is_set() && {1}_object_.get_object_id().is_set())
                {{
                    auto stub = __rpc_target_->__rpc_get_stub();
                    auto zone_ = stub ? stub->get_zone() : nullptr;
                    if (zone_)
                    {{
                        auto {1}_bind_result = CO_AWAIT rpc::stub_bind_in_param<{2}>(params.protocol_version, zone_, params.caller_zone_id, {1}_object_);
                        __rpc_ret = {1}_bind_result.error_code;
                        {1} = std::move({1}_bind_result.iface);
                    }}
                    else
                    {{
                        assert(false);
                        __rpc_ret = rpc::error::ZONE_NOT_FOUND();
                    }}
                }}
)__",
                object_type,
                name,
                bind_template_args);
        case STUB_PARAM_CAST:
            return fmt::format("{}", name);
        case STUB_CLEAN_IN:
            return fmt::format("{} = nullptr;", name);
        case STUB_MARSHALL_OUT:
            return fmt::format("static_cast<uint64_t>({}), ", name);
        case PROXY_VALUE_RETURN:
        case PROXY_OUT_DECLARATION:
            return fmt::format("  rpc::remote_object {}_;", name);

        case LOCAL_OPTIMISTIC_PTR_CALL:
            return fmt::format("{}", name);
        default:
            return "";
        }
    }

    std::string calling_renderer::render_interface_reference(
        int option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        bool is_in,
        bool is_out,
        bool is_const,
        const std::string& object_type,
        uint64_t& count)
    {
        std::ignore = from_host;
        std::ignore = lib;
        std::ignore = is_in;
        std::ignore = is_out;
        std::ignore = is_const;
        std::ignore = count;

        bool is_optimistic = object_type.find("rpc::optimistic_ptr") != std::string::npos;
        auto template_start = object_type.find('<');
        auto template_end = object_type.rfind('>');
        auto iface_type = object_type.substr(template_start + 1, template_end - template_start - 1);
        auto bind_template_args
            = fmt::format("{}, {}", iface_type, is_optimistic ? "rpc::optimistic_ptr" : "rpc::shared_ptr");

        switch (option)
        {
        case PROXY_PREPARE_IN:
            return fmt::format("std::shared_ptr<rpc::object_stub> {}_stub_;", name);
        case PROXY_PREPARE_IN_INTERFACE_ID:
            if (is_optimistic)
            {
                return fmt::format(
                    "rpc::remote_object {0}_stub_id_;\n"
                    "\t\t\tif(!rpc::error::is_error(__rpc_ret))\n"
                    "\t\t\t{{{{\n"
                    "\t\t\t\tauto {0}_bind_result = CO_AWAIT rpc::proxy_bind_in_param(__rpc_get_object_proxy(), "
                    "__rpc_sp->get_remote_rpc_version(), "
                    "{0});\n"
                    "\t\t\t\t__rpc_ret = {0}_bind_result.error_code;\n"
                    "\t\t\t\t{0}_stub_ = std::move({0}_bind_result.stub);\n"
                    "\t\t\t\t{0}_stub_id_ = {0}_bind_result.descriptor;\n"
                    "\t\t\t}}}}",
                    name);
            }
            return fmt::format(
                "RPC_ASSERT(rpc::are_in_same_zone(this, {0}.get()));\n"
                "\t\t\trpc::remote_object {0}_stub_id_;\n"
                "\t\t\tif(!rpc::error::is_error(__rpc_ret))\n"
                "\t\t\t{{{{\n"
                "\t\t\t\tauto {0}_bind_result = CO_AWAIT rpc::proxy_bind_in_param(__rpc_get_object_proxy(), "
                "__rpc_sp->get_remote_rpc_version(), "
                "{0});\n"
                "\t\t\t\t__rpc_ret = {0}_bind_result.error_code;\n"
                "\t\t\t\t{0}_stub_ = std::move({0}_bind_result.stub);\n"
                "\t\t\t\t{0}_stub_id_ = {0}_bind_result.descriptor;\n"
                "\t\t\t}}}}",
                name);
        case PROXY_MARSHALL_IN:
        {
            auto ret = fmt::format("{0}_stub_id_, ", name, count);
            count++;
            return ret;
        }
        case PROXY_MARSHALL_OUT:
            return fmt::format("{0}_, ", name);

        case PROXY_CLEAN_IN:
            return fmt::format(
                "if({0}_stub_) CO_AWAIT "
                "{0}_stub_->release_from_service(__rpc_sp->get_destination_zone_id());",
                name);

        case STUB_DEMARSHALL_DECLARATION:
            return fmt::format("{} {}", object_type, name);
        case STUB_PARAM_CAST:
            return name;
        case STUB_CLEAN_IN:
            return "";
        case PROXY_VALUE_RETURN:
            return fmt::format(
                "\t\t\t\tauto {0}_ret = CO_AWAIT rpc::proxy_bind_out_param<{1}>(__rpc_sp, __rpc_request_id, {0}_);\n"
                "\t\t\t\t__rpc_ret = {0}_ret.error_code;\n"
                "\t\t\t\t{0} = std::move({0}_ret.iface);\n",
                name,
                bind_template_args);
        case PROXY_OUT_DECLARATION:
            return fmt::format("rpc::remote_object {}_;", name);
        case STUB_ADD_REF_OUT_PREDECLARE:
            return fmt::format("rpc::remote_object {0}_;", name);
        case STUB_ADD_REF_OUT:
            return fmt::format(
                "auto {0}_bind_result = CO_AWAIT rpc::stub_bind_out_param(zone_, params.protocol_version, "
                "params.caller_zone_id, params.request_id, {0}); __rpc_ret = {0}_bind_result.error_code; {0}_ = "
                "{0}_bind_result.descriptor;",
                name);
        case STUB_MARSHALL_OUT:
            return fmt::format("{}_, ", name);

        case LOCAL_OPTIMISTIC_PTR_CALL:
            return fmt::format("{}", name);
        default:
            return "";
        }
    }

    bool do_in_param(
        print_type option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        const std::string& type,
        const attributes& attribs,
        uint64_t& count,
        std::string& output)
    {
        // UNIFIED: Use polymorphic renderer with print_type option
        calling_renderer r;
        return generator::do_in_param_unified(
            r, static_cast<int>(option), from_host, lib, name, type, attribs, count, output);
    }

    bool do_out_param(
        print_type option,
        bool from_host,
        const class_entity& lib,
        const std::string& name,
        const std::string& type,
        const attributes& attribs,
        uint64_t& count,
        std::string& output)
    {
        // UNIFIED: Use polymorphic renderer with print_type option
        calling_renderer r;
        return generator::do_out_param_unified(
            r, static_cast<int>(option), from_host, lib, name, type, attribs, count, output);
    }

    bool has_out_interface_params(
        const class_entity& m_ob,
        const std::shared_ptr<function_entity>& function)
    {
        for (auto& parameter : function->get_parameters())
        {
            if (!parameter.has_value("out"))
                continue;

            bool optimistic = false;
            std::shared_ptr<class_entity> obj;
            if (is_interface_param(m_ob, parameter.get_type(), optimistic, obj))
                return true;
        }
        return false;
    }

    bool is_serialized_struct_field(const std::shared_ptr<entity>& field)
    {
        if (!field || field->get_entity_type() != entity_type::FUNCTION_VARIABLE)
            return false;

        auto* function_variable = static_cast<const function_entity*>(field.get());
        return !function_variable->is_static();
    }

    void write_canonical_crypto_struct_methods(
        const class_entity& m_ob,
        writer& header)
    {
        header("");
        header("#ifdef CANOPY_BUILD_CANONICAL_CRYPTO");
        header("// Writes fields in IDL declaration order for deterministic crypto transcripts.");
        header("bool canonical_crypto_write_to(rpc::canonical_crypto_writer& __rpc_writer) const");
        header("{{");

        bool has_fields = false;
        for (const auto& field : m_ob.get_functions())
        {
            if (!is_serialized_struct_field(field))
                continue;

            has_fields = true;
            auto* function_variable = static_cast<const function_entity*>(field.get());
            if (is_serialized_pointer_field(*function_variable))
            {
                header(
                    "auto __canonical_{0}_address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>({0}));",
                    field->get_name());
                header("if (!rpc::canonical_crypto_write(__rpc_writer, __canonical_{}_address))", field->get_name());
            }
            else
            {
                header("if (!rpc::canonical_crypto_write(__rpc_writer, {}))", field->get_name());
            }
            header("{{");
            header("return false;");
            header("}}");
        }

        if (!has_fields)
            header("std::ignore = __rpc_writer;");
        header("return __rpc_writer.ok();");
        header("}}");
        header("");
        header("// Reads the same deterministic field sequence emitted by canonical_crypto_write_to().");
        header("bool canonical_crypto_read_from(rpc::canonical_crypto_reader& __rpc_reader)");
        header("{{");

        has_fields = false;
        for (const auto& field : m_ob.get_functions())
        {
            if (!is_serialized_struct_field(field))
                continue;

            has_fields = true;
            auto* function_variable = static_cast<const function_entity*>(field.get());
            if (is_serialized_pointer_field(*function_variable))
            {
                header("std::uint64_t __canonical_{}_address = 0;", field->get_name());
                header("if (!rpc::canonical_crypto_read(__rpc_reader, __canonical_{}_address))", field->get_name());
                header("{{");
                header("return false;");
                header("}}");
                header(
                    "{0} = reinterpret_cast<{1}>(static_cast<std::uintptr_t>(__canonical_{0}_address));",
                    field->get_name(),
                    pointer_cast_type(function_variable->get_return_type()));
            }
            else
            {
                header("if (!rpc::canonical_crypto_read(__rpc_reader, {}))", field->get_name());
                header("{{");
                header("return false;");
                header("}}");
            }
        }

        if (!has_fields)
            header("std::ignore = __rpc_reader;");
        header("return true;");
        header("}}");
        header("");
        header("// Helper used by rpc::serialise(..., rpc::encoding::canonical_crypto).");
        header("void canonical_crypto_serialise(std::vector<char>& buffer) const");
        header("{{");
        header("buffer.clear();");
        header("rpc::canonical_crypto_writer __rpc_writer(buffer);");
        header("if (!canonical_crypto_write_to(__rpc_writer) || !__rpc_writer.ok())");
        header("{{");
        header("throw std::runtime_error(\"canonical_crypto serialization failed\");");
        header("}}");
        header("}}");
        header("");
        header("// Helper used by rpc::deserialise(..., rpc::encoding::canonical_crypto).");
        header("std::string canonical_crypto_deserialise(const std::vector<char>& buffer)");
        header("{{");
        header("rpc::canonical_crypto_reader __rpc_reader(buffer);");
        header("if (!canonical_crypto_read_from(__rpc_reader) || !__rpc_reader.done())");
        header("{{");
        header("return \"canonical_crypto deserialization failed\";");
        header("}}");
        header("return {{}};");
        header("}}");
        header("#endif");
    }

    void write_canonical_crypto_proxy_send_method(
        bool from_host,
        const class_entity& m_ob,
        writer& proxy,
        const std::string& interface_name,
        const std::shared_ptr<function_entity>& function)
    {
        bool has_inparams = false;
        proxy("template<>");
        proxy(
            "{}",
            interface_declaration_generator::write_proxy_send_declaration(
                m_ob, interface_name + "::proxy_serialiser<rpc::serialiser::canonical_crypto>::", function, has_inparams, "", false));
        proxy("{{");
        proxy("__buffer.clear();");
        if (has_inparams)
        {
            proxy("rpc::canonical_crypto_writer __rpc_writer(__buffer);");
            for (const auto& parameter : function->get_parameters())
            {
                std::string output;
                uint64_t count = 1;
                if (!do_in_param(
                        PROXY_MARSHALL_IN, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                    continue;
                proxy("if (!rpc::canonical_crypto_write(__rpc_writer, {}))", parameter.get_name());
                proxy("{{");
                proxy("return rpc::error::INCOMPATIBLE_SERIALISATION();");
                proxy("}}");
            }
            proxy("if (!__rpc_writer.ok())");
            proxy("{{");
            proxy("return rpc::error::INCOMPATIBLE_SERIALISATION();");
            proxy("}}");
        }
        proxy("return rpc::error::OK();");
        proxy("}}");
        proxy("");
    }

    void write_canonical_crypto_proxy_receive_method(
        bool from_host,
        const class_entity& m_ob,
        writer& proxy,
        const std::string& interface_name,
        const std::shared_ptr<function_entity>& function)
    {
        bool has_outparams = false;
        proxy("template<>");
        proxy(
            "{}",
            interface_declaration_generator::write_proxy_receive_declaration(
                m_ob,
                interface_name + "::proxy_deserialiser<rpc::serialiser::canonical_crypto>::",
                function,
                has_outparams,
                "",
                false));
        proxy("{{");
        proxy("rpc::canonical_crypto_reader __rpc_reader(__rpc_data.data(), __rpc_data.size());");
        if (has_outparams)
        {
            for (const auto& parameter : function->get_parameters())
            {
                std::string output;
                uint64_t count = 1;
                if (!do_out_param(
                        PROXY_MARSHALL_OUT, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                    continue;
                proxy("if (!rpc::canonical_crypto_read(__rpc_reader, {}))", parameter.get_name());
                proxy("{{");
                proxy("return rpc::error::PROXY_DESERIALISATION_ERROR();");
                proxy("}}");
            }
        }
        proxy("if (!__rpc_reader.done())");
        proxy("{{");
        proxy("return rpc::error::PROXY_DESERIALISATION_ERROR();");
        proxy("}}");
        proxy("return rpc::error::OK();");
        proxy("}}");
        proxy("");
    }

    void write_canonical_crypto_stub_receive_method(
        bool from_host,
        const class_entity& m_ob,
        writer& proxy,
        const std::string& interface_name,
        const std::shared_ptr<function_entity>& function)
    {
        bool has_inparams = false;
        proxy("template<>");
        proxy(
            "{}",
            interface_declaration_generator::write_stub_receive_declaration(
                m_ob, interface_name + "::stub_deserialiser<rpc::serialiser::canonical_crypto>::", function, has_inparams, "", false));
        proxy("{{");
        proxy("rpc::canonical_crypto_reader __rpc_reader(__rpc_data.data(), __rpc_data.size());");
        if (has_inparams)
        {
            for (const auto& parameter : function->get_parameters())
            {
                std::string output;
                uint64_t count = 1;
                if (!do_in_param(
                        STUB_MARSHALL_IN, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                    continue;
                proxy("if (!rpc::canonical_crypto_read(__rpc_reader, {}))", parameter.get_name());
                proxy("{{");
                proxy("return rpc::error::STUB_DESERIALISATION_ERROR();");
                proxy("}}");
            }
        }
        proxy("if (!__rpc_reader.done())");
        proxy("{{");
        proxy("return rpc::error::STUB_DESERIALISATION_ERROR();");
        proxy("}}");
        proxy("return rpc::error::OK();");
        proxy("}}");
        proxy("");
    }

    void write_canonical_crypto_stub_reply_method(
        bool from_host,
        const class_entity& m_ob,
        writer& proxy,
        const std::string& interface_name,
        const std::shared_ptr<function_entity>& function)
    {
        bool has_outparams = false;
        proxy("template<>");
        proxy(
            "{}",
            interface_declaration_generator::write_stub_reply_declaration(
                m_ob, interface_name + "::stub_serialiser<rpc::serialiser::canonical_crypto>::", function, has_outparams, "", false));
        proxy("{{");
        proxy("__buffer.clear();");
        if (has_outparams)
        {
            proxy("rpc::canonical_crypto_writer __rpc_writer(__buffer);");
            for (const auto& parameter : function->get_parameters())
            {
                std::string output;
                uint64_t count = 1;
                if (!do_out_param(
                        STUB_MARSHALL_OUT, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                    continue;
                proxy("if (!rpc::canonical_crypto_write(__rpc_writer, {}))", parameter.get_name());
                proxy("{{");
                proxy("return rpc::error::INCOMPATIBLE_SERIALISATION();");
                proxy("}}");
            }
            proxy("if (!__rpc_writer.ok())");
            proxy("{{");
            proxy("return rpc::error::INCOMPATIBLE_SERIALISATION();");
            proxy("}}");
        }
        proxy("return rpc::error::OK();");
        proxy("}}");
        proxy("");
    }

    void write_canonical_crypto_interface_serializers(
        bool from_host,
        const class_entity& m_ob,
        writer& proxy)
    {
        if (m_ob.is_in_import())
            return;

        auto interface_name = m_ob.get_name();
        proxy("#ifdef CANOPY_BUILD_CANONICAL_CRYPTO");
        proxy("// canonical_crypto request and reply marshalling uses raw field order, not named fields.");

        {
            std::unordered_set<std::string> unique_signatures;
            for (const auto& function : m_ob.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                bool has_params = false;
                auto key = interface_declaration_generator::write_proxy_send_declaration(
                    m_ob, "", function, has_params, "", false);
                if (unique_signatures.emplace(key).second)
                    write_canonical_crypto_proxy_send_method(from_host, m_ob, proxy, interface_name, function);
            }
        }

        {
            std::unordered_set<std::string> unique_signatures;
            for (const auto& function : m_ob.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                bool has_params = false;
                auto key = interface_declaration_generator::write_proxy_receive_declaration(
                    m_ob, "", function, has_params, "", false);
                if (unique_signatures.emplace(key).second)
                    write_canonical_crypto_proxy_receive_method(from_host, m_ob, proxy, interface_name, function);
            }
        }

        {
            std::unordered_set<std::string> unique_signatures;
            for (const auto& function : m_ob.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                bool has_params = false;
                auto key = interface_declaration_generator::write_stub_receive_declaration(
                    m_ob, "", function, has_params, "", false);
                if (unique_signatures.emplace(key).second)
                    write_canonical_crypto_stub_receive_method(from_host, m_ob, proxy, interface_name, function);
            }
        }

        {
            std::unordered_set<std::string> unique_signatures;
            for (const auto& function : m_ob.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                bool has_params = false;
                auto key = interface_declaration_generator::write_stub_reply_declaration(
                    m_ob, "", function, has_params, "", false);
                if (unique_signatures.emplace(key).second)
                    write_canonical_crypto_stub_reply_method(from_host, m_ob, proxy, interface_name, function);
            }
        }

        proxy("#endif");
        proxy("");
    }

    // Lambda to emit PROXY_CLEAN_IN cleanup code - used at early return points and at end of function
    void emit_proxy_clean_in(
        bool from_host,
        const class_entity& m_ob,
        writer& proxy,
        const std::shared_ptr<function_entity>& function)
    {
        proxy("//PROXY_CLEAN_IN");
        uint64_t clean_count = 1;
        for (auto& clean_param : function->get_parameters())
        {
            std::string clean_output;
            if (do_in_param(
                    PROXY_CLEAN_IN, from_host, m_ob, clean_param.get_name(), clean_param.get_type(), clean_param, clean_count, clean_output))
            {
                proxy(clean_output);
            }
            clean_count++;
        }
        if (has_out_interface_params(m_ob, function))
        {
            proxy("if(__rpc_request_id != 0) __rpc_sp->get_operating_zone_service()->finish_out_param_request(__rpc_request_id);");
        }
    }

    void write_method(
        bool from_host,
        const class_entity& m_ob,
        writer& proxy,
        writer& stub,
        const std::string& interface_name,
        const std::shared_ptr<function_entity>& function,
        int& function_count,
        bool catch_stub_exceptions,
        const std::vector<std::string>& rethrow_exceptions,
        bool enable_yas,
        bool enable_protobuf,
        bool enable_nanopb,
        bool enable_canonical_crypto)
    {
        if (function->get_entity_type() == entity_type::FUNCTION_METHOD)
        {
            // Validate [post] attribute restrictions
            if (function->has_value("post"))
            {
                for (auto& parameter : function->get_parameters())
                {
                    // Check for [out] or [in,out] parameters
                    bool has_out = parameter.has_value("out");

                    if (has_out)
                    {
                        throw std::runtime_error(
                            std::string("Error in ") + m_ob.get_name() + "::" + function->get_name()
                            + ": [post] methods cannot have [out] or [in,out] parameters. Parameter '"
                            + parameter.get_name() + "' has [out] attribute.");
                    }

                    // Check for interface parameters (rpc::shared_ptr or rpc::optimistic_ptr)
                    bool optimistic = false;
                    std::shared_ptr<class_entity> obj;
                    bool is_interface = is_interface_param(m_ob, parameter.get_type(), optimistic, obj);
                    if (is_interface)
                    {
                        throw std::runtime_error(
                            std::string("Error in ") + m_ob.get_name() + "::"
                            + function->get_name() + ": [post] methods cannot have interface parameters (rpc::shared_ptr or rpc::optimistic_ptr). "
                            + "Parameter '" + parameter.get_name() + "' of type '" + parameter.get_type()
                            + "' is not supported. Posting interfaces is not currently supported.");
                    }
                }
            }

            std::string scoped_namespace;
            interface_declaration_generator::build_scoped_name(&m_ob, scoped_namespace);

            stub("case {}:", function_count);
            stub("{{");
            stub("auto __rpc_encoding = rpc::effective_encoding(params.encoding_type);");
            std::vector<std::string> unsupported_encoding_checks;
            if (enable_yas)
            {
                unsupported_encoding_checks.emplace_back("__rpc_encoding != rpc::encoding::yas_binary");
                unsupported_encoding_checks.emplace_back("__rpc_encoding != rpc::encoding::yas_compressed_binary");
                unsupported_encoding_checks.emplace_back("__rpc_encoding != rpc::encoding::yas_json");
            }
            if (enable_protobuf)
            {
                unsupported_encoding_checks.emplace_back("__rpc_encoding != rpc::encoding::protocol_buffers");
            }
            if (enable_nanopb)
            {
                unsupported_encoding_checks.emplace_back("__rpc_encoding != rpc::encoding::nanopb");
            }
            if (enable_canonical_crypto)
            {
                unsupported_encoding_checks.emplace_back("__rpc_encoding != rpc::encoding::canonical_crypto");
            }
            if (!unsupported_encoding_checks.empty())
            {
                stub("// Validate encoding format support");
                stub("if (");
                for (size_t i = 0; i < unsupported_encoding_checks.size(); ++i)
                {
                    stub("    {}{}", unsupported_encoding_checks[i], i + 1 < unsupported_encoding_checks.size() ? " &&" : "");
                }
                stub(")");
                stub("{{");
                stub("    CO_RETURN rpc::send_result{{rpc::error::INCOMPATIBLE_SERIALISATION(), {{}}, {{}}}};");
                stub("}}");
            }

            proxy.print_tabs();
            proxy.raw("CORO_TASK({}) {}(", function->get_return_type(), function->get_name());
            bool has_parameter = false;
            for (auto& parameter : function->get_parameters())
            {
                if (has_parameter)
                {
                    proxy.raw(", ");
                }
                has_parameter = true;
                render_parameter(proxy, m_ob, parameter);
            }
            bool function_is_const = function->has_value("const");
            if (function_is_const)
            {
                proxy.raw(") const override\n");
            }
            else
            {
                proxy.raw(") override\n");
            }
            proxy("{{");

            proxy("auto __rpc_op = get_object_proxy();");
            proxy("auto __rpc_sp = __rpc_op->get_service_proxy();");
            proxy("auto __rpc_encoding = rpc::effective_encoding(__rpc_sp->get_encoding());");
            proxy("auto __rpc_version = __rpc_sp->get_remote_rpc_version();");
            proxy("const auto __rpc_min_version = std::max<std::uint64_t>(rpc::LOWEST_SUPPORTED_VERSION, 1);");
            proxy("#ifdef CANOPY_USE_TELEMETRY");
            proxy("if (auto telemetry_service = rpc::telemetry::get_telemetry_service(); telemetry_service)");
            proxy("{{");
            proxy(
                "telemetry_service->on_interface_proxy_send("
                "{{\"{0}::{1}\", __rpc_sp->get_zone_id(), __rpc_sp->get_destination_zone_id(), "
                "__rpc_op->get_object_id(), {{{0}_proxy::get_id(rpc::get_version())}}, {{{2}}}}});",
                interface_name,
                function->get_name(),
                function_count);
            proxy("}}");
            proxy("#endif");

            {
                stub("//STUB_DEMARSHALL_DECLARATION");
                uint64_t count = 1;
                for (auto& parameter : function->get_parameters())
                {
                    std::string output;
                    if (do_in_param(
                            STUB_DEMARSHALL_DECLARATION,
                            from_host,
                            m_ob,
                            parameter.get_name(),
                            parameter.get_type(),
                            parameter,
                            count,
                            output))
                        ;
                    else
                        do_out_param(
                            STUB_DEMARSHALL_DECLARATION,
                            from_host,
                            m_ob,
                            parameter.get_name(),
                            parameter.get_type(),
                            parameter,
                            count,
                            output);
                    stub("{};", output);
                }
            }

            if (!function->has_value("post"))
            {
                proxy(
                    "std::vector<char> __rpc_out_buf(CANOPY_OUT_BUFFER_SIZE); //max size using short string "
                    "optimisation");
            }
            proxy("auto __rpc_ret = rpc::error::OK();");
            if (!function->has_value("post"))
            {
                proxy("uint64_t __rpc_request_id = 0;");
            }

            proxy("//PROXY_PREPARE_IN");

            proxy("if (__rpc_version < __rpc_min_version)");
            proxy("{{");
            proxy("CO_RETURN rpc::error::INVALID_VERSION();");
            proxy("}}");
            uint64_t count = 1;
            for (auto& parameter : function->get_parameters())
            {
                std::string output;
                {
                    if (!do_in_param(
                            PROXY_PREPARE_IN, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                        continue;
                    proxy(output);

                    if (!do_in_param(
                            PROXY_PREPARE_IN_INTERFACE_ID,
                            from_host,
                            m_ob,
                            parameter.get_name(),
                            parameter.get_type(),
                            parameter,
                            count,
                            output))
                        continue;

                    proxy(output);
                }
                count++;
            }

            proxy("while (!rpc::error::is_error(__rpc_ret) && __rpc_version >= __rpc_min_version)");
            proxy("{{");
            proxy("std::vector<char> __rpc_in_buf;");

            // Generate switch statement to select serializer based on encoding
            proxy("switch(__rpc_encoding)");
            proxy("{{");
            if (enable_yas)
            {
                proxy("case rpc::encoding::yas_binary:");
                proxy("case rpc::encoding::yas_compressed_binary:");
                proxy("case rpc::encoding::yas_json:");
                proxy("{{");
                {
                    proxy.print_tabs();
                    proxy.raw(
                        "__rpc_ret = {}proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::{}(",
                        scoped_namespace,
                        function->get_name());
                    count = 1;
                    for (auto& parameter : function->get_parameters())
                    {
                        std::string output;
                        {
                            if (!do_in_param(
                                    PROXY_MARSHALL_IN,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            proxy.raw(output);
                        }
                        count++;
                    }
                    proxy.raw("__rpc_in_buf, __rpc_encoding);\n");
                }
                proxy("break;");
                proxy("}}");
            }
            if (enable_protobuf)
            {
                proxy("case rpc::encoding::protocol_buffers:");
                proxy("{{");
                {
                    proxy.print_tabs();
                    proxy.raw(
                        "__rpc_ret = {}proxy_serialiser<rpc::serialiser::protocol_buffers>::{}(",
                        scoped_namespace,
                        function->get_name());
                    count = 1;
                    for (auto& parameter : function->get_parameters())
                    {
                        std::string output;
                        {
                            if (!do_in_param(
                                    PROXY_MARSHALL_IN,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            proxy.raw(output);
                        }
                        count++;
                    }
                    proxy.raw("__rpc_in_buf);\n");
                }
                proxy("break;");
                proxy("}}");
            }
            if (enable_nanopb)
            {
                proxy("case rpc::encoding::nanopb:");
                proxy("{{");
                {
                    proxy.print_tabs();
                    proxy.raw(
                        "__rpc_ret = {}proxy_serialiser<rpc::serialiser::nanopb>::{}(",
                        scoped_namespace,
                        function->get_name());
                    count = 1;
                    for (auto& parameter : function->get_parameters())
                    {
                        std::string output;
                        {
                            if (!do_in_param(
                                    PROXY_MARSHALL_IN,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            proxy.raw(output);
                        }
                        count++;
                    }
                    proxy.raw("__rpc_in_buf);\n");
                }
                proxy("break;");
                proxy("}}");
            }
            if (enable_canonical_crypto)
            {
                proxy("case rpc::encoding::canonical_crypto:");
                proxy("{{");
                {
                    proxy.print_tabs();
                    proxy.raw(
                        "__rpc_ret = {}proxy_serialiser<rpc::serialiser::canonical_crypto>::{}(",
                        scoped_namespace,
                        function->get_name());
                    count = 1;
                    for (auto& parameter : function->get_parameters())
                    {
                        std::string output;
                        {
                            if (!do_in_param(
                                    PROXY_MARSHALL_IN,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            proxy.raw(output);
                        }
                        count++;
                    }
                    proxy.raw("__rpc_in_buf);\n");
                }
                proxy("break;");
                proxy("}}");
            }
            proxy("default:");
            proxy("__rpc_ret = rpc::error::INCOMPATIBLE_SERIALISATION();");
            proxy("break;");
            proxy("}}");
            proxy("if(rpc::error::is_error(__rpc_ret))");
            proxy("{{");
            emit_proxy_clean_in(from_host, m_ob, proxy, function);
            proxy("CO_RETURN __rpc_ret;");
            proxy("}}");
            if (!function->has_value("post") && has_out_interface_params(m_ob, function))
            {
                // Interface out-params need a request-scoped handoff slot before the call is sent.
                proxy("__rpc_request_id = __rpc_sp->get_operating_zone_service()->begin_out_param_request();");
            }

            // Generate stub deserializer
            stub("int __rpc_ret = rpc::error::OK();");
            stub("auto __rpc_in_data = rpc::byte_span(params.in_data.data(), params.in_data.size());");
            stub("switch(__rpc_encoding)");
            stub("{{");
            if (enable_yas)
            {
                stub("case rpc::encoding::yas_binary:");
                stub("case rpc::encoding::yas_compressed_binary:");
                stub("case rpc::encoding::yas_json:");
                stub("{{");
                {
                    stub.print_tabs();
                    stub.raw(
                        "__rpc_ret = {}stub_deserialiser<rpc::serialiser::yas, rpc::encoding>::{}(",
                        scoped_namespace,
                        function->get_name());
                    count = 1;
                    for (auto& parameter : function->get_parameters())
                    {
                        std::string output;
                        {
                            if (!do_in_param(
                                    STUB_MARSHALL_IN,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            stub.raw(output);
                        }
                        count++;
                    }
                    stub.raw("__rpc_in_data, __rpc_encoding);\n");
                }
                stub("break;");
                stub("}}");
            }
            if (enable_protobuf)
            {
                stub("case rpc::encoding::protocol_buffers:");
                stub("{{");
                {
                    stub.print_tabs();
                    stub.raw(
                        "__rpc_ret = {}stub_deserialiser<rpc::serialiser::protocol_buffers>::{}(",
                        scoped_namespace,
                        function->get_name());
                    count = 1;
                    for (auto& parameter : function->get_parameters())
                    {
                        std::string output;
                        {
                            if (!do_in_param(
                                    STUB_MARSHALL_IN,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            stub.raw(output);
                        }
                        count++;
                    }
                    stub.raw("__rpc_in_data);\n");
                }
                stub("break;");
                stub("}}");
            }
            if (enable_nanopb)
            {
                stub("case rpc::encoding::nanopb:");
                stub("{{");
                {
                    stub.print_tabs();
                    stub.raw(
                        "__rpc_ret = {}stub_deserialiser<rpc::serialiser::nanopb>::{}(",
                        scoped_namespace,
                        function->get_name());
                    count = 1;
                    for (auto& parameter : function->get_parameters())
                    {
                        std::string output;
                        {
                            if (!do_in_param(
                                    STUB_MARSHALL_IN,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            stub.raw(output);
                        }
                        count++;
                    }
                    stub.raw("__rpc_in_data);\n");
                }
                stub("break;");
                stub("}}");
            }
            if (enable_canonical_crypto)
            {
                stub("case rpc::encoding::canonical_crypto:");
                stub("{{");
                {
                    stub.print_tabs();
                    stub.raw(
                        "__rpc_ret = {}stub_deserialiser<rpc::serialiser::canonical_crypto>::{}(",
                        scoped_namespace,
                        function->get_name());
                    count = 1;
                    for (auto& parameter : function->get_parameters())
                    {
                        std::string output;
                        {
                            if (!do_in_param(
                                    STUB_MARSHALL_IN,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            stub.raw(output);
                        }
                        count++;
                    }
                    stub.raw("__rpc_in_data);\n");
                }
                stub("break;");
                stub("}}");
            }
            stub("default:");
            stub("CO_RETURN rpc::send_result{{rpc::error::INCOMPATIBLE_SERIALISATION(), {{}}, {{}}}};");
            stub("}}");

            stub("if(rpc::error::is_error(__rpc_ret))");
            stub("{{");
            stub("CO_RETURN rpc::send_result{{__rpc_ret, {{}}, {{}}}};");
            stub("}}");

            std::string tag = function->get_value("tag");
            if (tag.empty())
                tag = "0";

            if (function->has_value("post"))
            {
                proxy(
                    "__rpc_ret = CO_AWAIT __rpc_op->post(__rpc_version, __rpc_encoding, static_cast<uint64_t>({}), "
                    "{}::get_id(__rpc_version), {{{}}}, std::move(__rpc_in_buf));",
                    tag,
                    interface_name,
                    function_count);
            }
            else
            {
                proxy(
                    "auto __rpc_send_result = CO_AWAIT __rpc_op->send(__rpc_version, __rpc_encoding, "
                    "static_cast<uint64_t>({}), {}::get_id(__rpc_version), {{{}}}, std::move(__rpc_in_buf), "
                    "__rpc_request_id);",
                    tag,
                    interface_name,
                    function_count);
                proxy("__rpc_ret = __rpc_send_result.error_code;");
                proxy("__rpc_out_buf = std::move(__rpc_send_result.out_buf);");
            }

            proxy("if(__rpc_ret == rpc::error::INVALID_VERSION())");
            proxy("{{");
            proxy("if(__rpc_version == __rpc_min_version)");
            proxy("{{");
            if (!function->has_value("post"))
            {
                proxy("__rpc_out_buf.clear();");
                emit_proxy_clean_in(from_host, m_ob, proxy, function);
                proxy("CO_RETURN __rpc_ret;");
            }
            proxy("}}");
            proxy("--__rpc_version;");
            proxy("__rpc_sp->update_remote_rpc_version(__rpc_version);");
            if (!function->has_value("post"))
            {
                proxy("__rpc_out_buf = std::vector<char>(CANOPY_OUT_BUFFER_SIZE);");
                if (has_out_interface_params(m_ob, function))
                {
                    // begin_out_param_request was called this iteration; release the slot
                    // before looping so the next iteration starts with a fresh id.
                    proxy("if(__rpc_request_id != 0) {{ __rpc_sp->get_operating_zone_service()->finish_out_param_request(__rpc_request_id); __rpc_request_id = 0; }}");
                }
            }
            proxy("continue;");
            proxy("}}");

            proxy("if(__rpc_ret == rpc::error::INCOMPATIBLE_SERIALISATION())");
            proxy("{{");
            if (enable_yas)
            {
                proxy("// Try fallback to yas_json if current encoding is not supported");
                proxy("if(__rpc_encoding != rpc::encoding::yas_json)");
                proxy("{{");
                proxy("__rpc_sp->set_encoding(rpc::encoding::yas_json);");
                proxy("__rpc_encoding = rpc::encoding::yas_json;");
                if (!function->has_value("post"))
                {
                    proxy("__rpc_out_buf = std::vector<char>(CANOPY_OUT_BUFFER_SIZE);");
                    if (has_out_interface_params(m_ob, function))
                    {
                        // Same as INVALID_VERSION retry: release the slot before looping.
                        proxy("if(__rpc_request_id != 0) {{ __rpc_sp->get_operating_zone_service()->finish_out_param_request(__rpc_request_id); __rpc_request_id = 0; }}");
                    }
                }
                proxy("continue;");
                proxy("}}");
                proxy("else");
                proxy("{{");
                proxy("// Already using yas_json, no more fallback options");
                emit_proxy_clean_in(from_host, m_ob, proxy, function);
                proxy("CO_RETURN __rpc_ret;");
                proxy("}}");
            }
            else
            {
                emit_proxy_clean_in(from_host, m_ob, proxy, function);
                proxy("CO_RETURN __rpc_ret;");
            }
            proxy("}}");

            proxy("if(rpc::error::is_critical(__rpc_ret))");
            proxy("{{");
            proxy(
                "//if you fall into this rabbit hole ensure that you have added any error offsets compatible with "
                "your error code system to the rpc library");
            proxy("//this is only here to handle rpc generated errors and not application errors");
            proxy("//clean up any input stubs, this code has to assume that the destination is behaving correctly");
            proxy("RPC_ERROR(\"failed in {}\");", function->get_name());
            if (!function->has_value("post"))
            {
                proxy("__rpc_out_buf.clear();");
            }
            emit_proxy_clean_in(from_host, m_ob, proxy, function);
            proxy("CO_RETURN __rpc_ret;");
            proxy("}}");

            proxy("break;");
            proxy("}}");

            stub("//STUB_PARAM_WRAP");

            {
                uint64_t count = 1;
                for (auto& parameter : function->get_parameters())
                {
                    std::string output;
                    if (!do_in_param(
                            STUB_PARAM_WRAP, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                        do_out_param(
                            STUB_PARAM_WRAP, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output);
                    stub.raw("{}", output);
                }
            }

            stub("//STUB_PARAM_CAST");
            stub("if(!rpc::error::is_error(__rpc_ret))");
            stub("{{");
            if (catch_stub_exceptions)
            {
                stub("try");
                stub("{{");
            }

            stub.print_tabs();
            stub.raw("__rpc_ret = CO_AWAIT __rpc_target_->{}(", function->get_name());

            {
                bool has_param = false;
                uint64_t count = 1;
                for (auto& parameter : function->get_parameters())
                {
                    std::string output;
                    if (!do_in_param(
                            STUB_PARAM_CAST, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                        do_out_param(
                            STUB_PARAM_CAST, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output);
                    if (has_param)
                    {
                        stub.raw(",");
                    }
                    has_param = true;
                    stub.raw("{}", output);
                }
            }
            stub.raw(");\n");
            if (catch_stub_exceptions)
            {
                stub("}}");

                for (auto& rethrow_stub_exception : rethrow_exceptions)
                {
                    stub("catch({}& __ex)", rethrow_stub_exception);
                    stub("{{");
                    stub("throw __ex;");
                    stub("}}");
                }

                stub("#ifdef CANOPY_USE_LOGGING");
                stub("catch(const std::exception& ex)");
                stub("{{");
                stub(
                    "RPC_ERROR(\"Exception has occurred in an {} implementation in function {} {{}}\", ex.what());",
                    interface_name,
                    function->get_name());
                stub("__rpc_ret = rpc::error::EXCEPTION();");
                stub("}}");
                stub("#endif");
                stub("catch(...)");
                stub("{{");
                stub(
                    "RPC_ERROR(\"Exception has occurred in an {} implementation in function {}\");",
                    interface_name,
                    function->get_name());
                stub("__rpc_ret = rpc::error::EXCEPTION();");
                stub("}}");
            }

            stub("}}");

            {
                uint64_t count = 1;
                stub("//STUB_CLEAN_IN");
                for (auto& parameter : function->get_parameters())
                {
                    std::string output;
                    if (!do_in_param(
                            STUB_CLEAN_IN, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                        continue;
                    if (!output.empty())
                        stub(output);
                }
            }

            {
                uint64_t count = 1;
                proxy("//PROXY_OUT_DECLARATION");
                for (auto& parameter : function->get_parameters())
                {
                    count++;
                    std::string output;
                    if (do_in_param(
                            PROXY_OUT_DECLARATION, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                        continue;
                    if (!do_out_param(
                            PROXY_OUT_DECLARATION, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                        continue;

                    proxy(output);
                }
            }
            {
                stub("//STUB_ADD_REF_OUT_PREDECLARE");
                uint64_t count = 1;
                for (auto& parameter : function->get_parameters())
                {
                    count++;
                    std::string output;

                    if (!do_out_param(
                            STUB_ADD_REF_OUT_PREDECLARE,
                            from_host,
                            m_ob,
                            parameter.get_name(),
                            parameter.get_type(),
                            parameter,
                            count,
                            output))
                        continue;

                    stub(output);
                }

                count = 1;
                bool has_preamble = false;
                for (auto& parameter : function->get_parameters())
                {
                    count++;
                    std::string output;

                    if (!do_out_param(
                            STUB_ADD_REF_OUT, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                        continue;

                    if (!has_preamble && !output.empty())
                    {
                        stub("//STUB_ADD_REF_OUT");
                        stub("if(!rpc::error::is_error(__rpc_ret))");
                        stub("{{");
                        stub("auto stub = __rpc_target_->__rpc_get_stub();");
                        stub("auto zone_ = stub ? stub->get_zone() : nullptr;");
                        stub("if (zone_)");
                        stub("{{");
                        has_preamble = true;
                    }
                    stub(output);
                }
                if (has_preamble)
                {
                    stub("}}");
                    stub("else");
                    stub("{{");
                    stub("assert(false);");
                    stub("}}");
                    stub("}}");
                }
            }
            if (!function->has_value("post"))
            {
                uint64_t count = 1;

                // Generate proxy deserializer switch statement
                proxy("if(__rpc_ret != rpc::error::OBJECT_GONE())");
                proxy("{{");
                proxy("switch(__rpc_encoding)");
                proxy("{{");
                if (enable_yas)
                {
                    proxy("case rpc::encoding::yas_binary:");
                    proxy("case rpc::encoding::yas_compressed_binary:");
                    proxy("case rpc::encoding::yas_json:");
                    proxy("{{");
                    {
                        proxy.print_tabs();
                        proxy.raw(
                            "__rpc_ret = {}proxy_deserialiser<rpc::serialiser::yas, rpc::encoding>::{}(",
                            scoped_namespace,
                            function->get_name());

                        for (auto& parameter : function->get_parameters())
                        {
                            count++;
                            std::string output;
                            if (!do_out_param(
                                    PROXY_MARSHALL_OUT,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;
                            proxy.raw(output);
                        }
                        proxy.raw("__rpc_out_buf, __rpc_encoding);\n");
                    }
                    proxy("break;");
                    proxy("}}");
                }
                if (enable_protobuf)
                {
                    proxy("case rpc::encoding::protocol_buffers:");
                    proxy("{{");
                    {
                        proxy.print_tabs();
                        proxy.raw(
                            "__rpc_ret = {}proxy_deserialiser<rpc::serialiser::protocol_buffers>::{}(",
                            scoped_namespace,
                            function->get_name());
                        count = 1;
                        for (auto& parameter : function->get_parameters())
                        {
                            count++;
                            std::string output;
                            if (!do_out_param(
                                    PROXY_MARSHALL_OUT,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;
                            proxy.raw(output);
                        }
                        proxy.raw("__rpc_out_buf);\n");
                    }
                    proxy("break;");
                    proxy("}}");
                }
                if (enable_nanopb)
                {
                    proxy("case rpc::encoding::nanopb:");
                    proxy("{{");
                    {
                        proxy.print_tabs();
                        proxy.raw(
                            "__rpc_ret = {}proxy_deserialiser<rpc::serialiser::nanopb>::{}(",
                            scoped_namespace,
                            function->get_name());
                        count = 1;
                        for (auto& parameter : function->get_parameters())
                        {
                            count++;
                            std::string output;
                            if (!do_out_param(
                                    PROXY_MARSHALL_OUT,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;
                            proxy.raw(output);
                        }
                        proxy.raw("__rpc_out_buf);\n");
                    }
                    proxy("break;");
                    proxy("}}");
                }
                if (enable_canonical_crypto)
                {
                    proxy("case rpc::encoding::canonical_crypto:");
                    proxy("{{");
                    {
                        proxy.print_tabs();
                        proxy.raw(
                            "__rpc_ret = {}proxy_deserialiser<rpc::serialiser::canonical_crypto>::{}(",
                            scoped_namespace,
                            function->get_name());
                        count = 1;
                        for (auto& parameter : function->get_parameters())
                        {
                            count++;
                            std::string output;
                            if (!do_out_param(
                                    PROXY_MARSHALL_OUT,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;
                            proxy.raw(output);
                        }
                        proxy.raw("__rpc_out_buf);\n");
                    }
                    proxy("break;");
                    proxy("}}");
                }
                proxy("default:");
                proxy("__rpc_ret = rpc::error::INCOMPATIBLE_SERIALISATION();");
                proxy("break;");
                proxy("}}");
                proxy("}}");
            }

            {
                // Generate stub serializer
                stub("switch(__rpc_encoding)");
                stub("{{");
                if (enable_yas)
                {
                    stub("case rpc::encoding::yas_binary:");
                    stub("case rpc::encoding::yas_compressed_binary:");
                    stub("case rpc::encoding::yas_json:");
                    stub("{{");
                    {
                        count = 1;
                        stub.print_tabs();
                        stub.raw(
                            "{}stub_serialiser<rpc::serialiser::yas, rpc::encoding>::{}(",
                            scoped_namespace,
                            function->get_name());

                        for (auto& parameter : function->get_parameters())
                        {
                            count++;
                            std::string output;
                            if (!do_out_param(
                                    STUB_MARSHALL_OUT,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            stub.raw(output);
                        }
                        stub.raw("__rpc_result.out_buf, __rpc_encoding);\n");
                    }
                    stub("break;");
                    stub("}}");
                }
                if (enable_protobuf)
                {
                    stub("case rpc::encoding::protocol_buffers:");
                    stub("{{");
                    {
                        count = 1;
                        stub.print_tabs();
                        stub.raw(
                            "{}stub_serialiser<rpc::serialiser::protocol_buffers>::{}(",
                            scoped_namespace,
                            function->get_name());

                        for (auto& parameter : function->get_parameters())
                        {
                            count++;
                            std::string output;
                            if (!do_out_param(
                                    STUB_MARSHALL_OUT,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            stub.raw(output);
                        }
                        stub.raw("__rpc_result.out_buf);\n");
                    }
                    stub("break;");
                    stub("}}");
                }
                if (enable_nanopb)
                {
                    stub("case rpc::encoding::nanopb:");
                    stub("{{");
                    {
                        count = 1;
                        stub.print_tabs();
                        stub.raw("{}stub_serialiser<rpc::serialiser::nanopb>::{}(", scoped_namespace, function->get_name());

                        for (auto& parameter : function->get_parameters())
                        {
                            count++;
                            std::string output;
                            if (!do_out_param(
                                    STUB_MARSHALL_OUT,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            stub.raw(output);
                        }
                        stub.raw("__rpc_result.out_buf);\n");
                    }
                    stub("break;");
                    stub("}}");
                }
                if (enable_canonical_crypto)
                {
                    stub("case rpc::encoding::canonical_crypto:");
                    stub("{{");
                    {
                        count = 1;
                        stub.print_tabs();
                        stub.raw(
                            "{}stub_serialiser<rpc::serialiser::canonical_crypto>::{}(",
                            scoped_namespace,
                            function->get_name());

                        for (auto& parameter : function->get_parameters())
                        {
                            count++;
                            std::string output;
                            if (!do_out_param(
                                    STUB_MARSHALL_OUT,
                                    from_host,
                                    m_ob,
                                    parameter.get_name(),
                                    parameter.get_type(),
                                    parameter,
                                    count,
                                    output))
                                continue;

                            stub.raw(output);
                        }
                        stub.raw("__rpc_result.out_buf);\n");
                    }
                    stub("break;");
                    stub("}}");
                }
                stub("default:");
                stub("__rpc_ret = rpc::error::INCOMPATIBLE_SERIALISATION();");
                stub("break;");
                stub("}}");
            }
            stub("__rpc_result.error_code = __rpc_ret;");
            stub("CO_RETURN __rpc_result;");

            proxy("//PROXY_VALUE_RETURN");
            {
                uint64_t count = 1;
                for (auto& parameter : function->get_parameters())
                {
                    count++;
                    std::string output;
                    if (do_in_param(
                            PROXY_VALUE_RETURN, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                        continue;
                    if (!do_out_param(
                            PROXY_VALUE_RETURN, from_host, m_ob, parameter.get_name(), parameter.get_type(), parameter, count, output))
                        continue;

                    proxy("if(!rpc::error::is_error(__rpc_ret))");
                    proxy("{{");
                    proxy(output);
                    proxy("}}");
                }
            }
            emit_proxy_clean_in(from_host, m_ob, proxy, function);

            proxy("CO_RETURN __rpc_ret;");
            proxy("}}");
            proxy("");

            function_count++;
            stub("}}");
            stub("break;");
        }
    }

    void write_interface(
        bool from_host,
        const class_entity& m_ob,
        writer& proxy,
        writer& stub,
        bool catch_stub_exceptions,
        const std::vector<std::string>& rethrow_exceptions,
        bool enable_yas,
        bool enable_protobuf,
        bool enable_nanopb,
        bool enable_canonical_crypto)
    {
        if (m_ob.is_in_import())
            return;

        auto interface_name = m_ob.get_name();

        if (enable_canonical_crypto)
            write_canonical_crypto_interface_serializers(from_host, m_ob, proxy);

        std::string base_class_declaration;
        auto bc = m_ob.get_base_classes();
        if (!bc.empty())
        {

            base_class_declaration = " : ";
            int i = 0;
            for (auto base_class : bc)
            {
                if (i)
                    base_class_declaration += ", ";
                base_class_declaration += base_class->get_name();
                i++;
            }
        }

        // generate the get_function_info function for the interface
        {
            proxy("std::vector<rpc::function_info> {0}::get_function_info()", interface_name);
            proxy("{{");
            proxy("std::vector<rpc::function_info> functions;");

            // generate unambiguous alias
            auto full_name = get_full_name(m_ob, true, false, ".");

            const auto& library = get_root(m_ob);
            int function_count = 1;
            for (auto& function : m_ob.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                std::string tag = function->get_value("tag");
                if (tag.empty())
                    tag = "0";

                bool marshalls_interfaces = false;

                for (const auto& parameter : function->get_parameters())
                {
                    std::string type_name = parameter.get_type();
                    std::string reference_modifiers;
                    generator::strip_reference_modifiers(type_name, reference_modifiers);

                    bool optimistic = false;
                    std::shared_ptr<class_entity> obj;
                    marshalls_interfaces |= is_interface_param(m_ob, parameter.get_type(), optimistic, obj);
                }

                // Get description attribute
                std::string description = function->get_value("description");
                if (!description.empty() && description.front() == '"' && description.back() == '"')
                {
                    description = description.substr(1, description.length() - 2);
                }

                // Generate JSON schemas for function parameters
                std::string in_json_schema;
                std::string out_json_schema;
                in_json_schema
                    = json_schema::generate_function_input_parameter_schema_with_recursion(library, m_ob, *function);
                out_json_schema
                    = json_schema::generate_function_output_parameter_schema_with_recursion(library, m_ob, *function);

                proxy(
                    "functions.emplace_back(rpc::function_info{{FLD(full_name)\"{0}.{1}\", FLD(name)\"{1}\", "
                    "FLD(id){{{2}}}, FLD(tag)static_cast<uint64_t>({3}), "
                    "FLD(marshalls_interfaces){4}, FLD(description)R\"__({5})__\", FLD(in_json_schema)R\"__({6})__\", "
                    "FLD(out_json_schema)R\"__({7})__\"}});",
                    full_name,
                    function->get_name(),
                    function_count,
                    tag,
                    marshalls_interfaces,
                    description,
                    in_json_schema,
                    out_json_schema);
                function_count++;
            }
            proxy("return functions;");
            proxy("}}");
        }

        {
            proxy("class __{0}_local_proxy : public rpc::local_proxy<{0}>", interface_name);
            proxy("{{");
            proxy("public:");
            proxy("__{0}_local_proxy(const rpc::weak_ptr<{0}>& ptr)", interface_name);
            proxy(": rpc::local_proxy<{0}>(ptr)", interface_name);
            proxy("{{}}");
            proxy("virtual ~__{0}_local_proxy() CANOPY_DEFAULT_DESTRUCTOR", interface_name);
            proxy(
                "[[nodiscard]] const rpc::casting_interface* __rpc_query_interface(rpc::interface_ordinal "
                "interface_id) const "
                "override");
            proxy("{{");
            proxy("std::ignore = interface_id;");
            proxy("return nullptr;");
            proxy("}}");

            for (auto& function : m_ob.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                std::string scoped_namespace;
                interface_declaration_generator::build_scoped_name(&m_ob, scoped_namespace);

                proxy.print_tabs();
                proxy.raw("CORO_TASK({}) {}(", function->get_return_type(), function->get_name());
                bool has_parameter = false;
                for (auto& parameter : function->get_parameters())
                {
                    if (has_parameter)
                    {
                        proxy.raw(", ");
                    }
                    has_parameter = true;

                    render_parameter(proxy, m_ob, parameter);
                }
                bool function_is_const = false;
                if (function->has_value("const"))
                    function_is_const = true;
                if (function_is_const)
                {
                    proxy.raw(") const override\n");
                }
                else
                {
                    proxy.raw(") override\n");
                }

                proxy("{{");
                proxy("auto ptr = get_ptr();");
                proxy("if(!ptr)");
                proxy("{{");
                proxy("CO_RETURN rpc::error::OBJECT_GONE();");
                proxy("}}");

                proxy.print_tabs();
                proxy.raw("CO_RETURN CO_AWAIT ptr->{}(", function->get_name());

                has_parameter = false;
                uint64_t count = 1;
                for (auto& parameter : function->get_parameters())
                {
                    if (has_parameter)
                    {
                        proxy.raw(", ");
                        count++;
                    }
                    has_parameter = true;

                    std::string output;
                    if (do_in_param(
                            LOCAL_OPTIMISTIC_PTR_CALL,
                            from_host,
                            m_ob,
                            parameter.get_name(),
                            parameter.get_type(),
                            parameter,
                            count,
                            output))
                        ;
                    else
                        do_out_param(
                            LOCAL_OPTIMISTIC_PTR_CALL,
                            from_host,
                            m_ob,
                            parameter.get_name(),
                            parameter.get_type(),
                            parameter,
                            count,
                            output);
                    proxy.raw("{}", output);
                }

                proxy.raw(");\n");
                proxy("}}");
            }
            proxy("}};");

            proxy("//RAII responsibilities are managed by the optimistic_ptr");
            proxy(
                "std::shared_ptr<rpc::local_proxy<{0}>> {0}::create_local_proxy(const rpc::weak_ptr<{0}>& ptr)",
                interface_name);
            proxy("{{");
            proxy("return std::make_shared<__{}_local_proxy>(ptr);", interface_name);
            proxy("}}");
        }

        proxy("class {0}_proxy : public rpc::interface_proxy<{0}>", interface_name);
        proxy("{{");
        proxy("mutable rpc::weak_ptr<{}_proxy> weak_this_;", interface_name);
        proxy("");
        proxy("{}_proxy(std::shared_ptr<rpc::object_proxy>&& object_proxy) : ", interface_name);
        proxy("  rpc::interface_proxy<{}>(std::move(object_proxy))", interface_name);
        proxy("{{");
        proxy("#ifdef CANOPY_USE_TELEMETRY");
        proxy("auto __rpc_op = get_object_proxy();");
        proxy("auto __rpc_sp = __rpc_op->get_service_proxy();");
        proxy("if (auto telemetry_service = rpc::telemetry::get_telemetry_service(); telemetry_service)");
        proxy("{{");
        proxy(
            "telemetry_service->on_interface_proxy_creation("
            "{{\"{0}\", __rpc_sp->get_zone_id(), __rpc_sp->get_destination_zone_id(), __rpc_op->get_object_id(), "
            "{{{0}_proxy::get_id(rpc::get_version())}}}});",
            interface_name);
        proxy("}}");
        proxy("#endif");
        proxy("}}");
        proxy("");
        proxy("public:");
        proxy("");
        proxy("~{}_proxy() override", interface_name);
        proxy("{{");
        proxy("#ifdef CANOPY_USE_TELEMETRY");
        proxy("if (auto telemetry_service = rpc::telemetry::get_telemetry_service(); telemetry_service)");
        proxy("{{");
        proxy("auto __rpc_op = get_object_proxy();");
        proxy("auto __rpc_sp = __rpc_op->get_service_proxy();");
        proxy(
            "telemetry_service->on_interface_proxy_deletion("
            "{{__rpc_sp->get_zone_id(), __rpc_sp->get_destination_zone_id(), __rpc_op->get_object_id(), "
            "{{{0}_proxy::get_id(rpc::get_version())}}}});",
            interface_name);
        proxy("}}");
        proxy("#endif");
        proxy("}}");
        proxy(
            "[[nodiscard]] static rpc::shared_ptr<{}> create(std::shared_ptr<rpc::object_proxy>&& "
            "object_proxy)",
            interface_name);
        proxy("{{");
        proxy("auto __rpc_ret = rpc::shared_ptr<{0}_proxy>(new {0}_proxy(std::move(object_proxy)));", interface_name);
        proxy("__rpc_ret->weak_this_ = __rpc_ret;", interface_name);
        proxy("return rpc::static_pointer_cast<{}>(__rpc_ret);", interface_name);
        proxy("}}");
        proxy(
            "rpc::shared_ptr<{0}_proxy> shared_from_this(){{return "
            "rpc::shared_ptr<{0}_proxy>(weak_this_);}}",
            interface_name);
        proxy("");

        stub("CORO_TASK(rpc::send_result) {0}::stub_caller::call({0}* __rpc_target_, rpc::send_params params)", interface_name);
        stub("{{");
        stub("if(!__rpc_target_)");
        stub("{{");
        stub("CO_RETURN rpc::send_result{{rpc::error::OBJECT_NOT_FOUND(), {{}}, {{}}}};");
        stub("}}");
        stub("rpc::send_result __rpc_result;");

        bool has_methods = false;
        for (auto& function : m_ob.get_functions())
        {
            if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                continue;
            has_methods = true;
        }

        if (has_methods)
        {
            stub("switch(params.method_id.get_val())");
            stub("{{");

            int function_count = 1;
            for (auto& function : m_ob.get_functions())
            {
                if (function->get_entity_type() == entity_type::FUNCTION_METHOD)
                    write_method(
                        from_host,
                        m_ob,
                        proxy,
                        stub,
                        interface_name,
                        function,
                        function_count,
                        catch_stub_exceptions,
                        rethrow_exceptions,
                        enable_yas,
                        enable_protobuf,
                        enable_nanopb,
                        enable_canonical_crypto);
            }

            stub("default:");
            stub("RPC_ERROR(\"Invalid method ID - unknown method in stub\");");
            stub("CO_RETURN rpc::send_result{{rpc::error::INVALID_METHOD_ID(), {{}}, {{}}}};");
            stub("}};");
        }
        proxy("}};");
        proxy("");

        stub("RPC_ERROR(\"Invalid method ID - no methods found\");");
        stub("CO_RETURN rpc::send_result{{rpc::error::INVALID_METHOD_ID(), {{}}, {{}}}};");
        stub("}}");
        stub("");
    }

    void write_interface_forward_declaration(
        const class_entity& m_ob,
        writer& header,
        writer& proxy)
    {
        header("class {};", m_ob.get_name());
        proxy("class {}_proxy;", m_ob.get_name());
    }

    void write_enum_forward_declaration(
        const entity& ent,
        writer& header)
    {
        if (!ent.is_in_import())
        {
            auto& enum_entity = static_cast<const class_entity&>(ent);
            if (enum_entity.get_base_classes().empty())
                header("enum class {}", enum_entity.get_name());
            else
                header("enum class {} : {}", enum_entity.get_name(), enum_entity.get_base_classes().front()->get_name());
            header("{{");
            auto enum_vals = enum_entity.get_functions();
            for (auto& enum_val : enum_vals)
            {
                if (enum_val->get_return_type().empty())
                    header("{},", enum_val->get_name());
                else
                    header("{} = {},", enum_val->get_name(), enum_val->get_return_type());
            }
            header("}};");
        }
    }

    void write_typedef_forward_declaration(
        const entity& ent,
        writer& header)
    {
        if (!ent.is_in_import())
        {
            auto& cls = static_cast<const class_entity&>(ent);
            header("using {} = {};", cls.get_name(), cls.get_alias_name());
        }
    }

    void write_struct_id(
        const class_entity& m_ob,
        writer& header)
    {
        if (m_ob.is_in_import())
            return;

        // Skip fingerprint generation if [no_fingerprint] attribute is set
        if (m_ob.has_value("no_fingerprint"))
            return;

        header("");
        header("/****************************************************************************/");
        if (!m_ob.get_is_template())
            header("template<>");
        else
        {
            header.print_tabs();
            header.raw("template<");
            bool first_pass = true;
            for (const auto& param : m_ob.get_template_params())
            {
                if (!first_pass)
                    header.raw(", ");
                first_pass = false;

                template_deduction deduction;
                m_ob.deduct_template_type(param, deduction);

                if (deduction.type == template_deduction_type::OTHER && deduction.identified_type)
                {
                    auto full_name = get_full_name(*deduction.identified_type, true);
                    header.raw("{} {}", full_name, param.get_name());
                }
                else
                {
                    header.raw("{} {}", param.type, param.get_name());
                }
            }
            header.raw(">\n");
        }

        header.print_tabs();
        header.raw("class id<{}", get_full_name(m_ob, true));
        if (m_ob.get_is_template() && !m_ob.get_template_params().empty())
        {
            header.raw("<");
            bool first_pass = true;
            for (const auto& param : m_ob.get_template_params())
            {
                if (!first_pass)
                    header.raw(", ");
                first_pass = false;
                header.raw("{}", param.get_name());
            }
            header.raw(">");
        }
        header.raw(">\n");

        header("{{");
        header("public:");
        header("static constexpr uint64_t get(uint64_t rpc_version)");
        header("{{");
        auto val = m_ob.get_value(rpc_attribute_types::use_template_param_in_id_attr);
        for (const auto& version : protocol_versions)
        {
            header("#ifdef {}", version.macro);
            header("if(rpc_version >= {})", version.symbol);
            header("{{");
            header("auto id = {}ull;", fingerprint::generate(m_ob, {}, &header, version.value));
            if (val != "false")
            {
                for (const auto& param : m_ob.get_template_params())
                {
                    template_deduction deduction;
                    m_ob.deduct_template_type(param, deduction);
                    if (deduction.type == template_deduction_type::CLASS
                        || deduction.type == template_deduction_type::TYPENAME)
                    {
                        header("id ^= rpc::id<{}>::get({});", param.get_name(), version.symbol);
                        header("id = (id << 1)|(id >> (sizeof(id) - 1));//rotl");
                    }
                    else if (deduction.identified_type)
                    {
                        if (deduction.identified_type->get_entity_type() == entity_type::ENUM)
                        {
                            header("id ^= static_cast<uint64_t>({});", param.get_name());
                            header("id = (id << 1)|(id >> (sizeof(id) - 1));//rotl");
                            break;
                        }
                        else if (param.get_name() == "size_t")
                        {
                            header("id ^= static_cast<uint64_t>({});", param.get_name());
                            header("id = (id << 1)|(id >> (sizeof(id) - 1));//rotl");
                            break;
                        }
                        else
                        {
                            header("static_assert(!\"not supported\"));//rotl");
                        }
                    }
                    else
                    {
                        header("id ^= static_cast<uint64_t>({});", param.get_name());
                        header("id = (id << 1)|(id >> (sizeof(id) - 1));//rotl");
                    }
                }
            }
            header("return id;");
            header("}}");
            header("#endif");
        }
        header("return 0;");
        header("}}");
        header("}};");
        header("");
    }

    void write_struct(
        const class_entity& m_ob,
        writer& header,
        bool enable_yas,
        bool enable_protobuf,
        bool enable_nanopb,
        bool enable_canonical_crypto)
    {
        if (m_ob.is_in_import())
            return;

        header("");
        header("/****************************************************************************/");

        std::string base_class_declaration;
        auto bc = m_ob.get_base_classes();
        if (!bc.empty())
        {

            base_class_declaration = " : ";
            int i = 0;
            for (auto base_class : bc)
            {
                if (i)
                    base_class_declaration += ", ";
                base_class_declaration += base_class->get_name();
                i++;
            }
        }
        if (m_ob.get_is_template())
        {
            header.print_tabs();
            header.raw("template<");
            bool first_pass = true;
            for (const auto& param : m_ob.get_template_params())
            {
                if (!first_pass)
                    header.raw(", ");
                first_pass = false;
                header.raw("{} {}", param.type, param.get_name());
                if (!param.default_value.empty())
                    header.raw(" = {}", param.default_value);
            }
            header.raw(">\n");
        }
        header("struct {}{}", m_ob.get_name(), base_class_declaration);
        header("{{");

        for (auto& field : m_ob.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (field->get_entity_type() == entity_type::FUNCTION_VARIABLE)
            {
                auto* function_variable = static_cast<const function_entity*>(field.get());
                header.print_tabs();
                render_function(header, m_ob, *function_variable);
                if (function_variable->get_array_string().size())
                    header.raw("[{}]", function_variable->get_array_string());
                if (!function_variable->get_default_value().empty())
                {
                    header.raw(" = {};\n", function_variable->get_default_value());
                }
                else
                {
                    header.raw(";\n");
                }
            }
            else if (field->get_entity_type() == entity_type::CPPQUOTE)
            {
                if (field->is_in_import())
                    continue;
                auto text = field->get_name();
                header.write_buffer(text);
                continue;
            }
            else if (field->get_entity_type() == entity_type::RUSTQUOTE)
            {
                continue;
            }
            else if (field->get_entity_type() == entity_type::FUNCTION_PRIVATE)
            {
                header("private:");
            }
            else if (field->get_entity_type() == entity_type::FUNCTION_PUBLIC)
            {
                header("public:");
            }
            else if (field->get_entity_type() == entity_type::CONSTEXPR)
            {
                interface_declaration_generator::write_constexpr(header, *field);
            }
        }

        if (!m_ob.get_is_template())
        {
            header("");
            header("static std::string get_schema();");
            header("static std::string get_schema(rpc::encoding encoding);");
            header("static constexpr const char* get_inner_schema();");
        }

        // Generate YAS serialization method if enabled
        if (enable_yas)
        {
            header("");
            header("// one member-function for save/load");
            header("template<typename Ar>");
            header("void serialize(Ar &ar)");
            header("{{");
            header("std::ignore = ar;");
            bool has_fields = false;
            for (auto& field : m_ob.get_functions())
            {
                auto type = field->get_entity_type();
                if (type != entity_type::CPPQUOTE && type != entity_type::RUSTQUOTE && type != entity_type::FUNCTION_PUBLIC
                    && type != entity_type::FUNCTION_PRIVATE && type != entity_type::CONSTEXPR)
                {
                    if (field->get_entity_type() == entity_type::FUNCTION_VARIABLE)
                    {
                        auto* function_variable = static_cast<const function_entity*>(field.get());
                        if (function_variable->is_static())
                        {
                            continue;
                        }
                    }

                    has_fields = true;
                    break;
                }
            }
            if (has_fields)
            {
                for (auto& field : m_ob.get_functions())
                {
                    if (field->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                        continue;

                    auto* function_variable = static_cast<const function_entity*>(field.get());
                    if (function_variable->is_static() || !is_serialized_pointer_field(*function_variable))
                        continue;

                    header(
                        "auto __yas_{0}_address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>({0}));",
                        field->get_name());
                }

                header("YAS_WARNINGS_PUSH");
                header("ar & YAS_OBJECT_NVP(\"{}\"", m_ob.get_name());

                for (auto& field : m_ob.get_functions())
                {
                    auto type = field->get_entity_type();
                    if (type != entity_type::CPPQUOTE && type != entity_type::RUSTQUOTE && type != entity_type::FUNCTION_PUBLIC
                        && type != entity_type::FUNCTION_PRIVATE && type != entity_type::CONSTEXPR)
                    {
                        if (field->get_entity_type() == entity_type::FUNCTION_VARIABLE)
                        {
                            auto* function_variable = static_cast<const function_entity*>(field.get());
                            if (function_variable->is_static())
                            {
                                continue;
                            }
                            if (is_serialized_pointer_field(*function_variable))
                            {
                                header("  ,(\"{0}\", __yas_{0}_address)", field->get_name());
                                continue;
                            }
                        }
                        header("  ,(\"{0}\", {0})", field->get_name());
                    }
                }
                header(");");
                header("YAS_WARNINGS_POP");

                for (auto& field : m_ob.get_functions())
                {
                    if (field->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                        continue;

                    auto* function_variable = static_cast<const function_entity*>(field.get());
                    if (function_variable->is_static() || !is_serialized_pointer_field(*function_variable))
                        continue;

                    header(
                        "{0} = reinterpret_cast<{1}>(static_cast<std::uintptr_t>(__yas_{0}_address));",
                        field->get_name(),
                        pointer_cast_type(function_variable->get_return_type()));
                }
            }

            header("}}");
        }

        // Generate protobuf serialization methods if enabled
        if (enable_protobuf)
        {
            header("");
            header("// protobuf serialization methods");
            header("void protobuf_serialise(std::vector<char>& buffer) const;");
            header("void protobuf_deserialise(const std::vector<char>& buffer);");
        }
        if (enable_nanopb)
        {
            header("void nanopb_serialise(std::vector<char>& buffer) const;");
            header("void nanopb_deserialise(const std::vector<char>& buffer);");
        }
        if (enable_canonical_crypto)
        {
            write_canonical_crypto_struct_methods(m_ob, header);
        }

        // Grant the generated JSON converters access to private members so
        // structs that hide their representation can still round-trip
        // through json::v1::object. The bodies are defined in the
        // _schema.h header. We only emit friends for structs the schema
        // generator will actually emit a converter for, so a friend never
        // references an undefined ADL overload.
        if (json_schema::struct_will_have_converter(m_ob))
        {
            header("");
            header("friend {} from_json_object(", m_ob.get_name());
            header("    ::json::v1::convert::tag<{}>,", m_ob.get_name());
            header("    const ::json::v1::object& __value);");
            header("friend ::json::v1::object to_json_object(const {}& __value);", m_ob.get_name());
        }

        header("}};");

        std::stringstream sstr;
        std::string obj_type(m_ob.get_name());
        {
            writer tmpl(sstr, header.get_tab_count());
            if (m_ob.get_is_template())
            {
                tmpl.print_tabs();
                tmpl.raw("template<");
                if (!m_ob.get_template_params().empty())
                {
                    obj_type += "<";
                    bool first_pass = true;
                    for (const auto& param : m_ob.get_template_params())
                    {
                        if (!first_pass)
                        {
                            tmpl.raw(", ");
                            obj_type += ", ";
                        }
                        first_pass = false;
                        tmpl.raw("{} {}", param.type, param.get_name());
                        if (!param.default_value.empty())
                            tmpl.raw(" = {}", param.default_value);
                        obj_type += param.get_name();
                    }
                    obj_type += ">";
                }
                tmpl.raw(">\n");
            }
        }
        header.raw(sstr.str());
        header("inline bool operator == (const {0}& lhs, const {0}& rhs)", obj_type);
        header("{{");
        bool has_params = true;
        {
            bool first_pass = true;
            for (auto& field : m_ob.get_functions())
            {
                auto type = field->get_entity_type();
                if (type != entity_type::CPPQUOTE && type != entity_type::RUSTQUOTE && type != entity_type::FUNCTION_PUBLIC
                    && type != entity_type::FUNCTION_PRIVATE && type != entity_type::CONSTEXPR)
                {
                    if (field->get_entity_type() == entity_type::FUNCTION_VARIABLE)
                    {
                        auto* function_variable = static_cast<const function_entity*>(field.get());
                        if (function_variable->is_static())
                        {
                            continue;
                        }
                    }
                    first_pass = false;
                }
            }
            has_params = !first_pass;
        }
        if (has_params)
        {
            header.print_tabs();
            header.raw("return ");

            bool first_pass = true;
            for (auto& field : m_ob.get_functions())
            {
                auto type = field->get_entity_type();
                if (type != entity_type::CPPQUOTE && type != entity_type::RUSTQUOTE && type != entity_type::FUNCTION_PUBLIC
                    && type != entity_type::FUNCTION_PRIVATE && type != entity_type::CONSTEXPR)
                {
                    if (field->get_entity_type() == entity_type::FUNCTION_VARIABLE)
                    {
                        auto* function_variable = static_cast<const function_entity*>(field.get());
                        if (function_variable->is_static())
                        {
                            continue;
                        }
                    }

                    header.raw("\n");
                    header.print_tabs();
                    header.raw("{1}lhs.{0} == rhs.{0}", field->get_name(), first_pass ? "" : "&& ");
                    first_pass = false;
                }
            }
            header.raw(";\n");
        }
        else
        {
            header("std::ignore = lhs;");
            header("std::ignore = rhs;");
            header("return true;");
        }
        header("}}\n");

        header.raw(sstr.str());
        header("inline bool operator != (const {0}& lhs, const {0}& rhs)", obj_type);
        header("{{");
        header("return !(lhs == rhs);");
        header("}}");
    }

    void write_encapsulate_outbound_interfaces(
        const class_entity& obj,
        writer& header,
        const std::vector<std::string>& namespaces)
    {
        auto interface_name = obj.get_name();
        std::string ns;

        for (auto& name : namespaces)
        {
            ns += name + "::";
        }

        auto owner = obj.get_owner();
        if (owner && !owner->get_name().empty())
        {
            interface_declaration_generator::build_scoped_name(owner, ns);
        }

        header(
            "template<> CORO_TASK(rpc::remote_object_bind_result) "
            "rpc::service::bind_in_proxy(uint64_t protocol_version, rpc::shared_ptr<::{}{}> "
            "iface, caller_zone caller_zone_id);",
            ns,
            interface_name);
        header(
            "template<> CORO_TASK(rpc::remote_object_bind_result) "
            "rpc::service::bind_in_proxy(uint64_t protocol_version, rpc::optimistic_ptr<::{}{}> "
            "iface, caller_zone caller_zone_id);",
            ns,
            interface_name);
    }

    void write_library_proxy_factory(
        writer& proxy,
        writer& stub,
        const class_entity& obj,
        const std::vector<std::string>& namespaces)
    {
        auto interface_name = obj.get_name();
        std::string ns;

        for (auto& name : namespaces)
        {
            ns += name + "::";
        }
        auto owner = obj.get_owner();
        if (owner && !owner->get_name().empty())
        {
            interface_declaration_generator::build_scoped_name(owner, ns);
        }

        proxy(
            "template<> void object_proxy::create_interface_proxy(rpc::shared_ptr<::{}{}>& "
            "inface)",
            ns,
            interface_name);
        proxy("{{");
        proxy("inface = ::{1}{0}_proxy::create(shared_from_this());", interface_name, ns);
        proxy("}}");
        proxy("");

        stub(
            "template<> CORO_TASK(rpc::remote_object_bind_result) service::bind_in_proxy([[maybe_unused]] "
            "uint64_t protocol_version, rpc::shared_ptr<::{}{}> iface, caller_zone caller_zone_id)",
            ns,
            interface_name);
        stub("{{");
        stub("if(!iface)");
        stub("{{");
        stub("CO_RETURN rpc::remote_object_bind_result{{rpc::error::INVALID_DATA(), nullptr, {{}}}};");
        stub("}}");
        stub("auto iface_cast = rpc::static_pointer_cast<rpc::casting_interface>(iface);");
        stub("CO_RETURN CO_AWAIT get_descriptor_from_interface_stub(caller_zone_id, iface_cast, false);");
        stub("}}");

        stub(
            "template<> CORO_TASK(rpc::remote_object_bind_result) service::bind_in_proxy([[maybe_unused]] "
            "uint64_t protocol_version, rpc::optimistic_ptr<::{}{}> iface, caller_zone caller_zone_id)",
            ns,
            interface_name);
        stub("{{");
        stub("if(!iface)");
        stub("{{");
        stub("CO_RETURN rpc::remote_object_bind_result{{rpc::error::INVALID_DATA(), nullptr, {{}}}};");
        stub("}}");
        stub("auto __rpc_make_shared_result = CO_AWAIT rpc::make_shared(iface);");
        stub("auto __rpc_ret = std::get<0>(__rpc_make_shared_result);");
        stub("if(rpc::error::is_error(__rpc_ret))");
        stub("{{");
        stub("CO_RETURN rpc::remote_object_bind_result{{__rpc_ret, nullptr, {{}}}};");
        stub("}}");
        stub("auto iface_shared = std::get<1>(std::move(__rpc_make_shared_result));");
        stub("auto iface_cast = rpc::static_pointer_cast<rpc::casting_interface>(iface_shared);");
        stub("CO_RETURN CO_AWAIT get_descriptor_from_interface_stub(caller_zone_id, iface_cast, true);");
        stub("}}");
    }

    // entry point
    void write_namespace_predeclaration(
        const class_entity& lib,
        writer& header,
        writer& proxy,
        writer& stub)
    {
        for (const auto& cls : lib.get_classes())
        {
            if (!cls->get_import_lib().empty())
                continue;
            if (cls->get_entity_type() == entity_type::INTERFACE)
                write_interface_forward_declaration(*cls, header, proxy);
        }

        for (const auto& cls : lib.get_classes())
        {
            if (!cls->get_import_lib().empty())
                continue;
            if (cls->get_entity_type() == entity_type::NAMESPACE)
            {
                bool is_inline = cls->has_value("inline");
                if (is_inline)
                {
                    header("inline namespace {}", cls->get_name());
                    proxy("inline namespace {}", cls->get_name());
                    stub("inline namespace {}", cls->get_name());
                }
                else
                {
                    header("namespace {}", cls->get_name());
                    proxy("namespace {}", cls->get_name());
                    stub("namespace {}", cls->get_name());
                }

                header("{{");
                proxy("{{");
                stub("{{");

                write_namespace_predeclaration(*cls, header, proxy, stub);

                header("}}");
                proxy("}}");
                stub("}}");
            }
        }
    }

    // entry point
    void write_namespace(
        bool from_host,
        const class_entity& lib,
        std::string prefix,
        writer& header,
        writer& proxy,
        writer& stub,
        bool catch_stub_exceptions,
        const std::vector<std::string>& rethrow_exceptions,
        bool enable_yas,
        bool enable_protobuf,
        bool enable_nanopb,
        bool enable_canonical_crypto)
    {
        for (auto& elem : lib.get_elements(entity_type::NAMESPACE_MEMBERS))
        {
            if (elem->is_in_import())
                continue;
            else if (elem->get_entity_type() == entity_type::ENUM)
                write_enum_forward_declaration(*elem, header);
            else if (elem->get_entity_type() == entity_type::TYPEDEF)
                write_typedef_forward_declaration(*elem, header);
            else if (elem->get_entity_type() == entity_type::NAMESPACE)
            {
                bool is_inline = elem->has_value("inline");

                if (is_inline)
                {
                    header("inline namespace {}", elem->get_name());
                    proxy("inline namespace {}", elem->get_name());
                    stub("inline namespace {}", elem->get_name());
                }
                else
                {
                    header("namespace {}", elem->get_name());
                    proxy("namespace {}", elem->get_name());
                    stub("namespace {}", elem->get_name());
                }
                header("{{");
                proxy("{{");
                stub("{{");
                auto& ent = static_cast<const class_entity&>(*elem);
                write_namespace(
                    from_host,
                    ent,
                    prefix + elem->get_name() + "::",
                    header,
                    proxy,
                    stub,
                    catch_stub_exceptions,
                    rethrow_exceptions,
                    enable_yas,
                    enable_protobuf,
                    enable_nanopb,
                    enable_canonical_crypto);
                header("}}");
                proxy("}}");
                stub("}}");
            }
            else if (elem->get_entity_type() == entity_type::STRUCT)
            {
                auto& ent = static_cast<const class_entity&>(*elem);
                write_struct(ent, header, enable_yas, enable_protobuf, enable_nanopb, enable_canonical_crypto);
            }

            else if (elem->get_entity_type() == entity_type::INTERFACE)
            {
                auto& ent = static_cast<const class_entity&>(*elem);
                interface_declaration_generator::write_interface(ent, header);
                write_interface(
                    from_host,
                    ent,
                    proxy,
                    stub,
                    catch_stub_exceptions,
                    rethrow_exceptions,
                    enable_yas,
                    enable_protobuf,
                    enable_nanopb,
                    enable_canonical_crypto);
            }
            else if (elem->get_entity_type() == entity_type::CONSTEXPR)
            {
                interface_declaration_generator::write_constexpr(header, *elem);
            }
            else if (elem->get_entity_type() == entity_type::CPPQUOTE)
            {
                if (!elem->is_in_import())
                {
                    auto text = elem->get_name();
                    header.write_buffer(text);
                }
            }
            else if (elem->get_entity_type() == entity_type::RUSTQUOTE)
            {
            }
        }
    }

    void write_variant_alternative_tag_specialization(
        const class_entity& m_ob,
        writer& header)
    {
        if (m_ob.is_in_import())
            return;
        if (m_ob.get_is_template())
            return;
        // The tag is the type's unqualified name. This is the same string the
        // schema and the convert.h-side dispatcher use so all three layers
        // agree on the JSON wire shape.
        const auto qualified = get_full_name(m_ob, true);
        const auto& unqualified = m_ob.get_name();
        header("");
        header("template<>");
        header("struct variant_alternative_tag<{}>", qualified);
        header("{{");
        header("    static constexpr const char* value = \"{}\";", unqualified);
        header("}};");
    }

    void write_epilog(
        bool from_host,
        const class_entity& lib,
        writer& header,
        writer& proxy,
        writer& stub,
        const std::vector<std::string>& namespaces)
    {
        for (const auto& cls : lib.get_classes())
        {
            if (!cls->get_import_lib().empty())
                continue;
            if (cls->get_entity_type() == entity_type::NAMESPACE)
            {

                write_epilog(from_host, *cls, header, proxy, stub, namespaces);
            }
            else if (cls->get_entity_type() == entity_type::STRUCT)
            {
                auto& ent = static_cast<const class_entity&>(*cls);
                write_struct_id(ent, header);
                write_variant_alternative_tag_specialization(ent, header);
            }
            else if (cls->get_entity_type() == entity_type::ENUM)
            {
                write_variant_alternative_tag_specialization(*cls, header);
            }
            else
            {
                if (cls->get_entity_type() == entity_type::INTERFACE)
                    write_encapsulate_outbound_interfaces(*cls, header, namespaces);

                if (cls->get_entity_type() == entity_type::INTERFACE)
                    write_library_proxy_factory(proxy, stub, *cls, namespaces);
            }
        }
    }

    // entry point
    void write_files(
        bool from_host,
        const class_entity& lib,
        std::ostream& hos,
        std::ostream& pos,
        std::ostream& sos,
        std::ostream& shos,
        const std::vector<std::string>& namespaces,
        const std::string& header_filename,
        const std::string& stub_header_filename,
        const std::list<std::string>& imports,
        const std::vector<std::string>& additional_headers,
        bool catch_stub_exceptions,
        const std::vector<std::string>& rethrow_exceptions,
        const std::vector<std::string>& additional_stub_headers,
        bool include_rpc_headers,
        bool enable_yas,
        bool enable_protobuf,
        bool enable_nanopb,
        bool enable_canonical_crypto)
    {
        writer header(hos);
        writer proxy(pos);
        writer stub(sos);
        writer stub_header(shos);

        header("#pragma once");
        header("");

        std::for_each(
            additional_headers.begin(),
            additional_headers.end(),
            [&](const std::string& additional_header) { header("#include <{}>", additional_header); });

        std::for_each(
            additional_stub_headers.begin(),
            additional_stub_headers.end(),
            [&](const std::string& additional_stub_header) { stub("#include <{}>", additional_stub_header); });

        header("#include <memory>");
        header("#include <vector>");
        header("#include <list>");
        header("#include <map>");
        header("#include <unordered_map>");
        header("#include <set>");
        header("#include <unordered_set>");
        header("#include <string>");
        header("#include <array>");
        header("#include <optional>");
        header("#include <rpc/internal/optional.h>");
        header("#include <variant>");
        header("#include <rpc/internal/variant.h>");
        header("#include <cstdint>");
        if (enable_canonical_crypto)
        {
            header("#include <stdexcept>");
            header("#ifdef CANOPY_BUILD_CANONICAL_CRYPTO");
            header("#include <rpc/serialization/canonical_crypto.h>");
            header("#endif");
        }

        if (include_rpc_headers)
        {
            header("#include <rpc/rpc.h>");
        }

        for (const auto& import : imports)
        {
            std::filesystem::path p(import);
            auto import_header = p.root_name() / p.parent_path() / p.stem();
            auto path = import_header.string();
            std::replace(path.begin(), path.end(), '\\', '/');
            header("#include \"{}.h\"", path);
        }

        // Forward declarations for the JSON converter ADL surface used by the
        // friend declarations emitted inside each struct body below. Keeping
        // this as a forward decl avoids dragging the full json/convert.h into
        // every consumer that just wants the IDL types.
        header("");
        header(
            "namespace json {{ inline namespace v1 {{ class object; namespace convert {{ template<typename T> "
            "struct tag; }} }} }}");
        header("");

        header("namespace rpc");
        header("{{");
        header("enum class encoding : uint64_t;");
        header("template<class T> class local_proxy;");
        header("template<class T> class weak_ptr;");
        header("template<class T> class shared_ptr;");
        header("template<class T> class optimistic_ptr;");
        header("}}");

        header("");
        header("// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)");

        proxy("#include <rpc/rpc.h>");
        proxy("#include <yas/mem_streams.hpp>");
        proxy("#include <yas/binary_iarchive.hpp>");
        proxy("#include <yas/binary_oarchive.hpp>");
        proxy("#include <yas/json_iarchive.hpp>");
        proxy("#include <yas/json_oarchive.hpp>");
        proxy("#include <yas/text_iarchive.hpp>");
        proxy("#include <yas/text_oarchive.hpp>");
        proxy("#include <yas/std_types.hpp>");
        proxy("#include <yas/count_streams.hpp>");
        proxy("#include \"{}\"", header_filename);
        proxy("// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)");

        proxy("");

        stub_header("#pragma once");
        stub_header("#include <rpc/rpc.h>");
        stub_header("");
        stub_header("// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)");

        stub("#include <rpc/rpc.h>");
        stub("#include <yas/mem_streams.hpp>");
        stub("#include <yas/binary_iarchive.hpp>");
        stub("#include <yas/binary_oarchive.hpp>");
        stub("#include <yas/count_streams.hpp>");
        stub("#include <yas/std_types.hpp>");
        stub("#include \"{}\"", header_filename);
        // stub("#include \"{}\"", yas_header_filename);
        stub("#include \"{}\"", stub_header_filename);
        stub("// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)");
        stub("");

        std::string prefix;
        for (auto& ns : namespaces)
        {
            header("namespace {}", ns);
            header("{{");
            proxy("namespace {}", ns);
            proxy("{{");
            stub("namespace {}", ns);
            stub("{{");
            stub_header("namespace {}", ns);
            stub_header("{{");

            prefix += ns + "::";
        }

        write_namespace_predeclaration(lib, header, proxy, stub);

        write_namespace(
            from_host,
            lib,
            prefix,
            header,
            proxy,
            stub,
            catch_stub_exceptions,
            rethrow_exceptions,
            enable_yas,
            enable_protobuf,
            enable_nanopb,
            enable_canonical_crypto);

        for (auto& ns : namespaces)
        {
            (void)ns;
            header("}}");
            proxy("}}");
            stub("}}");
            stub_header("}}");
        }

        header("");
        header("/****************************************************************************/");
        header("namespace rpc");
        header("{{");
        stub("namespace rpc");
        stub("{{");
        proxy("namespace rpc");
        proxy("{{");
        write_epilog(from_host, lib, header, proxy, stub, namespaces);
        header("}}");
        header("// NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)");
        proxy("}}");
        proxy("// NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)");
        stub("}}");
        stub("// NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)");
        stub_header("// NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)");
    }
}
