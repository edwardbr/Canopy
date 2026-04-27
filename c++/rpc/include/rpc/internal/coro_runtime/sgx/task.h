/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace rpc::coro::sgx
{
    template<typename return_type = void> class task;

    namespace detail
    {
        struct promise_base
        {
            friend struct final_awaitable;

            struct final_awaitable
            {
                auto await_ready() const noexcept -> bool { return false; }

                template<typename promise_type>
                auto await_suspend(std::coroutine_handle<promise_type> coroutine) noexcept -> std::coroutine_handle<>
                {
                    auto& promise = coroutine.promise();
                    if (promise.continuation_)
                        return promise.continuation_;
                    return std::noop_coroutine();
                }

                auto await_resume() noexcept -> void { }
            };

            promise_base() noexcept = default;
            ~promise_base() = default;

            auto initial_suspend() noexcept { return std::suspend_always{}; }
            auto final_suspend() noexcept { return final_awaitable{}; }
            auto continuation(std::coroutine_handle<> continuation) noexcept -> void { continuation_ = continuation; }

        protected:
            std::coroutine_handle<> continuation_{nullptr};
        };

        template<typename return_type> struct promise final : public promise_base
        {
            using task_type = task<return_type>;
            using coroutine_handle = std::coroutine_handle<promise<return_type>>;
            static constexpr bool return_type_is_reference = std::is_reference_v<return_type>;
            using stored_type
                = std::conditional_t<return_type_is_reference, std::remove_reference_t<return_type>*, std::remove_const_t<return_type>>;

            promise() noexcept = default;
            promise(const promise&) = delete;
            promise(promise&& other) = delete;
            auto operator=(const promise&) -> promise& = delete;
            auto operator=(promise&& other) -> promise& = delete;

            ~promise()
            {
                if (state_ == state::value)
                    value_ptr()->~stored_type();
            }

            auto get_return_object() noexcept -> task_type;

            template<typename value_type>
                requires(return_type_is_reference
                            && std::is_constructible_v<
                                return_type,
                                value_type &&>)
                        || (!return_type_is_reference
                            && std::is_constructible_v<
                                stored_type,
                                value_type &&>)
            auto return_value(value_type&& value) -> void
            {
                if constexpr (return_type_is_reference)
                {
                    return_type ref = static_cast<value_type&&>(value);
                    new (raw_) stored_type(std::addressof(ref));
                }
                else
                {
                    new (raw_) stored_type(std::forward<value_type>(value));
                }
                state_ = state::value;
            }

            auto return_value(stored_type&& value) -> void
                requires(!return_type_is_reference)
            {
                if constexpr (std::is_move_constructible_v<stored_type>)
                    new (raw_) stored_type(std::move(value));
                else
                    new (raw_) stored_type(value);
                state_ = state::value;
            }

            auto unhandled_exception() noexcept -> void { state_ = state::exception; }

            auto result() & -> decltype(auto)
            {
                if (state_ == state::value)
                {
                    if constexpr (return_type_is_reference)
                        return static_cast<return_type>(*value_ptr());
                    else
                        return static_cast<const return_type&>(*value_ptr());
                }
                if (state_ == state::exception)
                    throw std::bad_exception{};
                throw std::runtime_error{"The return value was never set, did you execute the coroutine?"};
            }

            auto result() const& -> decltype(auto)
            {
                if (state_ == state::value)
                {
                    if constexpr (return_type_is_reference)
                        return static_cast<std::add_const_t<return_type>>(*value_ptr());
                    else
                        return static_cast<const return_type&>(*value_ptr());
                }
                if (state_ == state::exception)
                    throw std::bad_exception{};
                throw std::runtime_error{"The return value was never set, did you execute the coroutine?"};
            }

            auto result() && -> decltype(auto)
            {
                if (state_ == state::value)
                {
                    if constexpr (return_type_is_reference)
                        return static_cast<return_type>(*value_ptr());
                    else if constexpr (std::is_move_constructible_v<return_type>)
                        return static_cast<return_type&&>(*value_ptr());
                    else
                        return static_cast<const return_type&&>(*value_ptr());
                }
                if (state_ == state::exception)
                    throw std::bad_exception{};
                throw std::runtime_error{"The return value was never set, did you execute the coroutine?"};
            }

        private:
            enum class state : unsigned char
            {
                unset,
                value,
                exception
            };

            state state_{state::unset};
            alignas(stored_type) unsigned char raw_[sizeof(stored_type)]{};

            auto value_ptr() noexcept -> stored_type* { return reinterpret_cast<stored_type*>(raw_); }
            auto value_ptr() const noexcept -> const stored_type* { return reinterpret_cast<const stored_type*>(raw_); }
        };

        template<> struct promise<void> : public promise_base
        {
            using task_type = task<void>;
            using coroutine_handle = std::coroutine_handle<promise<void>>;

            promise() noexcept = default;
            promise(const promise&) = delete;
            promise(promise&& other) = delete;
            auto operator=(const promise&) -> promise& = delete;
            auto operator=(promise&& other) -> promise& = delete;
            ~promise() = default;

            auto get_return_object() noexcept -> task_type;
            auto return_void() noexcept -> void { }
            auto unhandled_exception() noexcept -> void { exception_occurred_ = true; }

            auto result() -> void
            {
                if (exception_occurred_)
                    throw std::bad_exception{};
            }

        private:
            bool exception_occurred_{false};
        };
    } // namespace detail

    template<typename return_type> class [[nodiscard]] task
    {
    public:
        using task_type = task<return_type>;
        using promise_type = detail::promise<return_type>;
        using coroutine_handle = std::coroutine_handle<promise_type>;

        struct awaitable_base
        {
            explicit awaitable_base(coroutine_handle coroutine) noexcept
                : coroutine_(coroutine)
            {
            }

            awaitable_base(const awaitable_base&) = delete;
            awaitable_base(awaitable_base&& other) noexcept
                : coroutine_(
                      std::exchange(
                          other.coroutine_,
                          nullptr))
            {
            }
            auto operator=(const awaitable_base&) -> awaitable_base& = delete;
            auto operator=(awaitable_base&&) -> awaitable_base& = delete;
            ~awaitable_base() = default;

            auto await_ready() const noexcept -> bool { return !coroutine_ || coroutine_.done(); }

            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> std::coroutine_handle<>
            {
                coroutine_.promise().continuation(awaiting_coroutine);
                return coroutine_;
            }

            std::coroutine_handle<promise_type> coroutine_{nullptr};
        };

        task() noexcept = default;
        explicit task(coroutine_handle handle)
            : coroutine_(handle)
        {
        }
        task(const task&) = delete;
        task(task&& other) noexcept
            : coroutine_(
                  std::exchange(
                      other.coroutine_,
                      nullptr))
        {
        }

        ~task()
        {
            if (coroutine_)
            {
                coroutine_.destroy();
                coroutine_ = nullptr;
            }
        }

        auto operator=(const task&) -> task& = delete;

        auto operator=(task&& other) noexcept -> task&
        {
            if (std::addressof(other) != this)
            {
                if (coroutine_)
                    coroutine_.destroy();
                coroutine_ = std::exchange(other.coroutine_, nullptr);
            }
            return *this;
        }

        auto is_ready() const noexcept -> bool { return !coroutine_ || coroutine_.done(); }

        auto resume() -> bool
        {
            if (!coroutine_.done())
                coroutine_.resume();
            return !coroutine_.done();
        }

        auto destroy() -> bool
        {
            if (!coroutine_)
                return false;
            coroutine_.destroy();
            coroutine_ = nullptr;
            return true;
        }

        auto operator co_await() const& noexcept
        {
            struct awaitable : public awaitable_base
            {
                using awaitable_base::awaitable_base;
                auto await_resume() { return this->coroutine_.promise().result(); }
            };

            return awaitable{coroutine_};
        }

        auto operator co_await() && noexcept
        {
            struct awaitable : public awaitable_base
            {
                using awaitable_base::awaitable_base;
                auto await_resume() -> decltype(auto)
                {
                    if constexpr (std::is_void_v<return_type>)
                    {
                        std::move(this->coroutine_.promise()).result();
                    }
                    else if constexpr (std::is_reference_v<return_type>)
                    {
                        return std::move(this->coroutine_.promise()).result();
                    }
                    else
                    {
                        auto value = return_type(std::move(this->coroutine_.promise()).result());
                        return value;
                    }
                }
            };

            return awaitable{coroutine_};
        }

        auto promise() & -> promise_type& { return coroutine_.promise(); }
        auto promise() const& -> const promise_type& { return coroutine_.promise(); }
        auto promise() && -> promise_type&& { return std::move(coroutine_.promise()); }

        auto handle() -> coroutine_handle { return coroutine_; }

    private:
        coroutine_handle coroutine_{nullptr};
    };

    namespace detail
    {
        template<typename return_type>
        inline auto promise<return_type>::get_return_object() noexcept -> task<return_type>
        {
            return task<return_type>{coroutine_handle::from_promise(*this)};
        }

        inline auto promise<void>::get_return_object() noexcept -> task<>
        {
            return task<>{coroutine_handle::from_promise(*this)};
        }
    } // namespace detail
} // namespace rpc::coro::sgx
