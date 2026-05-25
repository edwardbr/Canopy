/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <io_uring/host_controller.h>
#include <rpc/rpc.h>
#include <transports/secure_coroutine_module/io_uring_data_conversion.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rpc::sgx::coro::host
{
    namespace detail
    {
        class enclave_io_uring_control
            : public rpc::base<enclave_io_uring_control, rpc::v4::secure_coroutine_module::i_io_uring_control>
        {
        public:
            enclave_io_uring_control(
                std::unique_ptr<rpc::io_uring::host_controller> controller,
                rpc::shared_ptr<rpc::i_noop> encapsulated_interface) noexcept
                : controller_(std::move(controller))
                , encapsulated_interface_(std::move(encapsulated_interface))
            {
            }

            ~enclave_io_uring_control() override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto fd : brokered_tcp_descriptors_)
                    ::close(fd);
                brokered_tcp_descriptors_.clear();
                for (auto fd : brokered_file_descriptors_)
                    ::close(fd);
                brokered_file_descriptors_.clear();
            }

            CORO_TASK(int) transfer_encapsulated_interface(rpc::shared_ptr<rpc::i_noop>& iface) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (encapsulated_interface_)
                {
                    // This object is a bootstrap envelope. Once the enclave has
                    // received the user's real interface, the envelope must stop
                    // owning it so long-running host lifetime is carried by the
                    // normal RPC reference path instead.
                    iface = std::move(encapsulated_interface_);
                }
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(int) wake_iouring() override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!controller_)
                    CO_RETURN rpc::error::RESOURCE_CLOSED();

                CO_RETURN controller_->wake_iouring();
            }

            CORO_TASK(int) get_iouring_data(rpc::v4::secure_coroutine_module::io_uring_data& ring_data) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!controller_)
                    CO_RETURN rpc::error::RESOURCE_CLOSED();

                rpc::io_uring::data native_data;
                auto err = controller_->get_iouring_data(native_data);
                if (err == rpc::error::OK())
                    rpc::v4::secure_coroutine_module::copy_to_wire(native_data, ring_data);
                CO_RETURN err;
            }

            CORO_TASK(int)
            brokered_io_operation(
                rpc::v4::secure_coroutine_module::brokered_io_request request,
                rpc::v4::secure_coroutine_module::brokered_io_result& result) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                result = handle_brokered_io_operation_locked(std::move(request));
                CO_RETURN rpc::error::OK();
            }

        private:
            static constexpr size_t max_brokered_io_payload_size = 16U * 1024U * 1024U;

            static auto native_error_result(int native_error) -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                rpc::v4::secure_coroutine_module::brokered_io_result result;
                result.error_code = rpc::error::NATIVE_IO_ERROR();
                result.native_result = -native_error;
                return result;
            }

            static auto invalid_data_result() -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                rpc::v4::secure_coroutine_module::brokered_io_result result;
                result.error_code = rpc::error::INVALID_DATA();
                return result;
            }

            static auto ok_result(int32_t native_result = 0) -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                rpc::v4::secure_coroutine_module::brokered_io_result result;
                result.error_code = rpc::error::OK();
                result.native_result = native_result;
                return result;
            }

            static int set_descriptor_flags(int fd) noexcept
            {
                auto flags = ::fcntl(fd, F_GETFD, 0);
                if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
                    return errno;

                flags = ::fcntl(fd, F_GETFL, 0);
                if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
                    return errno;

                return 0;
            }

            bool owns_descriptor_locked(uint32_t descriptor) const
            {
                if (descriptor > static_cast<uint32_t>(std::numeric_limits<int>::max()))
                    return false;
                return brokered_tcp_descriptors_.find(static_cast<int>(descriptor)) != brokered_tcp_descriptors_.end();
            }

            bool owns_brokered_file_descriptor_locked(uint32_t descriptor) const
            {
                if (descriptor > static_cast<uint32_t>(std::numeric_limits<int>::max()))
                    return false;
                return brokered_file_descriptors_.find(static_cast<int>(descriptor)) != brokered_file_descriptors_.end();
            }

            void track_descriptor_locked(int fd) { brokered_tcp_descriptors_.insert(fd); }
            void track_brokered_file_descriptor_locked(int fd) { brokered_file_descriptors_.insert(fd); }

            auto create_socket_locked(int family) -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                const auto max_descriptors = controller_ ? controller_->get_options().fixed_file_count : 0U;
                if (max_descriptors != 0 && brokered_tcp_descriptors_.size() >= max_descriptors)
                {
                    return native_error_result(EMFILE);
                }

                int fd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
                if (fd < 0)
                    return native_error_result(errno);

                if (auto err = set_descriptor_flags(fd); err != 0)
                {
                    ::close(fd);
                    return native_error_result(err);
                }

                track_descriptor_locked(fd);
                auto result = ok_result(fd);
                result.descriptor_id = fd;
                return result;
            }

            template<class Operation>
            auto descriptor_operation_locked(
                uint32_t descriptor,
                Operation operation) -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                if (!owns_descriptor_locked(descriptor))
                    return invalid_data_result();

                auto native_result = operation(static_cast<int>(descriptor));
                if (native_result < 0)
                    return native_error_result(errno);
                return ok_result(native_result);
            }

            auto bind_ipv4_locked(
                uint32_t descriptor,
                const std::vector<uint8_t>& address,
                uint16_t port) -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                if (!owns_descriptor_locked(descriptor) || address.size() != 4)
                    return invalid_data_result();

                sockaddr_in socket_address{};
                socket_address.sin_family = AF_INET;
                socket_address.sin_port = htons(port);
                std::memcpy(&socket_address.sin_addr.s_addr, address.data(), address.size());

                auto native_result = ::bind(
                    static_cast<int>(descriptor), reinterpret_cast<const sockaddr*>(&socket_address), sizeof(socket_address));
                if (native_result < 0)
                    return native_error_result(errno);
                return ok_result(native_result);
            }

            auto bind_ipv6_locked(
                uint32_t descriptor,
                const std::vector<uint8_t>& address,
                uint16_t port) -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                if (!owns_descriptor_locked(descriptor) || address.size() != 16)
                    return invalid_data_result();

                sockaddr_in6 socket_address{};
                socket_address.sin6_family = AF_INET6;
                socket_address.sin6_port = htons(port);
                std::memcpy(&socket_address.sin6_addr.s6_addr, address.data(), address.size());

                auto native_result = ::bind(
                    static_cast<int>(descriptor), reinterpret_cast<const sockaddr*>(&socket_address), sizeof(socket_address));
                if (native_result < 0)
                    return native_error_result(errno);
                return ok_result(native_result);
            }

            auto accept_locked(uint32_t descriptor) -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                if (!owns_descriptor_locked(descriptor))
                    return invalid_data_result();

                auto fd = ::accept(static_cast<int>(descriptor), nullptr, nullptr);
                if (fd < 0)
                    return native_error_result(errno);

                if (auto err = set_descriptor_flags(fd); err != 0)
                {
                    ::close(fd);
                    return native_error_result(err);
                }

                track_descriptor_locked(fd);
                auto result = ok_result(fd);
                result.descriptor_id = fd;
                return result;
            }

            auto connect_ipv4_loopback_locked(uint16_t port) -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                auto socket_result = create_socket_locked(AF_INET);
                if (socket_result.error_code != rpc::error::OK())
                    return socket_result;

                sockaddr_in socket_address{};
                socket_address.sin_family = AF_INET;
                socket_address.sin_port = htons(port);
                socket_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

                auto fd = socket_result.descriptor_id;
                auto native_result
                    = ::connect(fd, reinterpret_cast<const sockaddr*>(&socket_address), sizeof(socket_address));
                if (native_result < 0 && errno == EINPROGRESS)
                {
                    pollfd poll_descriptor{};
                    poll_descriptor.fd = fd;
                    poll_descriptor.events = POLLOUT;

                    auto poll_result = ::poll(&poll_descriptor, 1, 5000);
                    if (poll_result <= 0)
                    {
                        brokered_tcp_descriptors_.erase(fd);
                        ::close(fd);
                        return native_error_result(poll_result == 0 ? ETIMEDOUT : errno);
                    }

                    int socket_error = 0;
                    socklen_t socket_error_size = sizeof(socket_error);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) < 0)
                    {
                        brokered_tcp_descriptors_.erase(fd);
                        ::close(fd);
                        return native_error_result(errno);
                    }
                    if (socket_error != 0)
                    {
                        brokered_tcp_descriptors_.erase(fd);
                        ::close(fd);
                        return native_error_result(socket_error);
                    }

                    native_result = 0;
                }
                else if (native_result < 0)
                {
                    brokered_tcp_descriptors_.erase(fd);
                    ::close(fd);
                    return native_error_result(errno);
                }

                socket_result.native_result = native_result;
                return socket_result;
            }

            auto send_locked(const rpc::v4::secure_coroutine_module::brokered_io_request& request)
                -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                if (!owns_descriptor_locked(request.descriptor_id) || request.payload.size() > max_brokered_io_payload_size)
                    return invalid_data_result();

                auto flags = static_cast<int>(request.flags);
#ifdef MSG_NOSIGNAL
                flags |= MSG_NOSIGNAL;
#endif
                auto native_result = ::send(
                    static_cast<int>(request.descriptor_id), request.payload.data(), request.payload.size(), flags);
                if (native_result < 0)
                    return native_error_result(errno);

                auto result = ok_result(static_cast<int32_t>(native_result));
                result.bytes_transferred = static_cast<uint32_t>(native_result);
                return result;
            }

            auto receive_locked(const rpc::v4::secure_coroutine_module::brokered_io_request& request)
                -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                if (!owns_descriptor_locked(request.descriptor_id) || request.value > max_brokered_io_payload_size)
                    return invalid_data_result();

                rpc::v4::secure_coroutine_module::brokered_io_result result;
                result.payload.resize(request.value);
                auto native_result = ::recv(
                    static_cast<int>(request.descriptor_id),
                    result.payload.data(),
                    result.payload.size(),
                    static_cast<int>(request.flags));
                if (native_result < 0)
                    return native_error_result(errno);

                result.error_code = rpc::error::OK();
                result.native_result = static_cast<int32_t>(native_result);
                result.bytes_transferred = static_cast<uint32_t>(native_result);
                result.payload.resize(result.bytes_transferred);
                return result;
            }

            auto file_open_locked(const rpc::v4::secure_coroutine_module::brokered_io_request& request)
                -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                if (request.payload.empty())
                    return invalid_data_result();

                const std::string path(request.payload.begin(), request.payload.end());
                const auto fd = ::open(path.c_str(), static_cast<int>(request.flags), static_cast<mode_t>(request.value));
                if (fd < 0)
                    return native_error_result(errno);

                if (auto err = set_descriptor_flags(fd); err != 0)
                {
                    ::close(fd);
                    return native_error_result(err);
                }

                track_brokered_file_descriptor_locked(fd);
                auto result = ok_result(fd);
                result.descriptor_id = fd;
                return result;
            }

            auto file_read_at_locked(const rpc::v4::secure_coroutine_module::brokered_io_request& request)
                -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                if (!owns_brokered_file_descriptor_locked(request.descriptor_id) || request.value > max_brokered_io_payload_size
                    || request.offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
                    return invalid_data_result();

                rpc::v4::secure_coroutine_module::brokered_io_result result;
                result.payload.resize(request.value);
                auto native_result = ::pread(
                    static_cast<int>(request.descriptor_id),
                    result.payload.data(),
                    result.payload.size(),
                    static_cast<off_t>(request.offset));
                if (native_result < 0)
                    return native_error_result(errno);

                result.error_code = rpc::error::OK();
                result.native_result = static_cast<int32_t>(native_result);
                result.bytes_transferred = static_cast<uint32_t>(native_result);
                result.payload.resize(result.bytes_transferred);
                return result;
            }

            auto file_write_at_locked(const rpc::v4::secure_coroutine_module::brokered_io_request& request)
                -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                if (!owns_brokered_file_descriptor_locked(request.descriptor_id)
                    || request.payload.size() > max_brokered_io_payload_size
                    || request.offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
                    return invalid_data_result();

                auto native_result = ::pwrite(
                    static_cast<int>(request.descriptor_id),
                    request.payload.data(),
                    request.payload.size(),
                    static_cast<off_t>(request.offset));
                if (native_result < 0)
                    return native_error_result(errno);

                auto result = ok_result(static_cast<int32_t>(native_result));
                result.bytes_transferred = static_cast<uint32_t>(native_result);
                return result;
            }

            auto close_descriptor_locked(uint32_t descriptor) -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                const bool is_tcp_descriptor = owns_descriptor_locked(descriptor);
                const bool is_file_descriptor = owns_brokered_file_descriptor_locked(descriptor);
                if (!is_tcp_descriptor && !is_file_descriptor)
                    return invalid_data_result();

                auto fd = static_cast<int>(descriptor);
                if (is_tcp_descriptor)
                    brokered_tcp_descriptors_.erase(fd);
                if (is_file_descriptor)
                    brokered_file_descriptors_.erase(fd);
                auto native_result = ::close(fd);
                if (native_result < 0)
                    return native_error_result(errno);
                return ok_result(native_result);
            }

            auto handle_brokered_io_operation_locked(rpc::v4::secure_coroutine_module::brokered_io_request request)
                -> rpc::v4::secure_coroutine_module::brokered_io_result
            {
                using operation = rpc::v4::secure_coroutine_module::brokered_io_operation;

                switch (request.operation)
                {
                case operation::tcp_create_ipv4_socket:
                    return create_socket_locked(AF_INET);
                case operation::tcp_create_ipv6_socket:
                    return create_socket_locked(AF_INET6);
                case operation::tcp_set_socket_reuse_addr:
                    return descriptor_operation_locked(
                        request.descriptor_id,
                        [](int fd)
                        {
                            const int value = 1;
                            return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
                        });
                case operation::tcp_bind_ipv4_loopback:
                    return bind_ipv4_locked(request.descriptor_id, std::vector<uint8_t>{127, 0, 0, 1}, request.value);
                case operation::tcp_bind_ipv4:
                    return bind_ipv4_locked(request.descriptor_id, request.address, request.value);
                case operation::tcp_bind_ipv6:
                    return bind_ipv6_locked(request.descriptor_id, request.address, request.value);
                case operation::tcp_listen:
                    return descriptor_operation_locked(
                        request.descriptor_id,
                        [backlog = request.value](int fd) { return ::listen(fd, static_cast<int>(backlog)); });
                case operation::tcp_accept:
                    return accept_locked(request.descriptor_id);
                case operation::tcp_connect_ipv4_loopback:
                    return connect_ipv4_loopback_locked(request.value);
                case operation::tcp_set_tcp_no_delay:
                    return descriptor_operation_locked(
                        request.descriptor_id,
                        [](int fd)
                        {
                            const int value = 1;
                            return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
                        });
                case operation::tcp_send:
                    return send_locked(request);
                case operation::tcp_receive:
                case operation::tcp_receive_nonblocking:
                    return receive_locked(request);
                case operation::tcp_cancel:
                    if (!owns_descriptor_locked(request.descriptor_id))
                        return invalid_data_result();
                    return ok_result();
                case operation::tcp_close:
                    return close_descriptor_locked(request.descriptor_id);
                case operation::file_open:
                    return file_open_locked(request);
                case operation::file_read_at:
                    return file_read_at_locked(request);
                case operation::file_write_at:
                    return file_write_at_locked(request);
                }

                return invalid_data_result();
            }

            std::mutex mutex_;
            std::unique_ptr<rpc::io_uring::host_controller> controller_;
            rpc::shared_ptr<rpc::i_noop> encapsulated_interface_;
            std::unordered_set<int> brokered_tcp_descriptors_;
            std::unordered_set<int> brokered_file_descriptors_;
        };
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::service_connect_result<Local>)
    connect_to_enclave_zone(
        const std::shared_ptr<rpc::service>& service,
        const char* name,
        std::shared_ptr<rpc::transport> enclave_transport,
        rpc::shared_ptr<Remote> input_interface,
        rpc::io_uring::host_controller::options controller_options = {})
    {
        rpc::service_connect_result<Local> result{rpc::error::OK(), {}};
        if (!service || !enclave_transport)
        {
            result.error_code = rpc::error::INVALID_DATA();
            CO_RETURN result;
        }

        rpc::shared_ptr<rpc::i_noop> erased_interface;
        if (input_interface)
        {
            erased_interface = CO_AWAIT rpc::dynamic_pointer_cast<rpc::i_noop>(input_interface);
            if (!erased_interface)
            {
                result.error_code = rpc::error::INVALID_CAST();
                CO_RETURN result;
            }
        }

        std::unique_ptr<rpc::io_uring::host_controller> controller;
        auto controller_error
            = rpc::io_uring::host_controller::create(controller, controller_options, service->get_scheduler());
        if (controller_error != rpc::error::OK())
        {
            result.error_code = controller_error;
            CO_RETURN result;
        }

        rpc::shared_ptr<rpc::v4::secure_coroutine_module::i_io_uring_control> control;
        try
        {
            control = rpc::shared_ptr<rpc::v4::secure_coroutine_module::i_io_uring_control>(
                new detail::enclave_io_uring_control(std::move(controller), std::move(erased_interface)));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating enclave io_uring control interface");
            std::terminate();
        }

        CO_RETURN CO_AWAIT service->template connect_to_zone<rpc::v4::secure_coroutine_module::i_io_uring_control, Local>(
            name, std::move(enclave_transport), std::move(control));
    }
}
