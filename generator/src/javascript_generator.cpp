/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "javascript_generator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "attributes.h"
#include "coreclasses.h"
#include "fingerprint_generator.h"
#include "helpers.h"

namespace javascript_generator
{
    namespace
    {
        // Convert snake_case to camelCase (matches pbjs field name convention)
        std::string to_camel_case(const std::string& snake)
        {
            std::string result;
            bool next_upper = false;
            for (char c : snake)
            {
                if (c == '_')
                {
                    next_upper = true;
                }
                else if (next_upper)
                {
                    result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    next_upper = false;
                }
                else
                {
                    result += c;
                }
            }
            return result;
        }

        // Convert to UPPER_CASE for constant names (e.g., i_calculator → I_CALCULATOR)
        std::string to_upper(const std::string& s)
        {
            std::string result = s;
            for (auto& c : result)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return result;
        }

        // Convert snake_case to PascalCase for module export name
        std::string to_pascal_case(const std::string& snake)
        {
            std::string camel = to_camel_case(snake);
            if (!camel.empty())
                camel[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(camel[0])));
            return camel;
        }

        // Sanitize a proto field name (matches protobuf_generator.cpp sanitize_field_name)
        std::string sanitize_field_name(const std::string& name)
        {
            std::string result = name;
            for (auto& c : result)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                    c = '_';
            if (!result.empty() && !std::isalpha(static_cast<unsigned char>(result[0])) && result[0] != '_')
                result = "_" + result;
            return result;
        }

        // Proto field name as accessed in pbjs JS (sanitized then camelCase)
        std::string proto_js_field(const std::string& param_name)
        {
            return to_camel_case(sanitize_field_name(param_name));
        }

        // Proto request/response message name (matches protobuf_generator.cpp naming)
        std::string proto_request_name(const std::string& iface, const std::string& method)
        {
            return iface + "_" + method + "Request";
        }

        std::string proto_response_name(const std::string& iface, const std::string& method)
        {
            return iface + "_" + method + "Response";
        }

        bool has_callable_methods(const class_entity& iface_entity)
        {
            for (auto& fn : iface_entity.get_functions())
            {
                if (fn->get_entity_type() == entity_type::FUNCTION_METHOD && !fn->has_value("post"))
                    return true;
            }
            return false;
        }

        bool has_post_methods(const class_entity& iface_entity)
        {
            for (auto& fn : iface_entity.get_functions())
            {
                if (fn->get_entity_type() == entity_type::FUNCTION_METHOD && fn->has_value("post"))
                    return true;
            }
            return false;
        }

        // Get the 1-based ordinal of a named method in declaration order
        int method_ordinal(const class_entity& iface_entity, const std::string& method_name)
        {
            int ordinal = 0;
            for (auto& fn : iface_entity.get_functions())
            {
                if (fn->get_entity_type() == entity_type::FUNCTION_METHOD)
                {
                    ordinal++;
                    if (fn->get_name() == method_name)
                        return ordinal;
                }
            }
            return ordinal;
        }

        void write_proxy_class(std::ostream& out, const class_entity& lib, const class_entity& iface_entity)
        {
            const std::string iface = iface_entity.get_name();

            out << "    // Proxy for " << iface << " — makes async RPC calls to the server\n";
            out << "    function " << iface << "_proxy(transport, proto) {\n";
            out << "        this._transport = transport;\n";
            out << "        this._proto = proto;\n";
            out << "    }\n\n";

            for (auto& fn : iface_entity.get_functions())
            {
                if (fn->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                if (fn->has_value("post"))
                    continue;

                const std::string method = fn->get_name();
                const std::string req_name = proto_request_name(iface, method);
                const std::string resp_name = proto_response_name(iface, method);
                const bool has_return = !fn->get_return_type().empty() && fn->get_return_type() != "void";

                // Collect in-params
                std::vector<std::string> in_param_names;
                for (auto& p : fn->get_parameters())
                {
                    bool implicit_in = !is_in_param(p) && !is_out_param(p);
                    if (is_in_param(p) || implicit_in)
                        in_param_names.push_back(p.get_name());
                }

                // Collect out-params
                std::vector<std::string> out_param_names;
                for (auto& p : fn->get_parameters())
                {
                    if (is_out_param(p))
                        out_param_names.push_back(p.get_name());
                }

                // Function signature
                out << "    " << iface << "_proxy.prototype." << method << " = async function(";
                for (size_t i = 0; i < in_param_names.size(); i++)
                {
                    if (i > 0)
                        out << ", ";
                    out << in_param_names[i];
                }
                out << ") {\n";

                // Build request
                out << "        var req = this._proto." << req_name << ".create({\n";
                for (auto& p : fn->get_parameters())
                {
                    bool implicit_in = !is_in_param(p) && !is_out_param(p);
                    if (!is_in_param(p) && !implicit_in)
                        continue;
                    std::string field = proto_js_field(p.get_name());
                    bool optimistic = false;
                    std::shared_ptr<class_entity> obj;
                    if (is_interface_param(iface_entity, p.get_type(), optimistic, obj))
                    {
                        // Interface pointer: use stub.getRemoteObject()
                        out << "            " << field << ": " << p.get_name() << ".getRemoteObject(),\n";
                    }
                    else
                    {
                        out << "            " << field << ": " << p.get_name() << ",\n";
                    }
                }
                out << "        });\n";
                out << "        var reqBytes = this._proto." << req_name << ".encode(req).finish();\n";
                out << "        var respBytes = await this._transport.call(\n";
                out << "            " << to_upper(iface) << "_ID,\n";
                out << "            " << iface << "_method." << method << ",\n";
                out << "            reqBytes);\n";
                out << "        var resp = this._proto." << resp_name << ".decode(respBytes);\n";

                // Return object
                out << "        return {\n";
                if (has_return)
                    out << "            result: resp.result,\n";
                for (auto& op : out_param_names)
                    out << "            " << op << ": resp." << proto_js_field(op) << ",\n";
                out << "        };\n";
                out << "    };\n\n";
            }
            // Unused lib suppression
            (void)lib;
        }

        void write_stub_class(std::ostream& out, const class_entity& lib, const class_entity& iface_entity)
        {
            const std::string iface = iface_entity.get_name();

            out << "    // Stub for " << iface << " — dispatches server-initiated [post] events\n";
            out << "    function " << iface << "_stub(impl) {\n";
            out << "        this._impl = impl;\n";
            out << "    }\n\n";

            out << "    " << iface << "_stub.prototype.getInterfaceId = function() {\n";
            out << "        return " << to_upper(iface) << "_ID;\n";
            out << "    };\n\n";

            // getRemoteObject: used by proxy methods that accept this stub as an interface param
            out << "    " << iface << "_stub.prototype.getRemoteObject = function() {\n";
            out << "        return this._remoteObject || null;\n";
            out << "    };\n\n";

            out << "    " << iface << "_stub.prototype._setRemoteObject = function(obj) {\n";
            out << "        this._remoteObject = obj;\n";
            out << "    };\n\n";

            out << "    " << iface << "_stub.prototype.handlePost = function(proto, interfaceId, methodId, data) {\n";
            out << "        var methodNum = (methodId && methodId.toNumber) ? methodId.toNumber() : Number(methodId);\n";
            out << "        switch (methodNum) {\n";

            for (auto& fn : iface_entity.get_functions())
            {
                if (fn->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                if (!fn->has_value("post"))
                    continue;

                const std::string method = fn->get_name();
                const int ordinal = method_ordinal(iface_entity, method);
                const std::string req_name = proto_request_name(iface, method);

                out << "            case " << ordinal << ": {\n";
                out << "                var msg = proto." << req_name << ".decode(data);\n";
                out << "                if (this._impl." << method << ") {\n";
                out << "                    this._impl." << method << "(";
                bool first = true;
                for (auto& p : fn->get_parameters())
                {
                    bool implicit_in = !is_in_param(p) && !is_out_param(p);
                    if (!is_in_param(p) && !implicit_in)
                        continue;
                    if (!first)
                        out << ", ";
                    out << "msg." << proto_js_field(p.get_name());
                    first = false;
                }
                out << ");\n";
                out << "                }\n";
                out << "                break;\n";
                out << "            }\n";
            }

            out << "            default:\n";
            out << "                console.warn('[Canopy] Unknown post method id:', methodNum);\n";
            out << "        }\n";
            out << "    };\n\n";

            // Unused lib suppression
            (void)lib;
        }

        void write_namespace_interfaces(
            std::ostream& out,
            const class_entity& lib,
            const class_entity& ns,
            std::vector<std::string>& proxy_names,
            std::vector<std::string>& stub_names)
        {
            for (auto& elem : ns.get_elements(entity_type::NAMESPACE_MEMBERS))
            {
                if (elem->is_in_import())
                    continue;

                if (elem->get_entity_type() == entity_type::NAMESPACE)
                {
                    auto& sub_ns = static_cast<const class_entity&>(*elem);
                    write_namespace_interfaces(out, lib, sub_ns, proxy_names, stub_names);
                }
                else if (elem->get_entity_type() == entity_type::INTERFACE)
                {
                    auto& iface_entity = static_cast<const class_entity&>(*elem);
                    const std::string iface = iface_entity.get_name();

                    // Interface ID constant (V3 fingerprint, matching C++ get_id(rpc::VERSION_3))
                    uint64_t id = fingerprint::generate(iface_entity, {}, nullptr, 3);
                    out << "    var " << to_upper(iface) << "_ID = Long.fromString('" << std::to_string(id) << "', true);\n";

                    // Method ID constants (1-based, declaration order)
                    out << "    var " << iface << "_method = {\n";
                    int ordinal = 0;
                    for (auto& fn : iface_entity.get_functions())
                    {
                        if (fn->get_entity_type() == entity_type::FUNCTION_METHOD)
                        {
                            ordinal++;
                            out << "        " << fn->get_name() << ": Long.fromNumber(" << ordinal << ", true),\n";
                        }
                    }
                    out << "    };\n\n";

                    if (has_callable_methods(iface_entity))
                    {
                        write_proxy_class(out, lib, iface_entity);
                        proxy_names.push_back(iface);
                    }

                    if (has_post_methods(iface_entity))
                    {
                        write_stub_class(out, lib, iface_entity);
                        stub_names.push_back(iface);
                    }
                }
            }
        }
    }

    std::filesystem::path write_files(
        const class_entity& lib,
        const std::filesystem::path& output_path,
        const std::filesystem::path& base_filename)
    {
        std::filesystem::create_directories(output_path);
        auto js_path = output_path / (base_filename.string() + ".js");

        std::ostringstream out;

        const std::string module_name = to_pascal_case(base_filename.string());

        out << "// GENERATED FILE — DO NOT EDIT\n";
        out << "// Source IDL: " << base_filename.string() << ".idl\n";
        out << "// Regenerate by rebuilding the CMake target " << base_filename.string() << "_js_generate\n";
        out << "(function(root, factory) {\n";
        out << "    if (typeof module === 'object' && module.exports) {\n";
        out << "        module.exports = factory(require('long'));\n";
        out << "    } else {\n";
        out << "        root." << module_name << " = factory(root.Long);\n";
        out << "    }\n";
        out << "})(typeof globalThis !== 'undefined' ? globalThis : this, function(Long) {\n";
        out << "    'use strict';\n\n";

        std::vector<std::string> proxy_names;
        std::vector<std::string> stub_names;
        write_namespace_interfaces(out, lib, lib, proxy_names, stub_names);

        // Return object
        out << "    return {\n";
        for (const auto& name : proxy_names)
            out << "        " << name << "_proxy: " << name << "_proxy,\n";
        for (const auto& name : stub_names)
            out << "        " << name << "_stub: " << name << "_stub,\n";
        out << "        interfaceIds: {\n";
        for (const auto& name : proxy_names)
            out << "            " << name << ": " << to_upper(name) << "_ID,\n";
        for (const auto& name : stub_names)
        {
            bool already_proxy = std::find(proxy_names.begin(), proxy_names.end(), name) != proxy_names.end();
            if (!already_proxy)
                out << "            " << name << ": " << to_upper(name) << "_ID,\n";
        }
        out << "        },\n";
        out << "    };\n";
        out << "});\n";

        // Compare and write only if different (avoids unnecessary CMake rebuilds)
        std::string existing;
        {
            std::ifstream f(js_path);
            if (f)
                existing.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        const std::string generated = out.str();
        if (generated != existing)
        {
            std::ofstream f(js_path);
            f << generated;
        }

        return js_path;
    }
}
