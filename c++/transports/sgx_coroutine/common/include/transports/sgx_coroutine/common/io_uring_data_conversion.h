/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <edl/coroutine_enclave.h>
#include <io_uring/types.h>

namespace rpc::sgx::coro::protocol
{
    static_assert(sizeof(rpc::io_uring::fd_data) == sizeof(io_uring_fd_data));
    static_assert(sizeof(rpc::io_uring::setup_data) == sizeof(io_uring_setup_data));
    static_assert(sizeof(rpc::io_uring::sq_data) == sizeof(io_uring_sq_data));
    static_assert(sizeof(rpc::io_uring::cq_data) == sizeof(io_uring_cq_data));
    static_assert(sizeof(rpc::io_uring::buffer_data) == sizeof(io_uring_buffer_data));
    static_assert(sizeof(rpc::io_uring::fixed_file_data) == sizeof(io_uring_fixed_file_data));
    static_assert(sizeof(rpc::io_uring::data) == sizeof(io_uring_data));

    inline void copy_to_wire(
        const rpc::io_uring::data& input,
        io_uring_data& output) noexcept
    {
        // Keep this as an explicit field copy. The generated IDL type is a
        // wire contract; the native type is a host/enclave controller contract.
        // Padding and object layout must never become the marshalling path.
        output.descriptor_version = input.descriptor_version;

        output.file_descriptors.has_ring_fd = input.file_descriptors.has_ring_fd;
        output.file_descriptors.ring_fd = input.file_descriptors.ring_fd;
        output.file_descriptors.has_enter_ring_fd = input.file_descriptors.has_enter_ring_fd;
        output.file_descriptors.enter_ring_fd = input.file_descriptors.enter_ring_fd;

        output.setup.setup_flags = input.setup.setup_flags;
        output.setup.features = input.setup.features;
        output.setup.sq_thread_idle_ms = input.setup.sq_thread_idle_ms;
        output.setup.sq_entries = input.setup.sq_entries;
        output.setup.cq_entries = input.setup.cq_entries;

        output.submission_queue.sq_ring_mask = input.submission_queue.sq_ring_mask;
        output.submission_queue.sq_ring_entries = input.submission_queue.sq_ring_entries;
        output.submission_queue.sq_off_head = input.submission_queue.sq_off_head;
        output.submission_queue.sq_off_tail = input.submission_queue.sq_off_tail;
        output.submission_queue.sq_off_ring_mask = input.submission_queue.sq_off_ring_mask;
        output.submission_queue.sq_off_ring_entries = input.submission_queue.sq_off_ring_entries;
        output.submission_queue.sq_off_flags = input.submission_queue.sq_off_flags;
        output.submission_queue.sq_off_dropped = input.submission_queue.sq_off_dropped;
        output.submission_queue.sq_off_array = input.submission_queue.sq_off_array;
        output.submission_queue.sq_ring_ptr = input.submission_queue.sq_ring_ptr;
        output.submission_queue.sq_ring_size = input.submission_queue.sq_ring_size;
        output.submission_queue.sq_head_ptr = input.submission_queue.sq_head_ptr;
        output.submission_queue.sq_tail_ptr = input.submission_queue.sq_tail_ptr;
        output.submission_queue.sq_flags_ptr = input.submission_queue.sq_flags_ptr;
        output.submission_queue.sq_dropped_ptr = input.submission_queue.sq_dropped_ptr;
        output.submission_queue.sq_array_ptr = input.submission_queue.sq_array_ptr;
        output.submission_queue.sqes_ptr = input.submission_queue.sqes_ptr;

        output.completion_queue.cq_ring_mask = input.completion_queue.cq_ring_mask;
        output.completion_queue.cq_ring_entries = input.completion_queue.cq_ring_entries;
        output.completion_queue.cq_off_head = input.completion_queue.cq_off_head;
        output.completion_queue.cq_off_tail = input.completion_queue.cq_off_tail;
        output.completion_queue.cq_off_ring_mask = input.completion_queue.cq_off_ring_mask;
        output.completion_queue.cq_off_ring_entries = input.completion_queue.cq_off_ring_entries;
        output.completion_queue.cq_off_overflow = input.completion_queue.cq_off_overflow;
        output.completion_queue.cq_off_cqes = input.completion_queue.cq_off_cqes;
        output.completion_queue.cq_off_flags = input.completion_queue.cq_off_flags;
        output.completion_queue.cq_ring_ptr = input.completion_queue.cq_ring_ptr;
        output.completion_queue.cq_ring_size = input.completion_queue.cq_ring_size;
        output.completion_queue.cq_head_ptr = input.completion_queue.cq_head_ptr;
        output.completion_queue.cq_tail_ptr = input.completion_queue.cq_tail_ptr;
        output.completion_queue.cq_flags_ptr = input.completion_queue.cq_flags_ptr;
        output.completion_queue.cq_overflow_ptr = input.completion_queue.cq_overflow_ptr;
        output.completion_queue.cqes_ptr = input.completion_queue.cqes_ptr;

        output.buffers.buffer_region_ptr = input.buffers.buffer_region_ptr;
        output.buffers.buffer_region_size = input.buffers.buffer_region_size;
        output.buffers.buffer_size = input.buffers.buffer_size;
        output.buffers.buffer_count = input.buffers.buffer_count;
        output.buffers.buffers_registered = input.buffers.buffers_registered;
        output.buffers.registered_buffer_count = input.buffers.registered_buffer_count;

        output.fixed_files.fixed_files_registered = input.fixed_files.fixed_files_registered;
        output.fixed_files.fixed_file_count = input.fixed_files.fixed_file_count;
    }

    inline void copy_to_native(
        const io_uring_data& input,
        rpc::io_uring::data& output) noexcept
    {
        // The reverse path is also explicit so native host/enclave code can
        // change independently from the SGX-private marshalled representation.
        output.descriptor_version = input.descriptor_version;

        output.file_descriptors.has_ring_fd = input.file_descriptors.has_ring_fd;
        output.file_descriptors.ring_fd = input.file_descriptors.ring_fd;
        output.file_descriptors.has_enter_ring_fd = input.file_descriptors.has_enter_ring_fd;
        output.file_descriptors.enter_ring_fd = input.file_descriptors.enter_ring_fd;

        output.setup.setup_flags = input.setup.setup_flags;
        output.setup.features = input.setup.features;
        output.setup.sq_thread_idle_ms = input.setup.sq_thread_idle_ms;
        output.setup.sq_entries = input.setup.sq_entries;
        output.setup.cq_entries = input.setup.cq_entries;

        output.submission_queue.sq_ring_mask = input.submission_queue.sq_ring_mask;
        output.submission_queue.sq_ring_entries = input.submission_queue.sq_ring_entries;
        output.submission_queue.sq_off_head = input.submission_queue.sq_off_head;
        output.submission_queue.sq_off_tail = input.submission_queue.sq_off_tail;
        output.submission_queue.sq_off_ring_mask = input.submission_queue.sq_off_ring_mask;
        output.submission_queue.sq_off_ring_entries = input.submission_queue.sq_off_ring_entries;
        output.submission_queue.sq_off_flags = input.submission_queue.sq_off_flags;
        output.submission_queue.sq_off_dropped = input.submission_queue.sq_off_dropped;
        output.submission_queue.sq_off_array = input.submission_queue.sq_off_array;
        output.submission_queue.sq_ring_ptr = input.submission_queue.sq_ring_ptr;
        output.submission_queue.sq_ring_size = input.submission_queue.sq_ring_size;
        output.submission_queue.sq_head_ptr = input.submission_queue.sq_head_ptr;
        output.submission_queue.sq_tail_ptr = input.submission_queue.sq_tail_ptr;
        output.submission_queue.sq_flags_ptr = input.submission_queue.sq_flags_ptr;
        output.submission_queue.sq_dropped_ptr = input.submission_queue.sq_dropped_ptr;
        output.submission_queue.sq_array_ptr = input.submission_queue.sq_array_ptr;
        output.submission_queue.sqes_ptr = input.submission_queue.sqes_ptr;

        output.completion_queue.cq_ring_mask = input.completion_queue.cq_ring_mask;
        output.completion_queue.cq_ring_entries = input.completion_queue.cq_ring_entries;
        output.completion_queue.cq_off_head = input.completion_queue.cq_off_head;
        output.completion_queue.cq_off_tail = input.completion_queue.cq_off_tail;
        output.completion_queue.cq_off_ring_mask = input.completion_queue.cq_off_ring_mask;
        output.completion_queue.cq_off_ring_entries = input.completion_queue.cq_off_ring_entries;
        output.completion_queue.cq_off_overflow = input.completion_queue.cq_off_overflow;
        output.completion_queue.cq_off_cqes = input.completion_queue.cq_off_cqes;
        output.completion_queue.cq_off_flags = input.completion_queue.cq_off_flags;
        output.completion_queue.cq_ring_ptr = input.completion_queue.cq_ring_ptr;
        output.completion_queue.cq_ring_size = input.completion_queue.cq_ring_size;
        output.completion_queue.cq_head_ptr = input.completion_queue.cq_head_ptr;
        output.completion_queue.cq_tail_ptr = input.completion_queue.cq_tail_ptr;
        output.completion_queue.cq_flags_ptr = input.completion_queue.cq_flags_ptr;
        output.completion_queue.cq_overflow_ptr = input.completion_queue.cq_overflow_ptr;
        output.completion_queue.cqes_ptr = input.completion_queue.cqes_ptr;

        output.buffers.buffer_region_ptr = input.buffers.buffer_region_ptr;
        output.buffers.buffer_region_size = input.buffers.buffer_region_size;
        output.buffers.buffer_size = input.buffers.buffer_size;
        output.buffers.buffer_count = input.buffers.buffer_count;
        output.buffers.buffers_registered = input.buffers.buffers_registered;
        output.buffers.registered_buffer_count = input.buffers.registered_buffer_count;

        output.fixed_files.fixed_files_registered = input.fixed_files.fixed_files_registered;
        output.fixed_files.fixed_file_count = input.fixed_files.fixed_file_count;
    }
} // namespace rpc::sgx::coro::protocol
