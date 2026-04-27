# Request-Scoped Out-Param Handoff Plan

Date: 2026-04-19

Purpose:

- replace the current transport-lifetime handoff experiment with a request-scoped out-param ownership model
- keep passthrough teardown independent from caller-side out-param binding
- use `object_proxy` lifetime, not transport self-lifetime, to hold the returned remote object alive across stack unwind
- detect hostile or malformed reuse of out-param request identifiers

## Summary

The current staged direction should move away from transport self-ownership and
away from `pending_handoff_transports_`.

The new model is:

- each RPC call that may bind shared or optimistic out params allocates a
  service-local `request_id`
- that `request_id` travels on the RPC send path and on the out-param
  `add_ref`
- when the returned `add_ref` reaches the caller side, the caller creates or
  finds the `object_proxy` and stores it in a request-scoped holding
  container
- that request-scoped `object_proxy` hold keeps the returned remote object
  alive even if the original passthrough is released during stack unwind
- once proxy-side binding is complete, the request-scoped hold is released,
  even if the call completed with an error

This keeps the temporary lifetime in a layer that already has proven
object-reference semantics.

## Required Reverts

Before implementing this plan:

1. Revert the current unstaged `self_ref_` transport-lifetime experiment.
2. Revert any remaining unstaged code that keeps transports alive directly for
   out-param handoff.
3. Keep unrelated SGX path fixes only if they are still independently needed.

## Protocol Changes

### `send_params`

Add:

- `uint64_t request_id`

Rules:

- default is `0`
- nonzero only for RPC calls that may need request-scoped out-param handling
- uniqueness is required only within one `rpc::service`

### `add_ref_params`

Add:

- `uint64_t request_id`

Rules:

- default is `0`
- nonzero only when the `add_ref` is emitted by `stub_bind_out_param(...)`
- ordinary non-out-param `add_ref` traffic must not set this field

## Service State Changes

Replace:

- `std::unordered_map<destination_zone, std::shared_ptr<transport>> pending_handoff_transports_;`

With:

- `std::unordered_map<uint64_t, std::set<std::shared_ptr<object_proxy>>> pending_out_params_;`

Equivalent set-like storage is acceptable if it provides:

- deduplication for repeated references to the same `object_proxy`
- stable strong ownership while the RPC response is unwinding

The key requirement is request-scoped ownership of `object_proxy`, not
transport ownership.

## Ownership Model

### Caller Side Before Send

When the proxy starts a call:

- if the method has no shared or optimistic out params, use `request_id = 0`
- otherwise allocate a unique service-local `request_id`
- create an empty `pending_out_params_[request_id]`
- ensure that entry is removed when proxy-side handling completes, even if the
  call returns an error

### Callee Side Stub Bind

Only `stub_bind_out_param(...)` applies the `request_id` to the out-param
`add_ref`.

Rules:

- if the out param is null, no special `add_ref` is sent
- if the out param is non-null, emit the normal `add_ref` semantics plus the
  `request_id`
- this is the only intended producer of nonzero `add_ref_params::request_id`

### Caller Side On Returned `add_ref`

When the `add_ref` with nonzero `request_id` arrives at the caller zone:

1. Validate that the `request_id` exists in `pending_out_params_`.
2. Find or create the `object_proxy` for the returned `remote_object_id`.
3. If no `service_proxy` exists, create one using the ordinary transport-owned
   lifetime rules.
4. Apply the shared or optimistic reference to the `object_proxy`.
5. Insert the `object_proxy` into `pending_out_params_[request_id]`.

This request-scoped strong hold is what bridges the gap after passthrough
teardown.

### Proxy Bind During Response Unwind

When `proxy_bind_out_param(...)` runs:

- it should find the `object_proxy` already present with the correct counts
- it should create the interface proxy wrapper as it already does
- if a matching interface proxy is already registered for that interface id, it
  should reuse it
- it should not add another transport-lifetime workaround

### Caller Side Cleanup

After proxy-side response handling completes:

- erase `pending_out_params_[request_id]`
- do this even if the call completed with an error
- do not try to undo any remote side effects or remote releases

The remote side is allowed to destroy the passthrough independently. The local
goal is only to keep the call site consistent while the response is bound.

## Fraud / Validation Rules

Add a new critical error:

- `FRAUDULANT_REQUEST()`

Use it when a nonzero `request_id` is not valid for the receiving service.

Examples:

- `add_ref_params::request_id != 0` but there is no matching
  `pending_out_params_` entry
- non-out-param traffic presents a nonzero `request_id`
- the request id is reused after the local proxy has already completed cleanup

This should be treated as a critical protocol violation, not a soft miss.

## Explicitly Out Of Scope

The following remain out of scope for this change:

- special `inout` parameter semantics
- changes to passthrough counting rules
- broad transport refcount redesign
- destructor-based async cleanup

`inout` should be documented separately as future work.

## Design Constraints

1. Async cleanup must not depend on a target object's destructor.
2. Passthrough death is allowed to happen independently.
3. Temporary handoff lifetime should be modeled with `object_proxy`, because
   that lifetime is already well tested.
4. Transport lifetime must remain owned by normal service-proxy / transport
   rules.
5. The implementation must stay safe if multiple threads perform out-param
   calls concurrently.

## Proposed Work Order

1. Revert unstaged transport self-lifetime changes.
2. Add `request_id` to `send_params`.
3. Add `request_id` to `add_ref_params`.
4. Add service-local request-id generation.
5. Replace pending handoff transport state with `pending_out_params_`.
6. Mark outgoing calls that have shared or optimistic out params.
7. Propagate `request_id` only from `stub_bind_out_param(...)`.
8. On caller-side returned `add_ref`, validate and retain the `object_proxy`
   in `pending_out_params_`.
9. Let `proxy_bind_out_param(...)` reuse the prepared `object_proxy`.
10. Erase the request-scoped holding entry on proxy completion, including
    error exits.
11. Add `FRAUDULANT_REQUEST()` handling.
12. Re-run normal, SGX-sim, and coroutine `rpc_test` suites.

## Testing Expectations

The change is not complete until all of the following are clean:

- `build_debug/output/rpc_test`
- `build_debug_sgx_sim/output/rpc_test`
- `build_debug_coroutine/output/rpc_test`

The coroutine suite is especially important because it already exposed that
temporary lifetime tied to object destruction is unsafe for async cleanup.

## Open Note

For request-scoped storage, a set-like container is preferred over a vector if
deduplication of repeated references to the same `object_proxy` is desirable.
The exact container can be chosen during implementation, but the semantics
should remain:

- strong request-scoped ownership
- deduplicated when practical
- fully cleared once proxy-side handling finishes
