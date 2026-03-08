// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// stream.cpp - Default stream method implementations
#include <streaming/stream.h>

namespace streaming
{
    auto stream::write(std::span<const char> buffer) -> coro::task<coro::net::io_status>
    {
        while (!buffer.empty())
        {
            auto [status, remaining] = send(buffer);
            if (status.is_ok())
            {
                buffer = remaining;
            }
            else if (status.try_again())
            {
                co_await poll(coro::poll_op::write, std::chrono::milliseconds{1});
            }
            else
            {
                co_return status;
            }
        }
        co_return coro::net::io_status{coro::net::io_status::kind::ok};
    }

} // namespace streaming
