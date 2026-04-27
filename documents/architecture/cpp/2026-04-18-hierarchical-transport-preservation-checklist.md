# Hierarchical Transport Preservation Checklist

Date: 2026-04-18

Purpose:

- break the current transport/refcount problem into behavior slices
- identify what must keep working while investigating the route-loss and deadlock issues
- provide a short validation checklist for each iteration

## Current Framing

The current work should be judged against behavior, not against one tactical refcount idea.

There are at least three different classes of behavior in play:

- behavior that currently still works and must be preserved
- behavior that is currently broken and should become green
- behavior that has not been reproduced in this session but is suspected to regress, such as occasional deadlock

## Behaviors To Preserve

### 1. Direct Parent Child Zone Creation

Intent:

- parent can create a child zone
- returned interface is valid
- basic release and shutdown complete cleanly

Representative tests:

- `hierachical_transport_tests/0.call_host_create_local_zone`
- `hierachical_transport_tests/0.call_host_create_local_zone_and_run`

Current status in this session:

- passing in `build_debug`

What must remain true:

- direct `parent -> child` route is created
- returned interface can be called
- balanced cleanup removes the child route without recursion

### 2. Empty Host Registry Operations

Intent:

- looking up or unloading an app that is not present should remain harmless

Representative tests:

- `hierachical_transport_tests/0.look_up_app_and_return_with_nothing`
- `hierachical_transport_tests/0.call_host_unload_app_not_there`

Current status in this session:

- passing in `build_debug`

What must remain true:

- no spurious route creation
- no leaked service proxy
- no unnecessary passthrough activity

### 3. Subordinate Zone Creation

Intent:

- a zone can create a subordinate zone below itself
- nested parent/child routing still works

Representative tests:

- `hierachical_transport_tests/0.create_subordinate_zone`
- `hierachical_transport_tests/0.create_subordinate_zone_and_set_in_host`

Current status in this session:

- passing in `build_debug`

What must remain true:

- nested route establishment works through the intermediate zone
- passthrough creation and teardown remain balanced
- existing direct routes are not accidentally rebound through unrelated caller routes

### 4. Remote Zone Creation Through Multi-Hop Routing

Intent:

- a deeper zone can be created through an intermediate hierarchy
- multi-hop add-ref/release works for the successful path

Representative test:

- `remote_type_test/3.create_new_zone`

Current status in this session:

- passing in `build_debug`

What must remain true:

- routed add-ref reaches the correct destination
- routed release tears down the created zone without recursion
- passthrough removal only happens after the last dependent routed reference is gone

## Behaviors Currently Broken

### 5. Host Registry Store Then Release Original Reference

Intent:

- create object in child zone
- store it in host registry using a different ownership relationship
- later release the original caller-held reference
- object remains reachable because the host-held relationship is independent

Representative test:

- `hierachical_transport_tests/0.call_host_look_up_local_app_unload_app`

Current status in this session:

- failing in `build_debug`
- same failure also present in the baseline worktree used for comparison

Observed failure pattern:

1. zone 1 creates direct route to zone 3
2. zone 1 creates passthrough for zone 2 <-> zone 3
3. a later out-param reuse path reuses an existing object proxy
4. passthrough teardown removes registered `destination_zone=3` transport in zone 1
5. later `get_zone_proxy(dest=3, caller_zone=2)` falls back to caller transport
6. zone 1 recreates route to zone 3 through adjacent zone 2
7. destination-only add-ref loops between zones 1 and 2

Behavior that should exist instead:

- the host-held relationship and the original caller-held relationship remain distinct
- releasing the original relationship does not consume the surviving host-held route

## Behaviors To Watch For

### 6. No Fallback Rebinding Through Caller Route

Intent:

- once a direct destination transport exists for a surviving relationship, later lookups should not silently rebind that destination to a caller transport

Why it matters:

- this is the immediate precursor to the observed recursion

Signal of regression:

- `inner_add_transport service zone: 1 destination_zone=3 adjacent_zone=2`
  appearing after the direct `1 -> 3` route has previously existed

### 7. No Deadlock In Nested Add-Ref Release Handoffs

Intent:

- nested hierarchy operations should not deadlock while establishing a new relationship and tearing down an old one

Status in this session:

- occasional deadlock reported by user
- not yet independently reproduced in this session

What to watch:

- long hangs in tests that used to complete quickly
- service or transport methods holding one lock while awaiting or invoking a path that tries to reacquire related routing state

Likely sensitive areas:

- `service::get_zone_proxy(...)`
- `service_proxy::get_or_create_object_proxy(...)`
- `proxy_bind_out_param(...)`
- passthrough self-destruction and transport deregistration

### 8. SGX Failure Cleanup Must Not Assert

Intent:

- enclave startup failure may return an error, but cleanup must not assert on unset caller-zone transport lookups

Representative failing path seen in this session:

- `hierachical_transport_tests/4.call_host_create_local_zone`

Current status:

- still aborts in SGX setup cleanup

This should remain separate from the non-SGX route-loss investigation.

## Current Best Decomposition

The current issue should be worked in this order:

1. preserve all currently green direct and subordinate zone behaviors
2. isolate the out-param reuse handoff path
3. stop direct destination route loss during host-held relationship survival
4. only after that, look for deadlock-specific lock ordering or await-under-lock issues
5. treat SGX cleanup as a separate follow-up bug unless the same ownership fix clearly resolves it

## Short Validation Set For Each Iteration

Minimum non-SGX checks:

- `remote_type_test/3.create_new_zone`
- `hierachical_transport_tests/0.call_host_create_local_zone`
- `hierachical_transport_tests/0.call_host_create_local_zone_and_run`
- `hierachical_transport_tests/0.look_up_app_and_return_with_nothing`
- `hierachical_transport_tests/0.call_host_unload_app_not_there`
- `hierachical_transport_tests/0.create_subordinate_zone`
- `hierachical_transport_tests/0.create_subordinate_zone_and_set_in_host`
- `hierachical_transport_tests/0.call_host_look_up_local_app_unload_app`

Optional focused watchpoints:

- scan logs for direct destination rebinding through caller transport
- scan logs for repeated `inbound_add_ref` loops between the same two zones
- watch for hangs rather than just asserts
