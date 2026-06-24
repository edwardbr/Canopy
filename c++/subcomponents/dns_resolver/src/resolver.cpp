/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/dns_resolver/resolver.h>

#include <ares.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>

namespace canopy::dns_resolver
{
    resolve_options make_stream_resolve_options(
        bool ipv6,
        std::chrono::milliseconds timeout) noexcept
    {
        resolve_options result;
        result.family = ipv6 ? address_family::ipv6 : address_family::ipv4;
        result.type = socket_type::stream;
        result.timeout = timeout;
        return result;
    }

    bool sockaddr_from_endpoint(
        const endpoint& endpoint,
        sockaddr_storage& storage,
        socklen_t& storage_size) noexcept
    {
        storage = sockaddr_storage{};
        storage_size = 0;

        if (endpoint.family == address_family::ipv6)
        {
            auto* addr = reinterpret_cast<sockaddr_in6*>(&storage);
            addr->sin6_family = AF_INET6;
            addr->sin6_port = htons(endpoint.port);
            std::memcpy(&addr->sin6_addr, endpoint.ipv6.data(), endpoint.ipv6.size());
            storage_size = sizeof(sockaddr_in6);
            return true;
        }

        if (endpoint.family == address_family::ipv4)
        {
            auto* addr = reinterpret_cast<sockaddr_in*>(&storage);
            addr->sin_family = AF_INET;
            addr->sin_port = htons(endpoint.port);
            std::memcpy(&addr->sin_addr, endpoint.ipv4.data(), endpoint.ipv4.size());
            storage_size = sizeof(sockaddr_in);
            return true;
        }

        return false;
    }

    namespace
    {
        class cares_library
        {
        public:
            static int initialise() noexcept
            {
                static cares_library instance;
                return instance.status_;
            }

        private:
            cares_library() noexcept
                : status_(ares_library_init(ARES_LIB_INIT_ALL))
            {
            }

            int status_{ARES_SUCCESS};
        };

        class channel
        {
        public:
            explicit channel(const resolve_options& options)
            {
                const auto library_status = cares_library::initialise();
                if (library_status != ARES_SUCCESS)
                {
                    status_ = library_status;
                    return;
                }

                ares_options ares_options{};
                int optmask = 0;

                if (options.timeout > std::chrono::milliseconds{0})
                {
                    const auto timeout_ms = clamp_to_int(options.timeout.count());
                    ares_options.timeout = timeout_ms;
                    ares_options.maxtimeout = timeout_ms;
                    optmask |= ARES_OPT_TIMEOUTMS | ARES_OPT_MAXTIMEOUTMS;
                }

                if (options.tries > 0)
                {
                    ares_options.tries = options.tries;
                    optmask |= ARES_OPT_TRIES;
                }

                status_ = ares_init_options(&channel_, &ares_options, optmask);
            }

            ~channel()
            {
                if (channel_)
                    ares_destroy(channel_);
            }

            channel(const channel&) = delete;
            channel& operator=(const channel&) = delete;
            channel(channel&&) = delete;
            channel& operator=(channel&&) = delete;

            [[nodiscard]] int status() const noexcept { return status_; }
            [[nodiscard]] ares_channel_t* get() const noexcept { return channel_; }

        private:
            static int clamp_to_int(std::chrono::milliseconds::rep value) noexcept
            {
                if (value > static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max()))
                    return std::numeric_limits<int>::max();
                if (value < 0)
                    return 0;
                return static_cast<int>(value);
            }

            ares_channel_t* channel_{nullptr};
            int status_{ARES_SUCCESS};
        };

        struct addrinfo_deleter
        {
            void operator()(ares_addrinfo* value) const noexcept
            {
                if (value)
                    ares_freeaddrinfo(value);
            }
        };

        using addrinfo_ptr = std::unique_ptr<ares_addrinfo, addrinfo_deleter>;

        struct query_state
        {
            bool done{false};
            int status{ARES_SUCCESS};
            int timeouts{0};
            addrinfo_ptr addrinfo;
        };

        int native_family(address_family family) noexcept
        {
            switch (family)
            {
            case address_family::any:
                return AF_UNSPEC;
            case address_family::ipv4:
                return AF_INET;
            case address_family::ipv6:
                return AF_INET6;
            }
            return AF_UNSPEC;
        }

        int native_socket_type(socket_type type) noexcept
        {
            switch (type)
            {
            case socket_type::any:
                return 0;
            case socket_type::stream:
                return SOCK_STREAM;
            case socket_type::datagram:
                return SOCK_DGRAM;
            }
            return 0;
        }

        ares_addrinfo_hints make_hints(const resolve_options& options) noexcept
        {
            ares_addrinfo_hints hints{};
            hints.ai_family = native_family(options.family);
            hints.ai_socktype = native_socket_type(options.type);
            hints.ai_protocol = options.protocol;
            return hints;
        }

        int error_code_from_ares(int status) noexcept
        {
            switch (status)
            {
            case ARES_SUCCESS:
                return rpc::error::OK();
            case ARES_ETIMEOUT:
                return rpc::error::CALL_TIMEOUT();
            case ARES_ENODATA:
            case ARES_ENOTFOUND:
            case ARES_EBADNAME:
            case ARES_EBADQUERY:
            case ARES_EBADSTR:
            case ARES_EBADFLAGS:
            case ARES_ESERVICE:
                return rpc::error::INVALID_DATA();
            case ARES_ENOMEM:
                return rpc::error::OUT_OF_MEMORY();
            default:
                return rpc::error::NATIVE_IO_ERROR();
            }
        }

        resolve_result make_error(
            int status,
            std::string_view prefix = {})
        {
            resolve_result result;
            result.error_code = error_code_from_ares(status);
            result.native_error = status;
            if (!prefix.empty())
            {
                result.error_message = std::string(prefix);
                result.error_message += ": ";
            }
            result.error_message += ares_strerror(status);
            return result;
        }

        timeval milliseconds_to_timeval(std::chrono::milliseconds timeout) noexcept
        {
            if (timeout < std::chrono::milliseconds{0})
                timeout = std::chrono::milliseconds{0};

            timeval value{};
            value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
            value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);
            return value;
        }

        void addrinfo_callback(
            void* data,
            int status,
            int timeouts,
            ares_addrinfo* result)
        {
            auto* state = static_cast<query_state*>(data);
            state->done = true;
            state->status = status;
            state->timeouts = timeouts;
            state->addrinfo.reset(result);
        }

        bool append_endpoint_from_node(
            std::vector<endpoint>& endpoints,
            const ares_addrinfo_node& node)
        {
            endpoint output;
            output.native_socket_type = node.ai_socktype;
            output.native_protocol = node.ai_protocol;
            output.ttl_seconds = node.ai_ttl;

            char numeric_host[INET6_ADDRSTRLEN] = {};
            if (node.ai_family == AF_INET)
            {
                if (!node.ai_addr || node.ai_addrlen < static_cast<ares_socklen_t>(sizeof(sockaddr_in)))
                    return false;

                const auto* addr = reinterpret_cast<const sockaddr_in*>(node.ai_addr);
                output.family = address_family::ipv4;
                output.port = ntohs(addr->sin_port);
                std::memcpy(output.ipv4.data(), &addr->sin_addr, output.ipv4.size());
                if (!::inet_ntop(AF_INET, &addr->sin_addr, numeric_host, sizeof(numeric_host)))
                    return false;
            }
            else if (node.ai_family == AF_INET6)
            {
                if (!node.ai_addr || node.ai_addrlen < static_cast<ares_socklen_t>(sizeof(sockaddr_in6)))
                    return false;

                const auto* addr = reinterpret_cast<const sockaddr_in6*>(node.ai_addr);
                output.family = address_family::ipv6;
                output.port = ntohs(addr->sin6_port);
                std::memcpy(output.ipv6.data(), &addr->sin6_addr, output.ipv6.size());
                if (!::inet_ntop(AF_INET6, &addr->sin6_addr, numeric_host, sizeof(numeric_host)))
                    return false;
            }
            else
            {
                return false;
            }

            output.numeric_host = numeric_host;
            endpoints.push_back(std::move(output));
            return true;
        }

        resolve_result materialise_result(query_state& state)
        {
            if (state.status != ARES_SUCCESS)
            {
                auto error = make_error(state.status, "DNS lookup failed");
                error.timeouts = state.timeouts;
                return error;
            }

            resolve_result result;
            result.timeouts = state.timeouts;

            if (state.addrinfo && state.addrinfo->name)
                result.canonical_name = state.addrinfo->name;

            for (auto* node = state.addrinfo ? state.addrinfo->nodes : nullptr; node; node = node->ai_next)
                (void)append_endpoint_from_node(result.endpoints, *node);

            if (result.endpoints.empty())
                return make_error(ARES_ENODATA, "DNS lookup returned no usable address records");

            return result;
        }

        resolve_result make_select_error(int native_error)
        {
            resolve_result result;
            result.error_code = rpc::error::NATIVE_IO_ERROR();
            result.native_error = native_error;
            result.error_message = "DNS lookup select() failed: ";
            result.error_message += std::strerror(native_error);
            return result;
        }

        bool deadline_elapsed(std::chrono::steady_clock::time_point deadline) noexcept
        {
            return deadline != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= deadline;
        }

#ifdef CANOPY_BUILD_COROUTINE
        struct async_state
        {
            std::mutex mutex;
            resolve_result result;
            bool completed{false};
            bool suspended{false};
            bool abandoned{false};
            std::coroutine_handle<> continuation;
        };

        void complete_async_state(
            const std::shared_ptr<async_state>& state,
            resolve_result result)
        {
            std::coroutine_handle<> continuation;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = std::move(result);
                state->completed = true;
                if (state->suspended && !state->abandoned)
                    continuation = state->continuation;
            }

            if (continuation)
                continuation.resume();
        }

        class async_resolve_awaiter
        {
        public:
            async_resolve_awaiter(
                std::string host,
                std::string service,
                resolve_options options)
                : host_(std::move(host))
                , service_(std::move(service))
                , options_(options)
                , state_(std::make_shared<async_state>())
            {
            }

            ~async_resolve_awaiter()
            {
                if (!state_)
                    return;

                std::lock_guard<std::mutex> lock(state_->mutex);
                if (!state_->completed)
                {
                    state_->abandoned = true;
                    state_->continuation = {};
                }
            }

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> handle)
            {
                {
                    std::lock_guard<std::mutex> lock(state_->mutex);
                    state_->continuation = handle;
                    state_->suspended = true;
                }

                try
                {
                    auto state = state_;
                    auto host = host_;
                    auto service = service_;
                    auto options = options_;
                    std::thread([state, host = std::move(host), service = std::move(service), options]
                        { complete_async_state(state, resolve_service_blocking(host, service, options)); })
                        .detach();
                }
                catch (const std::system_error& ex)
                {
                    resolve_result result;
                    result.error_code = rpc::error::NATIVE_IO_ERROR();
                    result.native_error = ex.code().value();
                    result.error_message = "failed to start DNS resolver worker: " + std::string(ex.what());
                    {
                        std::lock_guard<std::mutex> lock(state_->mutex);
                        state_->result = std::move(result);
                        state_->completed = true;
                        state_->suspended = false;
                        state_->continuation = {};
                    }
                    return false;
                }
                catch (const std::bad_alloc&)
                {
                    resolve_result result;
                    result.error_code = rpc::error::OUT_OF_MEMORY();
                    result.error_message = "failed to allocate DNS resolver worker";
                    {
                        std::lock_guard<std::mutex> lock(state_->mutex);
                        state_->result = std::move(result);
                        state_->completed = true;
                        state_->suspended = false;
                        state_->continuation = {};
                    }
                    return false;
                }

                return true;
            }

            resolve_result await_resume()
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                return std::move(state_->result);
            }

        private:
            std::string host_;
            std::string service_;
            resolve_options options_;
            std::shared_ptr<async_state> state_;
        };
#endif
    }

    resolve_result resolve_service_blocking(
        const std::string& host,
        const std::string& service,
        resolve_options options)
    {
        if (host.empty())
            return make_error(ARES_EBADNAME, "DNS lookup host is empty");

        channel resolver(options);
        if (resolver.status() != ARES_SUCCESS)
            return make_error(resolver.status(), "failed to initialise DNS resolver");

        query_state state;
        const auto hints = make_hints(options);
        ares_getaddrinfo(
            resolver.get(), host.c_str(), service.empty() ? nullptr : service.c_str(), &hints, addrinfo_callback, &state);

        const bool use_deadline = options.timeout > std::chrono::milliseconds{0};
        const auto deadline = use_deadline ? std::chrono::steady_clock::now() + options.timeout
                                           : std::chrono::steady_clock::time_point{};

        while (!state.done)
        {
            if (deadline_elapsed(deadline))
            {
                ares_cancel(resolver.get());
                return make_error(ARES_ETIMEOUT, "DNS lookup timed out");
            }

            fd_set read_fds;
            fd_set write_fds;
            FD_ZERO(&read_fds);
            FD_ZERO(&write_fds);

            const auto nfds = ares_fds(resolver.get(), &read_fds, &write_fds);
            if (nfds == 0)
                break;

            timeval max_timeout{};
            timeval* max_timeout_ptr = nullptr;
            if (use_deadline)
            {
                const auto remaining
                    = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                max_timeout = milliseconds_to_timeval(remaining);
                max_timeout_ptr = &max_timeout;
            }

            timeval timeout_value{};
            timeval* timeout_ptr = ares_timeout(resolver.get(), max_timeout_ptr, &timeout_value);
            const int selected = ::select(nfds, &read_fds, &write_fds, nullptr, timeout_ptr);
            if (selected < 0)
            {
                if (errno == EINTR)
                    continue;
                return make_select_error(errno);
            }

            ares_process(resolver.get(), &read_fds, &write_fds);
        }

        if (!state.done)
            return make_error(ARES_ETIMEOUT, "DNS lookup did not complete");

        return materialise_result(state);
    }

    resolve_result resolve_host_blocking(
        const std::string& host,
        uint16_t port,
        resolve_options options)
    {
        return resolve_service_blocking(host, std::to_string(port), options);
    }

    CORO_TASK(resolve_result)
    resolve_service(
        const std::string& host,
        const std::string& service,
        resolve_options options)
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto result = CO_AWAIT async_resolve_awaiter(host, service, options);
        CO_RETURN result;
#else
        CO_RETURN resolve_service_blocking(host, service, options);
#endif
    }

    CORO_TASK(resolve_result)
    resolve_host(
        const std::string& host,
        uint16_t port,
        resolve_options options)
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto result = CO_AWAIT resolve_service(host, std::to_string(port), options);
        CO_RETURN result;
#else
        CO_RETURN resolve_host_blocking(host, port, options);
#endif
    }
}
