<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Design: `i_marshaller` Schema Introspection — toward an Agent-Callable MCP Path

Companion to `config_topology_schema_proposal.md` (the schema profiles) and
`config_topology_schema_design_change.md` (the generator change). This document
designs the **runtime** half: a method on `rpc::i_marshaller` that hands an agent
enough schema to build a JSON call and execute a method.

**End goal:** the WebSocket demo's client discovers an interface, fetches its
method schemas, builds a JSON call, and invokes a method on an MCP-style service
through this one API.

## Why on `i_marshaller`, next to `try_cast`

`try_cast` is already the marshaller's metadata query — "does this object
implement interface X?" — and it routes the canonical way: the owning zone
answers locally (`service.cpp:901`), transports/passthroughs forward to the
remote zone (`transport.cpp:1051`, `pass_through.cpp:170`). Schema introspection
is the deeper sibling — "*describe* interface X's methods" — so it belongs in the
same place and routes the same way. Implementors: `service` (answers locally
from a generated registry), `pass_through` and `transport` (forward like
try_cast).

Cost to acknowledge: `i_marshaller` is the core wire ABI. Adding a method bumps
the marshaller's protocol surface; every implementor must provide it, and it must
be gated by `protocol_version` so a peer that predates it can refuse cleanly
rather than mis-decode.

## The method

```cpp
// i_marshaller, peer of try_cast
virtual CORO_TASK(get_schema_result) get_schema(get_schema_params params) = 0;
```

```cpp
enum class schema_flavor { mcp, config };   // selects the generator profile

struct interface_selector
{
    // Prefer id (stable wire identity). qualified_name is resolved to an id via
    // the generated name<->id table, because the name is NOT runtime-recoverable
    // from the id today.
    rpc::optional<interface_ordinal> id;
    rpc::optional<std::string>       qualified_name;
};

struct get_schema_params
{
    uint64_t            protocol_version;
    encoding            encoding_type;     // primarily yas_json; reserved for others
    schema_flavor       flavor;
    interface_selector  interface;         // empty => "list everything for the object"
    rpc::optional<remote_object> object;   // optional object scope (discovery)
    caller_zone         caller_zone_id;
    destination_zone    destination_zone_id;
    std::vector<rpc::back_channel_entry> in_back_channel;
};

struct interface_schema_entry
{
    interface_ordinal id;
    std::string       qualified_name;      // e.g. "rpc::demo::i_calculator"
    std::string       schema;              // the JSON schema document for this interface
};

struct get_schema_result : standard_result
{
    encoding                            encoding_type;
    std::vector<interface_schema_entry> interfaces;  // 1 for a direct query; N for discovery
};
```

Three usage modes fall out of the one method (all three are in scope per the
design review):

1. **Type query** — `interface.id`/name set, no `object`: "describe this
   interface." Needs the global registry (below).
2. **Object discovery** — `object` set, `interface` empty: "what interfaces does
   this object expose, with their schemas?" (the MCP tool-list case). Backed by
   `rpc::base` directly — no registry needed (below).
3. **Scoped query** — both set: "describe interface X as implemented by object O."

`flavor` chooses the schema shape via the profile work: `mcp` => minimal,
inline, no `$id` (ready to hand to an LLM/tool layer); `config` => full
authoring schema. `encoding_type` is `yas_json` for now; the field exists so a
future binary/other schema encoding is additive.

**Deprecation is policy**, and the IDL expresses it at two levels:

- **Interface-level** via the `[status=deprecated]` attribute — the same `status`
  value the checksum step reads (`component_checksum.cpp:54`). Discovery filters
  deprecated interfaces out of the returned set by default (a policy flag can
  include them).
- **Method-level** via the `[deprecated]` function attribute
  (`rpc_attributes.h:26`, `has_value("deprecated")`), which already flows into
  the schema as `"deprecated": true` (`generator.cpp:434/871/...`). Deprecated
  methods within an included interface stay in the schema but carry the marker so
  a caller can skip them.

## What backs it — the generator does the work, the server does almost nothing

Design principle from the review: *the JSON code generator provides everything
the client needs; the remote server adds minimal effort.* Each interface's
generated code is the single source of its own MCP information.

The raw material already exists:

- **Per method**: `rpc::function_info` (`interfaces/rpc/rpc_types.idl:851`) carries
  `in_json_schema` and `out_json_schema`, emitted into every generated proxy's
  `get_function_info()` (`synchronous_generator.cpp:2290`). These are
  self-contained (no external `$ref`) — already MCP-shaped.
- **Per interface**: `i_foo::get_schema(rpc::encoding)` accessors exist and
  nest each method's send/receive schema under `definitions`. Each interface
  type also has `get_id(version)`.

### Object discovery is a compile-time fold — no registry

`rpc::base<Implementation, Interfaces...>` (`base.h:12`) holds the object's
**entire interface set as a compile-time pack**. It already folds over
`Interfaces...` in `__rpc_query_interface` (`base.h:24`) and `__rpc_call`
(`base.h:49`). Object discovery is one more fold of the same pack:

```cpp
// conceptually, generated/templated on base — folds over Interfaces...
std::vector<interface_schema_entry> __rpc_enumerate_schemas(
    encoding enc, schema_flavor flavor, bool include_deprecated) const
{
    std::vector<interface_schema_entry> out;
    ( [&]{
        if (include_deprecated || !Interfaces::__rpc_is_deprecated())
            out.push_back({ Interfaces::get_id(rpc::get_version()),
                            Interfaces::__rpc_qualified_name(),
                            Interfaces::get_schema(enc) });   // flavor-selected accessor
      }(), ... );
    return out;
}
```

So "the object knows all the interfaces it supports and each interface provides
all the information the MCP caller requires" is literal: the object's own type
supplies the full answer, with no global table and no per-object server wiring.

### Small additions each interface must expose (generated statics)

For the fold to be complete, the generator emits, per interface type, alongside
the existing `get_id` / `get_schema`:

- `__rpc_qualified_name()` — today the qualified name is only a comment; emit it
  as a string so discovery returns human names and `interface_selector::
  qualified_name` resolves.
- `__rpc_is_deprecated()` — from `has_value("deprecated")`, for the policy
  filter.
- method names as data (already implied by `function_info`) so a tool can be
  named `i_calculator.add`.

### A global registry — only for type-query without an object

Mode 1 (describe interface X when you do **not** hold an object of it) and
name->id resolution on the call path need a table: `interface_ordinal ->
{ qualified_name, deprecated, schema }` plus `name -> id`. Emit it as a
generated, per-IDL-module registration function gathered into the service. The
WebSocket-demo milestone does **not** need this — object discovery (the fold)
covers it — so the registry is a later slice.

### Tool identity and name -> id resolution

A caller works in **names** — qualified interface name + method name — and the
**`service_proxy` cache resolves them to `{interface_ordinal, method_id}`
locally**, which `send` then uses. So the cache (populated by discovery) stores,
per method: `{interface_name, method_name -> interface_ordinal, method_id,
in_json_schema, out_json_schema}`. An untyped client (browser JS) passes
`"i_calculator", "add"`, gets the ids back from its local cache, and issues a
normal `send` — no server round-trip to resolve, and no need to hard-code ids as
the demo does today.

This is safe because the name/id pair is an **immutable contract**, enforced at
build time, not negotiated at runtime:

- `component_checksum.cpp:44-64` writes `check_sums/<status>/<qualified_name>`
  containing the interface fingerprint, where `status` is the `[status=...]` IDL
  attribute. The intent is that an interface marked `production` must never change
  its fingerprint. **Enforcement is not yet wired** — the `production` directory
  is currently unprotected — but in a production setting this rule must be
  strictly controlled (a CI/diff check against the committed checksum that fails
  the build on any change). The schema API depends on this guarantee, so it is a
  prerequisite to harden before the name/id contract can be relied on across
  versioned deployments.
- To change a production interface or type you **rename it** — typically an
  inline namespace bumping the version (`v1` -> `v2`). The versioned name *is*
  the version.
- Therefore `method_id` (1-indexed declaration order) and `interface_ordinal`
  (the fingerprint) are stable for the lifetime of a given interface name. Names
  and ids never drift apart, so the `service_proxy` cache needs no version
  reconciliation — only the get_schema *method itself* needs ABI presence-gating
  for a peer that predates it.

### Exposure policy

Deprecation filtering (decided above) is one policy layer. A second is whether
discovery is **opt-in**: returning schemas for arbitrary internal interfaces
leaks shape, so the `[introspectable]` IDL attribute on the interface could
restrict the fold to marked interfaces, with unmarked ones answering
`INVALID_DATA`/not-found. Whether to require that opt-in or expose everything
non-deprecated is open question 3 below.

## Access tiers — don't go remote when the proxy is local

The remote `get_schema` call is the path for a caller that holds only an object
reference, not a typed binding. A C++ caller that holds an `rpc::shared_ptr`
already has the metadata locally and must not pay a round-trip. Three tiers:

- **Tier 0 — static type known.** The caller knows `i_foo`, so
  `i_foo::get_schema(enc)` / its `function_info` are available at compile time.
  Zero cost, no object needed.
- **Tier 1 — local proxy, dynamic.** The caller holds an `rpc::shared_ptr` whose
  `object_proxy` owns the live `interface_proxy` instances in `proxy_map`
  (`object_proxy.h:34`, a map `interface_ordinal -> weak_ptr<casting_interface>`,
  with `get_any_live_interface_proxy()` / `get_proxy_count()`). The object proxy
  interrogates those live proxies for their metadata **in process — no
  marshaller call**. This is the optimisation: never go remote for an interface
  you already hold.
- **Tier 2 — remote.** Only for (a) a generic/untyped client that does not bind
  to an `rpc::shared_ptr` — the browser JS client, an external agent — or (b)
  discovering interfaces the caller does not yet hold locally. This is the
  `i_marshaller::get_schema` path.

### One generated virtual serves all three

`interface_proxy<T>` (caller, `casting_interface.h:62`) and `rpc::base`/stubs
(callee) both derive from `casting_interface`, and `interface_proxy<T> : public
T` inherits the generated interface's schema statics. So the metadata accessor is
a single virtual added to `casting_interface` — conceptually
`__rpc_interface_metadata(encoding, schema_flavor)` plus `__rpc_qualified_name()`
/ `__rpc_is_deprecated()` — implemented once per interface by the generator. Then:

- **Tier 1** calls it on each live proxy in `object_proxy::proxy_map`.
- **Tier 2** calls the *same* virtual inside `rpc::base::__rpc_enumerate_schemas`
  on the callee.

No separate proxy-side and stub-side code; the generator emits one
implementation that both sides inherit. Caveats for Tier 1: `proxy_map` holds
`weak_ptr` ("if they exist" — expired entries are skipped) and covers only
interfaces already cast-to. The `object_proxy` does **not** necessarily know the
remote object's full interface set; discovering *un-held* interfaces is a Tier 2
call.

### Caching promotes Tier 2 to Tier 1

A first remote discovery need not stay remote. Once `get_schema` returns metadata
for a remote object, the result is cached so subsequent queries are served
locally — the second lookup is Tier 1 even for an interface never cast-to. Two
facts cache at different scopes, and conflating them wastes memory:

- **"Which interfaces does object O implement?"** is per-object. Cache the
  discovered interface-id set on the `object_proxy`.
- **"What is the schema for interface X?"** is per-interface-*type* and identical
  across every object in the remote zone. Cache the schema **on the
  `service_proxy`**, keyed by `interface_ordinal` — not on the object proxy.

The `service_proxy` is the right home for concrete lifetime reasons, not just
tidiness. It is the per-remote-zone parent: it owns the object proxies as
`weak_ptr` (`service_proxy.h:47`, `proxies_`), carries the `destination_zone_id`
(`:51`) and the connection's `protocol_version`, and is torn down on
transport-down (`:53`). So a schema cache there:

- is shared by every object proxy of that remote zone (the schema for interface
  X is identical across them);
- survives object-proxy churn — object proxies are weak and disappear when their
  objects are released, but the cache persists for the connection;
- dies with the connection, exactly when it should: a reconnect may reach a
  different remote build/version, so the cache must not outlive the
  `service_proxy` that scoped its `protocol_version`.

Cache validity within a session is then trivial: `interface_ordinal` is a version
hash, so for a fixed `protocol_version` the schema for an id is immutable — no
invalidation needed until the `service_proxy` itself goes away.

One further shortcut for C++ Tier 2 callers: if a remotely-discovered
`interface_ordinal` matches a locally-known generated type, its schema is already
available from Tier 0 statics — only genuinely foreign interface ids (no local
generated code, e.g. anything on the browser JS side) require the schema text to
come over the wire.

The WebSocket-demo client is Tier 2 (untyped JS), so the milestone needs the
remote path regardless — but a C++ MCP shim that holds typed proxies would use
Tier 0/1 and never round-trip.

## The call loop, end to end

Because `encoding::yas_json` makes `in_data` literally the JSON of the method's
parameters, and a schema-valid JSON object is valid `in_data`, the loop closes
with no new serialization machinery:

```text
agent: get_schema(object=O, flavor=mcp)              -> list of MCP tools (name = "i_calculator.add", inputSchema {first,second})
agent: builds JSON { "first": 2, "second": 3 } from inputSchema
agent: resolve "i_calculator.add" -> {interface_id, method_id}     (registry; can be folded into the send path)
agent: send(send_params{ encoding=yas_json, interface_id, method_id, in_data = that JSON bytes, remote_object=O, ... })
stub : from_yas_json -> typed params -> calls C++ impl -> to_yas_json(reply) -> out_buf
agent: parses out_buf against the method's out_json_schema
```

## Applying it to the WebSocket demo (the milestone)

Today (`c++/demos/websocket/`): browser JS <-> C++ WS server, exposes
`i_calculator` (`add/subtract/multiply/divide/...`); the JS proxy hard-codes
method ids and uses **protobuf**. The four gaps the exploration found map cleanly
onto this design:

| Gap (from demo `notes.md` / exploration) | Closed by |
|------------------------------------------|-----------|
| (a) interface discovery | `get_schema(object, flavor=mcp)` discovery mode |
| (b) runtime method-schema fetch | the per-method `in/out_json_schema` returned by the registry |
| (c) JSON call builder without codegen | use `encoding::yas_json`: `in_data` *is* the JSON the agent built from `inputSchema` — no protobuf, no browser descriptors |
| (d) server-side tool provider | mark `i_calculator` `[introspectable]`; the service registry exposes it; the existing `send` dispatch executes it |

Concrete demo change: keep the WebSocket transport, but add a JS path that (1)
calls `get_schema` over the same connection to list tools, (2) builds a JSON
argument object, (3) sends it with `yas_json` encoding instead of protobuf. The
calculator C++ side is unchanged — its stub already handles `yas_json`.

## Decisions from the design review

- **Placement:** on `i_marshaller`, peer of `try_cast` (a dedicated
  introspection interface was considered and set aside).
- **Discovery:** support **both** type-query and object-discovery. Object
  discovery is the compile-time fold over `rpc::base`'s pack; type-query uses the
  later global registry.
- **Deprecation policy:** deprecated interfaces are filtered from discovery by
  default; deprecated methods remain but keep their `deprecated` marker.
- **Generator-complete, server-minimal:** the generated per-interface code is the
  single source of all MCP info; the server only returns it. There is no
  hand-written per-object MCP wiring.
- **Storage:** reuse the schemas the generator already emits
  (`function_info.in/out_json_schema`, `get_schema`); the `flavor` selects the
  generator profile. No separate hand-maintained store.
- **Call identity:** result carries `{interface_name, method_name,
  interface_ordinal, method_id, in/out schema}`; the `service_proxy` cache does
  **name -> id resolution locally**, then `send` uses the ids. No server-side
  name resolution step.
- **Identity stability:** guaranteed at build time by the
  `check_sums/<status>/<name>` fingerprint mechanism + name/inline-namespace
  versioning — not by runtime version negotiation.
- **Exposure:** opt-in. Only interfaces marked `[introspectable]` are
  discoverable.

## Open questions that remain

1. **Where the (type-query) registry lives and is populated** — a generated
   `register_interface_schemas()` per IDL module, eager at service construction
   vs lazy. Only matters once mode-1 type-query is built; the demo does not need
   it.
2. **ABI presence-gating only.** Interface/method *identity* stability is settled
   (build-time checksum guarantee), so the only runtime versioning concern is the
   presence of the `get_schema` method itself: define the minimum protocol
   version and the not-supported error a pre-feature peer returns.
3. **Harden the `production` checksum guard.** Currently unprotected; must become
   a strictly enforced CI/diff check before the name/id contract is relied on in
   production. Prerequisite, not optional.
4. **Add the `[introspectable]` attribute.** It does not exist yet — new
   generator attribute + the per-interface gating it controls. (Open sub-question:
   should discovery also differ by caller zone / capability, or is the attribute
   enough?)
5. **MCP JSON-RPC framing.** The marshaller returns complete per-interface info;
   whatever ultimately speaks literal MCP `tools/list`/`tools/call` (the browser
   client itself, or a thin shim) is a *consumer* of that complete info, not work
   for the RPC core. Decide where that shim lives once the loop is proven — it
   does not block the milestone.

## Suggested sequence

1. Land the generator `schema_profile` + `mcp_profile` (separate design doc) so
   `flavor` has something to select.
2. Emit the small per-interface statics the fold needs: `__rpc_qualified_name()`,
   `__rpc_is_deprecated()`, and the flavor-selected `get_schema`.
3. Add the `__rpc_enumerate_schemas` fold to `rpc::base`/`casting_interface`, and
   add `get_schema` to `i_marshaller` in **object-discovery mode** + service local
   answer; version-gate it. Forwarding in transport/passthrough mirrors
   `try_cast`. Apply the deprecation filter.
4. WebSocket demo: JS calls `get_schema(object)`, lists non-deprecated tools,
   builds a `yas_json` call to `i_calculator.add`, executes it. **This is the
   milestone.**
5. Add the global registry for type-query (mode 1) and, if wanted, the MCP
   JSON-RPC shim.
