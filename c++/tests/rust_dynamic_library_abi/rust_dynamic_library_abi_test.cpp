/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <c_abi/dynamic_library/canopy_dynamic_library.h>
#include <protobuf_runtime_probe/basic_rpc_probe.h>
#include <src/protobuf_runtime_probe/protobuf/schema/probe.pb.h>
#include <rpc/rpc.h>
#include <transports/c_abi/dynamic_library_loader.h>
#include <transports/c_abi/transport.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef CANOPY_RUST_TEST_DLL_PATH
#  error "CANOPY_RUST_TEST_DLL_PATH must be defined"
#endif

namespace
{
    struct allocator_state
    {
        std::unordered_map<uint8_t*, std::vector<uint8_t>> allocations;
    };

    struct parent_callback_state
    {
        uint64_t send_call_count = 0;
        uint64_t get_new_zone_id_call_count = 0;
        uint64_t post_call_count = 0;
        uint64_t try_cast_call_count = 0;
        uint64_t add_ref_call_count = 0;
        uint64_t release_call_count = 0;
        uint64_t object_released_call_count = 0;
        uint64_t transport_down_call_count = 0;
        uint64_t observed_interface_id = 0;
        uint64_t observed_method_id = 0;
        std::string observed_payload;
        std::string observed_post_payload;
        std::vector<uint8_t> out_buf;
        std::vector<uint8_t> zone_blob;
    };

    std::vector<uint8_t> sample_parent_zone_blob()
    {
        auto zone_address = rpc::zone_address::create(
            rpc::zone_address_args(
                rpc::default_values::version_3, rpc::address_type::ipv4, 8081, {127, 0, 0, 1}, 32, 88, 16, 0, {}));
        EXPECT_TRUE(zone_address.has_value());
        return zone_address->get_blob();
    }

    extern "C" canopy_byte_buffer test_alloc(
        void* allocator_ctx,
        size_t size)
    {
        auto* state = static_cast<allocator_state*>(allocator_ctx);
        std::vector<uint8_t> bytes(size);
        auto* data = bytes.empty() ? nullptr : bytes.data();
        auto [it, inserted] = state->allocations.emplace(data, std::move(bytes));
        EXPECT_TRUE(inserted);
        return canopy_byte_buffer{data, size};
    }

    extern "C" void test_free(
        void* allocator_ctx,
        uint8_t* data,
        size_t)
    {
        auto* state = static_cast<allocator_state*>(allocator_ctx);
        state->allocations.erase(data);
    }

    extern "C" int32_t parent_send(
        canopy_parent_context parent_ctx,
        const canopy_send_params* params,
        canopy_send_result* result)
    {
        auto* state = static_cast<parent_callback_state*>(parent_ctx);
        state->send_call_count += 1;
        state->observed_interface_id = params->interface_id;
        state->observed_method_id = params->method_id;
        state->observed_payload.assign(reinterpret_cast<const char*>(params->in_data.data), params->in_data.size);
        state->out_buf.assign({'p', 'a', 'r', 'e', 'n', 't', '-', 'o', 'k'});

        *result = {};
        result->error_code = 0;
        result->out_buf.data = state->out_buf.data();
        result->out_buf.size = state->out_buf.size();
        return result->error_code;
    }

    extern "C" void parent_post(
        canopy_parent_context parent_ctx,
        const canopy_post_params* params)
    {
        auto* state = static_cast<parent_callback_state*>(parent_ctx);
        state->post_call_count += 1;
        state->observed_interface_id = params->interface_id;
        state->observed_method_id = params->method_id;
        state->observed_post_payload.assign(reinterpret_cast<const char*>(params->in_data.data), params->in_data.size);
    }

    extern "C" int32_t parent_try_cast(
        canopy_parent_context parent_ctx,
        const canopy_try_cast_params* params,
        canopy_standard_result* result)
    {
        auto* state = static_cast<parent_callback_state*>(parent_ctx);
        state->try_cast_call_count += 1;
        state->observed_interface_id = params->interface_id;
        *result = {};
        result->error_code = 0;
        return result->error_code;
    }

    extern "C" int32_t parent_add_ref(
        canopy_parent_context parent_ctx,
        const canopy_add_ref_params*,
        canopy_standard_result* result)
    {
        auto* state = static_cast<parent_callback_state*>(parent_ctx);
        state->add_ref_call_count += 1;
        *result = {};
        result->error_code = 0;
        return result->error_code;
    }

    extern "C" int32_t parent_release(
        canopy_parent_context parent_ctx,
        const canopy_release_params*,
        canopy_standard_result* result)
    {
        auto* state = static_cast<parent_callback_state*>(parent_ctx);
        state->release_call_count += 1;
        *result = {};
        result->error_code = 0;
        return result->error_code;
    }

    extern "C" void parent_object_released(
        canopy_parent_context parent_ctx,
        const canopy_object_released_params*)
    {
        auto* state = static_cast<parent_callback_state*>(parent_ctx);
        state->object_released_call_count += 1;
    }

    extern "C" void parent_transport_down(
        canopy_parent_context parent_ctx,
        const canopy_transport_down_params*)
    {
        auto* state = static_cast<parent_callback_state*>(parent_ctx);
        state->transport_down_call_count += 1;
    }

    extern "C" int32_t parent_get_new_zone_id(
        canopy_parent_context parent_ctx,
        const canopy_get_new_zone_id_params*,
        canopy_new_zone_id_result* result)
    {
        auto* state = static_cast<parent_callback_state*>(parent_ctx);
        state->get_new_zone_id_call_count += 1;
        state->zone_blob = sample_parent_zone_blob();

        *result = {};
        result->error_code = 0;
        result->zone_id.address.blob.data = state->zone_blob.data();
        result->zone_id.address.blob.size = state->zone_blob.size();
        return result->error_code;
    }

    void free_remote_object(
        canopy_allocator_vtable* allocator,
        canopy_remote_object* object)
    {
        if (!object->address.blob.data || !object->address.blob.size)
            return;

        allocator->free(
            allocator->allocator_ctx, const_cast<uint8_t*>(object->address.blob.data), object->address.blob.size);
        object->address.blob = {};
    }

    void free_send_result(
        canopy_allocator_vtable* allocator,
        canopy_send_result* result)
    {
        if (result->out_buf.data && result->out_buf.size)
            allocator->free(allocator->allocator_ctx, result->out_buf.data, result->out_buf.size);

        for (size_t i = 0; i < result->out_back_channel.size; ++i)
        {
            auto& entry = result->out_back_channel.data[i];
            if (entry.payload.data && entry.payload.size)
                allocator->free(allocator->allocator_ctx, const_cast<uint8_t*>(entry.payload.data), entry.payload.size);
        }

        if (result->out_back_channel.data && result->out_back_channel.size)
        {
            allocator->free(
                allocator->allocator_ctx,
                reinterpret_cast<uint8_t*>(result->out_back_channel.data),
                result->out_back_channel.size * sizeof(canopy_back_channel_entry));
        }

        *result = {};
    }

    void free_standard_result(
        canopy_allocator_vtable* allocator,
        canopy_standard_result* result)
    {
        for (size_t i = 0; i < result->out_back_channel.size; ++i)
        {
            auto& entry = result->out_back_channel.data[i];
            if (entry.payload.data && entry.payload.size)
                allocator->free(allocator->allocator_ctx, const_cast<uint8_t*>(entry.payload.data), entry.payload.size);
        }

        if (result->out_back_channel.data && result->out_back_channel.size)
        {
            allocator->free(
                allocator->allocator_ctx,
                reinterpret_cast<uint8_t*>(result->out_back_channel.data),
                result->out_back_channel.size * sizeof(canopy_back_channel_entry));
        }

        *result = {};
    }

    void free_new_zone_id_result(
        canopy_allocator_vtable* allocator,
        canopy_new_zone_id_result* result)
    {
        free_remote_object(allocator, reinterpret_cast<canopy_remote_object*>(&result->zone_id));
        free_standard_result(allocator, reinterpret_cast<canopy_standard_result*>(result));
    }

    std::string send_and_read_string(
        rpc::c_abi::dynamic_library_loader& loader,
        canopy_child_context child_ctx,
        canopy_allocator_vtable* allocator,
        const std::string& payload)
    {
        canopy_send_params send_params{};
        send_params.protocol_version = 3;
        send_params.encoding_type = 16;
        send_params.tag = 55;
        send_params.interface_id = 1;
        send_params.method_id = 2;
        send_params.in_data.data = reinterpret_cast<const uint8_t*>(payload.data());
        send_params.in_data.size = payload.size();

        canopy_send_result send_result{};
        EXPECT_EQ(loader.exports().send(child_ctx, &send_params, &send_result), 0);
        EXPECT_NE(send_result.out_buf.data, nullptr);

        std::string returned(reinterpret_cast<const char*>(send_result.out_buf.data), send_result.out_buf.size);
        free_send_result(allocator, &send_result);
        return returned;
    }

    class rust_dynamic_library_abi_test : public ::testing::Test
    {
    protected:
        rpc::c_abi::dynamic_library_loader loader_;
        allocator_state allocator_state_;
        parent_callback_state parent_state_;
        canopy_allocator_vtable allocator_{&allocator_state_, &test_alloc, &test_free};

        void SetUp() override { ASSERT_TRUE(loader_.load(CANOPY_RUST_TEST_DLL_PATH)); }

        void TearDown() override
        {
            EXPECT_TRUE(allocator_state_.allocations.empty());
            loader_.unload();
        }
    };

    TEST_F(
        rust_dynamic_library_abi_test,
        init_and_send_round_trip)
    {
        canopy_dll_init_params init_params{};
        init_params.name = "rust child";
        init_params.allocator = allocator_;
        init_params.parent_ctx = &parent_state_;
        init_params.parent_send = &parent_send;
        init_params.parent_post = &parent_post;
        init_params.parent_try_cast = &parent_try_cast;
        init_params.parent_add_ref = &parent_add_ref;
        init_params.parent_release = &parent_release;
        init_params.parent_object_released = &parent_object_released;
        init_params.parent_transport_down = &parent_transport_down;
        init_params.parent_get_new_zone_id = &parent_get_new_zone_id;

        ASSERT_EQ(loader_.exports().init(&init_params), 0);
        ASSERT_NE(init_params.child_ctx, nullptr);
        ASSERT_NE(init_params.output_obj.address.blob.data, nullptr);

        const std::string payload = "ping";
        canopy_send_params send_params{};
        send_params.protocol_version = 3;
        send_params.encoding_type = 16;
        send_params.tag = 55;
        send_params.interface_id = 1;
        send_params.method_id = 2;
        send_params.in_data.data = reinterpret_cast<const uint8_t*>(payload.data());
        send_params.in_data.size = payload.size();

        canopy_send_result send_result{};
        ASSERT_EQ(loader_.exports().send(init_params.child_ctx, &send_params, &send_result), 0);
        ASSERT_NE(send_result.out_buf.data, nullptr);

        std::string returned(reinterpret_cast<const char*>(send_result.out_buf.data), send_result.out_buf.size);
        EXPECT_EQ(returned, "rust-child:ping");

        free_send_result(&allocator_, &send_result);
        free_remote_object(&allocator_, &init_params.output_obj);
        loader_.exports().destroy(init_params.child_ctx);
    }

    TEST_F(
        rust_dynamic_library_abi_test,
        cxx_payload_can_call_generated_rust_protobuf_method)
    {
        canopy_dll_init_params init_params{};
        init_params.name = "rust child";
        init_params.allocator = allocator_;
        init_params.parent_ctx = &parent_state_;
        init_params.parent_send = &parent_send;
        init_params.parent_post = &parent_post;
        init_params.parent_try_cast = &parent_try_cast;
        init_params.parent_add_ref = &parent_add_ref;
        init_params.parent_release = &parent_release;
        init_params.parent_object_released = &parent_object_released;
        init_params.parent_transport_down = &parent_transport_down;
        init_params.parent_get_new_zone_id = &parent_get_new_zone_id;

        ASSERT_EQ(loader_.exports().init(&init_params), 0);
        ASSERT_NE(init_params.child_ctx, nullptr);

        // Use generated protobuf types for the request.
        protobuf::probe::i_math_addRequest request;
        request.set_a(20);
        request.set_b(22);
        std::string request_str;
        ASSERT_TRUE(request.SerializeToString(&request_str));

        canopy_send_params send_params{};
        send_params.protocol_version = 3;
        send_params.encoding_type = 16;
        send_params.tag = 56;
        send_params.interface_id = probe::i_math::get_id(rpc::VERSION_3).get_val();
        send_params.method_id = 1;
        send_params.in_data.data = reinterpret_cast<const uint8_t*>(request_str.data());
        send_params.in_data.size = request_str.size();

        canopy_send_result send_result{};
        ASSERT_EQ(loader_.exports().send(init_params.child_ctx, &send_params, &send_result), 0);
        EXPECT_EQ(send_result.error_code, 0);
        ASSERT_NE(send_result.out_buf.data, nullptr);

        // Use generated protobuf types to parse the response.
        protobuf::probe::i_math_addResponse response;
        std::string response_str(reinterpret_cast<const char*>(send_result.out_buf.data), send_result.out_buf.size);
        ASSERT_TRUE(response.ParseFromString(response_str));
        EXPECT_EQ(response.result(), 0);
        EXPECT_EQ(response.c(), 42);

        free_send_result(&allocator_, &send_result);
        free_remote_object(&allocator_, &init_params.output_obj);
        loader_.exports().destroy(init_params.child_ctx);
    }

    TEST_F(
        rust_dynamic_library_abi_test,
        generated_cxx_proxy_can_call_generated_rust_protobuf_method)
    {
        auto root_service = rpc::root_service::create("cxx host", rpc::DEFAULT_PREFIX);
        auto child_transport = std::make_shared<rpc::c_abi::child_transport>(
            "rust generated child", root_service, CANOPY_RUST_TEST_DLL_PATH);

        auto connect_result = root_service->connect_to_zone<probe::i_peer, probe::i_math>(
            "rust generated child", child_transport, rpc::shared_ptr<probe::i_peer>());

        ASSERT_EQ(connect_result.error_code, rpc::error::OK());
        ASSERT_NE(connect_result.output_interface, nullptr);

        int c = -1;
        EXPECT_EQ(connect_result.output_interface->add(20, 22, c), rpc::error::OK());
        EXPECT_EQ(c, 42);

        connect_result.output_interface = nullptr;
        root_service = nullptr;
    }

    TEST_F(
        rust_dynamic_library_abi_test,
        try_cast_release_and_get_new_zone_id_round_trip)
    {
        canopy_dll_init_params init_params{};
        init_params.name = "rust child";
        init_params.allocator = allocator_;
        init_params.parent_ctx = &parent_state_;
        init_params.parent_send = &parent_send;
        init_params.parent_post = &parent_post;
        init_params.parent_try_cast = &parent_try_cast;
        init_params.parent_add_ref = &parent_add_ref;
        init_params.parent_release = &parent_release;
        init_params.parent_object_released = &parent_object_released;
        init_params.parent_transport_down = &parent_transport_down;
        init_params.parent_get_new_zone_id = &parent_get_new_zone_id;

        ASSERT_EQ(loader_.exports().init(&init_params), 0);
        ASSERT_NE(init_params.child_ctx, nullptr);
        ASSERT_NE(init_params.output_obj.address.blob.data, nullptr);

        canopy_zone caller_zone{};
        caller_zone.address = init_params.output_obj.address;

        canopy_try_cast_params try_cast_params{};
        try_cast_params.protocol_version = 3;
        try_cast_params.caller_zone_id = caller_zone;
        try_cast_params.remote_object_id = init_params.output_obj;
        try_cast_params.interface_id = 1;

        canopy_standard_result try_cast_result{};
        ASSERT_EQ(loader_.exports().try_cast(init_params.child_ctx, &try_cast_params, &try_cast_result), 0);
        EXPECT_EQ(try_cast_result.error_code, 0);
        EXPECT_EQ(try_cast_result.out_back_channel.size, 0u);
        free_standard_result(&allocator_, &try_cast_result);

        canopy_release_params release_params{};
        release_params.protocol_version = 3;
        release_params.remote_object_id = init_params.output_obj;
        release_params.caller_zone_id = caller_zone;
        release_params.options = 0;

        canopy_standard_result release_result{};
        ASSERT_EQ(loader_.exports().release(init_params.child_ctx, &release_params, &release_result), 0);
        EXPECT_EQ(release_result.error_code, 0);
        EXPECT_EQ(release_result.out_back_channel.size, 0u);
        free_standard_result(&allocator_, &release_result);

        canopy_get_new_zone_id_params zone_params{};
        zone_params.protocol_version = 3;

        canopy_new_zone_id_result zone_result{};
        ASSERT_EQ(loader_.exports().get_new_zone_id(init_params.child_ctx, &zone_params, &zone_result), 0);
        EXPECT_EQ(zone_result.error_code, 0);
        ASSERT_NE(zone_result.zone_id.address.blob.data, nullptr);
        EXPECT_GT(zone_result.zone_id.address.blob.size, 0u);
        EXPECT_EQ(zone_result.out_back_channel.size, 0u);
        free_new_zone_id_result(&allocator_, &zone_result);

        free_remote_object(&allocator_, &init_params.output_obj);
        loader_.exports().destroy(init_params.child_ctx);
    }

    TEST_F(
        rust_dynamic_library_abi_test,
        post_add_ref_object_released_and_transport_down_are_observable)
    {
        canopy_dll_init_params init_params{};
        init_params.name = "rust child";
        init_params.allocator = allocator_;
        init_params.parent_ctx = &parent_state_;
        init_params.parent_send = &parent_send;
        init_params.parent_post = &parent_post;
        init_params.parent_try_cast = &parent_try_cast;
        init_params.parent_add_ref = &parent_add_ref;
        init_params.parent_release = &parent_release;
        init_params.parent_object_released = &parent_object_released;
        init_params.parent_transport_down = &parent_transport_down;
        init_params.parent_get_new_zone_id = &parent_get_new_zone_id;

        ASSERT_EQ(loader_.exports().init(&init_params), 0);
        ASSERT_NE(init_params.child_ctx, nullptr);
        ASSERT_NE(init_params.output_obj.address.blob.data, nullptr);

        canopy_zone caller_zone{};
        caller_zone.address = init_params.output_obj.address;

        const std::string post_payload = "fire-and-forget";
        canopy_post_params post_params{};
        post_params.protocol_version = 3;
        post_params.encoding_type = 16;
        post_params.tag = 77;
        post_params.caller_zone_id = caller_zone;
        post_params.remote_object_id = init_params.output_obj;
        post_params.interface_id = 1;
        post_params.method_id = 2;
        post_params.in_data.data = reinterpret_cast<const uint8_t*>(post_payload.data());
        post_params.in_data.size = post_payload.size();
        loader_.exports().post(init_params.child_ctx, &post_params);

        canopy_add_ref_params add_ref_params{};
        add_ref_params.protocol_version = 3;
        add_ref_params.remote_object_id = init_params.output_obj;
        add_ref_params.caller_zone_id = caller_zone;
        add_ref_params.requesting_zone_id = caller_zone;
        add_ref_params.build_out_param_channel = 0;

        canopy_standard_result add_ref_result{};
        ASSERT_EQ(loader_.exports().add_ref(init_params.child_ctx, &add_ref_params, &add_ref_result), 0);
        EXPECT_EQ(add_ref_result.error_code, 0);
        free_standard_result(&allocator_, &add_ref_result);

        canopy_object_released_params object_released_params{};
        object_released_params.protocol_version = 3;
        object_released_params.remote_object_id = init_params.output_obj;
        object_released_params.caller_zone_id = caller_zone;
        loader_.exports().object_released(init_params.child_ctx, &object_released_params);

        canopy_transport_down_params transport_down_params{};
        transport_down_params.protocol_version = 3;
        transport_down_params.destination_zone_id = caller_zone;
        transport_down_params.caller_zone_id = caller_zone;
        loader_.exports().transport_down(init_params.child_ctx, &transport_down_params);

        auto stats = send_and_read_string(loader_, init_params.child_ctx, &allocator_, "stats");
        EXPECT_EQ(stats, "post=1;add_ref=1;release=0;object_released=1;transport_down=1");

        free_remote_object(&allocator_, &init_params.output_obj);
        loader_.exports().destroy(init_params.child_ctx);
    }

    TEST_F(
        rust_dynamic_library_abi_test,
        rust_child_can_call_parent_send_callback)
    {
        canopy_dll_init_params init_params{};
        init_params.name = "rust child";
        init_params.allocator = allocator_;
        init_params.parent_ctx = &parent_state_;
        init_params.parent_send = &parent_send;
        init_params.parent_post = &parent_post;
        init_params.parent_try_cast = &parent_try_cast;
        init_params.parent_add_ref = &parent_add_ref;
        init_params.parent_release = &parent_release;
        init_params.parent_object_released = &parent_object_released;
        init_params.parent_transport_down = &parent_transport_down;
        init_params.parent_get_new_zone_id = &parent_get_new_zone_id;

        ASSERT_EQ(loader_.exports().init(&init_params), 0);
        ASSERT_NE(init_params.child_ctx, nullptr);

        auto returned = send_and_read_string(loader_, init_params.child_ctx, &allocator_, "call-parent-send");
        EXPECT_EQ(returned, "parent-ok");
        EXPECT_EQ(parent_state_.send_call_count, 1u);
        EXPECT_EQ(parent_state_.observed_interface_id, 41u);
        EXPECT_EQ(parent_state_.observed_method_id, 42u);
        EXPECT_EQ(parent_state_.observed_payload, "from-rust-child");

        free_remote_object(&allocator_, &init_params.output_obj);
        loader_.exports().destroy(init_params.child_ctx);
    }

    TEST_F(
        rust_dynamic_library_abi_test,
        rust_child_can_call_parent_get_new_zone_id_callback)
    {
        canopy_dll_init_params init_params{};
        init_params.name = "rust child";
        init_params.allocator = allocator_;
        init_params.parent_ctx = &parent_state_;
        init_params.parent_send = &parent_send;
        init_params.parent_post = &parent_post;
        init_params.parent_try_cast = &parent_try_cast;
        init_params.parent_add_ref = &parent_add_ref;
        init_params.parent_release = &parent_release;
        init_params.parent_object_released = &parent_object_released;
        init_params.parent_transport_down = &parent_transport_down;
        init_params.parent_get_new_zone_id = &parent_get_new_zone_id;

        ASSERT_EQ(loader_.exports().init(&init_params), 0);
        ASSERT_NE(init_params.child_ctx, nullptr);

        auto returned = send_and_read_string(loader_, init_params.child_ctx, &allocator_, "call-parent-get-new-zone-id");
        EXPECT_EQ(returned, "zone:88");
        EXPECT_EQ(parent_state_.get_new_zone_id_call_count, 1u);

        free_remote_object(&allocator_, &init_params.output_obj);
        loader_.exports().destroy(init_params.child_ctx);
    }

    TEST_F(
        rust_dynamic_library_abi_test,
        rust_child_can_call_remaining_parent_callbacks)
    {
        canopy_dll_init_params init_params{};
        init_params.name = "rust child";
        init_params.allocator = allocator_;
        init_params.parent_ctx = &parent_state_;
        init_params.parent_send = &parent_send;
        init_params.parent_post = &parent_post;
        init_params.parent_try_cast = &parent_try_cast;
        init_params.parent_add_ref = &parent_add_ref;
        init_params.parent_release = &parent_release;
        init_params.parent_object_released = &parent_object_released;
        init_params.parent_transport_down = &parent_transport_down;
        init_params.parent_get_new_zone_id = &parent_get_new_zone_id;

        ASSERT_EQ(loader_.exports().init(&init_params), 0);
        ASSERT_NE(init_params.child_ctx, nullptr);

        EXPECT_EQ(send_and_read_string(loader_, init_params.child_ctx, &allocator_, "call-parent-post"), "post-ok");
        EXPECT_EQ(parent_state_.post_call_count, 1u);
        EXPECT_EQ(parent_state_.observed_interface_id, 43u);
        EXPECT_EQ(parent_state_.observed_method_id, 44u);
        EXPECT_EQ(parent_state_.observed_post_payload, "post-from-rust-child");

        EXPECT_EQ(send_and_read_string(loader_, init_params.child_ctx, &allocator_, "call-parent-try-cast"), "try-cast:0");
        EXPECT_EQ(parent_state_.try_cast_call_count, 1u);
        EXPECT_EQ(parent_state_.observed_interface_id, 45u);

        EXPECT_EQ(send_and_read_string(loader_, init_params.child_ctx, &allocator_, "call-parent-add-ref"), "add-ref:0");
        EXPECT_EQ(parent_state_.add_ref_call_count, 1u);

        EXPECT_EQ(send_and_read_string(loader_, init_params.child_ctx, &allocator_, "call-parent-release"), "release:0");
        EXPECT_EQ(parent_state_.release_call_count, 1u);

        EXPECT_EQ(
            send_and_read_string(loader_, init_params.child_ctx, &allocator_, "call-parent-object-released"),
            "object-released-ok");
        EXPECT_EQ(parent_state_.object_released_call_count, 1u);

        EXPECT_EQ(
            send_and_read_string(loader_, init_params.child_ctx, &allocator_, "call-parent-transport-down"),
            "transport-down-ok");
        EXPECT_EQ(parent_state_.transport_down_call_count, 1u);

        free_remote_object(&allocator_, &init_params.output_obj);
        loader_.exports().destroy(init_params.child_ctx);
    }
} // namespace
