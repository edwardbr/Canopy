#pragma once

/*
 * First draft of the language-neutral C ABI for the non-coroutine
 * dynamic-library transport.
 *
 * This header is intentionally C-only at the boundary so it can be consumed by
 * C++, Rust, and future language FFI code.
 *
 * Notes for implementers:
 *
 * - This is a transport ABI, not the full Canopy runtime API.
 * - All runtime-specific objects must stay behind opaque context handles.
 * - Input buffers are borrowed for the duration of the call unless explicitly
 *   documented otherwise.
 * - Output buffers should be allocated through the provided allocator vtable so
 *   the caller can free them safely.
 * - No exceptions, panics, or coroutine objects may cross this boundary.
 * - Treat the inline comments below as part of the ABI contract, especially
 *   where they describe pointer validity, borrowed lifetimes, and ownership.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(CANOPY_C_ABI_DLL_BUILDING)
#    define CANOPY_C_ABI_EXPORT __declspec(dllexport)
#  else
#    define CANOPY_C_ABI_EXPORT __declspec(dllimport)
#  endif
#else
#  define CANOPY_C_ABI_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct canopy_byte_buffer
    {
        /*
         * Owned mutable output buffer.
         *
         * When returned from a call, ownership transfers to the receiver, which
         * must later release it using the matching allocator/free contract for
         * that call.
         */
        uint8_t* data;
        size_t size;
    } canopy_byte_buffer;

    typedef struct canopy_const_byte_buffer
    {
        /*
         * Borrowed immutable input buffer.
         *
         * The caller must ensure that `data` points to at least `size` readable
         * bytes for the duration of the ABI call that consumes this struct.
         */
        const uint8_t* data;
        size_t size;
    } canopy_const_byte_buffer;

    typedef struct canopy_back_channel_entry
    {
        /*
         * The caller must ensure that `payload` obeys the borrowing rules of
         * `canopy_const_byte_buffer` for the duration of the call.
         */
        uint64_t type_id;
        canopy_const_byte_buffer payload;
    } canopy_back_channel_entry;

    typedef struct canopy_back_channel_span
    {
        /*
         * Borrowed span of back-channel entries.
         *
         * The caller must ensure that `data` points to at least `size` readable
         * entries for the duration of the ABI call that consumes this span.
         */
        const canopy_back_channel_entry* data;
        size_t size;
    } canopy_back_channel_span;

    typedef struct canopy_mut_back_channel_span
    {
        /*
         * Mutable result span written by the callee.
         *
         * After the call returns, the receiver owns the referenced entry storage
         * and must release any owned allocations using the allocator/free contract
         * associated with that call.
         */
        canopy_back_channel_entry* data;
        size_t size;
    } canopy_mut_back_channel_span;

    /*
     * The exact address/object packing should remain aligned with the repository's
     * `rpc_types.idl` and `rpc_types.cpp` semantics. For the ABI, the neutral
     * representation is the packed zone-address blob itself.
     *
     * Implementations should treat this as an opaque protocol-level identity blob
     * and convert it into native runtime types at the boundary.
     */
    typedef struct canopy_zone_address
    {
        /*
         * Borrowed packed `zone_address` blob.
         *
         * The caller must ensure that `blob.data` points to at least `blob.size`
         * readable bytes for the duration of the ABI call that consumes this
         * struct.
         */
        canopy_const_byte_buffer blob;
    } canopy_zone_address;

    typedef struct canopy_zone
    {
        canopy_zone_address address;
    } canopy_zone;

    typedef struct canopy_remote_object
    {
        canopy_zone_address address;
    } canopy_remote_object;

    typedef struct canopy_connection_settings
    {
        /*
         * The caller must ensure that `remote_object_id` obeys the same borrowed
         * packed-address lifetime rules as `canopy_remote_object`.
         */
        uint64_t inbound_interface_id;
        uint64_t outbound_interface_id;
        canopy_remote_object remote_object_id;
    } canopy_connection_settings;

    typedef struct canopy_standard_result
    {
        /* Should match the function return value when both are provided. */
        int32_t error_code;

        /*
         * If non-null/non-empty on return, this references callee-produced output
         * that becomes owned by the caller after the ABI call returns.
         *
         * The callee should allocate this storage with the caller-supplied
         * allocator from the enclosing call.
         */
        canopy_mut_back_channel_span out_back_channel;
    } canopy_standard_result;

    typedef struct canopy_send_result
    {
        int32_t error_code;

        /*
         * If non-null/non-empty on return, this references callee-produced output
         * that becomes owned by the caller after the ABI call returns.
         *
         * The callee should allocate this storage with the caller-supplied
         * allocator from the enclosing call.
         */
        canopy_byte_buffer out_buf;

        /*
         * If non-null/non-empty on return, this references callee-produced output
         * that becomes owned by the caller after the ABI call returns.
         *
         * The callee should allocate this storage with the caller-supplied
         * allocator from the enclosing call.
         */
        canopy_mut_back_channel_span out_back_channel;
    } canopy_send_result;

    typedef struct canopy_new_zone_id_result
    {
        int32_t error_code;

        /*
         * Valid only for the returned result value. Any embedded packed address
         * data obeys the same result-ownership rules as other returned buffers.
         *
         * The callee should allocate any returned packed-address storage with the
         * caller-supplied allocator from the enclosing call.
         */
        canopy_zone zone_id;

        /*
         * If non-null/non-empty on return, this references callee-produced output
         * that becomes owned by the caller after the ABI call returns.
         *
         * The callee should allocate this storage with the caller-supplied
         * allocator from the enclosing call.
         */
        canopy_mut_back_channel_span out_back_channel;
    } canopy_new_zone_id_result;

    typedef struct canopy_send_params
    {
        /*
         * The caller must ensure that every borrowed field in this struct remains
         * valid for the duration of the `send` call, including `caller_zone_id`,
         * `remote_object_id`, `in_data`, and `in_back_channel`.
         */
        uint64_t protocol_version;
        uint64_t encoding_type;
        uint64_t tag;
        canopy_zone caller_zone_id;
        canopy_remote_object remote_object_id;
        uint64_t interface_id;
        uint64_t method_id;
        canopy_const_byte_buffer in_data;
        canopy_back_channel_span in_back_channel;
        uint64_t request_id;
    } canopy_send_params;

    typedef struct canopy_post_params
    {
        /*
         * The caller must ensure that every borrowed field in this struct remains
         * valid for the duration of the `post` call, including `caller_zone_id`,
         * `remote_object_id`, `in_data`, and `in_back_channel`.
         */
        uint64_t protocol_version;
        uint64_t encoding_type;
        uint64_t tag;
        canopy_zone caller_zone_id;
        canopy_remote_object remote_object_id;
        uint64_t interface_id;
        uint64_t method_id;
        canopy_const_byte_buffer in_data;
        canopy_back_channel_span in_back_channel;
    } canopy_post_params;

    typedef struct canopy_try_cast_params
    {
        /*
         * The caller must ensure that every borrowed field in this struct remains
         * valid for the duration of the `try_cast` call.
         */
        uint64_t protocol_version;
        canopy_zone caller_zone_id;
        canopy_remote_object remote_object_id;
        uint64_t interface_id;
        canopy_back_channel_span in_back_channel;
    } canopy_try_cast_params;

    typedef struct canopy_add_ref_params
    {
        /*
         * The caller must ensure that every borrowed field in this struct remains
         * valid for the duration of the `add_ref` call.
         */
        uint64_t protocol_version;
        canopy_remote_object remote_object_id;
        canopy_zone caller_zone_id;
        canopy_zone requesting_zone_id;
        uint8_t build_out_param_channel;
        canopy_back_channel_span in_back_channel;
        uint64_t request_id;
    } canopy_add_ref_params;

    typedef struct canopy_release_params
    {
        /*
         * The caller must ensure that every borrowed field in this struct remains
         * valid for the duration of the `release` call.
         */
        uint64_t protocol_version;
        canopy_remote_object remote_object_id;
        canopy_zone caller_zone_id;
        uint8_t options;
        canopy_back_channel_span in_back_channel;
    } canopy_release_params;

    typedef struct canopy_object_released_params
    {
        /*
         * The caller must ensure that every borrowed field in this struct remains
         * valid for the duration of the `object_released` call.
         */
        uint64_t protocol_version;
        canopy_remote_object remote_object_id;
        canopy_zone caller_zone_id;
        canopy_back_channel_span in_back_channel;
    } canopy_object_released_params;

    typedef struct canopy_transport_down_params
    {
        /*
         * The caller must ensure that every borrowed field in this struct remains
         * valid for the duration of the `transport_down` call.
         */
        uint64_t protocol_version;
        canopy_zone destination_zone_id;
        canopy_zone caller_zone_id;
        canopy_back_channel_span in_back_channel;
    } canopy_transport_down_params;

    typedef struct canopy_get_new_zone_id_params
    {
        /*
         * The caller must ensure that every borrowed field in this struct remains
         * valid for the duration of the `get_new_zone_id` call.
         */
        uint64_t protocol_version;
        canopy_back_channel_span in_back_channel;
    } canopy_get_new_zone_id_params;

    typedef void* canopy_parent_context;
    typedef void* canopy_child_context;

    typedef canopy_byte_buffer (*canopy_alloc_fn)(
        void* allocator_ctx,
        size_t size);
    typedef void (*canopy_free_fn)(
        void* allocator_ctx,
        uint8_t* data,
        size_t size);

    typedef struct canopy_allocator_vtable
    {
        /*
         * The caller supplies the allocator to be used for output buffers produced
         * during the call.
         *
         * The caller must ensure that this vtable remains valid for the duration
         * of any ABI call that uses it, and that `alloc`/`free` obey the same
         * allocation family.
         *
         * The allocator applies recursively to nested outputs as well: payload
         * buffers, returned packed-address blobs, and back-channel entry arrays.
         */
        void* allocator_ctx;
        canopy_alloc_fn alloc;
        canopy_free_fn free;
    } canopy_allocator_vtable;

    typedef int32_t (*canopy_parent_send_fn)(
        canopy_parent_context parent_ctx,
        const canopy_send_params* params,
        canopy_send_result* result);
    typedef void (*canopy_parent_post_fn)(
        canopy_parent_context parent_ctx,
        const canopy_post_params* params);
    typedef int32_t (*canopy_parent_try_cast_fn)(
        canopy_parent_context parent_ctx,
        const canopy_try_cast_params* params,
        canopy_standard_result* result);
    typedef int32_t (*canopy_parent_add_ref_fn)(
        canopy_parent_context parent_ctx,
        const canopy_add_ref_params* params,
        canopy_standard_result* result);
    typedef int32_t (*canopy_parent_release_fn)(
        canopy_parent_context parent_ctx,
        const canopy_release_params* params,
        canopy_standard_result* result);
    typedef void (*canopy_parent_object_released_fn)(
        canopy_parent_context parent_ctx,
        const canopy_object_released_params* params);
    typedef void (*canopy_parent_transport_down_fn)(
        canopy_parent_context parent_ctx,
        const canopy_transport_down_params* params);
    typedef int32_t (*canopy_parent_get_new_zone_id_fn)(
        canopy_parent_context parent_ctx,
        const canopy_get_new_zone_id_params* params,
        canopy_new_zone_id_result* result);

    typedef struct canopy_dll_init_params
    {
        /* Borrowed transport name. */
        const char* name;

        /* Parent and child zone identities. */
        canopy_zone parent_zone;
        canopy_zone child_zone;

        /*
         * Borrowed parent-provided connection settings.
         *
         * The caller must ensure that `input_descr` is either null or points to a
         * valid `canopy_connection_settings` for the duration of `canopy_dll_init`.
         */
        const canopy_connection_settings* input_descr;

        /* Opaque parent runtime context. */
        canopy_parent_context parent_ctx;

        /* Allocator to be used for buffers returned to the host. */
        canopy_allocator_vtable allocator;

        /*
         * Parent callbacks the child transport may invoke.
         *
         * The caller must ensure that these callbacks, together with `parent_ctx`,
         * remain valid for the lifetime of the initialized child transport unless a
         * narrower lifetime rule is documented in future revisions of this ABI.
         */
        canopy_parent_send_fn parent_send;
        canopy_parent_post_fn parent_post;
        canopy_parent_try_cast_fn parent_try_cast;
        canopy_parent_add_ref_fn parent_add_ref;
        canopy_parent_release_fn parent_release;
        canopy_parent_object_released_fn parent_object_released;
        canopy_parent_transport_down_fn parent_transport_down;
        canopy_parent_get_new_zone_id_fn parent_get_new_zone_id;

        /*
         * Outputs written by canopy_dll_init.
         *
         * On success, `child_ctx` becomes owned by the child implementation and is
         * later returned to the child via `canopy_dll_destroy`.
         *
         * If `output_obj` contains a non-empty packed address on return, that
         * storage becomes owned by the caller and should have been allocated using
         * `allocator`.
         */
        canopy_child_context child_ctx;
        canopy_remote_object output_obj;
    } canopy_dll_init_params;

    typedef int32_t (*canopy_dll_init_fn)(canopy_dll_init_params* params);
    typedef void (*canopy_dll_destroy_fn)(canopy_child_context child_ctx);
    typedef int32_t (*canopy_dll_send_fn)(
        canopy_child_context child_ctx,
        const canopy_send_params* params,
        canopy_send_result* result);
    typedef void (*canopy_dll_post_fn)(
        canopy_child_context child_ctx,
        const canopy_post_params* params);
    typedef int32_t (*canopy_dll_try_cast_fn)(
        canopy_child_context child_ctx,
        const canopy_try_cast_params* params,
        canopy_standard_result* result);
    typedef int32_t (*canopy_dll_add_ref_fn)(
        canopy_child_context child_ctx,
        const canopy_add_ref_params* params,
        canopy_standard_result* result);
    typedef int32_t (*canopy_dll_release_fn)(
        canopy_child_context child_ctx,
        const canopy_release_params* params,
        canopy_standard_result* result);
    typedef void (*canopy_dll_object_released_fn)(
        canopy_child_context child_ctx,
        const canopy_object_released_params* params);
    typedef void (*canopy_dll_transport_down_fn)(
        canopy_child_context child_ctx,
        const canopy_transport_down_params* params);
    typedef int32_t (*canopy_dll_get_new_zone_id_fn)(
        canopy_child_context child_ctx,
        const canopy_get_new_zone_id_params* params,
        canopy_new_zone_id_result* result);

    /*
     * Exported child-side entry points.
     *
     * The parent dynamically resolves these from the shared library.
     * The child implementation owns `child_ctx` and must release it from
     * `canopy_dll_destroy`.
     *
     * For every call below, the caller must ensure that all input pointers point
     * to valid storage for the duration of the call, and the callee must not
     * retain borrowed input pointers after return unless it first copies the data.
     *
     * If a callee begins constructing a nested output result and then encounters
     * an error, it must clean up any allocations already made for that result
     * before returning the error code.
     */
    CANOPY_C_ABI_EXPORT int32_t canopy_dll_init(canopy_dll_init_params* params);
    CANOPY_C_ABI_EXPORT void canopy_dll_destroy(canopy_child_context child_ctx);
    CANOPY_C_ABI_EXPORT int32_t canopy_dll_send(
        canopy_child_context child_ctx,
        const canopy_send_params* params,
        canopy_send_result* result);
    CANOPY_C_ABI_EXPORT void canopy_dll_post(
        canopy_child_context child_ctx,
        const canopy_post_params* params);
    CANOPY_C_ABI_EXPORT int32_t canopy_dll_try_cast(
        canopy_child_context child_ctx,
        const canopy_try_cast_params* params,
        canopy_standard_result* result);
    CANOPY_C_ABI_EXPORT int32_t canopy_dll_add_ref(
        canopy_child_context child_ctx,
        const canopy_add_ref_params* params,
        canopy_standard_result* result);
    CANOPY_C_ABI_EXPORT int32_t canopy_dll_release(
        canopy_child_context child_ctx,
        const canopy_release_params* params,
        canopy_standard_result* result);
    CANOPY_C_ABI_EXPORT void canopy_dll_object_released(
        canopy_child_context child_ctx,
        const canopy_object_released_params* params);
    CANOPY_C_ABI_EXPORT void canopy_dll_transport_down(
        canopy_child_context child_ctx,
        const canopy_transport_down_params* params);
    CANOPY_C_ABI_EXPORT int32_t canopy_dll_get_new_zone_id(
        canopy_child_context child_ctx,
        const canopy_get_new_zone_id_params* params,
        canopy_new_zone_id_result* result);

#ifdef __cplusplus
}
#endif
