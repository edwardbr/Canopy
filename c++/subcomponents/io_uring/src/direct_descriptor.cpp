/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/direct_descriptor.h>

#include <io_uring/controller.h>

#include <cerrno>
#include <utility>

namespace rpc::io_uring
{
    namespace
    {
        bool is_ignorable_cancel_result(int32_t native_result) noexcept
        {
            if (native_result >= 0)
            {
                return true;
            }

            const auto native_error = -native_result;
            return native_error == ENOENT || native_error == EALREADY;
        }

        CORO_TASK(void)
        close_descriptor_task(
            std::shared_ptr<controller> owner,
            uint32_t descriptor)
        {
            if (owner)
            {
                CO_AWAIT owner->close_direct(descriptor);
            }
            CO_RETURN;
        }
    } // namespace

    direct_descriptor::direct_descriptor(
        std::shared_ptr<controller> owner,
        uint32_t descriptor) noexcept
        : controller_(std::move(owner))
        , descriptor_(descriptor)
    {
    }

    direct_descriptor::~direct_descriptor()
    {
        // Destructors cannot co_await. Move the controller into a detached close
        // task when a scheduler is still accepting work so the fixed-file slot
        // is released without blocking object destruction.
        close_detached(std::move(controller_), descriptor_.exchange(invalid_descriptor, std::memory_order_acq_rel));
    }

    direct_descriptor::direct_descriptor(direct_descriptor&& other) noexcept
        : controller_(std::move(other.controller_))
        , descriptor_(other.descriptor_.exchange(
              invalid_descriptor,
              std::memory_order_acq_rel))
    {
    }

    auto direct_descriptor::operator=(direct_descriptor&& other) noexcept -> direct_descriptor&
    {
        if (this == &other)
        {
            return *this;
        }

        close_detached(std::move(controller_), descriptor_.exchange(invalid_descriptor, std::memory_order_acq_rel));

        controller_ = std::move(other.controller_);
        descriptor_.store(
            other.descriptor_.exchange(invalid_descriptor, std::memory_order_acq_rel), std::memory_order_release);
        return *this;
    }

    bool direct_descriptor::is_open() const noexcept
    {
        return get() != invalid_descriptor;
    }

    uint32_t direct_descriptor::get() const noexcept
    {
        return descriptor_.load(std::memory_order_acquire);
    }

    std::shared_ptr<controller> direct_descriptor::get_controller() const noexcept
    {
        return controller_;
    }

    CORO_TASK(int) direct_descriptor::close()
    {
        // Exchange first so concurrent or repeated close attempts collapse to a
        // single kernel close operation.
        auto descriptor = descriptor_.exchange(invalid_descriptor, std::memory_order_acq_rel);
        if (descriptor == invalid_descriptor)
        {
            CO_RETURN rpc::error::OK();
        }

        if (!controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        auto cancel_result = CO_AWAIT controller_->cancel_direct(descriptor);
        if (cancel_result.error_code != rpc::error::OK() && !is_ignorable_cancel_result(cancel_result.native_result))
        {
            CO_RETURN cancel_result.error_code;
        }

        auto result = CO_AWAIT controller_->close_direct(descriptor);
        CO_RETURN result.error_code;
    }

    void direct_descriptor::close_detached(
        std::shared_ptr<controller> owner,
        uint32_t descriptor) noexcept
    {
        if (!owner || descriptor == invalid_descriptor)
        {
            return;
        }

        auto* scheduler = owner->scheduler_;
        if (scheduler && !scheduler->is_shutdown())
        {
            auto task = close_descriptor_task(std::move(owner), descriptor);
            if (scheduler->spawn_detached(std::move(task)))
            {
                return;
            }
            // With a scheduler-backed controller, close_direct may suspend onto
            // that scheduler. A destructor must not block trying to drive it
            // synchronously after the scheduler has rejected detached work.
            return;
        }

        if (!scheduler)
        {
            // The no-scheduler path is the cooperative polling fallback used by
            // non-scheduled controller tests; it does not suspend onto a queue.
            (void)SYNC_WAIT(owner->close_direct(descriptor));
        }
    }
} // namespace rpc::io_uring
