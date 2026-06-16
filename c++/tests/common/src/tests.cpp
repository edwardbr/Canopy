/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)

#include "gtest/gtest.h"

#include <optional>
#include <variant>

#include <example_shared/example_shared.h>

#include <json/json_dom.h>
#include <common/tests.h>
#include <common/foo_impl.h>

namespace marshalled_tests
{
    xxx::optional_variant_json_holder make_optional_variant_json_holder()
    {
        xxx::optional_variant_json_holder value;
        value.optional_int = 42;
        value.variant_value = std::string("variant-string");
        value.json_value = json::v1::object(
            json::v1::map{
                {"name", "canopy"},
                {"values", json::v1::array{"one", true, nullptr}},
            });
        value.optional_json_value = json::v1::object(
            json::v1::map{
                {"enabled", true},
                {"label", "optional-json"},
            });
        value.rpc_optional_int = 43;
        value.rpc_optional_json_value = json::v1::object(
            json::v1::map{
                {"enabled", true},
                {"label", "rpc-optional-json"},
            });
        return value;
    }

    xxx::rpc_optional_holder make_rpc_optional_holder(bool with_optionals)
    {
        xxx::rpc_optional_holder value;
        value.required_int = 7;
        value.required_string = "required";
        if (with_optionals)
        {
            value.optional_int = 51;
            value.optional_string = std::string("present");
            value.optional_json_value = json::v1::object(
                json::v1::map{
                    {"kind", "rpc-optional"},
                    {"enabled", true},
                });
        }
        return value;
    }

    template<typename Serialiser>
    bool verify_json_exchange_serialiser_roundtrip(
        const json::v1::object& input,
        const json::v1::object& output)
    {
        std::vector<char> request;
        auto ret = xxx::i_foo::proxy_serialiser<Serialiser>::exchange_json_object(input, request);
        EXPECT_EQ(ret, rpc::error::OK());
        if (ret != rpc::error::OK())
        {
            return false;
        }

        json::v1::object decoded_request;
        ret = xxx::i_foo::stub_deserialiser<Serialiser>::exchange_json_object(decoded_request, rpc::byte_span(request));
        EXPECT_EQ(ret, rpc::error::OK());
        EXPECT_EQ(decoded_request, input);
        if (ret != rpc::error::OK() || decoded_request != input)
        {
            return false;
        }

        std::vector<char> response;
        ret = xxx::i_foo::stub_serialiser<Serialiser>::exchange_json_object(output, response);
        EXPECT_EQ(ret, rpc::error::OK());
        if (ret != rpc::error::OK())
        {
            return false;
        }

        json::v1::object decoded_response;
        ret = xxx::i_foo::proxy_deserialiser<Serialiser>::exchange_json_object(decoded_response, rpc::byte_span(response));
        EXPECT_EQ(ret, rpc::error::OK());
        EXPECT_EQ(decoded_response, output);
        if (ret != rpc::error::OK() || decoded_response != output)
        {
            return false;
        }

        return true;
    }

    CORO_TASK(bool)
    standard_tests(
        xxx::i_foo& foo,
        bool supports_process_local_reference_tests)
    {
        {
            CORO_ASSERT_EQ(CO_AWAIT foo.do_something_in_val(33), rpc::error::OK());
        }
        {
            int val = 33;
            CORO_ASSERT_EQ(CO_AWAIT foo.do_something_in_ref(val), rpc::error::OK());
        }
        {
            CORO_ASSERT_EQ(CO_AWAIT foo.do_something_in_move_ref(33), rpc::error::OK());
        }
        {
            int val = 33;
            CORO_ASSERT_EQ(CO_AWAIT foo.do_something_in_ptr(&val), rpc::error::OK());
        }
        {
            auto val = xxx::pointer_serialisation_state::pointer_serialisation_pending;
            CORO_ASSERT_EQ(CO_AWAIT foo.do_enum_ptr_in(&val), rpc::error::OK());
        }
        {
            int val = 0;
            CORO_ASSERT_EQ(CO_AWAIT foo.do_something_out_val(val), rpc::error::OK());
        }
        if (supports_process_local_reference_tests)
        {
            int* val = nullptr;
            CORO_ASSERT_EQ(CO_AWAIT foo.do_something_out_ptr_ref(val), rpc::error::OK());
            delete val;
        }
        if (supports_process_local_reference_tests)
        {
            int* val = nullptr;
            CORO_ASSERT_EQ(CO_AWAIT foo.do_something_out_ptr_ptr(&val), rpc::error::OK());
            delete val;
        }
        if (supports_process_local_reference_tests)
        {
            xxx::pointer_serialisation_state* val = nullptr;
            CORO_ASSERT_EQ(CO_AWAIT foo.do_enum_ptr_out(&val), rpc::error::OK());
            delete val;
        }
        {
            int val = 32;
            CORO_ASSERT_EQ(CO_AWAIT foo.do_something_in_out_ref(val), rpc::error::OK());
        }
        {
            xxx::something_complicated val{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_complicated_val(val), rpc::error::OK());
        }
        {
            xxx::something_complicated val{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_complicated_ref(val), rpc::error::OK());
        }
        {
            xxx::something_complicated val{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_complicated_ref_val(val), rpc::error::OK());
        }
        {
            xxx::something_complicated val{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_complicated_move_ref(std::move(val)), rpc::error::OK());
        }
        {
            xxx::something_complicated val{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_complicated_ptr(&val), rpc::error::OK());
        }
        {
            xxx::something_complicated val;
            CORO_ASSERT_EQ(CO_AWAIT foo.receive_something_complicated_ref(val), rpc::error::OK());
            RPC_INFO("got {}", val.string_val);
        }
        if (supports_process_local_reference_tests)
        {
            xxx::something_complicated* val = nullptr;
            CORO_ASSERT_EQ(CO_AWAIT foo.receive_something_complicated_ptr(val), rpc::error::OK());
            RPC_INFO("got {}", val->int_val);
            delete val;
        }
        {
            xxx::something_complicated val;
            val.int_val = 32;
            CORO_ASSERT_EQ(CO_AWAIT foo.receive_something_complicated_in_out_ref(val), rpc::error::OK());
            RPC_INFO("got {}", val.int_val);
        }
        {
            xxx::something_more_complicated val;
            val.map_val["22"] = xxx::something_complicated{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_more_complicated_val(val), rpc::error::OK());
        }
        if (supports_process_local_reference_tests)
        {
            xxx::something_more_complicated val;
            val.map_val["22"] = xxx::something_complicated{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_more_complicated_ref(val), rpc::error::OK());
        }
        {
            xxx::something_more_complicated val;
            val.map_val["22"] = xxx::something_complicated{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_more_complicated_move_ref(std::move(val)), rpc::error::OK());
        }
        {
            xxx::something_more_complicated val;
            val.map_val["22"] = xxx::something_complicated{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_more_complicated_ref_val(val), rpc::error::OK());
        }
        if (supports_process_local_reference_tests)
        {
            xxx::something_more_complicated val;
            val.map_val["22"] = xxx::something_complicated{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.give_something_more_complicated_ptr(&val), rpc::error::OK());
        }
        if (supports_process_local_reference_tests)
        {
            xxx::something_more_complicated val;
            CORO_ASSERT_EQ(CO_AWAIT foo.receive_something_more_complicated_ref(val), rpc::error::OK());
            if (val.map_val.size() == 0)
            {
                RPC_ERROR("receive_something_more_complicated_ref returned no data");
            }
            else
            {
                RPC_INFO("got {}", val.map_val.begin()->first);
            }
        }
        if (supports_process_local_reference_tests)
        {
            xxx::something_more_complicated* val = nullptr;
            CORO_ASSERT_EQ(CO_AWAIT foo.receive_something_more_complicated_ptr(val), rpc::error::OK());
            if (val->map_val.size() == 0)
            {
                RPC_ERROR("receive_something_more_complicated_ref returned no data");
            }
            else
            {
                RPC_INFO("got {}", val->map_val.begin()->first);
            }
            delete val;
        }
        {
            xxx::something_more_complicated val;
            val.map_val["22"] = xxx::something_complicated{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.receive_something_more_complicated_in_out_ref(val), rpc::error::OK());
            if (val.map_val.size() == 0)
            {
                RPC_ERROR("receive_something_more_complicated_in_out_ref returned no data");
            }
            else
            {
                RPC_INFO("got {}", val.map_val.begin()->first);
            }
        }
        {
            int val1 = 1;
            int val2 = 2;
            CORO_ASSERT_EQ(CO_AWAIT foo.do_multi_val(val1, val2), rpc::error::OK());
        }
        {
            xxx::something_more_complicated val1;
            xxx::something_more_complicated val2;
            val1.map_val["22"] = xxx::something_complicated{.int_val = 33, .string_val = "22"};
            val2.map_val["22"] = xxx::something_complicated{.int_val = 33, .string_val = "22"};
            CORO_ASSERT_EQ(CO_AWAIT foo.do_multi_complicated_val(val1, val2), rpc::error::OK());
        }
        {
            json::v1::object input(
                json::v1::map{
                    {"name", "canopy"},
                    {"flags", json::v1::array{true, nullptr}},
                    {"nested",
                        json::v1::map{
                            {"mode", "standard"},
                            {"count", 7},
                        }},
                });
            json::v1::object output;
            CORO_ASSERT_EQ(CO_AWAIT foo.exchange_json_object(input, output), rpc::error::OK());
            CORO_ASSERT_EQ(output.get_type(), json::v1::object::type::map_type);

            const auto& output_map = output.as_map();
            const auto echo = output_map.find("echo");
            CORO_ASSERT_NE(echo, output_map.end());
            CORO_ASSERT_EQ(echo->second, input);
            CORO_ASSERT_EQ(output_map.at("handled").get<bool>(), true);
            CORO_ASSERT_EQ(output_map.at("details").get<json::v1::array>().size(), size_t{2});

#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
            CORO_ASSERT_EQ(
                verify_json_exchange_serialiser_roundtrip<rpc::serialiser::protocol_buffers>(input, output), true);
#endif
#ifdef CANOPY_BUILD_NANOPB
            CORO_ASSERT_EQ(verify_json_exchange_serialiser_roundtrip<rpc::serialiser::nanopb>(input, output), true);
#endif
        }
        {
            const auto input = make_optional_variant_json_holder();

            CORO_ASSERT_EQ(CO_AWAIT foo.give_optional_variant_json_holder(input), rpc::error::OK());

            xxx::optional_variant_json_holder received;
            CORO_ASSERT_EQ(CO_AWAIT foo.receive_optional_variant_json_holder(received), rpc::error::OK());
            CORO_ASSERT_EQ(received.optional_int.has_value(), true);
            CORO_ASSERT_EQ(received.optional_int.value(), 33);
            CORO_ASSERT_EQ(rpc::holds_alternative<std::string>(received.variant_value), true);
            CORO_ASSERT_EQ(rpc::get<std::string>(received.variant_value), std::string("received-variant"));
            CORO_ASSERT_EQ(received.optional_json_value.has_value(), true);
            CORO_ASSERT_EQ(received.rpc_optional_int.has_value(), true);
            CORO_ASSERT_EQ(received.rpc_optional_int.value(), 44);
            CORO_ASSERT_EQ(received.rpc_optional_json_value.has_value(), true);

            rpc::optional<int32_t> optional_int_out;
            rpc::variant<int32_t, std::string> variant_value_out;
            json::v1::object json_value_out;
            rpc::optional<json::v1::object> optional_json_value_out;
            CORO_ASSERT_EQ(
                CO_AWAIT foo.exchange_optional_variant_json(
                    input.optional_int,
                    input.variant_value,
                    input.json_value,
                    input.optional_json_value,
                    optional_int_out,
                    variant_value_out,
                    json_value_out,
                    optional_json_value_out),
                rpc::error::OK());
            CORO_ASSERT_EQ(optional_int_out, input.optional_int);
            CORO_ASSERT_EQ(variant_value_out, input.variant_value);
            CORO_ASSERT_EQ(json_value_out.get_type(), json::v1::object::type::map_type);
            CORO_ASSERT_EQ(json_value_out.as_map().at("echo"), input.json_value);
            CORO_ASSERT_EQ(optional_json_value_out, input.optional_json_value);

            const auto rpc_optional_holder = make_rpc_optional_holder(true);
            xxx::rpc_optional_holder rpc_optional_holder_out;
            CORO_ASSERT_EQ(
                CO_AWAIT foo.exchange_rpc_optional_holder(rpc_optional_holder, rpc_optional_holder_out), rpc::error::OK());
            CORO_ASSERT_EQ(rpc_optional_holder_out, rpc_optional_holder);

            rpc::optional<int32_t> rpc_optional_int_out;
            rpc::optional<json::v1::object> rpc_optional_json_value_out;
            CORO_ASSERT_EQ(
                CO_AWAIT foo.exchange_rpc_optional_values(
                    input.rpc_optional_int, input.rpc_optional_json_value, rpc_optional_int_out, rpc_optional_json_value_out),
                rpc::error::OK());
            CORO_ASSERT_EQ(rpc_optional_int_out, input.rpc_optional_int);
            CORO_ASSERT_EQ(rpc_optional_json_value_out, input.rpc_optional_json_value);

            const auto empty_rpc_optional_holder = make_rpc_optional_holder(false);
            CORO_ASSERT_EQ(
                CO_AWAIT foo.exchange_rpc_optional_holder(empty_rpc_optional_holder, rpc_optional_holder_out),
                rpc::error::OK());
            CORO_ASSERT_EQ(rpc_optional_holder_out, empty_rpc_optional_holder);
        }
        CO_RETURN true;
    }

    CORO_TASK(bool)
    remote_tests(
        bool use_host_in_child,
        rpc::shared_ptr<yyy::i_example> example_ptr)
    {
        int val = 0;
        CO_AWAIT example_ptr->add(1, 2, val);

        {
            // check the creation of an object that is passed back via interface
            rpc::shared_ptr<xxx::i_foo> foo;
            CORO_ASSERT_EQ(CO_AWAIT example_ptr->create_foo(foo), rpc::error::OK());
            CORO_ASSERT_EQ(CO_AWAIT foo->do_something_in_val(22), rpc::error::OK());

            // test casting logic
            auto i_bar_ptr = CO_AWAIT rpc::dynamic_pointer_cast<xxx::i_bar>(foo);
            CORO_ASSERT_EQ(i_bar_ptr, nullptr);

            // test recursive interface passing
            rpc::shared_ptr<xxx::i_foo> other_foo;
            int err_code = CO_AWAIT foo->receive_interface(other_foo);
            if (err_code != rpc::error::OK())
            {
                RPC_ERROR("create_foo failed");
            }
            else
            {
                CORO_ASSERT_EQ(CO_AWAIT other_foo->do_something_in_val(22), rpc::error::OK());
            }

            if (use_host_in_child)
            {
                rpc::shared_ptr<xxx::i_baz> b(new baz());
                err_code = CO_AWAIT foo->call_baz_interface(b);
            }

            if (CO_AWAIT foo->exception_test() != rpc::error::EXCEPTION())
            {
                RPC_ERROR("exception_test failed");
            }
        }
        {
            rpc::shared_ptr<xxx::i_baz> i_baz_ptr;
            CO_AWAIT example_ptr->create_multiple_inheritance(i_baz_ptr);
            // repeat twice
            for (int i = 0; i < 2; i++)
            {
                auto i_bar_ptr1 = CO_AWAIT rpc::dynamic_pointer_cast<xxx::i_bar>(i_baz_ptr);
                CORO_ASSERT_NE(i_bar_ptr1, nullptr);
                auto i_baz_ptr2 = CO_AWAIT rpc::dynamic_pointer_cast<xxx::i_baz>(i_bar_ptr1);
                CORO_ASSERT_EQ(i_baz_ptr2, i_baz_ptr);
                auto i_bar_ptr2 = CO_AWAIT rpc::dynamic_pointer_cast<xxx::i_bar>(i_baz_ptr2);
                CORO_ASSERT_EQ(i_bar_ptr2, i_bar_ptr1);
                auto i_foo = CO_AWAIT rpc::dynamic_pointer_cast<xxx::i_foo>(i_baz_ptr2);
                CORO_ASSERT_EQ(i_foo, nullptr);
            }
        }
        CO_RETURN true;
    }
}
// NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
