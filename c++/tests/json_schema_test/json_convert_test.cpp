/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Tests for the generator-emitted `from_json_object` / `to_json_object`
// converters. The converters live in the IDL struct's namespace and are
// found by ADL through `json::v1::convert::tag<T>`.

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <json/config_loader.h>
#include <json/config.h>
#include <json/convert.h>
#include <json/json_dom.h>
#include <json/json_utils.h>
#include <json/schema_validator.h>
#include <connection_factory/connection_factory.h>
#include <connection_factory/application_config.h>
#include <connection_factory/context.h>
#include <connection_factory_config/connection_factory_config_schema.h>
#include <example_shared/example_shared_schema.h>
#include <tcp_blocking_stream/tcp_blocking_stream_config_schema.h>
#include <tcp_coroutine_stream/tcp_coroutine_stream_config_schema.h>
#include <network_config/network_config_schema.h>
#include <rpc/rpc_types_schema.h>
#include <schema_cycle/schema_cycle_schema.h>
#include <set_fixture/set_fixture_schema.h>
#ifdef CANOPY_BUILD_COROUTINE
#  include <streaming/tcp_coroutine/factory.h>
#else
#  include <streaming/tcp_blocking/factory.h>
#endif
#ifdef CANOPY_SECURE_STREAM_BACKEND_OPENSSL
#  include <openssl_tls_stream/openssl_tls_stream_config_schema.h>
#endif
#ifdef CANOPY_CONNECTION_FACTORY_HAS_WEBSOCKET
#  include <websocket_stream/websocket_stream_config_schema.h>
#endif
#ifdef CANOPY_CONNECTION_FACTORY_HAS_COMPRESSION
#  include <compression_stream/compression_stream_config_schema.h>
#endif
#ifdef CANOPY_HAS_HTTP_SERVER_CONFIG
#  include <http_server/http_server_config_schema.h>
#endif
#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
#  include <local_transport/local_transport_config_schema.h>
#endif
#ifdef CANOPY_CONNECTION_FACTORY_HAS_IPC_SPSC
#  include <ipc_spsc/config_schema.h>
#endif
#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
#  include <sgx_coroutine_transport/sgx_coroutine_transport_config_schema.h>
#endif
#ifdef CANOPY_HAS_SGX_BLOCKING_TRANSPORT_CONFIG
#  include <sgx_blocking_transport/sgx_blocking_transport_config_schema.h>
#endif
#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
#  include <transports/sgx_blocking/transport.h>
#endif
#ifdef CANOPY_HAS_SGX_ENCLAVE_RUNTIME_CONFIG
#  include <sgx_enclave_runtime/sgx_enclave_runtime_config_schema.h>
#endif

namespace
{
    using json::v1::parse;
    using json::v1::convert::from_json_object;
    using json::v1::convert::to_json_object;

    TEST(
        JsonConvert,
        JsonDomRawKeepsNestedContainersConst)
    {
        using raw_type
            = std::remove_cv_t<std::remove_reference_t<decltype(std::declval<const json::v1::object&>().raw())>>;
        using array_slot = std::variant_alternative_t<json::v1::object::ARRAY_TYPE_INDEX, raw_type>;
        using map_slot = std::variant_alternative_t<json::v1::object::MAP_TYPE_INDEX, raw_type>;

        static_assert(std::is_same_v<array_slot, std::unique_ptr<const json::v1::array>>);
        static_assert(std::is_same_v<map_slot, std::unique_ptr<const json::v1::map>>);

        const auto value = parse(R"json({"items":[{"name":"immutable"}]})json");
        const auto& items = value.as_map().at("items").as_array();
        ASSERT_EQ(items.size(), size_t{1});
        EXPECT_EQ(items.front().as_map().at("name").get<std::string>(), "immutable");
    }

#ifdef CANOPY_BUILD_COROUTINE
    constexpr const char* active_tcp_stream_type = "tcp_coroutine";
    constexpr const char* inactive_tcp_stream_type = "tcp_blocking";
    static_assert(std::is_same_v<
        streaming::tcp::endpoint,
        rpc::tcp_coroutine_stream::endpoint>);
#  ifdef __linux__
#    ifndef CANOPY_CONNECTION_FACTORY_HAS_TCP_COROUTINE
#      error "Coroutine Linux builds should register tcp_coroutine in the connection factory"
#    endif
#  endif
#  ifdef CANOPY_CONNECTION_FACTORY_HAS_TCP_BLOCKING
#    error "Coroutine builds must not register tcp_blocking in the connection factory"
#  endif
#else
    constexpr const char* active_tcp_stream_type = "tcp_blocking";
    constexpr const char* inactive_tcp_stream_type = "tcp_coroutine";
    static_assert(std::is_same_v<
        streaming::tcp::endpoint,
        rpc::tcp_blocking_stream::endpoint>);
#  ifndef CANOPY_CONNECTION_FACTORY_HAS_TCP_BLOCKING
#    error "Blocking builds should register tcp_blocking in the connection factory"
#  endif
#  ifdef CANOPY_CONNECTION_FACTORY_HAS_TCP_COROUTINE
#    error "Blocking builds must not register tcp_coroutine in the connection factory"
#  endif
#endif

    TEST(
        JsonConvert,
        TcpEndpointRoundTripThroughTypedStruct)
    {
        const auto input = parse(R"json({
            "host": "192.168.0.1",
            "port": 8443,
            "ipv6": false,
            "connect_timeout": 7500
        })json");

        const auto typed = from_json_object<streaming::tcp::endpoint>(input);
        EXPECT_EQ(typed.host, "192.168.0.1");
        EXPECT_EQ(typed.port, 8443);
        EXPECT_FALSE(typed.ipv6);
        EXPECT_EQ(typed.connect_timeout, 7500u);

        // Round-trip: typed -> json::v1::object -> typed.
        const auto round_tripped = from_json_object<streaming::tcp::endpoint>(to_json_object(typed));
        EXPECT_EQ(round_tripped.host, typed.host);
        EXPECT_EQ(round_tripped.port, typed.port);
        EXPECT_EQ(round_tripped.ipv6, typed.ipv6);
        EXPECT_EQ(round_tripped.connect_timeout, typed.connect_timeout);
    }

    TEST(
        JsonConvert,
        RpcOptionalAcceptsValuesConvertibleToStoredType)
    {
        rpc::optional<std::string> direct = "server_transport";
        ASSERT_TRUE(direct.has_value());
        EXPECT_EQ(direct.value(), "server_transport");

        direct = "client_transport";
        ASSERT_TRUE(direct.has_value());
        EXPECT_EQ(direct.value(), "client_transport");

        rpc::connection_factory::service_settings named;
        named.name = "listener_transport";
        ASSERT_TRUE(named.name.has_value());
        EXPECT_EQ(named.name.value(), "listener_transport");
    }

    TEST(
        JsonConvert,
        MissingTcpEndpointFieldsUseIdlDefaults)
    {
        const auto input = parse(R"json({})json");
        const auto typed = from_json_object<streaming::tcp::endpoint>(input);
        EXPECT_EQ(typed.host, "127.0.0.1");
        EXPECT_EQ(typed.port, 0);
        EXPECT_FALSE(typed.ipv6);
        EXPECT_EQ(typed.connect_timeout, 5000u);
    }

    TEST(
        JsonConvert,
        ExplicitNullUsesTcpEndpointDefault)
    {
        const auto input = parse(R"json({"host": null, "port": 9000})json");
        const auto typed = from_json_object<streaming::tcp::endpoint>(input);
        EXPECT_EQ(typed.host, "127.0.0.1");
        EXPECT_EQ(typed.port, 9000);
    }

    TEST(
        JsonConvert,
        ToJsonWritesConcreteTcpEndpointDefaults)
    {
        streaming::tcp::endpoint typed;
        typed.port = uint16_t{443};
        const auto serialised = to_json_object(typed);
        ASSERT_EQ(serialised.get_type(), json::v1::object::type::map_type);
        const auto& map = serialised.as_map();
#ifdef CANOPY_BUILD_COROUTINE
        EXPECT_GE(map.size(), 4u);
#else
        EXPECT_EQ(map.size(), 4u);
#endif
        ASSERT_NE(map.find("host"), map.end());
        ASSERT_NE(map.find("port"), map.end());
        ASSERT_NE(map.find("ipv6"), map.end());
        ASSERT_NE(map.find("connect_timeout"), map.end());
    }

    TEST(
        JsonConvert,
        NestedStructIsBuiltViaAdl)
    {
        const auto input = parse(R"json({
            "type": "stream_rpc",
            "settings": {"call_timeout": 42}
        })json");

        const auto typed = from_json_object<rpc::connection_factory::typed_settings>(input);

        EXPECT_EQ(typed.type, "stream_rpc");
        ASSERT_TRUE(typed.settings.has_value());
        const auto stream_rpc_settings
            = from_json_object<rpc::stream_transport::transport_settings>(typed.settings.value());
        ASSERT_TRUE(stream_rpc_settings.call_timeout.has_value());
        EXPECT_EQ(*stream_rpc_settings.call_timeout, uint64_t{42});
    }

    TEST(
        JsonConvert,
        RequiredFieldMissingThrows)
    {
        // schema_cycle::config_node has a required `name` field with no IDL
        // default and a recursive optional `children` list of nested nodes.
        const auto input = parse(R"json({"children": []})json");
        EXPECT_THROW(static_cast<void>(from_json_object<schema_cycle::config_node>(input)), json::v1::config_error);
    }

    TEST(
        JsonConvert,
        RecursiveStructPopulatesChildrenViaAdl)
    {
        // schema_cycle::config_node declares `children` without a default, so
        // each nested node must spell its (possibly empty) children list out.
        const auto input = parse(R"json({
            "name": "root",
            "children": [
                {"name": "child_a", "children": []},
                {"name": "child_b", "children": []}
            ]
        })json");

        const auto typed = from_json_object<schema_cycle::config_node>(input);
        EXPECT_EQ(typed.name, "root");
        ASSERT_EQ(typed.children.size(), 2u);
        EXPECT_EQ(typed.children[0].name, "child_a");
        EXPECT_EQ(typed.children[1].name, "child_b");
    }

    TEST(
        JsonConvert,
        RequiredVectorMissingThrows)
    {
        // No IDL default on `children` means missing the key is an error,
        // matching the strict-required semantics for non-optional fields.
        const auto input = parse(R"json({"name": "lonely"})json");
        EXPECT_THROW(static_cast<void>(from_json_object<schema_cycle::config_node>(input)), json::v1::config_error);
    }

    TEST(
        JsonConvert,
        EnumRoundTripsByName)
    {
        const auto from_string = from_json_object<canopy::network_config::ip_address_family>(parse("\"ipv6\""));
        EXPECT_EQ(from_string, canopy::network_config::ip_address_family::ipv6);

        const auto from_int = from_json_object<canopy::network_config::ip_address_family>(parse("0"));
        EXPECT_EQ(from_int, canopy::network_config::ip_address_family::ipv4);

        const auto serialised = to_json_object(canopy::network_config::ip_address_family::ipv6);
        ASSERT_EQ(serialised.get_type(), json::v1::object::type::string_type);
        EXPECT_EQ(serialised.get<std::string>(), "ipv6");

        EXPECT_THROW(
            static_cast<void>(from_json_object<canopy::network_config::ip_address_family>(parse("\"ipx\""))),
            json::v1::config_error);
    }

    TEST(
        JsonConvert,
        EnumFieldInStructUsesIdlDefaultWhenMissing)
    {
        // tcp_endpoint declares `family = ip_address_family::ipv4` in the IDL.
        const auto input = parse(R"json({"name": "lo", "port": 22})json");
        const auto typed = from_json_object<canopy::network_config::tcp_endpoint>(input);
        EXPECT_EQ(typed.name, "lo");
        EXPECT_EQ(typed.port, 22);
        EXPECT_EQ(typed.family, canopy::network_config::ip_address_family::ipv4);
    }

    TEST(
        JsonConvert,
        RpcVariantRoundTripsTagKeyedShape)
    {
        // variant_value is rpc::variant<int32_t, std::string>. The new wire
        // shape keys each alternative by its canonical type tag ("int32" /
        // "string") rather than the old index-based "caseN" form.
        const auto input = parse(R"json({
            "optional_int": 42,
            "variant_value": {"string": "hello"},
            "json_value": {"nested": [1, 2, 3]},
            "optional_json_value": null,
            "rpc_optional_int": 9,
            "rpc_optional_json_value": {"x": true}
        })json");

        const auto typed = from_json_object<xxx::optional_variant_json_holder>(input);

        ASSERT_TRUE(typed.optional_int.has_value());
        EXPECT_EQ(*typed.optional_int, 42);
        ASSERT_EQ(typed.variant_value.index(), 1u);
        EXPECT_EQ(rpc::get<std::string>(typed.variant_value), "hello");
        ASSERT_EQ(typed.json_value.get_type(), json::v1::object::type::map_type);
        EXPECT_FALSE(typed.optional_json_value.has_value());

        // Round-trip preserves the tag-keyed variant shape.
        const auto serialised = to_json_object(typed);
        const auto& serialised_map = serialised.as_map();
        ASSERT_NE(serialised_map.find("variant_value"), serialised_map.end());
        const auto& variant_obj = serialised_map.at("variant_value").as_map();
        ASSERT_EQ(variant_obj.size(), 1u);
        EXPECT_EQ(variant_obj.begin()->first, "string");
    }

    TEST(
        JsonConvert,
        VariantUnknownTagThrows)
    {
        const auto input = parse(R"json({
            "optional_int": null,
            "variant_value": {"wibble": 0},
            "json_value": null,
            "rpc_optional_int": null
        })json");
        EXPECT_THROW(static_cast<void>(from_json_object<xxx::optional_variant_json_holder>(input)), json::v1::config_error);
    }

    TEST(
        JsonConvert,
        VariantLegacyCaseKeyIsRejected)
    {
        // Clean break with the old wire shape: the index-keyed "caseN" form
        // no longer reads. Documents the migration contract.
        const auto input = parse(R"json({
            "optional_int": null,
            "variant_value": {"case0": 1},
            "json_value": null,
            "rpc_optional_int": null
        })json");
        EXPECT_THROW(static_cast<void>(from_json_object<xxx::optional_variant_json_holder>(input)), json::v1::config_error);
    }

    TEST(
        JsonConvert,
        EnumRoundTripValidatesAgainstGeneratedSchema)
    {
        // The schema must accept the string form the converter emits.
        // Previously the enum schema was integer-only, so a round-tripped
        // typed value would fail validation against its own schema.
        canopy::network_config::tcp_endpoint typed;
        typed.name = "lo";
        typed.family = canopy::network_config::ip_address_family::ipv6;

        const auto serialised = to_json_object(typed);
        const auto schema = json::v1::parse(canopy::network_config::tcp_endpoint::get_schema());

        json::v1::schema::schema_validator validator(schema);
        const auto result = validator.validate(serialised);
        EXPECT_TRUE(result) << (result.errors().empty() ? "" : result.errors().front().message);
    }

    TEST(
        JsonConvert,
        RawJsonObjectFieldAcceptsExplicitNull)
    {
        // json::v1::object is the late-bound passthrough type; null is a
        // legitimate JSON value, not "field missing".
        const auto input = parse(R"json({
            "optional_int": null,
            "variant_value": {"int32": 1},
            "json_value": null,
            "rpc_optional_int": null
        })json");

        const auto typed = from_json_object<xxx::optional_variant_json_holder>(input);
        EXPECT_EQ(typed.json_value.get_type(), json::v1::object::type::null_type);
    }

    TEST(
        JsonConvert,
        VectorOfNestedStructsRoundTrips)
    {
        const auto input = parse(R"json({
            "name": "root",
            "children": [
                {"name": "child_a", "children": []}
            ]
        })json");
        const auto typed = from_json_object<schema_cycle::config_node>(input);
        const auto round_tripped = from_json_object<schema_cycle::config_node>(to_json_object(typed));
        ASSERT_EQ(round_tripped.children.size(), 1u);
        EXPECT_EQ(round_tripped.children.front().name, "child_a");
    }

    TEST(
        JsonConvert,
        StringKeyedMapRoundTripsAndValidatesAgainstSchema)
    {
        // map_val is std::map<std::string, something_complicated>. The schema
        // and converter must agree on the wire shape — JSON object, not the
        // legacy array-of-{k,v}.
        xxx::something_more_complicated typed;
        typed.vector_val = {};
        typed.map_val.emplace("alpha", xxx::something_complicated{});
        typed.map_val.emplace("beta", xxx::something_complicated{});

        const auto serialised = to_json_object(typed);
        const auto& serialised_map = serialised.as_map();
        ASSERT_NE(serialised_map.find("map_val"), serialised_map.end());
        EXPECT_EQ(serialised_map.at("map_val").get_type(), json::v1::object::type::map_type);

        const auto schema = json::v1::parse(xxx::something_more_complicated::get_schema());
        json::v1::schema::schema_validator validator(schema);
        const auto result = validator.validate(serialised);
        EXPECT_TRUE(result) << (result.errors().empty() ? "" : result.errors().front().message);
    }

    TEST(
        JsonConvert,
        SetFixtureRoundTripValidatesAgainstSchema)
    {
        // End-to-end exercise of the set/unordered_set path through an
        // actual IDL: schema declares uniqueItems, converter writes an array,
        // and the array passes uniqueItems validation.
        set_fixture::unique_collections typed;
        typed.ordered_ids = {1, 2, 3};
        typed.tags = {"alpha", "beta"};

        const auto serialised = to_json_object(typed);
        ASSERT_EQ(serialised.get_type(), json::v1::object::type::map_type);

        const auto schema = json::v1::parse(set_fixture::unique_collections::get_schema());
        json::v1::schema::schema_validator validator(schema);
        const auto result = validator.validate(serialised);
        EXPECT_TRUE(result) << (result.errors().empty() ? "" : result.errors().front().message);

        const auto round_tripped = from_json_object<set_fixture::unique_collections>(serialised);
        EXPECT_EQ(round_tripped.ordered_ids, typed.ordered_ids);
        EXPECT_EQ(round_tripped.tags, typed.tags);
    }

    TEST(
        JsonConvert,
        SetFixtureRejectsDuplicateInput)
    {
        // The converter throws on duplicate input rather than silently
        // dropping, matching the uniqueItems contract in the schema.
        const auto input = parse(R"json({
            "ordered_ids": [1, 2, 2],
            "tags": ["alpha", "beta"]
        })json");
        EXPECT_THROW(static_cast<void>(from_json_object<set_fixture::unique_collections>(input)), json::v1::config_error);

        const auto dup_string = parse(R"json({
            "ordered_ids": [1, 2, 3],
            "tags": ["alpha", "alpha"]
        })json");
        EXPECT_THROW(
            static_cast<void>(from_json_object<set_fixture::unique_collections>(dup_string)), json::v1::config_error);
    }

    TEST(
        JsonConvert,
        SchemaRejectsDuplicateArrayForSetField)
    {
        // Independent check that the schema itself rejects duplicates, even
        // before the converter would have a chance to throw.
        const auto schema = json::v1::parse(set_fixture::unique_collections::get_schema());
        const auto bad = parse(R"json({
            "ordered_ids": [1, 1],
            "tags": ["alpha"]
        })json");
        json::v1::schema::schema_validator validator(schema);
        EXPECT_FALSE(validator.validate(bad));
    }

    TEST(
        JsonConvert,
        VariantRoundTripValidatesAgainstGeneratedSchema)
    {
        // The variant schema emits oneOf of {"caseN": <T>} alternatives that
        // must match exactly what the converter writes.
        xxx::optional_variant_json_holder typed;
        typed.variant_value = std::string("payload");
        typed.json_value = parse(R"json({"nested": [1, 2]})json");

        const auto serialised = to_json_object(typed);
        const auto schema = json::v1::parse(xxx::optional_variant_json_holder::get_schema());
        json::v1::schema::schema_validator validator(schema);
        const auto result = validator.validate(serialised);
        EXPECT_TRUE(result) << (result.errors().empty() ? "" : result.errors().front().message);

        // The same struct with the integer alternative should also validate.
        typed.variant_value = int32_t{17};
        const auto serialised_int = to_json_object(typed);
        EXPECT_TRUE(validator.validate(serialised_int));
    }

    TEST(
        JsonConvert,
        SchemaRejectsBadVariantShape)
    {
        // A tag the schema doesn't enumerate should be rejected. The schema
        // now lists exactly the canonical alternative tags ("int32" and
        // "string" for this variant) — anything else fails validation, which
        // is the layer that catches a bad config before the converter runs.
        const auto schema = json::v1::parse(xxx::optional_variant_json_holder::get_schema());
        const auto bad_unknown = parse(R"json({
            "variant_value": {"wibble": 0},
            "json_value": null
        })json");
        json::v1::schema::schema_validator validator(schema);
        EXPECT_FALSE(validator.validate(bad_unknown));

        // Legacy "caseN" is also schema-rejected — a clean break from the
        // index-keyed shape.
        const auto bad_legacy = parse(R"json({
            "variant_value": {"case0": 1},
            "json_value": null
        })json");
        EXPECT_FALSE(validator.validate(bad_legacy));
    }

    TEST(
        JsonConvert,
        ConcreteTcpEndpointDefaultsAbsentAndPresent)
    {
        // TCP endpoint is the IDL-generated concrete TCP config type. Sparse
        // JSON can omit fields; conversion applies the IDL defaults. Explicit
        // null is handled by overlay policy at the config boundary rather than
        // by the schema for the concrete field.
        const auto schema = json::v1::parse(streaming::tcp::endpoint::get_schema(rpc::encoding::yas_json));
        json::v1::schema::schema_validator validator(schema);

        const auto absent = parse(R"json({})json");
        EXPECT_TRUE(validator.validate(absent));
        const auto typed_absent = from_json_object<streaming::tcp::endpoint>(absent);
        EXPECT_EQ(typed_absent.host, "127.0.0.1");
        EXPECT_EQ(typed_absent.port, 0);

        const auto explicit_null = parse(R"json({"host": null})json");
        EXPECT_FALSE(validator.validate(explicit_null));

        const auto valid = parse(R"json({"host": "10.0.0.1", "port": 8443})json");
        EXPECT_TRUE(validator.validate(valid));
        const auto typed_valid = from_json_object<streaming::tcp::endpoint>(valid);
        EXPECT_EQ(typed_valid.host, "10.0.0.1");
        EXPECT_EQ(typed_valid.port, 8443);
    }

    TEST(
        JsonConvert,
        ConcreteTcpEndpointInvalidInnerIsRejectedBySchema)
    {
        // Wrong type for an inner field (port should be integer) should fail
        // the parent schema. The converter has the same behaviour because
        // from_json_object<uint16_t> throws when given a string.
        const auto schema = json::v1::parse(streaming::tcp::endpoint::get_schema(rpc::encoding::yas_json));
        json::v1::schema::schema_validator validator(schema);

        const auto bad = parse(R"json({"host": "10.0.0.1", "port": "not-a-number"})json");
        EXPECT_FALSE(validator.validate(bad));
        EXPECT_THROW(static_cast<void>(from_json_object<streaming::tcp::endpoint>(bad)), std::exception);
    }

    TEST(
        JsonConvert,
        OptionalJsonObjectFieldAcceptsAnyJsonOrNull)
    {
        // rpc::optional<json::v1::object> accepts the full JSON spectrum
        // plus null and absence. The example_shared holder exercises this
        // because optional_json_value is exactly that type.
        const auto schema = json::v1::parse(xxx::optional_variant_json_holder::get_schema());
        json::v1::schema::schema_validator validator(schema);

        // The base shape needs the required fields, plus we vary
        // optional_json_value across absent / null / object / array.
        const auto base = R"json("variant_value": {"int32": 1}, "json_value": null)json";
        const auto absent = parse(std::string("{") + base + "}");
        EXPECT_TRUE(validator.validate(absent));
        EXPECT_FALSE(from_json_object<xxx::optional_variant_json_holder>(absent).optional_json_value.has_value());

        const auto explicit_null = parse(std::string("{") + base + R"json(, "optional_json_value": null)json" + "}");
        EXPECT_TRUE(validator.validate(explicit_null));
        EXPECT_FALSE(from_json_object<xxx::optional_variant_json_holder>(explicit_null).optional_json_value.has_value());

        const auto raw_object = parse(std::string("{") + base + R"json(, "optional_json_value": {"x": 1})json" + "}");
        EXPECT_TRUE(validator.validate(raw_object));
        const auto typed_raw_obj = from_json_object<xxx::optional_variant_json_holder>(raw_object);
        ASSERT_TRUE(typed_raw_obj.optional_json_value.has_value());
        EXPECT_EQ(typed_raw_obj.optional_json_value->get_type(), json::v1::object::type::map_type);

        const auto raw_array = parse(std::string("{") + base + R"json(, "optional_json_value": [1,2,3])json" + "}");
        EXPECT_TRUE(validator.validate(raw_array));
        const auto typed_raw_arr = from_json_object<xxx::optional_variant_json_holder>(raw_array);
        ASSERT_TRUE(typed_raw_arr.optional_json_value.has_value());
        EXPECT_EQ(typed_raw_arr.optional_json_value->get_type(), json::v1::object::type::array_type);
    }

    TEST(
        JsonConvert,
        VariantSchemaFailsClosedForLegacyCaseShape)
    {
        // The defence-in-depth check from the earlier audit becomes much
        // tighter now: legacy {"caseN":…} is not just unsupported by the
        // converter, the schema actively rejects it.
        const auto schema = json::v1::parse(xxx::optional_variant_json_holder::get_schema());
        json::v1::schema::schema_validator validator(schema);
        const auto bad = parse(R"json({
            "variant_value": {"case0": 1},
            "json_value": null
        })json");
        EXPECT_FALSE(validator.validate(bad));
    }

    TEST(
        JsonConvert,
        DomNumericEqualityIsLenientAcrossSignedness)
    {
        // The DOM and the schema validator now agree that 1 == 1.0 and
        // signed/unsigned values that represent the same magnitude compare
        // equal. Strict identity-of-representation is available via the
        // typed `number` accessors.
        const auto signed_one = parse(R"json(1)json");
        const auto float_one = parse(R"json(1.0)json");
        const auto unsigned_one = parse(R"json(1)json"); // parses unsigned by default

        EXPECT_EQ(signed_one, float_one);
        EXPECT_EQ(unsigned_one, float_one);

        const auto signed_two = parse(R"json(2)json");
        EXPECT_NE(signed_one, signed_two);

        const auto signed_neg = parse(R"json(-1)json");
        EXPECT_NE(unsigned_one, signed_neg);
    }

    TEST(
        JsonConvert,
        JsonDumpIsKeySortedAndStable)
    {
        // The YAS JSON writer now sorts map keys, matching the protobuf and
        // canonical_crypto paths. Two calls produce identical bytes; a map
        // built in reverse key order serialises in alphabetic order.
        json::v1::map m;
        m.emplace("zulu", json::v1::object(int64_t{3}));
        m.emplace("alpha", json::v1::object(int64_t{1}));
        m.emplace("mike", json::v1::object(int64_t{2}));

        const auto first = json::v1::dump(json::v1::object(json::v1::map(m)));
        const auto second = json::v1::dump(json::v1::object(json::v1::map(m)));
        EXPECT_EQ(first, second);
        EXPECT_EQ(first, R"({"alpha":1,"mike":2,"zulu":3})");
    }

    TEST(
        JsonConvert,
        SchemaCarriesDefaultsTranslatedFromIdl)
    {
        // tcp_endpoint declares `port = 0`, `family = ip_address_family::ipv4`,
        // and `addr = {}`. The first two translate to JSON literals and land
        // on the schema; the brace-init form is intentionally skipped because
        // there is no JSON representation that round-trips through a typed
        // array.
        const auto schema = json::v1::parse(canopy::network_config::tcp_endpoint::get_schema());
        const auto& definitions = schema.as_map().at("definitions").as_map();
        const auto& tcp_endpoint = definitions.at("canopy_network_config_tcp_endpoint").as_map();
        const auto& properties = tcp_endpoint.at("properties").as_map();

        const auto& port_schema = properties.at("port").as_map();
        ASSERT_NE(port_schema.find("default"), port_schema.end());
        EXPECT_EQ(port_schema.at("default"), json::v1::object(int64_t{0}));

        const auto& family_schema = properties.at("family").as_map();
        ASSERT_NE(family_schema.find("default"), family_schema.end());
        EXPECT_EQ(family_schema.at("default"), json::v1::object(std::string("ipv4")));

        // addr's IDL default is `{}`, which we deliberately do not translate.
        const auto& addr_schema = properties.at("addr").as_map();
        EXPECT_EQ(addr_schema.find("default"), addr_schema.end());
    }

    TEST(
        JsonConvert,
        FieldsWithIdlDefaultsAreNotMarkedRequired)
    {
        // The schema's `required` list now matches the converter's "must be
        // supplied" rule: a field with an IDL default is omittable because
        // the converter applies the default on its behalf.
        const auto schema = json::v1::parse(canopy::network_config::tcp_endpoint::get_schema());
        const auto& definitions = schema.as_map().at("definitions").as_map();
        const auto& tcp_endpoint = definitions.at("canopy_network_config_tcp_endpoint").as_map();
        const auto& required = tcp_endpoint.at("required").as_array();

        // `name` has no default → required. The rest have defaults.
        ASSERT_EQ(required.size(), 1u);
        EXPECT_EQ(required[0], json::v1::object(std::string("name")));
    }

    TEST(
        JsonConvert,
        SchemaDefaultsOverlayPopulatesIdlDefaults)
    {
        // schema_default_values walks the full schema document (entering
        // through the top-level $ref) and produces a JSON object the
        // converter can accept. After overlay the converter sees `family`
        // already populated from the schema default and never has to fall
        // back to the IDL expression.
        const auto schema = json::v1::parse(canopy::network_config::tcp_endpoint::get_schema());
        const auto schema_defaults = json::v1::schema_default_values(schema);
        const auto& schema_defaults_map = schema_defaults.as_map();
        ASSERT_NE(schema_defaults_map.find("port"), schema_defaults_map.end());
        ASSERT_NE(schema_defaults_map.find("family"), schema_defaults_map.end());

        const auto user_input = json::v1::parse(R"json({"name": "lo"})json");
        const auto overlayed = json::v1::merge_overlay(schema_defaults, user_input);
        const auto typed = from_json_object<canopy::network_config::tcp_endpoint>(overlayed);
        EXPECT_EQ(typed.name, "lo");
        EXPECT_EQ(typed.port, 0);
        EXPECT_EQ(typed.family, canopy::network_config::ip_address_family::ipv4);
    }

    TEST(
        JsonConvert,
        SchemaDefaultsAndConverterFallbackAgree)
    {
        // Parity check: with and without the overlay the converter produces
        // the same typed result. The C++ struct-default initializer, the
        // converter's else branch, and the schema's default annotation must
        // all carry the same IDL-declared value.
        const auto user_input = json::v1::parse(R"json({"name": "lo"})json");
        const auto without_overlay = from_json_object<canopy::network_config::tcp_endpoint>(user_input);

        const auto schema = json::v1::parse(canopy::network_config::tcp_endpoint::get_schema());
        const auto schema_defaults = json::v1::schema_default_values(schema);
        const auto overlayed = json::v1::merge_overlay(schema_defaults, user_input);
        const auto with_overlay = from_json_object<canopy::network_config::tcp_endpoint>(overlayed);

        EXPECT_EQ(without_overlay.port, with_overlay.port);
        EXPECT_EQ(without_overlay.family, with_overlay.family);
        EXPECT_EQ(without_overlay.name, with_overlay.name);
    }

    TEST(
        JsonConvert,
        ScopedIntegerConstantIsNotTranslatedAsEnumName)
    {
        // rpc::zone_address_args::version is `uint8_t = default_values::version_3;`
        // — `default_values` is a struct of static constexprs, not an IDL
        // enum. The translator must refuse: emitting `"default":
        // "version_3"` next to `"type": "integer"` would contradict the
        // field's declared type and any external validator would reject
        // the value. The C++ path still pastes the constexpr verbatim.
        const auto schema = json::v1::parse(rpc::zone_address_args::get_schema());
        const auto& definitions = schema.as_map().at("definitions").as_map();
        const auto& zaa = definitions.at("rpc_zone_address_args").as_map();
        const auto& version_schema = zaa.at("properties").as_map().at("version").as_map();

        EXPECT_NE(version_schema.find("type"), version_schema.end());
        EXPECT_EQ(version_schema.find("default"), version_schema.end());
    }

    TEST(
        JsonConvert,
        EnumDefaultIsWrappedInAllOfBesideDefault)
    {
        // Draft-07 ignores siblings of `$ref`. The schema now emits
        // {default, allOf:[{$ref}]} so an external Draft-07 client picks
        // the default up correctly.
        const auto schema = json::v1::parse(canopy::network_config::tcp_endpoint::get_schema());
        const auto& definitions = schema.as_map().at("definitions").as_map();
        const auto& tcp_endpoint = definitions.at("canopy_network_config_tcp_endpoint").as_map();
        const auto& family_schema = tcp_endpoint.at("properties").as_map().at("family").as_map();

        ASSERT_NE(family_schema.find("default"), family_schema.end());
        EXPECT_EQ(family_schema.find("$ref"), family_schema.end());
        ASSERT_NE(family_schema.find("allOf"), family_schema.end());

        const auto& all_of = family_schema.at("allOf").as_array();
        ASSERT_EQ(all_of.size(), 1u);
        const auto& ref_only = all_of[0].as_map();
        EXPECT_NE(ref_only.find("$ref"), ref_only.end());

        // The overlay still works against the allOf-wrapped form because
        // schema_default_values walks allOf recursively.
        const auto schema_defaults = json::v1::schema_default_values(schema);
        const auto& defaults_map = schema_defaults.as_map();
        ASSERT_NE(defaults_map.find("family"), defaults_map.end());
        EXPECT_EQ(defaults_map.at("family"), json::v1::object(std::string("ipv4")));
    }

    TEST(
        JsonConvert,
        ConnectionFactoryMaterialisesActiveTcpSparseSettings)
    {
        // Config-driven construction materialises raw JSON at the connection
        // factory boundary. The typed TCP factory API receives the generated
        // endpoint type directly and does not expose JSON helpers.
        const auto user_input = json::v1::parse(R"json({
            "host": "10.0.0.42",
            "port": 9000
        })json");
        const auto materialised = rpc::connection_factory::materialise_settings<streaming::tcp::endpoint>(user_input);
        ASSERT_EQ(materialised.error_code, rpc::error::OK());
        const auto& typed = materialised.settings;

        EXPECT_EQ(typed.host, "10.0.0.42");
        EXPECT_EQ(typed.port, 9000);
        EXPECT_FALSE(typed.ipv6);
        EXPECT_EQ(typed.connect_timeout, 5000u);
    }

    TEST(
        JsonConvert,
        ActiveTcpDefaultsMatchBuildMode)
    {
        const auto defaults = to_json_object(streaming::tcp::endpoint{});
        ASSERT_EQ(defaults.get_type(), json::v1::object::type::map_type);
        const auto& defaults_map = defaults.as_map();

        ASSERT_NE(defaults_map.find("host"), defaults_map.end());
        ASSERT_NE(defaults_map.find("port"), defaults_map.end());
        ASSERT_NE(defaults_map.find("ipv6"), defaults_map.end());
        ASSERT_NE(defaults_map.find("connect_timeout"), defaults_map.end());

#ifdef CANOPY_BUILD_COROUTINE
        EXPECT_NE(defaults_map.find("controller"), defaults_map.end());
        EXPECT_NE(defaults_map.find("first_port"), defaults_map.end());
        EXPECT_NE(defaults_map.find("last_port"), defaults_map.end());
        EXPECT_NE(defaults_map.find("stream"), defaults_map.end());
#else
        EXPECT_EQ(defaults_map.find("controller"), defaults_map.end());
        EXPECT_EQ(defaults_map.find("first_port"), defaults_map.end());
        EXPECT_EQ(defaults_map.find("last_port"), defaults_map.end());
        EXPECT_EQ(defaults_map.find("stream"), defaults_map.end());
#endif
    }

    TEST(
        JsonConvert,
        ConcreteTcpIdlsMaterialiseSparseConfigsIndependently)
    {
        const auto blocking_overlay = json::v1::parse(R"json({
            "port": 12001
        })json");
        const auto blocking_schema
            = json::v1::parse(rpc::tcp_blocking_stream::endpoint::get_schema(rpc::encoding::yas_json));
        const auto blocking_defaults = to_json_object(rpc::tcp_blocking_stream::endpoint{});
        const auto blocking_endpoint = json::v1::load_typed_config<rpc::tcp_blocking_stream::endpoint>(
            blocking_schema, blocking_defaults, blocking_overlay);

        EXPECT_EQ(blocking_endpoint.host, "127.0.0.1");
        EXPECT_EQ(blocking_endpoint.port, uint16_t{12001});
        EXPECT_FALSE(blocking_endpoint.ipv6);
        EXPECT_EQ(blocking_endpoint.connect_timeout, uint64_t{5000});

        const auto coroutine_overlay = json::v1::parse(R"json({
            "port": 12002,
            "stream": {"timeout_strategy": "nonblocking_poll"}
        })json");
        const auto coroutine_schema
            = json::v1::parse(rpc::tcp_coroutine_stream::endpoint::get_schema(rpc::encoding::yas_json));
        const auto coroutine_defaults = to_json_object(rpc::tcp_coroutine_stream::endpoint{});
        const auto coroutine_endpoint = json::v1::load_typed_config<rpc::tcp_coroutine_stream::endpoint>(
            coroutine_schema, coroutine_defaults, coroutine_overlay);

        EXPECT_EQ(coroutine_endpoint.host, "127.0.0.1");
        EXPECT_EQ(coroutine_endpoint.port, uint16_t{12002});
        EXPECT_FALSE(coroutine_endpoint.ipv6);
        EXPECT_EQ(coroutine_endpoint.connect_timeout, uint64_t{5000});
        EXPECT_EQ(
            coroutine_endpoint.stream.timeout_strategy,
            rpc::tcp_coroutine_stream::receive_timeout_strategy::nonblocking_poll);
    }

    TEST(
        JsonConvert,
        ConnectionFactoryRejectsInvalidActiveTcpJson)
    {
        // Validation still happens just once. An unknown top-level key
        // is rejected by the schema before the converter runs.
        const auto bad_input = json::v1::parse(R"json({
            "unsupported_key": 1
        })json");
        EXPECT_EQ(
            rpc::connection_factory::materialise_settings<streaming::tcp::endpoint>(bad_input).error_code,
            rpc::error::INVALID_DATA());
    }

    TEST(
        JsonConvert,
        ConnectionFactoryTreatsNullActiveTcpOverlayAsOmitted)
    {
        // User config is a sparse overlay, not a complete object to validate
        // before defaults. With the default null policy, null means "not
        // supplied", so a null override disappears before final validation.
        const auto null_keyed = json::v1::parse(R"json({
            "unsupported_key": null
        })json");
        EXPECT_EQ(
            rpc::connection_factory::materialise_settings<streaming::tcp::endpoint>(null_keyed).error_code,
            rpc::error::OK());
    }

    TEST(
        JsonConvert,
        StreamRpcTransportSettingsMaterialiserUsesGeneratedEncodingEnum)
    {
        const auto valid = json::v1::parse(R"json({
            "encoding": "yas_binary"
        })json");
        const auto materialised
            = rpc::connection_factory::materialise_settings<rpc::stream_transport::transport_settings>(valid);
        ASSERT_EQ(materialised.error_code, rpc::error::OK());
        ASSERT_TRUE(materialised.settings.encoding.has_value());
        EXPECT_EQ(*materialised.settings.encoding, rpc::encoding::yas_binary);

        const auto legacy_alias = json::v1::parse(R"json({
            "encoding": "binary"
        })json");
        EXPECT_EQ(
            rpc::connection_factory::materialise_settings<rpc::stream_transport::transport_settings>(legacy_alias).error_code,
            rpc::error::INVALID_DATA());
    }

    TEST(
        JsonConvert,
        IoUringTimeoutStrategyUsesGeneratedEnum)
    {
        const auto valid = json::v1::parse(R"json({
            "stream": {"timeout_strategy": "nonblocking_poll"}
        })json");
        const auto schema = json::v1::parse(rpc::tcp_coroutine_stream::endpoint::get_schema(rpc::encoding::yas_json));
        const auto defaults = to_json_object(rpc::tcp_coroutine_stream::endpoint{});
        const auto materialised
            = json::v1::load_typed_config<rpc::tcp_coroutine_stream::endpoint>(schema, defaults, valid);
        EXPECT_FALSE(materialised.controller.use_sqpoll);
        EXPECT_EQ(materialised.controller.fixed_file_count, uint32_t{0});
        EXPECT_FALSE(materialised.controller.register_fixed_files);
        EXPECT_EQ(
            materialised.stream.timeout_strategy, rpc::tcp_coroutine_stream::receive_timeout_strategy::nonblocking_poll);

        const auto invalid = json::v1::parse(R"json({
            "stream": {"timeout_strategy": "poll_once"}
        })json");
        EXPECT_THROW(
            static_cast<void>(json::v1::load_typed_config<rpc::tcp_coroutine_stream::endpoint>(schema, defaults, invalid)),
            std::exception);
    }

#ifdef CANOPY_BUILD_COROUTINE
    TEST(
        JsonConvert,
        TcpCoroutineEndpointValidationAllowsNetworkAddresses)
    {
        rpc::tcp_coroutine_stream::endpoint ipv4;
        ipv4.host = "192.0.2.1";
        ipv4.port = 443;
        ipv4.connect_timeout = 123;
        EXPECT_EQ(rpc::tcp_coroutine::validate_connect_endpoint(ipv4), rpc::error::OK());

        const auto materialised
            = rpc::connection_factory::materialise_settings<rpc::tcp_coroutine_stream::endpoint>(json::v1::parse(R"json({
            "host": "192.0.2.1",
            "port": 443,
            "connect_timeout": 123
        })json"));
        ASSERT_EQ(materialised.error_code, rpc::error::OK());
        EXPECT_EQ(materialised.settings.connect_timeout, uint64_t{123});

        rpc::tcp_coroutine_stream::endpoint ipv6;
        ipv6.host = "2001:db8::1";
        ipv6.port = 443;
        ipv6.ipv6 = true;
        EXPECT_EQ(rpc::tcp_coroutine::validate_connect_endpoint(ipv6), rpc::error::OK());

        rpc::tcp_coroutine_stream::endpoint missing_port;
        missing_port.host = "192.0.2.1";
        EXPECT_EQ(rpc::tcp_coroutine::validate_connect_endpoint(missing_port), rpc::error::INVALID_DATA());
    }
#endif

#ifdef CANOPY_SECURE_STREAM_BACKEND_OPENSSL
    TEST(
        JsonConvert,
        TlsLayerSettingsUseOpenSslGeneratedConfig)
    {
        const auto valid = json::v1::parse(R"json({
            "client": {"verify_peer": true, "trust_anchor_file": "client-ca.pem"},
            "server": {
                "verify_peer": "required",
                "credentials": {
                    "certificate_file": "server.crt",
                    "private_key_file": "server.key",
                    "trust_anchor_file": "server-ca.pem"
                }
            }
        })json");

        const auto materialised
            = rpc::connection_factory::materialise_settings<rpc::openssl_tls_stream::stream_settings>(valid);
        ASSERT_EQ(materialised.error_code, rpc::error::OK());
        EXPECT_TRUE(materialised.settings.client.verify_peer);
        EXPECT_EQ(materialised.settings.client.trust_anchor_file, "client-ca.pem");
        EXPECT_EQ(materialised.settings.server.verify_peer, rpc::openssl_tls_stream::peer_verification::required);
        ASSERT_TRUE(materialised.settings.server.credentials.has_value());
        EXPECT_EQ(materialised.settings.server.credentials.value().certificate_file, "server.crt");
        EXPECT_EQ(materialised.settings.server.credentials.value().private_key_file, "server.key");
        EXPECT_EQ(materialised.settings.server.credentials.value().trust_anchor_file, "server-ca.pem");

        const auto sparse = rpc::connection_factory::materialise_settings<rpc::openssl_tls_stream::stream_settings>(
            json::v1::parse(R"json({})json"));
        ASSERT_EQ(sparse.error_code, rpc::error::OK());
        EXPECT_FALSE(sparse.settings.client.verify_peer);
        EXPECT_TRUE(sparse.settings.client.trust_anchor_file.empty());
        EXPECT_EQ(sparse.settings.server.verify_peer, rpc::openssl_tls_stream::peer_verification::none);
        EXPECT_FALSE(sparse.settings.server.credentials.has_value());
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_WEBSOCKET
    TEST(
        JsonConvert,
        WebSocketLayerSettingsUseGeneratedConfig)
    {
        const auto valid = json::v1::parse(R"json({
            "role": "client",
            "keep_alive": {
                "enabled": true,
                "interval_ms": 1234,
                "timeout_ms": 5678
            },
            "permessage_deflate": {
                "enabled": true,
                "server_no_context_takeover": true,
                "client_no_context_takeover": true
            },
            "max_message_bytes": 1000000,
            "max_frame_payload_bytes": 2048,
            "max_decoded_messages": 4
        })json");

        const auto materialised
            = rpc::connection_factory::materialise_settings<rpc::websocket_stream::stream_settings>(valid);
        ASSERT_EQ(materialised.error_code, rpc::error::OK());
        ASSERT_TRUE(materialised.settings.role.has_value());
        EXPECT_EQ(materialised.settings.role.value(), rpc::websocket_stream::endpoint_role::client);
        EXPECT_TRUE(materialised.settings.keep_alive.enabled);
        EXPECT_EQ(materialised.settings.keep_alive.interval_ms, uint64_t{1234});
        EXPECT_EQ(materialised.settings.keep_alive.timeout_ms, uint64_t{5678});
        EXPECT_TRUE(materialised.settings.permessage_deflate.enabled);
        EXPECT_TRUE(materialised.settings.permessage_deflate.server_no_context_takeover);
        EXPECT_TRUE(materialised.settings.permessage_deflate.client_no_context_takeover);
        EXPECT_EQ(materialised.settings.max_message_bytes, uint64_t{1000000});
        EXPECT_EQ(materialised.settings.max_frame_payload_bytes, uint64_t{2048});
        EXPECT_EQ(materialised.settings.max_decoded_messages, uint64_t{4});

        const auto sparse = rpc::connection_factory::materialise_settings<rpc::websocket_stream::stream_settings>(
            json::v1::parse(R"json({})json"));
        ASSERT_EQ(sparse.error_code, rpc::error::OK());
        EXPECT_FALSE(sparse.settings.role.has_value());
        EXPECT_FALSE(sparse.settings.keep_alive.enabled);
        EXPECT_EQ(sparse.settings.keep_alive.interval_ms, uint64_t{30000});
        EXPECT_EQ(sparse.settings.keep_alive.timeout_ms, uint64_t{10000});
        EXPECT_FALSE(sparse.settings.permessage_deflate.enabled);
        EXPECT_TRUE(sparse.settings.permessage_deflate.server_no_context_takeover);
        EXPECT_TRUE(sparse.settings.permessage_deflate.client_no_context_takeover);
        EXPECT_EQ(sparse.settings.max_message_bytes, uint64_t{1048576});
        EXPECT_EQ(sparse.settings.max_frame_payload_bytes, uint64_t{1048576});
        EXPECT_EQ(sparse.settings.max_decoded_messages, uint64_t{16});

        const auto stale_path = rpc::connection_factory::materialise_settings<rpc::websocket_stream::stream_settings>(
            json::v1::parse(R"json({"path": "/rpc"})json"));
        EXPECT_EQ(stale_path.error_code, rpc::error::INVALID_DATA());
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_COMPRESSION
    TEST(
        JsonConvert,
        CompressionLayerSettingsUseGeneratedConfig)
    {
        const auto valid = json::v1::parse(R"json({
            "algorithm": "zstd",
            "level": 5,
            "send_buffer_bytes": 32768,
            "receive_buffer_bytes": 32768,
            "max_expansion_ratio": 32,
            "max_decompressed_chunk_bytes": 1048576
        })json");

        const auto materialised
            = rpc::connection_factory::materialise_settings<rpc::compression_stream::stream_settings>(valid);
        ASSERT_EQ(materialised.error_code, rpc::error::OK());
        EXPECT_EQ(materialised.settings.algorithm, rpc::compression_stream::compression_algorithm::zstd);
        EXPECT_EQ(materialised.settings.level, int32_t{5});
        EXPECT_EQ(materialised.settings.send_buffer_bytes, uint64_t{32768});
        EXPECT_EQ(materialised.settings.receive_buffer_bytes, uint64_t{32768});
        EXPECT_EQ(materialised.settings.max_expansion_ratio, uint64_t{32});
        EXPECT_EQ(materialised.settings.max_decompressed_chunk_bytes, uint64_t{1048576});

        const auto sparse = rpc::connection_factory::materialise_settings<rpc::compression_stream::stream_settings>(
            json::v1::parse(R"json({})json"));
        ASSERT_EQ(sparse.error_code, rpc::error::OK());
        EXPECT_EQ(sparse.settings.algorithm, rpc::compression_stream::compression_algorithm::zstd);
        EXPECT_EQ(sparse.settings.level, int32_t{3});
        EXPECT_EQ(sparse.settings.send_buffer_bytes, uint64_t{16384});
        EXPECT_EQ(sparse.settings.receive_buffer_bytes, uint64_t{16384});
        EXPECT_EQ(sparse.settings.max_expansion_ratio, uint64_t{0});
        EXPECT_EQ(sparse.settings.max_decompressed_chunk_bytes, uint64_t{16777216});
    }
#endif

#ifdef CANOPY_HAS_HTTP_SERVER_CONFIG
    TEST(
        JsonConvert,
        HttpClientConnectionLimitsUseGeneratedConfig)
    {
        const auto valid = json::v1::parse(R"json({
            "max_method_bytes": 16,
            "max_url_bytes": 128,
            "max_header_name_bytes": 32,
            "max_header_value_bytes": 256,
            "max_header_count": 8,
            "max_body_bytes": 4096,
            "max_pending_input_bytes": 1024,
            "receive_poll_timeout_ms": 5,
            "header_timeout_ms": 100,
            "request_timeout_ms": 200,
            "allowed_websocket_hosts": ["example.test", "api.example.test:443"],
            "require_websocket_origin": true,
            "allowed_websocket_origins": ["https://example.test"]
        })json");

        const auto materialised
            = rpc::connection_factory::materialise_settings<canopy::http_server::client_connection_limits>(valid);
        ASSERT_EQ(materialised.error_code, rpc::error::OK());
        EXPECT_EQ(materialised.settings.max_method_bytes, uint64_t{16});
        EXPECT_EQ(materialised.settings.max_url_bytes, uint64_t{128});
        EXPECT_EQ(materialised.settings.max_header_name_bytes, uint64_t{32});
        EXPECT_EQ(materialised.settings.max_header_value_bytes, uint64_t{256});
        EXPECT_EQ(materialised.settings.max_header_count, uint64_t{8});
        EXPECT_EQ(materialised.settings.max_body_bytes, uint64_t{4096});
        EXPECT_EQ(materialised.settings.max_pending_input_bytes, uint64_t{1024});
        EXPECT_EQ(materialised.settings.receive_poll_timeout_ms, uint64_t{5});
        EXPECT_EQ(materialised.settings.header_timeout_ms, uint64_t{100});
        EXPECT_EQ(materialised.settings.request_timeout_ms, uint64_t{200});
        ASSERT_EQ(materialised.settings.allowed_websocket_hosts.size(), size_t{2});
        EXPECT_EQ(materialised.settings.allowed_websocket_hosts[0], "example.test");
        EXPECT_EQ(materialised.settings.allowed_websocket_hosts[1], "api.example.test:443");
        EXPECT_TRUE(materialised.settings.require_websocket_origin);
        ASSERT_EQ(materialised.settings.allowed_websocket_origins.size(), size_t{1});
        EXPECT_EQ(materialised.settings.allowed_websocket_origins[0], "https://example.test");

        const auto sparse = rpc::connection_factory::materialise_settings<canopy::http_server::client_connection_limits>(
            json::v1::parse(R"json({})json"));
        ASSERT_EQ(sparse.error_code, rpc::error::OK());
        EXPECT_EQ(sparse.settings.max_method_bytes, uint64_t{256});
        EXPECT_EQ(sparse.settings.max_url_bytes, uint64_t{4096});
        EXPECT_EQ(sparse.settings.max_header_name_bytes, uint64_t{256});
        EXPECT_EQ(sparse.settings.max_header_value_bytes, uint64_t{8192});
        EXPECT_EQ(sparse.settings.max_header_count, uint64_t{128});
        EXPECT_EQ(sparse.settings.max_body_bytes, uint64_t{1048576});
        EXPECT_EQ(sparse.settings.max_pending_input_bytes, uint64_t{65536});
        EXPECT_EQ(sparse.settings.receive_poll_timeout_ms, uint64_t{250});
        EXPECT_EQ(sparse.settings.header_timeout_ms, uint64_t{10000});
        EXPECT_EQ(sparse.settings.request_timeout_ms, uint64_t{30000});
        EXPECT_TRUE(sparse.settings.allowed_websocket_hosts.empty());
        EXPECT_FALSE(sparse.settings.require_websocket_origin);
        EXPECT_TRUE(sparse.settings.allowed_websocket_origins.empty());
    }
#endif

    TEST(
        JsonConvert,
        ConnectionSettingsPreserveStreamSpecificBlobs)
    {
        const auto valid_json = std::string(R"json({
            "service": {"type": "service", "settings": {"name": "client"}},
            "transport": {
                "type": "stream_rpc",
                "settings": {"name": "client_transport", "service_proxy_name": "server", "encoding": "nanopb"}
            },
            "stream_layers": [
)json") + R"json(                {"type": ")json"
                                + active_tcp_stream_type + R"json(", "settings": {"host": "127.0.0.1", "port": 8080}},
                {"type": "websocket", "settings": {"keep_alive": {"enabled": false}}}
            ]
        })json";
        const auto valid = json::v1::parse(valid_json);

        const auto materialised = rpc::connection_factory::materialise_connection_settings(valid);
        ASSERT_EQ(materialised.error_code, rpc::error::OK());
        ASSERT_EQ(materialised.settings.stream_layers.size(), 2u);
        EXPECT_EQ(materialised.settings.stream_layers[0].type, active_tcp_stream_type);
        EXPECT_EQ(materialised.settings.stream_layers[1].type, "websocket");
        ASSERT_TRUE(materialised.settings.stream_layers[0].settings.has_value());

        const auto tcp_options
            = from_json_object<streaming::tcp::endpoint>(materialised.settings.stream_layers[0].settings.value());
        EXPECT_EQ(tcp_options.host, "127.0.0.1");
        EXPECT_EQ(tcp_options.port, 8080);
    }

    TEST(
        JsonConvert,
        BuiltInConnectionFactoryMaterialisesSparseTcpSettings)
    {
        rpc::stream_layers::stream_layer_settings sparse_tcp;
        sparse_tcp.type = active_tcp_stream_type;
        sparse_tcp.settings = parse(R"json({
            "port": 0
        })json");
        rpc::connection_factory::connection_settings sparse_tcp_settings;
        sparse_tcp_settings.stream_layers.push_back(sparse_tcp);

        auto acceptor = SYNC_WAIT(rpc::connection_factory::open_stream_acceptor(sparse_tcp_settings));
        EXPECT_EQ(acceptor.error_code, rpc::error::OK());
        EXPECT_NE(acceptor.acceptor, nullptr);
#ifdef CANOPY_BUILD_COROUTINE
        EXPECT_NE(acceptor.port, uint16_t{0});
#else
        EXPECT_EQ(acceptor.port, uint16_t{0});
#endif

        rpc::stream_layers::stream_layer_settings invalid_tcp;
        invalid_tcp.type = active_tcp_stream_type;
        invalid_tcp.settings = parse(R"json({
            "unexpected_tcp_field": true
        })json");
        rpc::connection_factory::connection_settings invalid_tcp_settings;
        invalid_tcp_settings.stream_layers.push_back(std::move(invalid_tcp));

        auto invalid = SYNC_WAIT(rpc::connection_factory::open_stream_acceptor(invalid_tcp_settings));
        EXPECT_EQ(invalid.error_code, rpc::error::INVALID_DATA());
    }

    TEST(
        JsonConvert,
        BuiltInConnectionFactoryRejectsInactiveTcpImplementation)
    {
        rpc::stream_layers::stream_layer_settings inactive_tcp;
        inactive_tcp.type = inactive_tcp_stream_type;
        inactive_tcp.settings = parse(R"json({
            "port": 0
        })json");
        rpc::connection_factory::connection_settings inactive_tcp_settings;
        inactive_tcp_settings.stream_layers.push_back(std::move(inactive_tcp));

        auto inactive = SYNC_WAIT(rpc::connection_factory::open_stream_acceptor(inactive_tcp_settings));
        EXPECT_EQ(inactive.error_code, rpc::error::INVALID_DATA());
        EXPECT_EQ(inactive.acceptor, nullptr);
    }

    TEST(
        JsonConvert,
        ConnectionFactoryValidatesStreamRpcLayerTopology)
    {
        rpc::connection_factory::connection_settings settings;
        const rpc::connection_factory::context context;
        const auto layer = [](std::string type, const char* options_json)
        {
            rpc::stream_layers::stream_layer_settings result;
            result.type = std::move(type);
            result.settings = parse(options_json);
            return result;
        };

        settings.stream_layers = {
            layer(active_tcp_stream_type, R"json({"port": 0})json"),
        };
        auto valid_acceptor = SYNC_WAIT(rpc::connection_factory::open_stream_acceptor(settings, {}, context));
        EXPECT_EQ(valid_acceptor.error_code, rpc::error::OK());
        EXPECT_NE(valid_acceptor.acceptor, nullptr);

        settings.stream_layers = {
            layer("websocket", R"json({})json"),
        };
        EXPECT_EQ(
            SYNC_WAIT(rpc::connection_factory::open_stream_acceptor(settings, {}, context)).error_code,
            rpc::error::INVALID_DATA());

        settings.stream_layers = {
            layer(active_tcp_stream_type, R"json({"port": 0})json"),
            layer(active_tcp_stream_type, R"json({"port": 0})json"),
        };
        EXPECT_EQ(
            SYNC_WAIT(rpc::connection_factory::open_stream_acceptor(settings, {}, context)).error_code,
            rpc::error::INVALID_DATA());

        settings.stream_layers = {
            layer(active_tcp_stream_type, R"json({"port": 0})json"),
            layer("definitely_not_registered", R"json({})json"),
        };
        EXPECT_EQ(
            SYNC_WAIT(rpc::connection_factory::open_stream_acceptor(settings, {}, context)).error_code,
            rpc::error::INVALID_DATA());

        settings.stream_layers = {
            layer(inactive_tcp_stream_type, R"json({"port": 0})json"),
        };
        EXPECT_EQ(
            SYNC_WAIT(rpc::connection_factory::open_stream_acceptor(settings, {}, context)).error_code,
            rpc::error::INVALID_DATA());
    }

    TEST(
        JsonConvert,
        ConnectionContextStoresTypedNamedDependencies)
    {
        rpc::connection_factory::context context;

        auto unnamed_string = std::make_shared<std::string>("default");
        auto named_string = std::make_shared<std::string>("named");
        context.set_dependency(unnamed_string);
        context.set_dependency(named_string, "alpha");

        EXPECT_EQ(context.get_dependency<std::string>(), unnamed_string);
        EXPECT_EQ(context.get_dependency<std::string>("alpha"), named_string);
        EXPECT_EQ(context.get_dependency<std::string>("missing"), nullptr);

        auto strings = context.get_dependencies<std::string>();
        ASSERT_EQ(strings.size(), 2u);
        EXPECT_EQ(strings[""], unnamed_string);
        EXPECT_EQ(strings["alpha"], named_string);

        context.set_dependency<std::string>(std::shared_ptr<std::string>{}, "alpha");
        EXPECT_EQ(context.get_dependency<std::string>("alpha"), nullptr);
        strings = context.get_dependencies<std::string>();
        ASSERT_EQ(strings.size(), 1u);
        EXPECT_EQ(strings[""], unnamed_string);

        context.set_dependency_value<uint32_t>(42, "answer");
        auto answer = context.get_dependency<uint32_t>("answer");
        ASSERT_NE(answer, nullptr);
        EXPECT_EQ(*answer, uint32_t{42});
    }

    TEST(
        JsonConvert,
        RegisteredConnectionFactoryComponentOwnsTypedMaterialisation)
    {
        rpc::connection_factory::context context;
        bool factory_called = false;
        std::string materialised_host;
        uint16_t materialised_port = 0;

        context.register_connect_base_stream<streaming::tcp::endpoint>(
            "test_lora",
            [&](streaming::tcp::endpoint settings,
                std::shared_ptr<rpc::service>,
                const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
            {
                factory_called = true;
                materialised_host = std::move(settings.host);
                materialised_port = settings.port;
                CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), {}};
            });

        rpc::stream_layers::stream_layer_settings layer;
        layer.type = "test_lora";
        layer.settings = parse(R"json({
            "port": 42
        })json");
        rpc::connection_factory::connection_settings settings;
        settings.stream_layers.push_back(std::move(layer));

        auto result = SYNC_WAIT(rpc::connection_factory::connect_stream(settings, {}, context));
        EXPECT_EQ(result.error_code, rpc::error::OK());
        EXPECT_TRUE(factory_called);
        EXPECT_EQ(materialised_host, "127.0.0.1");
        EXPECT_EQ(materialised_port, uint16_t{42});
    }

    TEST(
        JsonConvert,
        RegisteredStreamLayersAreAppliedFromConnectionSettings)
    {
        rpc::connection_factory::context context;
        std::vector<std::string> calls;

        context.register_connect_base_stream<rpc::connection_factory::service_settings>(
            "test_base",
            [](rpc::connection_factory::service_settings,
                std::shared_ptr<rpc::service>,
                const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
            { CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), {}}; });
        context.register_accept_single_stream<rpc::connection_factory::service_settings>(
            "test_base",
            [](rpc::connection_factory::service_settings,
                std::shared_ptr<rpc::service>,
                const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
            { CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), {}}; });

        auto register_layer = [&](const std::string& type)
        {
            context.register_stream_layer<rpc::connection_factory::service_settings>(
                type,
                [&, type](
                    std::shared_ptr<::streaming::stream> stream,
                    rpc::connection_factory::service_settings settings,
                    rpc::connection_factory::layer_direction direction,
                    const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
                {
                    if (!settings.name)
                        CO_RETURN rpc::connection_factory::stream_result{rpc::error::INVALID_DATA(), {}};

                    calls.push_back(
                        std::string(direction == rpc::connection_factory::layer_direction::connect ? "connect:" : "accept:")
                        + type + ":" + settings.name.value());
                    CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), std::move(stream)};
                });
        };

        register_layer("test_layer_alpha");
        register_layer("test_layer_beta");

        const auto valid_json = std::string(R"json({
            "transport": {"type": "stream_rpc", "settings": {"encoding": "nanopb"}},
            "stream_layers": [
                {"type": "test_base", "settings": {}},
                {"type": "test_layer_alpha", "settings": {"name": "alpha"}},
                {"type": "test_layer_beta", "settings": {"name": "beta"}}
            ]
        })json");
        const auto materialised = rpc::connection_factory::materialise_connection_settings(parse(valid_json));
        ASSERT_EQ(materialised.error_code, rpc::error::OK());
        ASSERT_EQ(materialised.settings.stream_layers.size(), 3u);

        auto connected = SYNC_WAIT(rpc::connection_factory::connect_stream(materialised.settings, {}, context));
        ASSERT_EQ(connected.error_code, rpc::error::OK());
        EXPECT_EQ(connected.stream, nullptr);
        ASSERT_EQ(calls.size(), 2u);
        EXPECT_EQ(calls[0], "connect:test_layer_alpha:alpha");
        EXPECT_EQ(calls[1], "connect:test_layer_beta:beta");

        calls.clear();
        auto accepted = SYNC_WAIT(rpc::connection_factory::accept_stream(materialised.settings, {}, context));
        ASSERT_EQ(accepted.error_code, rpc::error::OK());
        EXPECT_EQ(accepted.stream, nullptr);
        ASSERT_EQ(calls.size(), 2u);
        EXPECT_EQ(calls[0], "accept:test_layer_alpha:alpha");
        EXPECT_EQ(calls[1], "accept:test_layer_beta:beta");
    }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC_WRAPPING
    TEST(
        JsonConvert,
        SpscWrappingLayerRequiresServiceExecutor)
    {
        rpc::connection_factory::context context;
        context.register_connect_base_stream<rpc::connection_factory::service_settings>(
            "test_base",
            [](rpc::connection_factory::service_settings,
                std::shared_ptr<rpc::service>,
                const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
            { CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), {}}; });

        rpc::connection_factory::connection_settings settings;
        rpc::stream_layers::stream_layer_settings base_layer;
        base_layer.type = "test_base";
        settings.stream_layers.push_back(std::move(base_layer));

        rpc::stream_layers::stream_layer_settings wrapper_layer;
        wrapper_layer.type = "spsc_wrapping";
        settings.stream_layers.push_back(std::move(wrapper_layer));

        auto service
            = rpc::root_service::create("spsc_wrapping_without_executor", rpc::DEFAULT_PREFIX, rpc::executor_ptr{});
        ASSERT_NE(service, nullptr);
        EXPECT_EQ(service->get_executor(), nullptr);

        auto result = SYNC_WAIT(rpc::connection_factory::connect_stream(settings, service, context));
        EXPECT_EQ(result.error_code, rpc::error::INVALID_DATA());
        EXPECT_EQ(result.stream, nullptr);
    }
#endif

    TEST(
        JsonConvert,
        RegisteredTypedConnectionFactoryComponentRejectsInvalidSettingsBeforeBuilder)
    {
        rpc::connection_factory::context context;
        bool builder_called = false;

        context.register_connect_base_stream<streaming::tcp::endpoint>(
            "test_invalid_endpoint",
            [&](streaming::tcp::endpoint,
                std::shared_ptr<rpc::service>,
                const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
            {
                builder_called = true;
                CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), {}};
            });

        rpc::stream_layers::stream_layer_settings layer;
        layer.type = "test_invalid_endpoint";
        layer.settings = parse(R"json({
            "port": "not-a-port"
        })json");

        rpc::connection_factory::connection_settings settings;
        settings.stream_layers.push_back(std::move(layer));

        auto result = SYNC_WAIT(rpc::connection_factory::connect_stream(settings, {}, context));
        EXPECT_EQ(result.error_code, rpc::error::INVALID_DATA());
        EXPECT_FALSE(builder_called);
    }

    TEST(
        JsonConvert,
        ConnectionTransportSelectionIsSeparateFromStreamLayers)
    {
        rpc::connection_factory::connection_settings default_settings;
        const auto default_transport = rpc::connection_factory::detail::transport_from_connection(default_settings);
        ASSERT_EQ(default_transport.error_code, rpc::error::OK());
        EXPECT_EQ(default_transport.type, "stream_rpc");
        EXPECT_EQ(default_transport.settings, nullptr);

        const auto local_config = rpc::connection_factory::materialise_connection_settings(parse(R"json({
            "transport": {
                "type": "local",
                "settings": {"name": "child_zone", "service_proxy_name": "child_proxy", "encoding": "nanopb"}
            }
        })json"));
        ASSERT_EQ(local_config.error_code, rpc::error::OK());

        const auto local_transport = rpc::connection_factory::detail::transport_from_connection(local_config.settings);
        ASSERT_EQ(local_transport.error_code, rpc::error::OK());
        EXPECT_EQ(local_transport.type, "local");
        ASSERT_NE(local_transport.settings, nullptr);
        EXPECT_EQ(
            rpc::connection_factory::detail::resolve_stream_rpc_settings(local_config.settings).error_code,
            rpc::error::INVALID_DATA());

#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
        const auto local_settings
            = rpc::connection_factory::materialise_settings<rpc::local_transport::transport_settings>(
                *local_transport.settings);
        ASSERT_EQ(local_settings.error_code, rpc::error::OK());
        ASSERT_TRUE(local_settings.settings.name.has_value());
        EXPECT_EQ(local_settings.settings.name.value(), "child_zone");
        ASSERT_TRUE(local_settings.settings.service_proxy_name.has_value());
        EXPECT_EQ(local_settings.settings.service_proxy_name.value(), "child_proxy");
        ASSERT_TRUE(local_settings.settings.encoding.has_value());
        EXPECT_EQ(local_settings.settings.encoding.value(), rpc::encoding::nanopb);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
        const auto sgx_config = rpc::connection_factory::materialise_connection_settings(parse(R"json({
            "transport": {
                "type": "sgx_coroutine",
                "settings": {
                    "name": "secure_zone",
                    "service_proxy_name": "secure_proxy",
                    "encoding": "yas_binary",
                    "enclave_path": "/tmp/test_enclave.signed.so",
                    "worker_thread_count": 2,
                    "use_sidecar": true,
                    "enclave": {
                        "io_uring": { "queue_depth": 128 },
                        "services": {
                            "attestation": {
                                "type": "simulation",
                                "settings": { "allow_debug_peer": true }
                            }
                        }
                    },
                    "startup_applications": {
                        "filesystem": { "root": "/tmp/canopy" }
                    }
                }
            }
        })json"));
        ASSERT_EQ(sgx_config.error_code, rpc::error::OK());

        const auto sgx_transport = rpc::connection_factory::detail::transport_from_connection(sgx_config.settings);
        ASSERT_EQ(sgx_transport.error_code, rpc::error::OK());
        EXPECT_EQ(sgx_transport.type, "sgx_coroutine");
        ASSERT_NE(sgx_transport.settings, nullptr);

        const auto sgx_settings
            = rpc::connection_factory::materialise_settings<rpc::sgx_coroutine_transport::transport_settings>(
                *sgx_transport.settings);
        ASSERT_EQ(sgx_settings.error_code, rpc::error::OK());
        ASSERT_TRUE(sgx_settings.settings.name.has_value());
        EXPECT_EQ(sgx_settings.settings.name.value(), "secure_zone");
        ASSERT_TRUE(sgx_settings.settings.service_proxy_name.has_value());
        EXPECT_EQ(sgx_settings.settings.service_proxy_name.value(), "secure_proxy");
        ASSERT_TRUE(sgx_settings.settings.encoding.has_value());
        EXPECT_EQ(sgx_settings.settings.encoding.value(), rpc::encoding::yas_binary);
        EXPECT_EQ(sgx_settings.settings.enclave_path, "/tmp/test_enclave.signed.so");
        EXPECT_EQ(sgx_settings.settings.worker_thread_count, 2u);
        EXPECT_TRUE(sgx_settings.settings.use_sidecar);
        ASSERT_TRUE(sgx_settings.settings.enclave.has_value());
        EXPECT_EQ(sgx_settings.settings.enclave.value().io_uring.queue_depth, 128u);
        EXPECT_TRUE(sgx_settings.settings.enclave.value().io_uring.use_sqpoll);
        EXPECT_EQ(sgx_settings.settings.enclave.value().io_uring.fixed_file_count, 128u);
        EXPECT_TRUE(sgx_settings.settings.enclave.value().io_uring.register_fixed_files);
        ASSERT_EQ(sgx_settings.settings.enclave.value().services.size(), 1u);
        ASSERT_TRUE(sgx_settings.settings.enclave.value().services.count("attestation"));
        EXPECT_EQ(sgx_settings.settings.enclave.value().services.at("attestation").type, "simulation");
        ASSERT_TRUE(sgx_settings.settings.enclave.value().services.at("attestation").settings.has_value());
        EXPECT_EQ(sgx_settings.settings.startup_applications.size(), 1u);
        EXPECT_TRUE(sgx_settings.settings.startup_applications.count("filesystem"));
#endif

#ifdef CANOPY_HAS_SGX_BLOCKING_TRANSPORT_CONFIG
        const auto sgx_blocking_settings
            = rpc::connection_factory::materialise_settings<rpc::sgx_blocking_transport::transport_settings>(parse(R"json({
                "name": "blocking_secure_zone",
                "service_proxy_name": "blocking_secure_proxy",
                "encoding": "yas_binary",
                "enclave_path": "/tmp/blocking_enclave.signed.so",
                "enclave": {
                    "io_uring": { "queue_depth": 64 },
                    "services": {
                        "attestation": {
                            "type": "sgx_dcap",
                            "settings": { "quote_provider": "host" }
                        }
                    }
                }
            })json"));
        ASSERT_EQ(sgx_blocking_settings.error_code, rpc::error::OK());
        ASSERT_TRUE(sgx_blocking_settings.settings.name.has_value());
        EXPECT_EQ(sgx_blocking_settings.settings.name.value(), "blocking_secure_zone");
        ASSERT_TRUE(sgx_blocking_settings.settings.service_proxy_name.has_value());
        EXPECT_EQ(sgx_blocking_settings.settings.service_proxy_name.value(), "blocking_secure_proxy");
        ASSERT_TRUE(sgx_blocking_settings.settings.encoding.has_value());
        EXPECT_EQ(sgx_blocking_settings.settings.encoding.value(), rpc::encoding::yas_binary);
        EXPECT_EQ(sgx_blocking_settings.settings.enclave_path, "/tmp/blocking_enclave.signed.so");
        ASSERT_TRUE(sgx_blocking_settings.settings.enclave.has_value());
        EXPECT_EQ(sgx_blocking_settings.settings.enclave.value().io_uring.queue_depth, 64u);
        EXPECT_TRUE(sgx_blocking_settings.settings.enclave.value().io_uring.use_sqpoll);
        EXPECT_EQ(sgx_blocking_settings.settings.enclave.value().io_uring.fixed_file_count, 128u);
        EXPECT_TRUE(sgx_blocking_settings.settings.enclave.value().io_uring.register_fixed_files);
        ASSERT_TRUE(sgx_blocking_settings.settings.enclave.value().services.count("attestation"));
        EXPECT_EQ(sgx_blocking_settings.settings.enclave.value().services.at("attestation").type, "sgx_dcap");
#endif
    }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_IPC_SPSC
    TEST(
        JsonConvert,
        ApplicationRuntimeResolvesIpcSpscPathsAgainstConfigDirectory)
    {
        rpc::ipc_spsc::transport_settings ipc_settings;
        ipc_settings.use_sidecar = false;
        ipc_settings.peer_to_peer_shared_memory_file = "run/queues/ipc.map";
        ipc_settings.create_peer_to_peer_shared_memory_file = true;
        ipc_settings.sidecar_executable_path = "bin/ipc-sidecar";
        ipc_settings.dynamic_library_path = "lib/libpayload.so";

        rpc::connection_factory::typed_settings transport;
        transport.type = "ipc_spsc";
        transport.settings = to_json_object(ipc_settings);

        rpc::connection_factory::named_connection_settings connection;
        connection.name = "client";
        connection.connection.transport = std::move(transport);

        rpc::connection_factory::topology_settings topology;
        topology.connections.push_back(std::move(connection));

        const std::filesystem::path base_directory{"/tmp/canopy-config"};
        auto runtime = rpc::connection_factory::make_application_runtime(std::move(topology), base_directory);
        ASSERT_EQ(runtime.error_code, rpc::error::OK()) << runtime.message;
        ASSERT_NE(runtime.runtime, nullptr);

        const auto* resolved_connection = runtime.runtime->find_connection("client");
        ASSERT_NE(resolved_connection, nullptr);
        ASSERT_TRUE(resolved_connection->connection.transport.has_value());

        const auto resolved = rpc::connection_factory::materialise_settings<rpc::ipc_spsc::transport_settings>(
            resolved_connection->connection.transport.value());
        ASSERT_EQ(resolved.error_code, rpc::error::OK());
        EXPECT_EQ(resolved.settings.peer_to_peer_shared_memory_file, (base_directory / "run/queues/ipc.map").string());
        EXPECT_EQ(resolved.settings.sidecar_executable_path, (base_directory / "bin/ipc-sidecar").string());
        EXPECT_EQ(resolved.settings.dynamic_library_path, (base_directory / "lib/libpayload.so").string());
        EXPECT_TRUE(resolved.settings.create_peer_to_peer_shared_memory_file);
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
    TEST(
        JsonConvert,
        SgxBlockingConnectionFactoryRejectsStreamLayers)
    {
        const auto config = rpc::connection_factory::materialise_connection_settings(parse(R"json({
            "transport": {
                "type": "sgx_blocking",
                "settings": {
                    "name": "blocking_secure_zone",
                    "enclave_path": "/tmp/blocking_enclave.signed.so"
                }
            },
            "stream_layers": [
                { "type": "tls", "settings": { "client": { "verify_peer": false } } }
            ]
        })json"));
        ASSERT_EQ(config.error_code, rpc::error::OK());

        const auto transport = rpc::connection_factory::detail::transport_from_connection(config.settings);
        ASSERT_EQ(transport.error_code, rpc::error::OK());
        ASSERT_NE(transport.settings, nullptr);

        const auto context
            = rpc::connection_factory::detail::make_native_transport_connect_context(transport, config.settings, {});
        EXPECT_EQ(context.error_code, rpc::error::INVALID_DATA());
        EXPECT_EQ(context.service, nullptr);
        EXPECT_EQ(context.transport, nullptr);
    }

    TEST(
        JsonConvert,
        SgxBlockingConnectionFactoryCopiesNameToServiceProxyName)
    {
        const auto config = rpc::connection_factory::materialise_connection_settings(parse(R"json({
            "transport": {
                "type": "sgx_blocking",
                "settings": {
                    "name": "blocking_secure_zone",
                    "encoding": "yas_binary",
                    "enclave_path": "/tmp/blocking_enclave.signed.so",
                    "enclave": {
                        "io_uring": { "queue_depth": 64 }
                    }
                }
            }
        })json"));
        ASSERT_EQ(config.error_code, rpc::error::OK());

        const auto transport = rpc::connection_factory::detail::transport_from_connection(config.settings);
        ASSERT_EQ(transport.error_code, rpc::error::OK());
        ASSERT_NE(transport.settings, nullptr);

        auto context
            = rpc::connection_factory::detail::make_native_transport_connect_context(transport, config.settings, {});
        ASSERT_EQ(context.error_code, rpc::error::OK());
        ASSERT_NE(context.service, nullptr);
        ASSERT_NE(context.transport, nullptr);
        EXPECT_EQ(context.service_proxy_name, "blocking_secure_zone");

        auto enclave_transport
            = std::dynamic_pointer_cast<rpc::sgx_blocking_transport::enclave_transport>(context.transport);
        ASSERT_NE(enclave_transport, nullptr);
        EXPECT_EQ(enclave_transport->get_enclave_path(), "/tmp/blocking_enclave.signed.so");

        const auto runtime_settings = enclave_transport->get_enclave_runtime_startup_settings();
        ASSERT_TRUE(runtime_settings.has_value());
        EXPECT_EQ(runtime_settings.value().io_uring.queue_depth, 64u);
        EXPECT_TRUE(runtime_settings.value().io_uring.use_sqpoll);
        EXPECT_EQ(runtime_settings.value().io_uring.fixed_file_count, 128u);
        EXPECT_TRUE(runtime_settings.value().io_uring.register_fixed_files);
    }
#endif

    TEST(
        JsonConvert,
        UnknownPropertiesAreIgnoredButPreservedTypeIsCorrect)
    {
        // Unknown JSON properties pass through silently — schema validation is
        // the place to reject unexpected keys; the converter just builds the
        // typed view of what it recognises.
        const auto input = parse(R"json({
            "host": "127.0.0.1",
            "uninvented_key": 42
        })json");
        const auto typed = from_json_object<streaming::tcp::endpoint>(input);
        EXPECT_EQ(typed.host, "127.0.0.1");
    }
}
