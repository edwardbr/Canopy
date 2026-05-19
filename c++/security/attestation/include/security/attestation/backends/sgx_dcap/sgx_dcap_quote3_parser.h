/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace canopy::security::attestation
{
    inline constexpr size_t sgx_dcap_quote3_report_data_size = 64U;
    inline constexpr size_t sgx_dcap_quote3_report_data_offset = 368U;
    inline constexpr size_t sgx_dcap_quote3_signature_data_len_offset = 432U;
    inline constexpr size_t sgx_dcap_quote3_signature_data_offset = 436U;

    // Extracts the application report_data field from an SGX quote3/DCAP quote.
    //
    // This parser is intentionally small and bounds-checked because it runs
    // before quote authentication. It only locates the fixed report_data field;
    // it does not verify the quote signature, PCK chain, collateral, QvE
    // report, or enclave identity. Those checks remain the verifier's job.
    [[nodiscard]] auto extract_sgx_quote3_report_data(const std::vector<uint8_t>& quote)
        -> std::optional<std::vector<uint8_t>>;
} // namespace canopy::security::attestation
