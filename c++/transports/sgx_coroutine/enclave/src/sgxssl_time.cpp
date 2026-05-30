/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <sgx_error.h>
#include <cstdint>
#include <cstring>
#include <time.h>

namespace rpc::sgx_coroutine_transport::enclave
{
    uint64_t runtime_unix_epoch_milliseconds() noexcept;
}

namespace
{
    // SGXSSL's Linux shim passes a pointer to its internal `struct timeb`.
    // The enclave libc does not expose <sys/timeb.h>, so keep the compatible
    // Linux x86_64 layout here and copy it into the caller-provided enclave buffer.
    struct sgxssl_timeb
    {
        time_t time;
        unsigned short millitm;
        short timezone;
        short dstflag;
    };

    static_assert(sizeof(sgxssl_timeb) == 16);
}

extern "C" sgx_status_t u_sgxssl_ftime(
    void* timeptr,
    uint32_t timeb_len)
{
    if (!timeptr || timeb_len < sizeof(sgxssl_timeb))
        return SGX_ERROR_INVALID_PARAMETER;

    const auto epoch_milliseconds = rpc::sgx_coroutine_transport::enclave::runtime_unix_epoch_milliseconds();

    sgxssl_timeb result{};
    result.time = static_cast<time_t>(epoch_milliseconds / 1000);
    result.millitm = static_cast<unsigned short>(epoch_milliseconds % 1000);

    std::memcpy(timeptr, &result, sizeof(result));
    return SGX_SUCCESS;
}
