/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backends/sgx_dcap/sgx_dcap_quote3_parser.h>

#include <algorithm>
#include <cstring>
#include <limits>

#if defined(CANOPY_ATTESTATION_USE_INTEL_DCAP_QUOTE3_LAYOUT)
#  include <cstddef>
#  include <sgx_quote_3.h>
#endif

namespace canopy::security::attestation
{
    namespace
    {
#if defined(CANOPY_ATTESTATION_USE_INTEL_DCAP_QUOTE3_LAYOUT)
        constexpr size_t intel_report_data_offset
            = offsetof(sgx_quote3_t, report_body) + offsetof(sgx_report_body_t, report_data);
        constexpr size_t intel_signature_data_len_offset = offsetof(sgx_quote3_t, signature_data_len);
        constexpr size_t intel_signature_data_offset = offsetof(sgx_quote3_t, signature_data);

        static_assert(
            intel_report_data_offset == sgx_dcap_quote3_report_data_offset,
            "unexpected SGX quote3 report_data offset");
        static_assert(
            intel_signature_data_len_offset == sgx_dcap_quote3_signature_data_len_offset,
            "unexpected SGX quote3 signature_data_len offset");
        static_assert(
            intel_signature_data_offset == sgx_dcap_quote3_signature_data_offset,
            "unexpected SGX quote3 signature_data offset");
        static_assert(
            sizeof(sgx_report_data_t) == sgx_dcap_quote3_report_data_size,
            "unexpected SGX report_data size");
#endif

        [[nodiscard]] auto read_little_endian_u32(const uint8_t* bytes) noexcept -> uint32_t
        {
            return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U)
                   | (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
        }

        [[nodiscard]] auto signature_data_fits(const std::vector<uint8_t>& quote) noexcept -> bool
        {
            if (quote.size() < sgx_dcap_quote3_signature_data_offset)
                return false;

            const auto signature_data_len
                = read_little_endian_u32(quote.data() + sgx_dcap_quote3_signature_data_len_offset);
            const auto remaining = quote.size() - sgx_dcap_quote3_signature_data_offset;
            return signature_data_len == remaining;
        }
    } // namespace

    auto extract_sgx_quote3_report_data(const std::vector<uint8_t>& quote) -> std::optional<std::vector<uint8_t>>
    {
        if (!signature_data_fits(quote))
            return std::nullopt;

        const auto begin = quote.begin() + static_cast<std::ptrdiff_t>(sgx_dcap_quote3_report_data_offset);
        const auto end = begin + static_cast<std::ptrdiff_t>(sgx_dcap_quote3_report_data_size);
        return std::vector<uint8_t>(begin, end);
    }
} // namespace canopy::security::attestation
