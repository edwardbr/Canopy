/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>

// Forward declaration
namespace rpc
{
    template<typename T> class shared_ptr;
}

namespace stdex
{
    /**
     * @brief Thread-safe wrapper for std::shared_ptr with internal synchronization
     *
     * member_ptr provides thread-safe access to a shared_ptr by using an internal
     * shared_mutex (reader-writer lock) to protect all operations. This class is
     * designed to be used as a member variable in classes that need thread-safe
     * shared pointer semantics without external synchronization.
     *
     * Thread-Safety Guarantees:
     * - get_nullable(): Multiple threads can read concurrently (shared_lock)
     * - reset(), assignment operators: Exclusive access (unique_lock)
     * - All operations are safe to call from multiple threads simultaneously
     *
     * Design Philosophy:
     * The purpose of member_ptr is to force threads to extract a local copy of
     * the internal shared_ptr via get_nullable(), which prevents race conditions
     * on the member variable itself. Once a local copy is obtained, the reference
     * counting of shared_ptr provides thread-safe lifetime management.
     *
     * Usage Pattern:
     * @code
     * class MyClass {
     *     stdex::member_ptr<Resource> resource_;
     *
     *     void worker_thread() {
     *         // Thread-safe: get local copy
     *         auto local_copy = resource_.get_nullable();
     *         if (local_copy) {
     *             local_copy->do_work(); // Safe to use local copy
     *         }
     *     }
     *
     *     void shutdown_thread() {
     *         // Thread-safe: exclusive access to reset
     *         resource_.reset();
     *     }
     * };
     * @endcode
     */
    template<typename T> class member_ptr
    {
    private:
        std::shared_ptr<T> ptr_;
        mutable std::shared_mutex mutex_; // Protects ptr_ access

    public:
        // Default constructor
        member_ptr() = default;

        // Constructor from shared_ptr
        explicit member_ptr(const std::shared_ptr<T>& ptr)
            : ptr_(ptr)
        {
        }
        explicit member_ptr(std::shared_ptr<T>&& ptr)
            : ptr_(std::move(ptr))
        {
        }

        // Copy constructor - thread-safe
        member_ptr(const member_ptr& other)
        {
            std::shared_lock lock(other.mutex_);
            ptr_ = other.ptr_;
        }

        // Move constructor - assumes other is not being accessed
        member_ptr(member_ptr&& other) noexcept
        {
            std::unique_lock lock(other.mutex_);
            ptr_ = std::move(other.ptr_);
        }

        // Copy assignment operator - thread-safe
        member_ptr& operator=(const member_ptr& other)
        {
            if (this != &other)
            {
                // Lock both mutexes in consistent order to prevent deadlock
                std::scoped_lock lock(mutex_, other.mutex_);
                ptr_ = other.ptr_;
            }
            return *this;
        }

        // Move assignment operator - thread-safe
        member_ptr& operator=(member_ptr&& other) noexcept
        {
            if (this != &other)
            {
                // Lock both mutexes in consistent order to prevent deadlock
                std::scoped_lock lock(mutex_, other.mutex_);
                ptr_ = std::move(other.ptr_);
            }
            return *this;
        }

        // Assignment from shared_ptr - thread-safe (exclusive access)
        member_ptr& operator=(const std::shared_ptr<T>& ptr)
        {
            std::unique_lock lock(mutex_);
            ptr_ = ptr;
            return *this;
        }

        member_ptr& operator=(std::shared_ptr<T>&& ptr)
        {
            std::unique_lock lock(mutex_);
            ptr_ = std::move(ptr);
            return *this;
        }

        /**
         * @brief Get a thread-safe copy of the shared_ptr
         *
         * Returns a local copy of the internal shared_ptr under a shared_lock,
         * allowing multiple threads to call this method concurrently. Once the
         * copy is obtained, it can be safely used without additional locking.
         *
         * @return std::shared_ptr<T> A copy of the managed pointer (may be nullptr)
         */
        std::shared_ptr<T> get_nullable() const
        {
            std::shared_lock lock(mutex_);
            return ptr_;
        }

        /**
         * @brief Reset the pointer to nullptr
         *
         * Acquires exclusive access (unique_lock) to safely reset the pointer.
         * This operation blocks all concurrent reads and other writes.
         */
        void reset()
        {
            std::unique_lock lock(mutex_);
            ptr_.reset();
        }

        // Destructor
        ~member_ptr() = default;
    };
}

namespace rpc
{
    /**
     * @brief Thread-safe wrapper for rpc::shared_ptr with internal synchronization
     *
     * member_ptr provides thread-safe access to an rpc::shared_ptr by using an internal
     * shared_mutex (reader-writer lock) to protect all operations. This class is
     * designed to be used as a member variable in classes that need thread-safe
     * shared pointer semantics without external synchronization.
     *
     * Thread-Safety Guarantees:
     * - get_nullable(): Multiple threads can read concurrently (shared_lock)
     * - reset(), assignment operators: Exclusive access (unique_lock)
     * - All operations are safe to call from multiple threads simultaneously
     *
     * Design Philosophy:
     * The purpose of member_ptr is to force threads to extract a local copy of
     * the internal shared_ptr via get_nullable(), which prevents race conditions
     * on the member variable itself. Once a local copy is obtained, the reference
     * counting of rpc::shared_ptr provides thread-safe lifetime management across
     * RPC zone boundaries.
     *
     * Usage Pattern:
     * @code
     * class tcp_transport {
     *     rpc::member_ptr<tcp_transport> keep_alive_;
     *
     *     CORO_TASK(void) pump_messages() {
     *         while (true) {
     *             // Thread-safe: get local copy
     *             auto keep_alive_copy = keep_alive_.get_nullable();
     *             if (!keep_alive_copy) break;
     *             // Safe to use local copy during loop iteration
     *         }
     *     }
     *
     *     void release_last_reference() {
     *         // Thread-safe: exclusive access to reset
     *         keep_alive_.reset();
     *     }
     * };
     * @endcode
     */
    template<typename T> class member_ptr
    {
    private:
        rpc::shared_ptr<T> ptr_;
        mutable std::shared_mutex mutex_; // Protects ptr_ access

    public:
        // Default constructor
        member_ptr() = default;

        // Constructor from shared_ptr
        explicit member_ptr(const rpc::shared_ptr<T>& ptr)
            : ptr_(ptr)
        {
        }
        explicit member_ptr(rpc::shared_ptr<T>&& ptr)
            : ptr_(std::move(ptr))
        {
        }

        // Copy constructor - thread-safe
        member_ptr(const member_ptr& other)
        {
            std::shared_lock lock(other.mutex_);
            ptr_ = other.ptr_;
        }

        // Move constructor - assumes other is not being accessed
        member_ptr(member_ptr&& other) noexcept
        {
            std::unique_lock lock(other.mutex_);
            ptr_ = std::move(other.ptr_);
        }

        // Copy assignment operator - thread-safe
        member_ptr& operator=(const member_ptr& other)
        {
            if (this != &other)
            {
                // Lock both mutexes in consistent order to prevent deadlock
                std::scoped_lock lock(mutex_, other.mutex_);
                ptr_ = other.ptr_;
            }
            return *this;
        }

        // Move assignment operator - thread-safe
        member_ptr& operator=(member_ptr&& other) noexcept
        {
            if (this != &other)
            {
                // Lock both mutexes in consistent order to prevent deadlock
                std::scoped_lock lock(mutex_, other.mutex_);
                ptr_ = std::move(other.ptr_);
            }
            return *this;
        }

        // Assignment from shared_ptr - thread-safe (exclusive access)
        member_ptr& operator=(const rpc::shared_ptr<T>& ptr)
        {
            std::unique_lock lock(mutex_);
            ptr_ = ptr;
            return *this;
        }

        member_ptr& operator=(rpc::shared_ptr<T>&& ptr)
        {
            std::unique_lock lock(mutex_);
            ptr_ = std::move(ptr);
            return *this;
        }

        /**
         * @brief Get a thread-safe copy of the shared_ptr
         *
         * Returns a local copy of the internal rpc::shared_ptr under a shared_lock,
         * allowing multiple threads to call this method concurrently. Once the
         * copy is obtained, it can be safely used without additional locking,
         * with lifetime management handled across RPC zone boundaries.
         *
         * @return rpc::shared_ptr<T> A copy of the managed pointer (may be nullptr)
         */
        rpc::shared_ptr<T> get_nullable() const
        {
            std::shared_lock lock(mutex_);
            return ptr_;
        }

        /**
         * @brief Reset the pointer to nullptr
         *
         * Acquires exclusive access (unique_lock) to safely reset the pointer.
         * This operation blocks all concurrent reads and other writes.
         */
        void reset()
        {
            std::unique_lock lock(mutex_);
            ptr_.reset();
        }

        // Destructor
        ~member_ptr() = default;
    };
}
