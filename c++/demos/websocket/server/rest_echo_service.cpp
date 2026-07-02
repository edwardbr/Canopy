// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "rest_echo_service.h"

#include <utility>

namespace websocket_demo
{
    namespace v1
    {
        namespace
        {
            namespace rest_v1 = websocket_demo::rest::v1;

            // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters): generated IDL uses output refs.
            class echo_service final : public rpc::base<echo_service, rest_v1::i_echo>
            {
            public:
                auto echo(
                    const std::string& message,
                    std::string& response) -> CORO_TASK(error_code) override
                {
                    response = message;
                    CO_RETURN rpc::error::OK();
                }
            };
            // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
        }

        auto make_echo_service() -> rpc::shared_ptr<rest_v1::i_echo>
        {
            return rpc::shared_ptr<rest_v1::i_echo>(new echo_service());
        }
    }
}
