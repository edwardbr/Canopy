/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include <io_uring/host_controller.h>
#include <io_uring/io_uring_handle.h>

namespace rpc::io_uring
{
    class linux_io_uring_handle : public io_uring_handle
    {
    public:
        using options = host_controller::options;

        [[nodiscard]] static int create(
            std::shared_ptr<linux_io_uring_handle>& handle,
            options handle_options = {},
            std::shared_ptr<coro::scheduler> scheduler = {}) noexcept;

        explicit linux_io_uring_handle(std::unique_ptr<host_controller> controller) noexcept;
        ~linux_io_uring_handle() override;

        linux_io_uring_handle(const linux_io_uring_handle&) = delete;
        linux_io_uring_handle& operator=(const linux_io_uring_handle&) = delete;
        linux_io_uring_handle(linux_io_uring_handle&&) = delete;
        linux_io_uring_handle& operator=(linux_io_uring_handle&&) = delete;

        CORO_TASK(int) get_iouring_data(data& ring_data) override;
        CORO_TASK(int)
        notify_submitted(
            data ring_data,
            uint32_t sqe_count) override;

        CORO_TASK(descriptor_result)
        open_file(
            std::string path,
            uint32_t open_flags,
            uint32_t mode) override;
        CORO_TASK(transfer_result)
        read_at(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer,
            uint64_t offset) override;
        CORO_TASK(transfer_result)
        write_at(
            uint32_t descriptor,
            rpc::byte_span buffer,
            uint64_t offset) override;

        CORO_TASK(descriptor_result) create_tcp_ipv4_socket() override;
        CORO_TASK(descriptor_result) create_tcp_ipv6_socket() override;
        CORO_TASK(operation_result) set_socket_reuse_addr(uint32_t descriptor) override;
        CORO_TASK(operation_result)
        bind_tcp_ipv4_loopback(
            uint32_t descriptor,
            uint16_t port) override;
        CORO_TASK(operation_result)
        bind_tcp_ipv4(
            uint32_t descriptor,
            std::array<
                uint8_t,
                4> address,
            uint16_t port) override;
        CORO_TASK(operation_result)
        bind_tcp_ipv6(
            uint32_t descriptor,
            std::array<
                uint8_t,
                16> address,
            uint16_t port) override;
        CORO_TASK(operation_result)
        listen(
            uint32_t descriptor,
            uint32_t backlog) override;
        CORO_TASK(descriptor_result) accept(uint32_t descriptor) override;
        CORO_TASK(descriptor_result)
        connect_tcp_ipv4_loopback(
            uint16_t port,
            std::chrono::milliseconds timeout) override;
        CORO_TASK(descriptor_result)
        connect_tcp_ipv4(
            std::array<
                uint8_t,
                4> address,
            uint16_t port,
            std::chrono::milliseconds timeout) override;
        CORO_TASK(descriptor_result)
        connect_tcp_ipv6(
            std::array<
                uint8_t,
                16> address,
            uint16_t port,
            std::chrono::milliseconds timeout) override;
        CORO_TASK(operation_result) set_tcp_no_delay(uint32_t descriptor) override;
        CORO_TASK(transfer_result)
        send(
            uint32_t descriptor,
            rpc::byte_span buffer,
            uint32_t msg_flags) override;
        CORO_TASK(transfer_result)
        receive(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer,
            uint32_t msg_flags) override;
        CORO_TASK(operation_result) close_direct(uint32_t descriptor) override;

        void close() noexcept override;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] int ring_fd() const noexcept;

    private:
        [[nodiscard]] host_controller* controller_locked() const noexcept;
        [[nodiscard]] bool owns_tcp_descriptor_locked(uint32_t descriptor) const noexcept;
        [[nodiscard]] bool owns_file_descriptor_locked(uint32_t descriptor) const noexcept;
        [[nodiscard]] int max_tcp_descriptors_locked() const noexcept;
        [[nodiscard]] descriptor_result create_socket_locked(int family) noexcept;
        [[nodiscard]] operation_result close_descriptor_locked(uint32_t descriptor) noexcept;

        mutable std::mutex mutex_;
        std::unique_ptr<host_controller> controller_;
        std::unordered_set<int> tcp_descriptors_;
        std::unordered_set<int> file_descriptors_;
    };
} // namespace rpc::io_uring
