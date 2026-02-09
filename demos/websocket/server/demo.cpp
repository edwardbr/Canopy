// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <rpc/rpc.h>

#include "websocket_demo/websocket_demo.h"

namespace websocket_demo
{
    class demo : public rpc::base<demo, v1::i_calculator>
    {
    public:
        ~demo() override = default;

        CORO_TASK(int) add(double first_val, double second_val, double& response) override
        {
            response = first_val + second_val;
            CO_RETURN rpc::error::OK();
        }
        CORO_TASK(int) subtract(double first_val, double second_val, double& response) override
        {
            response = first_val - second_val;
            CO_RETURN rpc::error::OK();
        }
        CORO_TASK(int) multiply(double first_val, double second_val, double& response) override
        {
            response = first_val * second_val;
            CO_RETURN rpc::error::OK();
        }
        CORO_TASK(int) divide(double first_val, double second_val, double& response) override
        {
            response = first_val / second_val;
            CO_RETURN rpc::error::OK();
        }
    };

    rpc::shared_ptr<v1::i_calculator> create_websocket_demo_instance()
    {
        return rpc::make_shared<demo>();
    }
}
