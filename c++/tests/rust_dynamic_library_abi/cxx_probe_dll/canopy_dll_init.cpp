/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 *
 *   C++ child DLL implementing probe::i_math for the reverse cross-language
 *   proof: generated Rust proxy -> C ABI -> generated C++ protobuf object.
 */

#include <transports/c_abi/dll_transport.h>

#include <protobuf_runtime_probe/basic_rpc_probe.h>
#include <rpc/rpc.h>

namespace
{
    class probe_peer_impl : public rpc::base<probe_peer_impl, probe::i_peer>
    {
    public:
        CORO_TASK(error_code) ping(int& value) override
        {
            value = 11;
            CO_RETURN rpc::error::OK();
        }
    };

    class probe_math_impl : public rpc::base<probe_math_impl, probe::i_math>,
                            public rpc::enable_shared_from_this<probe_math_impl>
    {
    public:
        CORO_TASK(error_code)
        add(int a,
            int b,
            int& c) override
        {
            c = a + b;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        bounce_text(
            const std::string& input,
            std::string& output) override
        {
            output = "cxx-probe:" + input;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        reverse_sequence(
            const std::vector<uint64_t>& input,
            std::vector<uint64_t>& output) override
        {
            output.assign(input.rbegin(), input.rend());
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        increment_sequence(
            const std::vector<int>& input,
            std::vector<int>& output) override
        {
            output.resize(input.size());
            for (std::size_t i = 0; i < input.size(); ++i)
                output[i] = input[i] + 1;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        decorate_many(
            const std::vector<std::string>& input,
            std::vector<std::string>& output) override
        {
            output.resize(input.size());
            for (std::size_t i = 0; i < input.size(); ++i)
                output[i] = "[" + input[i] + "]";
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        echo_bytes(
            const std::vector<uint8_t>& input,
            std::vector<uint8_t>& output) override
        {
            output = input;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        negate_signed_bytes(
            const std::vector<signed char>& input,
            std::vector<signed char>& output) override
        {
            output.resize(input.size());
            for (std::size_t i = 0; i < input.size(); ++i)
                output[i] = static_cast<signed char>(-input[i]);
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        translate_point(
            const probe::point& p,
            int dx,
            int dy,
            probe::point& translated) override
        {
            translated.x = p.x + dx;
            translated.y = p.y + dy;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        label_value(
            const probe::labeled_value& input,
            probe::labeled_value& output) override
        {
            output.label = "[" + input.label + "]";
            output.value = input.value * 2;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        accept_shared_peer(
            const rpc::shared_ptr<probe::i_peer>& peer,
            int& seen) override
        {
            int value = 0;
            auto result = CO_AWAIT peer->ping(value);
            if (result == rpc::error::OK())
                seen = value + 300;
            CO_RETURN result;
        }

        CORO_TASK(error_code)
        accept_optimistic_peer(
            const rpc::optimistic_ptr<probe::i_peer>& peer,
            int& seen) override
        {
            int value = 0;
            auto result = CO_AWAIT peer->ping(value);
            if (result == rpc::error::OK())
                seen = value + 400;
            CO_RETURN result;
        }

        CORO_TASK(error_code) create_shared_peer(rpc::shared_ptr<probe::i_peer>& created_peer) override
        {
            peer_ = rpc::shared_ptr<probe::i_peer>(new probe_peer_impl());
            created_peer = peer_;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code) create_optimistic_peer(rpc::optimistic_ptr<probe::i_peer>& created_peer) override
        {
            if (!peer_)
                peer_ = rpc::shared_ptr<probe::i_peer>(new probe_peer_impl());
            auto [err, optimistic] = CO_AWAIT rpc::make_optimistic(peer_);
            if (err == rpc::error::OK())
                created_peer = std::move(optimistic);
            CO_RETURN err;
        }

        CORO_TASK(error_code)
        echo_shared_peer(
            const rpc::shared_ptr<probe::i_peer>& input,
            rpc::shared_ptr<probe::i_peer>& output) override
        {
            output = input;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(error_code)
        echo_optimistic_peer(
            const rpc::optimistic_ptr<probe::i_peer>& input,
            rpc::optimistic_ptr<probe::i_peer>& output) override
        {
            output = input;
            CO_RETURN rpc::error::OK();
        }

    private:
        rpc::shared_ptr<probe::i_peer> peer_;
    };
} // namespace

extern "C" CANOPY_C_ABI_EXPORT int32_t canopy_dll_init(canopy_dll_init_params* params)
{
    return rpc::c_abi::init_child_zone<probe::i_peer, probe::i_math>(
        params,
        [](rpc::shared_ptr<probe::i_peer> /*host*/,
            std::shared_ptr<rpc::child_service> /*svc*/) -> rpc::service_connect_result<probe::i_math>
        {
            // host is null when the Rust parent passes a zone-only remote_object_id
            // (no live i_peer registered), which is fine for scalar-only methods.
            auto impl = rpc::shared_ptr<probe::i_math>(new probe_math_impl());
            return rpc::service_connect_result<probe::i_math>{rpc::error::OK(), std::move(impl)};
        });
}
