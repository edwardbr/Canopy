# Hierarchical Transport Refcount Findings

Date: 2026-04-18

Issue under investigation:

- `hierachical_transport_tests/0.call_host_look_up_local_app_unload_app`
- `hierachical_transport_tests/4.call_host_look_up_local_app_unload_app`
- Trigger site observed in generated SGX stub:
  `build_debug_sgx_sim/generated/src/example/example_stub.cpp:2645`

Current findings:

- The generated stub is only the trigger site. The canonical logic lives in:
  - `c++/rpc/include/rpc/internal/bindings.h`
  - `c++/rpc/include/rpc/internal/service.h`
  - `c++/rpc/src/transport.cpp`
  - `c++/rpc/src/pass_through.cpp`
  - `c++/rpc/src/stub.cpp`
  - `c++/rpc/src/service_proxy.cpp`
- `stub_bind_out_param(...)` selects either:
  - `service::stub_add_ref(...)` for local interfaces
  - `service::remote_add_ref(...)` for remote interfaces
- `service::remote_add_ref(...)` uses `build_destination_route | build_caller_route` for remote out-params and relies on transport/passthrough routing to keep the path valid while downstream zones create their own local proxy/stub state.
- The in-proc repro shows an intermediate zone can lose its route to the remote destination, then later recreate a fallback `service_proxy` using the caller transport instead of the original destination transport. After that, `add_ref` bounces between two zones instead of reaching the destination.
Follow-up findings:

- The passthrough bootstrap case and the long-lived parent-secondary route are separate concerns.
- In the `build_destination_route | build_caller_route` handoff path:
  - the passthrough should still be allowed to die
  - but the direct destination transport may still need to survive after the passthrough is gone
- The latest in-proc repro shows:
  - the destination-side add-ref hold can be made to land on the direct `1 -> 3` transport
  - but `transport::remove_passthrough(...)` still removes the registered
    `destination_zone=3` transport during passthrough self-destruction
  - once that direct route is removed, zone 1 later recreates `destination_zone=3`
    over the caller transport to zone 2
  - from there, a plain destination-side `add_ref` loops between zones 1 and 2 because
    zone 1 no longer has either:
    - the original direct transport to zone 3
    - or the old passthrough for `2 <-> 3`

Patches applied after the first finding:

- Follow-up experiment moved the blanket bookkeeping out of `pass_through` and into
  `transport::add_ref(...)` / `transport::release(...)`:
  - successful destination-side `transport::add_ref(...)` increments
    `proxy_count` for `remote_object_id.as_zone()`
  - successful `transport::release(...)` decrements the same count
  - `build_destination_route | build_caller_route` is still excluded on add-ref

Result of that transport-only experiment:

- It does not fix the in-proc repro.
- The path still reaches:
  - passthrough self-destruction
  - `remove_passthrough: Removing transport! zone=1, adjacent=3, target_zone=3`
  - later fallback recreation of `destination_zone=3` over `adjacent_zone=2`
  - then repeated destination-only `add_ref` looping between zones 1 and 2
- So even with bookkeeping restricted to transport add-ref/release, the registered direct
  `1 -> 3` transport is still being removed too early during passthrough teardown.

Latest narrowing:

- `hierachical_transport_tests/0.call_host_create_local_zone` already shows the first bad
  transition even though the test still passes overall.
- During `call_host_create_local_zone`:
  - zone 1 removes the registered `destination_zone=3` transport during passthrough teardown
  - zone 1 later recreates `destination_zone=3` through `adjacent_zone=2`
  - cleanup already reports `release returned zone not found for object 1`
- `call_host_set_app` is therefore not the source of the corruption. It only preserves the
  interface returned by `create_local_zone`, so later calls walk into the bad route state and
  recurse.

Fix constraints to preserve:

- Prefer a simple lifetime fix centered on transport teardown.
- Do not add blocking coordination across coroutine calls.
- Do not introduce coarse locking or sequencing that interferes with other threads using the
  same transport/service functions.

Current strongest hypothesis:

- The remaining bug is now in teardown, not acquisition:
  - `remove_passthrough(...)` can still remove the registered direct transport entry for
    the outbound destination even though `proxy_count` for that destination is expected to
    keep the direct route alive
  - either the wrong transport instance is being decremented/consulted, or
    passthrough removal is allowed to drop a service transport mapping that should survive
    beyond the passthrough bootstrap

Observed repro notes:

- SGX repro currently also hits enclave startup failure (`sgx_create_enclave ... 8207`) before the reference-count assertion path.
- In-proc repro is still valuable because it reproduces the routing collapse without SGX startup noise.

Current implementation direction:

- Keep the fix centered in `transport::add_ref(...)` / `transport::release(...)`.
- Treat passthrough teardown and stub bookkeeping as separate concerns for now.
- The intended rule is:
  - successful direct transport add-ref acquires a transport-owned hold in `zone_counts_`
  - successful matching direct transport release drops that hold
  - `build_destination_route | build_caller_route` remains excluded because that path is only
    establishing downstream routes, not creating a transport-owned hold in the current zone

Latest validation from the transport-only branch:

- Non-SGX hierarchical slices now pass in `build_debug` for:
  - `hierachical_transport_tests/*.call_host_create_local_zone`
  - `hierachical_transport_tests/*.call_host_look_up_local_app_unload_app`
- Related in-proc remote-zone creation slices also pass:
  - `remote_type_test/*.create_new_zone`
- However, the transport logs still show accounting warnings even in passing runs:
  - `inner_decrement_outbound_proxy_count: Proxy count already zero`
  - `inner_decrement_outbound_proxy_count: No zone count found`

What those warnings mean:

- The transport-only experiment currently has an add/remove asymmetry.
- `transport::add_ref(...)` only increments `proxy_count` when:
  - the call is direct from the current zone (`caller_zone_id == zone_id_`)
  - and `should_track_remote_service_count(...)` decides the add-ref represents a real
    transport-owned hold
- `transport::release(...)` currently decrements on every successful direct release.
- That is too broad.

Observed state transition behind the warnings:

```text
1. A remote out-param/bootstrap add-ref uses:
   build_destination_route | build_caller_route

2. transport::add_ref(...)
   -> intentionally does NOT increment proxy_count
      because this is only a route-building handoff

3. Later, cleanup sends a plain release through service_proxy::sp_release(...)
   -> release_options only says normal/optimistic
   -> it does not preserve the original route-building intent

4. transport::release(...)
   -> currently decrements proxy_count anyway

5. Result
   -> decrement of a transport-owned count that was never acquired
   -> zero-count / missing-zone warnings
```

This is the clearest non-SGX regression risk in the current transport-only implementation.

SGX-specific finding:

- The current SGX-sim failure is not reaching the old recursive route-loss behaviour first.
- The failing path is:

```text
sgx_setup::set_up()
-> service::connect_to_zone(...)
-> child_transport->connect(...)
-> sgx_create_enclave returns 8207
-> connect_to_zone error cleanup calls release_local_stub(...)
-> object_stub::release(...)
-> zone_->get_transport(caller_zone_id)
-> caller_zone_id has subnet 0
-> service::inner_get_transport asserts destination_zone_id.get_subnet()
```

- So the SGX abort is currently a setup-failure cleanup bug:
  - enclave creation fails first
  - cleanup then attempts a transport lookup using an unset caller zone
- This is separate from the transport-owned route lifetime issue above.

Simple state model to preserve going forward:

```text
Direct transport-owned reference:
  add_ref acquires proxy_count
  release drops proxy_count

Route-building handoff:
  add_ref builds downstream routes only
  no proxy_count acquired locally
  matching cleanup must not drop a local transport-owned count

Service proxy lifetime:
  service_proxy create/destroy already has its own transport count pair
  this must remain independent from per-object add_ref/release bookkeeping

Explicit transport-side permutation matrix:

- Dimensions:
  - route mode:
    - `normal`
    - `build_destination_route`
    - `build_caller_route`
    - `build_destination_route | build_caller_route`
  - pointer kind:
    - shared
    - optimistic
  - source shape:
    - ordinary proxy ownership
    - out-param handoff / route-building

- This gives the practical 12-case view the user described:
  - 4 route modes
  - 2 pointer kinds
  - split between ordinary ownership vs out-param handoff

Current transport labeling added in `c++/rpc/src/transport.cpp`:

- `route_mode=normal`
- `route_mode=destination_only`
- `route_mode=caller_only`
- `route_mode=destination_and_caller`

Current bucket interpretation from the instrumentation:

- `normal`
  - bucket = `outbound_proxy_candidate`
  - current code tracks outbound proxy count only when `caller_zone_id == zone_id_`
- `destination_only`
  - bucket = `outbound_proxy_candidate`
  - current code also tracks outbound proxy count only when `caller_zone_id == zone_id_`
- `caller_only`
  - bucket = `inbound_stub_candidate`
  - current code does not track any transport-owned count in `transport::add_ref(...)`
- `destination_and_caller`
  - bucket = `downstream_handoff`
  - current code does not track any transport-owned count in `transport::add_ref(...)`

Observed live cases from the labeled traces:

- `normal` direct local caller:
  - example:
    `transport::add_ref result ... route_mode=normal ... direct_local_caller=true, track_outbound_proxy=true`
  - this is the expected existing `proxy_count` case

- `caller_only`:
  - example:
    `transport::add_ref result ... route_mode=caller_only ... track_outbound_proxy=false`
  - this currently has no dedicated transport count
  - semantically this is the strongest candidate for using `stub_count` if we extend transport bookkeeping

- `destination_only`:
  - example from remote out-param split path:
    `transport::add_ref result ... route_mode=destination_only ... direct_local_caller=true, track_outbound_proxy=true`
  - and also intermediary forwarding cases with:
    `direct_local_caller=false, track_outbound_proxy=false`
  - this is the case most likely to belong to `proxy_count`

- `destination_and_caller`:
  - example:
    `transport::add_ref result ... route_mode=destination_and_caller ... bucket=downstream_handoff ... track_outbound_proxy=false`
  - this matches the intended rule that the intermediate zone should not own the count

Current asymmetry proven by the labeled logs:

- `destination_only` can be split out of a `destination_and_caller` handoff by passthrough forwarding.
- That forwarded destination-side leg may then be tracked in one zone as outbound proxy ownership.

Latest cleanup after reverting the transport-only experiment:

- The extra local `proxy_count` mutation in `transport::add_ref(...)` has been removed again.
- That returns `transport.cpp` to the baseline behavior for add-ref/release:
  - no transport-owned `proxy_count` mutation in `transport::add_ref(...)`
  - no transport-owned `proxy_count` mutation in `transport::release(...)`
- This keeps service-proxy lifetime as the only direct owner of `proxy_count` on the caller side.

What this cleaned state proves:

- `hierachical_transport_tests/0.call_host_create_local_zone` still passes.
- `hierachical_transport_tests/0.call_host_look_up_local_app_unload_app` still fails with the same recursion.
- Baseline worktree at `fe3fe9976ca6ee5b2f193caa6cc25c40fda39850` shows the same failure pattern.
- So the remaining recursive route-loss is not caused by the temporary transport bookkeeping experiment.

Latest concrete failure sequence in the clean baseline-style state:

```text
1. create_local_zone establishes direct zone 1 -> zone 3 transport
2. passthrough 2 <-> 3 is created through zone 1
3. later passthrough self-destruction calls remove_passthrough(...)
4. zone 1 removes its registered destination_zone=3 transport:
   remove_passthrough: Removing transport! zone=1, adjacent=3, target_zone=3
5. a later get_zone_proxy(dest=3, caller_zone=2) call can only see the caller transport
6. service::get_zone_proxy(...) recreates destination zone 3 over adjacent zone 2
7. subsequent destination-only add_ref then loops between zones 1 and 2
```

Most important new interpretation:

- The surviving failure is not "generic passthrough logic is wrong" in isolation.
- It is specifically the out-param reuse / handoff path where an existing proxy is reused and
  cleanup releases one relationship instance while another relationship instance is meant to remain.
- The critical code path to inspect next is:
  - `c++/rpc/include/rpc/internal/bindings.h`
  - `proxy_bind_out_param(...)`
  - `service_proxy::get_or_create_object_proxy(..., RELEASE_IF_NOT_NEW, ...)`

Why that path matters:

- `proxy_bind_out_param(...)` is used when the caller receives an object for a zone different
  from the current service proxy destination.
- In that case it deliberately reuses an existing `object_proxy` when present and then sends
  `sp_release(...)` through `RELEASE_IF_NOT_NEW`.
- That matches the original corner case:
  - a new ownership relationship is being established
  - an old relationship is being released in the same logical operation
  - but the implementation may still be collapsing those two relationship instances into one
    shared service-proxy / transport lifetime fact

Rejected local experiment:

- A follow-up experiment tried adding a per-object local transport hold on
  `object_proxy::add_ref(0 -> 1)` and dropping it after `send_object_release(...)`.
- That did not change the recursive failure and introduced unmatched local decrements.
- That experiment has been reverted.

Current best next step:

- Keep passthrough transport teardown logic unchanged for now.
- Keep `stub.cpp` unchanged.
- Focus on the out-param reuse path so that:
  - acquire of the new relationship stays distinct
  - release of the old relationship does not consume the surviving direct route
- Later plain `release()` still only knows:
  - caller zone
  - optimistic vs shared
- It does not know whether it is unwinding:
  - a normal direct proxy hold
  - a destination-only leg of an out-param handoff
  - a caller-only reverse-route setup

Concrete warning-producing pattern seen in the labeled runs:

```text
1. route_mode=destination_only
   direct_local_caller=true
   track_outbound_proxy=true

2. later object/service_proxy cleanup performs its normal proxy teardown

3. transport::release result ... direct_local_caller=true, decrements_outbound_proxy=true

4. object_proxy / service_proxy destruction also drops the transport-owned proxy count

5. result:
   Proxy count already zero
   No zone count found
```

Interpretation:

- The current transport-only experiment is overlapping with existing service-proxy lifetime accounting.
- So even before solving release metadata, the transport layer must avoid double-owning cases already covered by service-proxy create/destroy.
```
