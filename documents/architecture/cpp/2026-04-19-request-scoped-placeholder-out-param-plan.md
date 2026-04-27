Request-scoped out-param placeholder plan

- Replace object_proxy-based pending-out-param bookkeeping with placeholder interface pointers owned by `rpc::shared_ptr` / `rpc::optimistic_ptr`.
- Keep `request_id` as the outer map key and `remote_object` as the inner map key.
- Build the placeholder at add-ref time using a no-op interface proxy backed by the resolved `object_proxy`, with `do_remote_check = false`.
- On proxy-side bind, look up the placeholder by `request_id + remote_object`, recover the `object_proxy` from the placeholder, and create or reuse the real typed interface proxy with `do_remote_check = false`.
- Remove manual release logic from `finish_out_param_request`; erasing the map should let pointer destruction release the remote hold through normal pointer mechanics.
- Fix generator request-id emission so interface out-params consistently call `begin_out_param_request()` / `finish_out_param_request()`.
- Rebuild generated code and validate with targeted and full `rpc_test` runs in `Debug`, `Debug_Coroutine`, and `Debug_SGX_Sim`.
