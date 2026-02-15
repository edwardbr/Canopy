// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <rpc/rpc.h>
#include <websocket_demo/websocket_demo.h>
#include "demo.h"

namespace websocket_demo
{
    namespace v1
    {
        class websocket_service : public rpc::service
        {
            rpc::shared_ptr<i_calculator> demo_;

        public:
            websocket_service(std::string name, rpc::zone zone_id, std::shared_ptr<coro::io_scheduler> scheduler);

            virtual ~websocket_service() CANOPY_DEFAULT_DESTRUCTOR;

            rpc::shared_ptr<i_calculator> get_demo_instance();
        };
    }
}
