/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/linux_io_uring_handle.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <new>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace rpc::io_uring
{
    namespace
    {
        constexpr size_t max_tcp_payload_size = size_t{16U} * 1024U * 1024U;

        auto native_descriptor_error_result(int native_error) noexcept -> descriptor_result
        {
            return descriptor_result{rpc::error::NATIVE_IO_ERROR(), 0, -native_error, 0};
        }

        auto native_operation_error_result(int native_error) noexcept -> operation_result
        {
            return operation_result{rpc::error::NATIVE_IO_ERROR(), -native_error, 0};
        }

        auto native_transfer_error_result(int native_error) noexcept -> transfer_result
        {
            return transfer_result{rpc::error::NATIVE_IO_ERROR(), 0, -native_error, 0};
        }

        auto invalid_descriptor_result() noexcept -> descriptor_result
        {
            return descriptor_result{rpc::error::INVALID_DATA(), 0, 0, 0};
        }

        auto invalid_operation_result() noexcept -> operation_result
        {
            return operation_result{rpc::error::INVALID_DATA(), 0, 0};
        }

        auto invalid_transfer_result() noexcept -> transfer_result
        {
            return transfer_result{rpc::error::INVALID_DATA(), 0, 0, 0};
        }

        int set_descriptor_flags(int fd) noexcept
        {
            auto flags = ::fcntl(fd, F_GETFD, 0);
            if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
                return errno;

            flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
                return errno;

            return 0;
        }

        auto bind_ipv4_descriptor(
            int fd,
            const std::array<
                uint8_t,
                4>& address,
            uint16_t port) noexcept -> operation_result
        {
            sockaddr_in socket_address{};
            socket_address.sin_family = AF_INET;
            socket_address.sin_port = htons(port);
            std::memcpy(&socket_address.sin_addr.s_addr, address.data(), address.size());

            const auto native_result
                = ::bind(fd, reinterpret_cast<const sockaddr*>(&socket_address), sizeof(socket_address));
            if (native_result < 0)
                return native_operation_error_result(errno);
            return operation_result{rpc::error::OK(), native_result, 0};
        }

        auto bind_ipv6_descriptor(
            int fd,
            const std::array<
                uint8_t,
                16>& address,
            uint16_t port) noexcept -> operation_result
        {
            sockaddr_in6 socket_address{};
            socket_address.sin6_family = AF_INET6;
            socket_address.sin6_port = htons(port);
            std::memcpy(&socket_address.sin6_addr.s6_addr, address.data(), address.size());

            const auto native_result
                = ::bind(fd, reinterpret_cast<const sockaddr*>(&socket_address), sizeof(socket_address));
            if (native_result < 0)
                return native_operation_error_result(errno);
            return operation_result{rpc::error::OK(), native_result, 0};
        }

        auto connect_descriptor(
            int fd,
            const sockaddr* address,
            socklen_t address_size,
            std::chrono::milliseconds timeout) noexcept -> descriptor_result
        {
            auto native_result = ::connect(fd, address, address_size);
            if (native_result < 0 && errno == EINPROGRESS)
            {
                pollfd poll_descriptor{};
                poll_descriptor.fd = fd;
                poll_descriptor.events = POLLOUT;
                const int timeout_ms = timeout.count() > 0 ? static_cast<int>(timeout.count()) : -1;

                const auto poll_result = ::poll(&poll_descriptor, 1, timeout_ms);
                if (poll_result <= 0)
                {
                    if (poll_result == 0)
                        return descriptor_result{rpc::error::CALL_TIMEOUT(), 0, -ETIMEDOUT, 0};
                    return native_descriptor_error_result(errno);
                }

                int socket_error = 0;
                socklen_t socket_error_size = sizeof(socket_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) < 0)
                    return native_descriptor_error_result(errno);
                if (socket_error != 0)
                    return native_descriptor_error_result(socket_error);

                native_result = 0;
            }
            else if (native_result < 0)
            {
                return native_descriptor_error_result(errno);
            }

            return descriptor_result{rpc::error::OK(), static_cast<uint32_t>(fd), native_result, 0};
        }
    } // namespace

    int linux_io_uring_handle::create(
        std::shared_ptr<linux_io_uring_handle>& handle,
        options handle_options,
        std::shared_ptr<coro::scheduler> scheduler) noexcept
    {
        handle.reset();

        std::unique_ptr<host_controller> controller;
        auto err = host_controller::create(controller, handle_options, std::move(scheduler));
        if (err != rpc::error::OK())
        {
            return err;
        }

        try
        {
            handle = std::make_shared<linux_io_uring_handle>(std::move(controller));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating linux_io_uring_handle");
            std::terminate();
        }

        return rpc::error::OK();
    }

    linux_io_uring_handle::linux_io_uring_handle(std::unique_ptr<host_controller> controller) noexcept
        : controller_(std::move(controller))
    {
    }

    linux_io_uring_handle::~linux_io_uring_handle()
    {
        close();
    }

    CORO_TASK(int) linux_io_uring_handle::get_iouring_data(data& ring_data)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* controller = controller_locked();
        if (!controller)
            CO_RETURN rpc::error::RESOURCE_CLOSED();

        CO_RETURN controller->get_iouring_data(ring_data);
    }

    CORO_TASK(int)
    linux_io_uring_handle::notify_submitted(
        const data&,
        uint32_t)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* controller = controller_locked();
        if (!controller)
            CO_RETURN rpc::error::RESOURCE_CLOSED();

        // The first direct-ring slice uses SQPOLL when available. If SQPOLL was
        // unavailable during controller setup, wake_iouring() submits pending
        // entries with io_uring_enter().
        CO_RETURN controller->wake_iouring();
    }

    CORO_TASK(descriptor_result)
    linux_io_uring_handle::open_file(
        std::string path,
        uint32_t open_flags,
        uint32_t mode)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path.empty())
            CO_RETURN invalid_descriptor_result();

        const auto fd = ::open(path.c_str(), static_cast<int>(open_flags), static_cast<mode_t>(mode));
        if (fd < 0)
            CO_RETURN native_descriptor_error_result(errno);

        if (auto err = set_descriptor_flags(fd); err != 0)
        {
            ::close(fd);
            CO_RETURN native_descriptor_error_result(err);
        }

        try
        {
            file_descriptors_.insert(fd);
        }
        catch (const std::bad_alloc&)
        {
            ::close(fd);
            CO_RETURN descriptor_result{rpc::error::OUT_OF_MEMORY(), 0, 0, 0};
        }

        CO_RETURN descriptor_result{rpc::error::OK(), static_cast<uint32_t>(fd), fd, 0};
    }

    CORO_TASK(transfer_result)
    linux_io_uring_handle::read_at(
        uint32_t descriptor,
        rpc::mutable_byte_span buffer,
        uint64_t offset)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_file_descriptor_locked(descriptor))
            CO_RETURN invalid_transfer_result();
        if (buffer.empty())
            CO_RETURN transfer_result{rpc::error::OK(), 0, 0, 0};
        if (buffer.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())
            || offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
            CO_RETURN invalid_transfer_result();

        const auto native_result
            = ::pread(static_cast<int>(descriptor), buffer.data(), buffer.size(), static_cast<off_t>(offset));
        if (native_result < 0)
            CO_RETURN native_transfer_error_result(errno);

        CO_RETURN transfer_result{
            rpc::error::OK(), static_cast<uint32_t>(native_result), static_cast<int32_t>(native_result), 0};
    }

    CORO_TASK(transfer_result)
    linux_io_uring_handle::write_at(
        uint32_t descriptor,
        rpc::byte_span buffer,
        uint64_t offset)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_file_descriptor_locked(descriptor))
            CO_RETURN invalid_transfer_result();
        if (buffer.empty())
            CO_RETURN transfer_result{rpc::error::OK(), 0, 0, 0};
        if (buffer.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())
            || offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
            CO_RETURN invalid_transfer_result();

        const auto native_result
            = ::pwrite(static_cast<int>(descriptor), buffer.data(), buffer.size(), static_cast<off_t>(offset));
        if (native_result < 0)
            CO_RETURN native_transfer_error_result(errno);

        CO_RETURN transfer_result{
            rpc::error::OK(), static_cast<uint32_t>(native_result), static_cast<int32_t>(native_result), 0};
    }

    CORO_TASK(descriptor_result) linux_io_uring_handle::create_tcp_ipv4_socket()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CO_RETURN create_socket_locked(AF_INET);
    }

    CORO_TASK(descriptor_result) linux_io_uring_handle::create_tcp_ipv6_socket()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CO_RETURN create_socket_locked(AF_INET6);
    }

    CORO_TASK(operation_result) linux_io_uring_handle::set_socket_reuse_addr(uint32_t descriptor)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_tcp_descriptor_locked(descriptor))
            CO_RETURN invalid_operation_result();

        const int value = 1;
        const auto native_result
            = ::setsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
        if (native_result < 0)
            CO_RETURN native_operation_error_result(errno);
        CO_RETURN operation_result{rpc::error::OK(), native_result, 0};
    }

    CORO_TASK(operation_result)
    linux_io_uring_handle::bind_tcp_ipv4_loopback(
        uint32_t descriptor,
        uint16_t port)
    {
        CO_RETURN CO_AWAIT bind_tcp_ipv4(descriptor, {127, 0, 0, 1}, port);
    }

    CORO_TASK(operation_result)
    linux_io_uring_handle::bind_tcp_ipv4(
        uint32_t descriptor,
        const std::array<
            uint8_t,
            4>& address,
        uint16_t port)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_tcp_descriptor_locked(descriptor))
            CO_RETURN invalid_operation_result();

        CO_RETURN bind_ipv4_descriptor(static_cast<int>(descriptor), address, port);
    }

    CORO_TASK(operation_result)
    linux_io_uring_handle::bind_tcp_ipv6(
        uint32_t descriptor,
        const std::array<
            uint8_t,
            16>& address,
        uint16_t port)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_tcp_descriptor_locked(descriptor))
            CO_RETURN invalid_operation_result();

        CO_RETURN bind_ipv6_descriptor(static_cast<int>(descriptor), address, port);
    }

    CORO_TASK(operation_result)
    linux_io_uring_handle::listen(
        uint32_t descriptor,
        uint32_t backlog)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_tcp_descriptor_locked(descriptor) || backlog > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            CO_RETURN invalid_operation_result();

        const auto native_result = ::listen(static_cast<int>(descriptor), static_cast<int>(backlog));
        if (native_result < 0)
            CO_RETURN native_operation_error_result(errno);
        CO_RETURN operation_result{rpc::error::OK(), native_result, 0};
    }

    CORO_TASK(descriptor_result) linux_io_uring_handle::accept(uint32_t descriptor)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_tcp_descriptor_locked(descriptor))
            CO_RETURN invalid_descriptor_result();

        const auto max_descriptors = max_tcp_descriptors_locked();
        if (max_descriptors != 0 && tcp_descriptors_.size() >= static_cast<size_t>(max_descriptors))
            CO_RETURN native_descriptor_error_result(EMFILE);

        auto fd = ::accept(static_cast<int>(descriptor), nullptr, nullptr);
        if (fd < 0)
            CO_RETURN native_descriptor_error_result(errno);

        if (auto err = set_descriptor_flags(fd); err != 0)
        {
            ::close(fd);
            CO_RETURN native_descriptor_error_result(err);
        }

        try
        {
            tcp_descriptors_.insert(fd);
        }
        catch (const std::bad_alloc&)
        {
            ::close(fd);
            CO_RETURN descriptor_result{rpc::error::OUT_OF_MEMORY(), 0, 0, 0};
        }

        CO_RETURN descriptor_result{rpc::error::OK(), static_cast<uint32_t>(fd), fd, 0};
    }

    CORO_TASK(descriptor_result)
    linux_io_uring_handle::connect_tcp_ipv4_loopback(
        uint16_t port,
        std::chrono::milliseconds timeout)
    {
        CO_RETURN CO_AWAIT connect_tcp_ipv4({127, 0, 0, 1}, port, timeout);
    }

    CORO_TASK(descriptor_result)
    linux_io_uring_handle::connect_tcp_ipv4(
        const std::array<
            uint8_t,
            4>& address,
        uint16_t port,
        std::chrono::milliseconds timeout)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto socket_result = create_socket_locked(AF_INET);
        if (socket_result.error_code != rpc::error::OK())
            CO_RETURN socket_result;

        sockaddr_in socket_address{};
        socket_address.sin_family = AF_INET;
        socket_address.sin_port = htons(port);
        std::memcpy(&socket_address.sin_addr.s_addr, address.data(), address.size());

        const auto fd = static_cast<int>(socket_result.descriptor);
        auto connect_result
            = connect_descriptor(fd, reinterpret_cast<const sockaddr*>(&socket_address), sizeof(socket_address), timeout);
        if (connect_result.error_code != rpc::error::OK())
        {
            (void)close_descriptor_locked(static_cast<uint32_t>(fd));
            CO_RETURN connect_result;
        }

        CO_RETURN connect_result;
    }

    CORO_TASK(descriptor_result)
    linux_io_uring_handle::connect_tcp_ipv6(
        const std::array<
            uint8_t,
            16>& address,
        uint16_t port,
        std::chrono::milliseconds timeout)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto socket_result = create_socket_locked(AF_INET6);
        if (socket_result.error_code != rpc::error::OK())
            CO_RETURN socket_result;

        sockaddr_in6 socket_address{};
        socket_address.sin6_family = AF_INET6;
        socket_address.sin6_port = htons(port);
        std::memcpy(&socket_address.sin6_addr.s6_addr, address.data(), address.size());

        const auto fd = static_cast<int>(socket_result.descriptor);
        auto connect_result
            = connect_descriptor(fd, reinterpret_cast<const sockaddr*>(&socket_address), sizeof(socket_address), timeout);
        if (connect_result.error_code != rpc::error::OK())
        {
            (void)close_descriptor_locked(static_cast<uint32_t>(fd));
            CO_RETURN connect_result;
        }

        CO_RETURN connect_result;
    }

    CORO_TASK(operation_result) linux_io_uring_handle::set_tcp_no_delay(uint32_t descriptor)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_tcp_descriptor_locked(descriptor))
            CO_RETURN invalid_operation_result();

        const int value = 1;
        const auto native_result
            = ::setsockopt(static_cast<int>(descriptor), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
        if (native_result < 0)
            CO_RETURN native_operation_error_result(errno);
        CO_RETURN operation_result{rpc::error::OK(), native_result, 0};
    }

    CORO_TASK(transfer_result)
    linux_io_uring_handle::send(
        uint32_t descriptor,
        rpc::byte_span buffer,
        uint32_t msg_flags)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_tcp_descriptor_locked(descriptor) || buffer.size() > max_tcp_payload_size)
            CO_RETURN invalid_transfer_result();

        auto flags = static_cast<int>(msg_flags);
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const auto native_result = ::send(static_cast<int>(descriptor), buffer.data(), buffer.size(), flags);
        if (native_result < 0)
            CO_RETURN native_transfer_error_result(errno);

        CO_RETURN transfer_result{
            rpc::error::OK(), static_cast<uint32_t>(native_result), static_cast<int32_t>(native_result), 0};
    }

    CORO_TASK(transfer_result)
    linux_io_uring_handle::receive(
        uint32_t descriptor,
        rpc::mutable_byte_span buffer,
        uint32_t msg_flags)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_tcp_descriptor_locked(descriptor) || buffer.size() > max_tcp_payload_size)
            CO_RETURN invalid_transfer_result();

        const auto native_result
            = ::recv(static_cast<int>(descriptor), buffer.data(), buffer.size(), static_cast<int>(msg_flags));
        if (native_result < 0)
            CO_RETURN native_transfer_error_result(errno);

        CO_RETURN transfer_result{
            rpc::error::OK(), static_cast<uint32_t>(native_result), static_cast<int32_t>(native_result), 0};
    }

    CORO_TASK(operation_result) linux_io_uring_handle::close_direct(uint32_t descriptor)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CO_RETURN close_descriptor_locked(descriptor);
    }

    void linux_io_uring_handle::close() noexcept
    {
        std::unique_ptr<host_controller> controller;
        std::unordered_set<int> tcp_descriptors;
        std::unordered_set<int> file_descriptors;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tcp_descriptors = std::move(tcp_descriptors_);
            file_descriptors = std::move(file_descriptors_);
            controller = std::move(controller_);
        }

        for (auto fd : tcp_descriptors)
            ::close(fd);
        for (auto fd : file_descriptors)
            ::close(fd);

        if (controller)
            controller->close();
    }

    bool linux_io_uring_handle::is_open() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* controller = controller_locked();
        return controller && controller->is_open();
    }

    int linux_io_uring_handle::ring_fd() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* controller = controller_locked();
        return controller ? controller->ring_fd() : -1;
    }

    host_controller* linux_io_uring_handle::controller_locked() const noexcept
    {
        return controller_.get();
    }

    bool linux_io_uring_handle::owns_tcp_descriptor_locked(uint32_t descriptor) const noexcept
    {
        if (descriptor > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            return false;
        return tcp_descriptors_.find(static_cast<int>(descriptor)) != tcp_descriptors_.end();
    }

    bool linux_io_uring_handle::owns_file_descriptor_locked(uint32_t descriptor) const noexcept
    {
        if (descriptor > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            return false;
        return file_descriptors_.find(static_cast<int>(descriptor)) != file_descriptors_.end();
    }

    int linux_io_uring_handle::max_tcp_descriptors_locked() const noexcept
    {
        const auto* controller = controller_locked();
        if (!controller)
            return 0;

        const auto fixed_file_count = controller->get_options().fixed_file_count;
        if (fixed_file_count > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            return std::numeric_limits<int>::max();
        return static_cast<int>(fixed_file_count);
    }

    descriptor_result linux_io_uring_handle::create_socket_locked(int family) noexcept
    {
        const auto max_descriptors = max_tcp_descriptors_locked();
        if (max_descriptors != 0 && tcp_descriptors_.size() >= static_cast<size_t>(max_descriptors))
            return native_descriptor_error_result(EMFILE);

        const auto fd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0)
            return native_descriptor_error_result(errno);

        if (auto err = set_descriptor_flags(fd); err != 0)
        {
            ::close(fd);
            return native_descriptor_error_result(err);
        }

        try
        {
            tcp_descriptors_.insert(fd);
        }
        catch (const std::bad_alloc&)
        {
            ::close(fd);
            return descriptor_result{rpc::error::OUT_OF_MEMORY(), 0, 0, 0};
        }

        return descriptor_result{rpc::error::OK(), static_cast<uint32_t>(fd), fd, 0};
    }

    operation_result linux_io_uring_handle::close_descriptor_locked(uint32_t descriptor) noexcept
    {
        const bool is_tcp_descriptor = owns_tcp_descriptor_locked(descriptor);
        const bool is_file_descriptor = owns_file_descriptor_locked(descriptor);
        if (!is_tcp_descriptor && !is_file_descriptor)
            return invalid_operation_result();

        const auto fd = static_cast<int>(descriptor);
        if (is_tcp_descriptor)
            tcp_descriptors_.erase(fd);
        if (is_file_descriptor)
            file_descriptors_.erase(fd);
        const auto native_result = ::close(fd);
        if (native_result < 0)
            return native_operation_error_result(errno);
        return operation_result{rpc::error::OK(), native_result, 0};
    }
} // namespace rpc::io_uring
