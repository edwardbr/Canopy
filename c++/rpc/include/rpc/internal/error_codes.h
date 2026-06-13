/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/**
 * @file error_codes.h
 * @brief RPC error code definitions for Canopy
 *
 * This file defines all error codes returned by RPC operations. Error codes
 * are represented as integers to allow integration with existing error handling
 * systems (errno, HRESULT, etc.).
 *
 * Error Code Ranges:
 * - OK() returns 0 (success)
 * - MIN() to MAX() define the valid error range
 * - Negative or positive offsets can be configured via set_offset_val()
 *
 * Common Usage:
 * @code
 * int err = CO_AWAIT proxy->some_method();
 * if (err != rpc::error::OK()) {
 *     // Handle error
 *     std::string_view msg = rpc::error::to_string(err);
 * }
 * @endcode
 *
 * See documents/architecture/06-error-handling.md for error handling patterns.
 */

#pragma once

#include <string_view>

namespace rpc
{
    namespace error
    {
        int OK();
        int MIN();
        int OUT_OF_MEMORY(); // service has no more memory
        int NEED_MORE_MEMORY(); // a call needs more memory for its out parameters, specifically for synchronous out parameters from enclave calls
        int SECURITY_ERROR();              // a security specific issue
        int INVALID_DATA();                // invalid data
        int TRANSPORT_ERROR();             // an error with the custom transport has occurred
        int INVALID_METHOD_ID();           // a method call is invalid based on the wrong ordinal
        int INVALID_INTERFACE_ID();        // an interface is not implemented by an object
        int INVALID_CAST();                // unable to cast an interface to another one
        int ZONE_NOT_SUPPORTED();          // zone is not consistent with the proxy
        int ZONE_NOT_INITIALISED();        // zone is not ready
        int ZONE_NOT_FOUND();              // zone not found
        int OBJECT_NOT_FOUND();            // object id is invalid
        int INVALID_VERSION();             // a service proxy does not supports a version of rpc
        int EXCEPTION();                   // an uncaught exception has occurred
        int PROXY_DESERIALISATION_ERROR(); // a proxy is unable to deserialise data from a service
        int STUB_DESERIALISATION_ERROR();  // a stub is unable to deserialise data from a caller
        int INCOMPATIBLE_SERVICE();        // a service proxy is incompatible with the client
        int INCOMPATIBLE_SERIALISATION();  // service proxy does not support this serialisation format try JSON
        int REFERENCE_COUNT_ERROR();       // reference count error
        // int UNABLE_TO_CREATE_SERVICE_PROXY(); // unable to create service proxy
        int SERVICE_PROXY_LOST_CONNECTION(); // channel is no longer available
        int CALL_CANCELLED();                // Service proxy remote call is cancelled
        int OBJECT_GONE();                   // optimistic pointer target was released by the owning service
        int CALL_TIMEOUT(); // an outbound RPC call received no response within the transport timeout window
        int NOT_IMPLEMENTED(); // operation exists in the interface surface but is not implemented on this platform/path yet
        int FRAUDULANT_REQUEST();  // request violates protocol/security sequencing and may be malicious
        int RESOURCE_CLOSED();     // local resource was closed or is no longer accepting work
        int OPERATION_CANCELLED(); // local asynchronous operation was cancelled before completion
        int RESOURCE_EXHAUSTED();  // local resource capacity was exhausted after retry/backpressure handling
        int PROTOCOL_ERROR();      // peer, transport, or kernel-facing protocol state is inconsistent
        int NATIVE_IO_ERROR(); // native I/O operation failed; inspect operation-specific native result when available
        int MAX();             // the biggest value

        bool is_error(int err);                 // any error listed above that is >= MIN() && <= MAX
        bool is_critical(int err);              // any error listed above other than OBJECT_GONE and INVALID_CAST
        bool is_public_control_status(int err); // OK or a built-in RPC error, never an application-domain result
        int sanitise_public_control_status(
            int err,
            std::string_view operation);

        void set_OK_val(int val);
        void set_offset_val(int val);
        void set_offset_val_is_negative(bool val);
        std::string_view to_string(int);
    };
}
