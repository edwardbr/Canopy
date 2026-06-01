<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Implementation Plan: `i_marshaller` Schema Introspection -> WebSocket-Demo MCP Call

Step-by-step plan for the design in `marshaller_schema_api_design.md`. Scoped to
the milestone: the WebSocket demo's browser client discovers `i_calculator`,
fetches its method schemas, and executes `add` via a `yas_json` call.

**Deferred (explicitly out of this plan):** ABI presence-gating of `get_schema`,
hardening the `production` checksum guard, the type-query global registry, and
the MCP JSON-RPC shim. The demo runs against a single build/version, so the
name/id contract is stable enough to prototype without the production guards.

Names below are proposals; all live in namespace `rpc` unless noted.

---

## Guardrail: do not compromise configuration schema generation

This effort is primarily for MCP, but the configuration use of JSON schema
generation (connection-factory topology, `materialise_settings`, struct/enum
authoring schemas) is a first-class feature and must not regress. Invariants that
every phase below is bound by:

1. **`config` is the default flavor, everywhere.** The existing
   `get_schema(rpc::encoding)` accessor (no flavor argument) keeps its current
   behaviour and output. The new `get_schema(encoding, schema_flavor)` is an
   **additive overload** defaulting to `schema_flavor::config`; existing callers
   such as `materialise_settings` are untouched.
2. **MCP-profile transforms never leak into the config path.** String-only enums,
   dropped defaults, inlined refs, and the absence of `$id` are properties of
   `mcp_profile()` only, selected per call. The config schema keeps full
   descriptions, defaults, string|integer enums, `additionalProperties: false`,
   and (when added) `$id`.
3. **The `[introspectable]` attribute gates *runtime discovery only*.** It
   must never gate or remove build-time `.json` / `*_schema.h` emission. Config
   schemas are still generated for every struct/interface regardless of the
   attribute.
4. **Phase 1 additions are purely additive C++.** New statics
   (`__rpc_qualified_name`, `__rpc_methods`, ...) are extra members on generated
   types; they do not alter the emitted config schema documents.
5. **Golden tests pin the config output** at every generator-touching phase (0,
   1, 5a): the full set of generated config `.json` files and the `config`-flavor
   `*_schema.h` strings must stay byte-identical except where a change is
   explicitly intended and reviewed.
6. **The config-authoring track remains a deliverable, not a casualty.** The
   profile work (`config_strict` / `config_overlay`), `$id`, and the
   connection-factory composer in the sibling design docs stay on the roadmap;
   sharing one canonical emitter with MCP is what protects them, not what dilutes
   them.

---

## Guardrail: do not impact standard JSON RPC

Separate from schema *generation*, the existing **JSON-encoded RPC call path**
(`encoding::yas_json` through `send`/`post`, and `try_cast`) must be wholly
unaffected. The two are different layers and must stay so:

1. **The call codec is untouched.** `schema_flavor` and the generator profiles
   concern *schema documents* only. The serialization/deserialization of actual
   method arguments and replies (`to_yas_json`/`from_yas_json`, and likewise the
   binary/nanopb/protobuf encodings) is not changed in any phase.
2. **`get_schema` is purely additive to `i_marshaller`.** It is appended; the
   existing methods' params, results, and message framing for `send`, `post`,
   `try_cast`, `add_ref`, `release`, etc. are unchanged. (The cost of *adding* a
   marshaller message — presence/ABI gating — is the deferred follow-up; it does
   not alter existing message wire shapes.)
3. **No new cost on the normal call hot path.** Schema discovery, the
   `service_proxy` cache, and name->id resolution are opt-in and lazy. A caller
   issuing an ordinary id-based `send` (typed C++ today, or the demo's existing
   path) pays nothing and sees no behavioural change.
4. **The MCP path is additive, not a replacement.** The demo's current protobuf
   calls keep working exactly as now; the `yas_json` MCP call path is a new,
   parallel option, not a migration of the existing one.
5. **Regression coverage.** `rpc_test` / `serialiser_test` and the existing
   transport/demo tests must pass unchanged through every phase; the JSON
   round-trip serializer tests are the canary for codec impact.

---

## Review adjustments & branch state (2026-06-01)

Reconciles these notes with the branch and a design review. Where this section
conflicts with text further down, this section wins.

**Already landed on the branch** (commits `fbcd953`, `be2cc8a`, `23c28c3`,
`6f88d73`): `schema_profile` + `config_strict_profile()` / `mcp_profile()` and the
gated emitter; `rpc::schema_flavor` + `rpc::interface_descriptor` defined in
`rpc_types.idl` (reusing `function_info` by value); the protobuf nested-struct
codegen fix; per-interface `__rpc_qualified_name()` / `__rpc_is_deprecated()`.

**Not yet done:** `$id` threading from `generator/src/main.cpp`; the
`get_schema(encoding, rpc::schema_flavor)` accessor overload; the descriptor
metadata + composer.

1. **`config_overlay_profile()` is dropped, not pending.** It is superseded by the
   accurate-`required` rule (non-optional ∧ no explicit/defaulted value). Do not
   implement a `required`-stripped variant; remove it from the presets.

2. **The composer consumes a static, build-time source of truth.** Runtime lambda
   maps are *not* the composer input. Component descriptors must carry
   **declarative static** metadata (`type`, `role`, `status`, `settings_schema_id`,
   `settings_definition`) that a small **schema-composer executable, run during the
   build**, reads to emit the discriminated schema deterministically. The runtime
   builders and the static descriptors may sit together but the composer only
   touches the static part.

3. **`$id` is one canonical scheme; `settings_schema_id` is the exact emitted
   `$id`.** Define `$id = id_base + "<component>/<name>.json"`; the generator emits
   exactly that, and a descriptor's `settings_schema_id` stores that exact string
   (or data that forms it unambiguously) — never a second "path-like vs absolute"
   formulation. Hence **`$id` threading is a hard prerequisite for the composer.**

4. **Invariant (tested): imported IDL types are referenced, not redeclared.** An
   importing generated header must reference an imported type via its generated
   header + `get_inner_schema()`, never redeclare it. `interface_descriptor`
   already follows this (reuses `function_info` by value). Add a test that scans
   generated headers for duplicate `id<>` / `variant_alternative_tag<>`
   specializations.

5. **Active global profile pointer is a deliberate short-term choice.** Fine for
   today's single-threaded generator; the cleaner long-term form is a
   `const schema_profile&` threaded through the walker. Noted, not a regression.

6. **ABI presence-gating is required for production, deferred only for the demo.**
   The `i_marshaller::get_schema` wire change must be protocol-version gated
   before any production merge; the local/demo prototype is the sole exception.
   (Aligns the API-design doc and this plan.)

7. **"Schema known" ≠ "runnable in this build".** Two orthogonal axes. The strict
   runnable config schema includes only components available in this build, and
   validation rejects unavailable ones; a separate catalog/doc schema may include
   `planned`/`disabled`. The composer emits the strict schema from the
   "runnable-here" axis.

**Narrowed next step:** (a) deterministic `$id` threading (#3); (b) declarative
descriptor metadata on the existing `stream_component_factory` /
`transport_component_factory` (#2); (c) the duplicate-reference test (#4). No
runtime marshaller surface yet.

---

## Phase 0 — Generator: `schema_profile` seam (no output change)

**Goal:** one canonical emitter that can later produce mcp vs config shapes;
default reproduces today's output byte-for-byte.

**Files:** `generator/include/json_schema/schema_profile.h` (new),
`generator/src/json_schema/schema_profile.cpp` (new),
`generator/include/json_schema/generator.h`,
`generator/src/json_schema/generator.cpp`, `generator/src/main.cpp`.

**Proposed names / changes:**
- `struct json_schema::schema_profile { ... };` with presets
  `config_strict_profile()` (default), `mcp_profile()`.
- Add trailing `const schema_profile& = config_strict_profile()` to
  `write_json_schema(...)` and `write_cpp_schema_accessors(...)`; thread by
  const-ref into `write_json_schema_document`, `write_schema_document_start`,
  `write_definition_set`, `map_idl_type_to_json_schema`, and the struct-member
  loop (`generator.cpp:895-966`).
- Gate the already-present sites: defaults (`:442/916/1511/1546`), enum
  string|int `oneOf` (`:1017-1034`), `required` (`:926/956`), descriptions,
  `additionalProperties` (`:966`).

**Enum:** `enum class rpc::schema_flavor { mcp, config };` in a shared rpc header
(used by both generator and runtime).

**Verify:** golden test — every generated `.json` and `*_schema.h` byte-identical
with the default profile; spot-check `mcp_profile()` drops defaults/int-enums on
one interface.

---

## Phase 1 — Generator: per-interface metadata statics

**Goal:** each generated interface type carries everything the runtime fold
needs, so the server does no hand-work.

**Files:** `generator/src/synchronous_generator.cpp`,
`generator/src/interface_declaration_generator.cpp`.

**Proposed generated members on each interface `i_foo`:**
- `static const char* __rpc_qualified_name();` — returns e.g.
  `"rpc::demo::i_calculator"` (today this is only a code comment).
- `static bool __rpc_is_deprecated();` — from interface `[status=deprecated]`.
- `static std::string get_schema(rpc::encoding, rpc::schema_flavor = schema_flavor::config);`
  — overload of the existing accessor selecting the profile.
- A per-method table accessor:
  `static const rpc::interface_method_info* __rpc_methods(size_t& count);`
  built from existing `function_info` data (`in_json_schema`, `out_json_schema`,
  method name) plus the `method_id` the stub switch already assigns
  (`synchronous_generator.cpp:2495`) and the method `[deprecated]` flag.

**New plain structs** (in an rpc runtime header, populated by generated code):
```cpp
struct rpc::interface_method_info
{
    const char*   method_name;
    rpc::method   method_id;
    const char*   in_schema;     // points at generated static strings
    const char*   out_schema;
    bool          deprecated;
};

struct rpc::interface_schema      // one entry per interface
{
    rpc::interface_ordinal id;
    std::string            qualified_name;
    bool                   deprecated;
    std::string            schema;            // whole-interface document (flavor-selected)
    std::vector<rpc::interface_method_info> methods;
};
```

**Verify:** unit test compiles a sample IDL and asserts
`i_calculator::__rpc_qualified_name()`, method count, and that
`__rpc_methods()` exposes `add` with `method_id == 1` and a non-empty in_schema.

---

## Phase 2 — Runtime: one `casting_interface` virtual serves all tiers

**Goal:** a single virtual both proxy and stub inherit, so Tier 1 (local proxy)
and Tier 2 (callee fold) share generated code.

**Files:** `c++/rpc/include/rpc/internal/casting_interface.h`,
`c++/rpc/include/rpc/internal/base.h`, generated proxy/stub emission in
`synchronous_generator.cpp`.

**Proposed virtual on `casting_interface`:**
```cpp
[[nodiscard]] virtual void __rpc_enumerate_schemas(
    rpc::encoding enc,
    rpc::schema_flavor flavor,
    bool include_deprecated,
    std::vector<rpc::interface_schema>& out) const = 0;
```

- **`interface_proxy<T>` (caller side, `casting_interface.h:62`)** and the
  generated single-interface stub: append **one** entry built from
  `T::__rpc_qualified_name()` / `T::get_schema(enc, flavor)` / `T::__rpc_methods()`,
  skipping when deprecated and `!include_deprecated`.
- **`rpc::base<Implementation, Interfaces...>` (`base.h`)**: fold over
  `Interfaces...` (same shape as `__rpc_query_interface`/`__rpc_call`) appending
  each interface's entry.

**Verify:** local C++ test — construct a `base`-derived object implementing two
interfaces, call `__rpc_enumerate_schemas`, assert both appear and a
`[status=deprecated]` one is filtered unless `include_deprecated`.

---

## Phase 3 — `i_marshaller::get_schema` + service/transport routing

**Goal:** the wire entry point, object-discovery mode, routed like `try_cast`.

**Files:** `c++/rpc/include/rpc/internal/marshaller.h`,
`marshaller_params.h`, `c++/rpc/src/service.cpp`,
`c++/rpc/src/transport.cpp`, `c++/rpc/src/pass_through.cpp`.

**Proposed method (peer of `try_cast`):**
```cpp
virtual CORO_TASK(get_schema_result) get_schema(get_schema_params params) = 0;
```
```cpp
struct rpc::get_schema_params
{
    uint64_t            protocol_version;
    rpc::encoding       encoding_type;     // yas_json for now
    rpc::schema_flavor  flavor;
    rpc::remote_object  remote_object_id;  // object-discovery mode (milestone)
    rpc::optional<rpc::interface_ordinal> interface_id;   // type/scoped query (later)
    bool                include_deprecated{false};
    rpc::caller_zone    caller_zone_id;
    rpc::destination_zone destination_zone_id;
    std::vector<rpc::back_channel_entry> in_back_channel;
};

struct rpc::get_schema_result : rpc::standard_result
{
    rpc::encoding                       encoding_type;
    std::vector<rpc::interface_schema>  interfaces;
};
```

**Implementations (mirror `try_cast`):**
- `service::get_schema` (`service.h:554` neighbour): same-zone guard, `get_object(remote_object_id)`,
  then on the stub's root object call `__rpc_enumerate_schemas(...)` and return.
  Apply the `[introspectable]` opt-in filter here (Phase 5b).
- `transport::inbound_get_schema` / `transport::get_schema` /
  `outbound_get_schema` — answer locally if destination is the local zone, else
  forward (parallels `transport.cpp:1051-1087`).
- `pass_through::get_schema` — forward to target transport
  (`pass_through.cpp:170` parallel).

(No ABI version gating in this plan; deferred.)

**Verify:** two in-process zones; caller invokes `get_schema(remote_object)`,
asserts the remote object's interfaces + method schemas come back, and that a
forwarded (cross-zone) call returns the same as a local one.

---

## Phase 4 — Caller-side cache + name resolution

**Goal:** Tier 1/Tier 2 access and local name->id resolution, scoped correctly.

**Files:** `c++/rpc/include/rpc/internal/service_proxy.h` (+ src),
`c++/rpc/include/rpc/internal/object_proxy.h` (+ src), a new public helper header.

**On `service_proxy` (per-remote-zone schema cache):**
```cpp
// keyed by interface type — shared across all objects of this zone
std::unordered_map<rpc::interface_ordinal, rpc::interface_schema> schema_cache_;
// name -> id index, for local resolution
std::unordered_map<std::string /*qualified iface*/, rpc::interface_ordinal> name_index_;

void cache_interface_schemas(const std::vector<rpc::interface_schema>&);
const rpc::interface_schema* find_interface_schema(rpc::interface_ordinal) const;

struct resolve_result { int error_code; rpc::interface_ordinal interface_id; rpc::method method_id; };
resolve_result resolve_method(const std::string& qualified_interface,
                              const std::string& method_name) const;
```

**On `object_proxy` (per-object interface-set + tiered discovery):**
```cpp
// which interfaces THIS object exposes (ids); schemas live on the service_proxy
std::vector<rpc::interface_ordinal> discovered_interfaces_;

// Tier 1 first (walk live proxy_map entries' __rpc_enumerate_schemas), then
// Tier 2 (service_proxy_->send get_schema), populating the service_proxy cache.
CORO_TASK(int) discover_schemas(rpc::encoding, rpc::schema_flavor, bool include_deprecated);
```

**Public helper (untyped-client friendly):**
```cpp
// describe an object you hold a reference to
CORO_TASK(rpc::get_schema_result) describe_object(
    const rpc::shared_ptr<rpc::casting_interface>& obj,
    rpc::schema_flavor flavor = rpc::schema_flavor::mcp);

// resolve a name to ids using the cache populated by discovery
rpc::service_proxy::resolve_result resolve_call(
    const rpc::shared_ptr<rpc::casting_interface>& obj,
    const std::string& qualified_interface,
    const std::string& method_name);
```

**Verify:** Tier-1 test (held typed proxy resolves with no `send`); Tier-2 test
(discover an un-held interface, second lookup served from `service_proxy` cache);
lifetime test (release the object proxy, confirm the schema cache survives on the
`service_proxy`).

---

## Phase 5 — Opt-in attribute + filter

**5a (generator):** add the `[introspectable]` interface attribute
(`rpc_attributes.h`) and a generated `static bool __rpc_is_introspectable();`.
**5b (runtime):** the fold / `service::get_schema` skip interfaces where
`!__rpc_is_introspectable()`, returning not-found.

**Verify:** an unmarked interface is absent from discovery; a marked one appears.

---

## Phase 6 — WebSocket demo: the milestone

**Goal:** browser client discovers tools and calls `i_calculator.add` over
`yas_json`.

**Files:** `c++/demos/websocket/idl/websocket_demo/websocket_demo.idl`
(mark `i_calculator` `[introspectable]`), `c++/demos/websocket/server/www/*.js`,
`untrusted_web` JS transport.

**Steps:**
1. Mark `i_calculator` `[introspectable]`; rebuild so its schema statics/attribute exist.
2. JS: send a `get_schema` request for the bound object over the existing
   WebSocket envelope; render the returned non-deprecated methods as a tool list.
3. JS: build `{ "first": ..., "second": ... }` from the method `in_schema`,
   look up `{interface_id, method_id}` from the returned descriptor, and `send`
   with `encoding = yas_json` and that JSON as `in_data`.
4. Parse the reply (`out_buf`) against `out_schema`.

**Verify:** end-to-end — the browser lists `add/subtract/multiply/divide`,
issues a `yas_json` `add`, and displays the result, with no hard-coded method
ids and no protobuf on that path.

---

## Deferred follow-ups (post-milestone)

1. ABI presence-gating of `get_schema` (min protocol version + not-supported
   error).
2. Harden the `production` checksum directory into a strict CI/diff guard.
3. Type-query mode (mode 1) + generated `register_interface_schemas()` per IDL
   module for callers without an object.
4. `config` flavor + the connection-factory composer (the other design docs).
5. MCP JSON-RPC `tools/list` / `tools/call` shim as a consumer of `get_schema`.

## Suggested commit boundaries

Phase 0 / Phase 1 / Phase 2 / Phase 3 / (Phase 4 + 5) / Phase 6 — each compiles
and is independently testable; Phases 0-2 change no runtime behaviour, so they
land safely ahead of the wire change in Phase 3.
