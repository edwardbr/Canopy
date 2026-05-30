/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <json/schema_validator.h>
#include <json/json_utils.h>
#include <connection_factory/connection_factory.h>
#include <connection_factory/options.h>

#include <gtest/gtest.h>

namespace
{
    using json::v1::parse;
    using json::v1::schema::schema_validator;

    TEST(
        JsonSchemaValidator,
        ValidatesObjectConfiguration)
    {
        const auto schema = parse(R"json({
            "type": "object",
            "required": ["transport", "port"],
            "additionalProperties": false,
            "properties": {
                "transport": {"type": "string", "enum": ["tcp_blocking", "tcp_coroutine", "spsc"]},
                "port": {"type": "integer", "minimum": 1, "maximum": 65535},
                "tls": {
                    "type": "object",
                    "required": ["certificate_path", "private_key_path"],
                    "properties": {
                        "certificate_path": {"type": "string", "minLength": 1},
                        "private_key_path": {"type": "string", "minLength": 1}
                    },
                    "additionalProperties": false
                }
            }
        })json");

        const auto valid = parse(R"json({
            "transport": "tcp_coroutine",
            "port": 18080,
            "tls": {
                "certificate_path": "server.crt",
                "private_key_path": "server.key"
            }
        })json");

        const auto invalid = parse(R"json({
            "transport": "udp",
            "port": 70000,
            "extra": true,
            "tls": {
                "certificate_path": "",
                "private_key_path": "server.key"
            }
        })json");

        schema_validator validator(schema);
        EXPECT_TRUE(validator.validate(valid));

        const auto result = validator.validate(invalid);
        EXPECT_FALSE(result);
        EXPECT_GE(result.errors().size(), size_t{4});
    }

    TEST(
        JsonSchemaValidator,
        ResolvesLocalRefs)
    {
        const auto result = schema_validator::validate_json(
            R"json({
                "type": "object",
                "properties": {
                    "listen": {"$ref": "#/$defs/listen"}
                },
                "$defs": {
                    "listen": {
                        "type": "object",
                        "required": ["address", "port"],
                        "properties": {
                            "address": {"type": "string"},
                            "port": {"type": "integer", "minimum": 1}
                        }
                    }
                }
            })json",
            R"json({
                "listen": {
                    "address": "127.0.0.1",
                    "port": 8080
                }
            })json");

        EXPECT_TRUE(result);
    }

    TEST(
        JsonSchemaValidator,
        ResolvesRecursiveLocalRefs)
    {
        schema_validator validator(R"json({
            "$ref": "#/$defs/node",
            "$defs": {
                "node": {
                    "type": "object",
                    "required": ["name"],
                    "properties": {
                        "name": {"type": "string"},
                        "children": {
                            "type": "array",
                            "items": {"$ref": "#/$defs/node"}
                        }
                    },
                    "additionalProperties": false
                }
            }
        })json");

        EXPECT_TRUE(validator.validate(parse(R"json({
            "name": "root",
            "children": [
                {"name": "child", "children": [{"name": "grandchild"}]}
            ]
        })json")));

        const auto result = validator.validate(parse(R"json({
            "name": "root",
            "children": [
                {"name": 7}
            ]
        })json"));

        EXPECT_FALSE(result);
    }

    TEST(
        JsonSchemaValidator,
        SelfRefDoesNotRecurseForever)
    {
        EXPECT_TRUE(schema_validator::validate_json(R"json({"$ref": "#"})json", R"json({"anything": true})json"));
    }

    TEST(
        JsonSchemaValidator,
        SupportsCombinatorsAndConditionals)
    {
        schema_validator validator(R"json({
            "type": "object",
            "required": ["mode"],
            "properties": {
                "mode": {"enum": ["plain", "tls"]},
                "certificate_path": {"type": "string"}
            },
            "if": {
                "properties": {"mode": {"const": "tls"}},
                "required": ["mode"]
            },
            "then": {
                "required": ["certificate_path"]
            }
        })json");

        EXPECT_TRUE(validator.validate(parse(R"json({"mode": "plain"})json")));
        EXPECT_TRUE(validator.validate(parse(R"json({"mode": "tls", "certificate_path": "server.crt"})json")));
        EXPECT_FALSE(validator.validate(parse(R"json({"mode": "tls"})json")));
    }

    TEST(
        JsonSchemaValidator,
        ArrayItemsAndUniqueItemsWorkWithoutPatternProperties)
    {
        // patternProperties used to live in this test; it has since been
        // stripped from the validator because the IDL-driven schema
        // generator never emits regex keywords and supporting them brought a
        // thread-safety / locale / regex-engine surface we didn't want. The
        // surrounding non-pattern constraints still work as before.
        schema_validator validator(R"json({
            "type": "object",
            "properties": {
                "ports": {
                    "type": "array",
                    "minItems": 2,
                    "uniqueItems": true,
                    "items": {"type": "integer", "minimum": 1}
                }
            },
            "additionalProperties": false
        })json");

        EXPECT_TRUE(validator.validate(parse(R"json({"ports": [1000, 1001]})json")));
        EXPECT_FALSE(validator.validate(parse(R"json({"ports": [1000, 1000]})json")));
        EXPECT_FALSE(validator.validate(parse(R"json({"ports": [1000]})json")));
        EXPECT_FALSE(validator.validate(parse(R"json({"ports": [1000, 1001], "extra": 1})json")));
    }

    TEST(
        JsonSchemaValidator,
        PatternKeywordIsRejectedAsUnsupported)
    {
        // The validator surfaces hand-written schemas that use regex
        // keywords instead of silently dropping them on the floor.
        schema_validator validator(R"json({
            "type": "string",
            "pattern": "^canopy-"
        })json");

        const auto result = validator.validate(parse(R"json("canopy-svc")json"));
        ASSERT_FALSE(result);
        EXPECT_NE(result.errors().front().message.find("`pattern`"), std::string::npos);
        EXPECT_NE(result.errors().front().message.find("not supported"), std::string::npos);
    }

    TEST(
        JsonSchemaValidator,
        PatternPropertiesKeywordIsRejectedAsUnsupported)
    {
        schema_validator validator(R"json({
            "type": "object",
            "patternProperties": {
                "^CANOPY_": {"type": "string"}
            }
        })json");

        const auto result = validator.validate(parse(R"json({"CANOPY_LOG": "debug"})json"));
        ASSERT_FALSE(result);
        EXPECT_NE(result.errors().front().message.find("`patternProperties`"), std::string::npos);
        EXPECT_NE(result.errors().front().message.find("not supported"), std::string::npos);
    }

    TEST(
        JsonSchemaValidator,
        TreatsJsonNumbersByValueForSchemaEquality)
    {
        EXPECT_TRUE(schema_validator::validate_json(R"json({"const": 1.0})json", R"json(1)json"));
        EXPECT_TRUE(schema_validator::validate_json(R"json({"enum": [2.0]})json", R"json(2)json"));
        EXPECT_FALSE(
            schema_validator::validate_json(R"json({"type": "array", "uniqueItems": true})json", R"json([3, 3.0])json"));
    }

    TEST(
        JsonSchemaValidator,
        AppliesSchemaDefaultsAndClientOverrides)
    {
        const auto schema = parse(R"json({
            "type": "object",
            "properties": {
                "endpoint": {
                    "type": "object",
                    "properties": {
                        "host": {"type": "string", "default": "127.0.0.1"},
                        "port": {"type": "integer", "default": 8080},
                        "ipv6": {"type": "boolean", "default": false}
                    },
                    "additionalProperties": false
                },
                "rpc": {
                    "type": "object",
                    "properties": {
                        "encoding": {"type": "string", "default": "yas_json"},
                        "call_timeout": {"type": "integer"}
                    },
                    "additionalProperties": false
                }
            },
            "additionalProperties": false
        })json");

        const auto defaults = parse(R"json({
            "endpoint": {"port": 18080},
            "rpc": {"call_timeout": 30000}
        })json");

        const auto client_overrides = parse(R"json({
            "endpoint": {"host": "127.0.0.2", "ipv6": null},
            "rpc": {"encoding": "nanopb"}
        })json");

        const auto effective = json::v1::apply_schema_overlay(schema, defaults, client_overrides);
        const auto endpoint = json::v1::config_view(effective).at("endpoint");
        const auto rpc = json::v1::config_view(effective).at("rpc");

        EXPECT_EQ(endpoint.at("host").require<std::string>(), "127.0.0.2");
        EXPECT_EQ(endpoint.at("port").require<uint16_t>(), 18080);
        EXPECT_FALSE(endpoint.at("ipv6").require<bool>());
        EXPECT_EQ(rpc.at("encoding").require<std::string>(), "nanopb");
        EXPECT_EQ(rpc.at("call_timeout").require<uint64_t>(), uint64_t{30000});

        EXPECT_THROW(
            static_cast<void>(json::v1::apply_schema_overlay(
                schema, json::v1::object(json::v1::map{}), parse(R"json({"bad": true})json"))),
            json::v1::schema::validation_exception);
    }

    TEST(
        JsonSchemaValidator,
        ConnectionSettingsRejectLegacyAliases)
    {
        const auto valid = rpc::connection_factory::materialise_connection_settings(parse(R"json({
            "service": {"type": "service", "settings": {"name": "service"}},
            "transport": {
                "type": "stream_rpc",
                "settings": {"name": "transport", "service_proxy_name": "connection", "encoding": "nanopb", "call_timeout": 30000}
            },
            "listener": {"type": "stream_rpc", "settings": {"name": "listener"}}
        })json"));
        EXPECT_EQ(valid.error_code, rpc::error::OK());

        const auto legacy_aliases = rpc::connection_factory::materialise_connection_settings(parse(R"json({
            "endpoint": {"host": "127.0.0.1", "port": 8080},
            "tcp_coroutine": {"controller": {"queue_depth": 256}, "stream": {"timeout_strategy": "linked_timeout"}},
            "host": "127.0.0.1",
            "port": 8080,
            "service_name": "service",
            "transport_name": "transport",
            "listener_name": "listener",
            "connection_name": "connection",
            "encoding": "nanopb",
            "call_timeout": 30000
        })json"));
        EXPECT_EQ(legacy_aliases.error_code, rpc::error::INVALID_DATA());
    }

    TEST(
        JsonSchemaValidator,
        ConnectionSettingsAcceptSparseStreamLayersConfig)
    {
        const auto valid = rpc::connection_factory::materialise_connection_settings(parse(R"json({
            "transport": {
                "type": "stream_rpc",
                "settings": {"name": "client_transport", "service_proxy_name": "server", "encoding": "nanopb"}
            },
            "stream_layers": [
                {"type": "tcp_blocking", "settings": {"host": "127.0.0.1", "port": 8080}},
                {"type": "tls", "settings": {"client": {"verify_peer": false}}},
                {"type": "websocket", "settings": {"keep_alive": {"enabled": false}}},
                {"type": "compression", "settings": {"algorithm": "zstd", "level": 3}}
            ]
        })json"));

        ASSERT_EQ(valid.error_code, rpc::error::OK());
        ASSERT_EQ(valid.settings.stream_layers.size(), 4u);
        EXPECT_EQ(valid.settings.stream_layers[0].type, "tcp_blocking");
        EXPECT_EQ(valid.settings.stream_layers[1].type, "tls");
        EXPECT_EQ(valid.settings.stream_layers[2].type, "websocket");
        EXPECT_EQ(valid.settings.stream_layers[3].type, "compression");
    }
}
