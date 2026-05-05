/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "test_uring.h"
#include "end_2_end_setup.h"

namespace io_uring_test_enclave
{
    namespace
    {
        constexpr uint32_t client_peer_bias = 0x1000U;
        constexpr uint32_t server_peer_bias = 0x2000U;

        class peer2peer_endpoint final : public rpc::base<peer2peer_endpoint, io_uring_test::i_peer2peer>,
                                         public rpc::enable_shared_from_this<peer2peer_endpoint>
        {
        public:
            peer2peer_endpoint(
                rpc::shared_ptr<io_uring_test::i_peer2peer> peer_remote,
                uint32_t peer_bias)
                : peer_remote_(peer_remote)
                , peer_bias_(peer_bias)
            {
            }

            CORO_TASK(int)
            peer_ping(
                uint32_t value,
                uint32_t& response) override
            {
                response = value + peer_bias_;

                // Server endpoints hold the client endpoint supplied during
                // connect_to_zone. The callback proves that one accepted TCP
                // connection can carry RPC traffic in both directions.
                if (peer_remote_)
                {
                    uint32_t callback_response = 0;
                    auto err = CO_AWAIT peer_remote_->peer_ping(value + 1U, callback_response);
                    if (err != rpc::error::OK())
                    {
                        CO_RETURN err;
                    }

                    response += callback_response;
                }

                CO_RETURN rpc::error::OK();
            }

        private:
            rpc::shared_ptr<io_uring_test::i_peer2peer> peer_remote_;
            uint32_t peer_bias_{0};
        };

        rpc::shared_ptr<io_uring_test::i_peer2peer> make_client_peer_endpoint(
            std::shared_ptr<rpc::service>,
            rpc::shared_ptr<io_uring_test::i_peer2peer> remote)
        {
            return rpc::shared_ptr<io_uring_test::i_peer2peer>(new peer2peer_endpoint(remote, client_peer_bias));
        }

        rpc::shared_ptr<io_uring_test::i_peer2peer> make_server_peer_endpoint(
            std::shared_ptr<rpc::service>,
            rpc::shared_ptr<io_uring_test::i_peer2peer> remote)
        {
            return rpc::shared_ptr<io_uring_test::i_peer2peer>(new peer2peer_endpoint(remote, server_peer_bias));
        }
    } // namespace

    CORO_TASK(int) test_uring::peer_to_peer_rpc_test(uint32_t iterations)
    {
        CO_RETURN CO_AWAIT end_2_end_setup(
            controller_, child_service_, iterations, &make_client_peer_endpoint, &make_server_peer_endpoint);
    }
} // namespace io_uring_test_enclave
