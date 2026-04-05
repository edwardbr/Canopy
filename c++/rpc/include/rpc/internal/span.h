/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rpc
{
    // ── Non-templated byte span ────────────────────────────────────────────────
    // Hardcoded to const uint8_t — the canonical read-only byte buffer type used
    // throughout the RPC layer for serialised data.
    struct byte_span
    {
        // ── Construction ──────────────────────────────────────────────────────

        byte_span() noexcept
            : data_(nullptr)
            , size_(0)
        {
        }

        byte_span(
            const uint8_t* data,
            size_t size) noexcept
            : data_(data)
            , size_(size)
        {
        }

        byte_span(
            const uint8_t* begin,
            const uint8_t* end) noexcept
            : data_(begin)
            , size_(static_cast<size_t>(end - begin))
        {
        }

        byte_span(
            const char* data,
            size_t size) noexcept
            : data_(reinterpret_cast<const uint8_t*>(data))
            , size_(size)
        {
        }

        byte_span(
            const char* begin,
            const char* end) noexcept
            : data_(reinterpret_cast<const uint8_t*>(begin))
            , size_(static_cast<size_t>(end - begin))
        {
        }

        byte_span(
            const int8_t* begin,
            const int8_t* end) noexcept
            : data_(reinterpret_cast<const uint8_t*>(begin))
            , size_(static_cast<size_t>(end - begin))
        {
        }

        byte_span(const std::string& s) noexcept
            : data_(reinterpret_cast<const uint8_t*>(s.data()))
            , size_(s.size())
        {
        }

        template<class ByteType>
        byte_span(const std::vector<ByteType>& v) noexcept
            : data_(reinterpret_cast<const uint8_t*>(v.data()))
            , size_(v.size())
        {
            static_assert(sizeof(ByteType) == 1, "ByteType must be a single-byte type");
        }

        template<class ByteType>
        byte_span(std::vector<ByteType>& v) noexcept
            : data_(reinterpret_cast<const uint8_t*>(v.data()))
            , size_(v.size())
        {
            static_assert(sizeof(ByteType) == 1, "ByteType must be a single-byte type");
        }

        template<
            class ByteType,
            size_t N>
        byte_span(
            const std::array<
                ByteType,
                N>& a) noexcept
            : data_(reinterpret_cast<const uint8_t*>(a.data()))
            , size_(N)
        {
            static_assert(sizeof(ByteType) == 1, "ByteType must be a single-byte type");
        }

        template<
            class ByteType,
            size_t N>
        byte_span(
            std::array<
                ByteType,
                N>& a) noexcept
            : data_(reinterpret_cast<const uint8_t*>(a.data()))
            , size_(N)
        {
            static_assert(sizeof(ByteType) == 1, "ByteType must be a single-byte type");
        }

        // ── Accessors ─────────────────────────────────────────────────────────

        [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
        [[nodiscard]] size_t size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        [[nodiscard]] const uint8_t* begin() const noexcept { return data_; }
        [[nodiscard]] const uint8_t* end() const noexcept { return data_ + size_; }

        const uint8_t& operator[](size_t idx) const noexcept { return data_[idx]; }

        // subspan(offset, count): count == size_t(-1) means "to end"
        [[nodiscard]] byte_span subspan(
            size_t offset,
            size_t count = static_cast<size_t>(-1)) const noexcept
        {
            if (offset > size_)
                offset = size_;
            size_t remaining = size_ - offset;
            size_t n = (count == static_cast<size_t>(-1)) ? remaining : (count < remaining ? count : remaining);
            return byte_span(data_ + offset, n);
        }

    private:
        const uint8_t* data_;
        size_t size_;
    };

    // ── Mutable non-templated byte span ───────────────────────────────────────
    // Hardcoded to uint8_t — the canonical read-write byte buffer type.
    // Constructed only from mutable sources; implicitly converts to byte_span
    // (const view) via widening conversion.
    struct mutable_byte_span
    {
        // ── Construction ──────────────────────────────────────────────────────

        mutable_byte_span() noexcept
            : data_(nullptr)
            , size_(0)
        {
        }

        mutable_byte_span(
            uint8_t* data,
            size_t size) noexcept
            : data_(data)
            , size_(size)
        {
        }

        mutable_byte_span(
            uint8_t* begin,
            uint8_t* end) noexcept
            : data_(begin)
            , size_(static_cast<size_t>(end - begin))
        {
        }

        mutable_byte_span(
            char* data,
            size_t size) noexcept
            : data_(reinterpret_cast<uint8_t*>(data))
            , size_(size)
        {
        }

        mutable_byte_span(
            char* begin,
            char* end) noexcept
            : data_(reinterpret_cast<uint8_t*>(begin))
            , size_(static_cast<size_t>(end - begin))
        {
        }

        mutable_byte_span(std::string& s) noexcept
            : data_(reinterpret_cast<uint8_t*>(s.data()))
            , size_(s.size())
        {
        }

        template<class ByteType>
        mutable_byte_span(std::vector<ByteType>& v) noexcept
            : data_(reinterpret_cast<uint8_t*>(v.data()))
            , size_(v.size())
        {
            static_assert(sizeof(ByteType) == 1, "ByteType must be a single-byte type");
        }

        template<
            class ByteType,
            size_t N>
        mutable_byte_span(
            std::array<
                ByteType,
                N>& a) noexcept
            : data_(reinterpret_cast<uint8_t*>(a.data()))
            , size_(N)
        {
            static_assert(sizeof(ByteType) == 1, "ByteType must be a single-byte type");
        }

        // ── Implicit widening to const view ───────────────────────────────────

        operator byte_span() const noexcept { return byte_span(data_, size_); }

        // ── Accessors ─────────────────────────────────────────────────────────

        [[nodiscard]] uint8_t* data() noexcept { return data_; }
        [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
        [[nodiscard]] size_t size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        [[nodiscard]] uint8_t* begin() noexcept { return data_; }
        [[nodiscard]] uint8_t* end() noexcept { return data_ + size_; }
        [[nodiscard]] const uint8_t* begin() const noexcept { return data_; }
        [[nodiscard]] const uint8_t* end() const noexcept { return data_ + size_; }

        uint8_t& operator[](size_t idx) noexcept { return data_[idx]; }
        const uint8_t& operator[](size_t idx) const noexcept { return data_[idx]; }

        // subspan(offset, count): count == size_t(-1) means "to end"
        [[nodiscard]] mutable_byte_span subspan(
            size_t offset,
            size_t count = static_cast<size_t>(-1)) noexcept
        {
            if (offset > size_)
                offset = size_;
            size_t remaining = size_ - offset;
            size_t n = (count == static_cast<size_t>(-1)) ? remaining : (count < remaining ? count : remaining);
            return mutable_byte_span(data_ + offset, n);
        }

        [[nodiscard]] byte_span subspan(
            size_t offset,
            size_t count = static_cast<size_t>(-1)) const noexcept
        {
            if (offset > size_)
                offset = size_;
            size_t remaining = size_ - offset;
            size_t n = (count == static_cast<size_t>(-1)) ? remaining : (count < remaining ? count : remaining);
            return byte_span(data_ + offset, n);
        }

    private:
        uint8_t* data_;
        size_t size_;
    };

} // namespace rpc
