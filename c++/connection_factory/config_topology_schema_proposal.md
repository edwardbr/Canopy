<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Proposal: A Composed Configuration Schema, with the Connection Factory as the Single Route

This proposal builds on `dependency_injection_plan.md`. That note describes the
connection factory as Canopy's dependency-injection boundary. This one says: the
same boundary should also be the **single source of the authoring schema** that
external tooling (editors, language servers, config validators) consumes.

The aim is that a third party authoring a Canopy configuration file gets
completion, hover documentation, defaults, and validation **inside** the
implementation-specific `settings` blocks — not just at the topology level.

## The gap, stated precisely

The public configuration document is a topology envelope built from two
identical discriminated-union structs:

- `rpc::connection_factory::typed_settings`
  (`c++/connection_factory/interfaces/connection_factory_config/connection_factory_config.idl`)
- `rpc::stream_layers::stream_layer_settings`
  (`interfaces/streaming/stream_layers.idl`)

Both are:

```idl
struct <envelope>
{
    std::string type;                       // discriminator
    rpc::optional<json::v1::object> settings; // opaque to the factory
};
```

The generator emits an excellent self-contained draft-07 schema **per
component** (`tcp_blocking_stream_config.json`, `tls_stream_config.json`,
`sgx_blocking_transport_config.json`, …) with `description`, `default`, `enum`,
`required`, and `additionalProperties: false`. That is the hard part, and it is
already done.

But the envelope's `settings` field is a free-form `json::v1::object`. So the
schema for `connection_factory_config` describes the topology — `service`,
`listener`, `transport`, `stream_layers[]` — and then says nothing about what
goes inside any `settings`. An editor pointed at a config file can complete
`"type"` and `"settings"`, but the moment the cursor enters `settings` it has no
idea that `type: "tcp_blocking"` means `host`/`port` and `type: "tls"` means
`client.verify_peer`.

**Nothing currently stitches the per-component schemas onto the `type`
discriminator.** That stitching is the deliverable.

## Why the connection factory owns this

The `type` -> settings-type mapping already lives in the connection factory and
its layer factory — but imperatively:

| Role | `type` values | Owning settings type | Source |
|------|---------------|----------------------|--------|
| Base stream | `tcp_blocking` | `rpc::tcp_blocking_stream::endpoint` | `src/adapters/streams/tcp_blocking.cpp` |
| Base stream | `tcp_coroutine` | `rpc::tcp_coroutine_stream::…` | `src/adapters/streams/tcp_coroutine.cpp` |
| Base stream | `spsc`, `spsc_queue` | spsc endpoint | `src/adapters/streams/spsc_queue.cpp` |
| Transport | `stream_rpc` | `rpc::stream_transport::transport_settings` | `options.h` / `detail/stream_rpc.h` |
| Transport | `local` | local transport settings | `src/adapters/transports/local.cpp` |
| Transport | `ipc_spsc` | ipc_spsc settings | `src/adapters/transports/ipc_spsc.cpp` |
| Transport | `blocking_dll` | blocking_dll settings | `src/adapters/transports/blocking_dll.cpp` |
| Transport | `shared_scheduler_dll`, `unshared_scheduler_dll` | dll transport settings | `src/adapters/transports/*scheduler_dll.cpp` |
| Transport | `sgx_blocking` | `rpc::sgx_blocking_transport::transport_settings` | `src/adapters/transports/sgx_blocking.cpp` |
| Transport | `sgx_coroutine` | sgx_coroutine settings | `src/adapters/transports/sgx_coroutine.cpp` |
| Stream layer | `tls` | TLS layer settings | `c++/streaming/layer_factory/src/factory.cpp` |
| Stream layer | `websocket` | websocket settings | `layer_factory/src/factory.cpp` |
| Stream layer | `compression`, `zstd` | compression settings | `layer_factory/src/factory.cpp` |
| Stream layer | `spsc_wrapping`, `spsc_wrapper` | spsc-wrapping settings | `layer_factory/src/factory.cpp` |
| Stream layer | `attestation`, `attestation_stream` | `rpc::attestation_stream::stream_settings` | `layer_factory/src/factory.cpp` |

Two facts follow:

1. This table is the **single source** from which both runtime dispatch and the
   authoring schema should be generated. They must not drift — the same
   discipline the IDL already applies to struct/schema/converter.
2. Today the table is `emplace("tcp_blocking", …)` plus
   `materialise_settings<rpc::tcp_blocking_stream::endpoint>(…)` and a chain of
   `if (layer.type == "…")`. A generator cannot read that. **Making the table
   declarative is the one genuinely new piece of work.**

## What the composed schema looks like

A discriminated union is native draft-07. For each envelope we emit a `oneOf`
of `if`/`then` branches keyed on the `type` const, where each branch points
`settings` at the owning component's schema:

```jsonc
"stream_layer_settings": {
  "type": "object",
  "properties": {
    "type":     { "type": "string", "enum": ["tcp_blocking", "tls", "websocket", "compression", "attestation"] },
    "settings": {}
  },
  "required": ["type"],
  "allOf": [
    { "if":   { "properties": { "type": { "const": "tls" } } },
      "then": { "properties": { "settings": { "$ref": "tls_stream_config.json#/definitions/<tls_settings>" } } } },
    { "if":   { "properties": { "type": { "const": "websocket" } } },
      "then": { "properties": { "settings": { "$ref": "websocket_stream_config.json#/definitions/<ws_settings>" } } } },
    { "if":   { "properties": { "type": { "const": "compression" } } },
      "then": { "properties": { "settings": { "$ref": "compression_stream_config.json#/..." } } } }
    // ... one branch per registered type
  ]
}
```

VS Code's built-in JSON language server (and `redhat.vscode-yaml`, and the
reusable `vscode-json-languageservice`) handle this out of the box: pick
`"type": "tls"` and it completes `settings.client.verify_peer`; switch to
`"websocket"` and the offered keys change.

**We do not write a language server.** We emit this schema; the existing engine
provides context-sensitive completion, hover (from the IDL `[description]`s),
defaults, and live validation. A standalone LSP process is only warranted later
for editor-agnostic delivery (Neovim/Emacs), and even then it just wraps
`vscode-json-languageservice` — no custom completion logic.

## Schema profiles: one base, multiple projections

The same IDL struct feeds two consumers with **structurally incompatible**
needs, not merely different verbosity:

- An **MCP / tool schema** must be minimal and self-contained: it is fed to an
  LLM tool-use layer that does not run a draft-07 `$ref` resolver. It wants
  everything inlined, no `$id`, `oneOf`/`if-then` avoided or flattened,
  string-only enums, usually no `default`s, terse descriptions, and a scope of
  exactly one RPC method's parameters.
- An **application configuration schema** wants the full gamut: full
  descriptions, `default`s, string|integer enums, `additionalProperties: false`,
  cross-file `$ref` with `$id`, and the composed discriminated topology — all of
  which a real editor language server resolves and exploits.

| Axis | MCP / tool profile | Config-authoring profile |
|------|--------------------|--------------------------|
| Composition | inline everything; no external `$ref`/`$id` | `$id` + cross-file `$ref`, composed topology |
| `oneOf` / `if-then` | avoid / flatten | embraced (drives `settings` completion) |
| Enum form | string-only | string\|integer `oneOf` |
| `default` | usually omitted | essential (hover + insert) |
| `required` | minimal: only no-default fields | minimal: only no-default fields (IDL baseline refined by app defaults) |
| Scope | one method's parameters | whole topology, all reachable defs |
| `description` | terse (token cost) | full |

The codebase already has the two paths, but as **independent AST walkers** that
can drift: `generator/src/json_schema/per_function_generator.cpp` (per-method
input/output parameter schemas — the MCP-shaped path) and `write_json_schema`
in `generator.cpp` (whole-IDL, all definitions — the config path).

The resolution is **not** more bespoke emitters. It is to treat emission as a
set of named **profiles (projections) over one canonical model**. The pipeline
already holds the canonical pieces — `collect_definition_info` ->
`write_definition_set` via `json_writer`. Thread a profile through it:

```cpp
struct schema_profile
{
    bool include_descriptions;                       // both; mcp may truncate
    bool include_defaults;                           // config: yes, mcp: no
    enum { strict, relaxed, none } required_mode;
    enum { string_only, string_and_int } enum_form;
    enum { inline_all, external_refs } ref_mode;     // mcp: inline; config: $id
    bool allow_discriminated_oneof;                  // config: yes; mcp: flatten
    bool emit_id;                                    // config: yes; mcp: no
};
```

"They share the same base" then means literally: same AST, same definition
collection, one parametrised writer. The per-function (MCP) and whole-IDL
(config) paths become two **entry points that pick a profile and a scope**, not
two code paths — which also lets the duplicate type mapping in
`per_function_generator.cpp` be retired in favour of the shared walker.

This also disposes of a question raised while drafting this plan: **`$id` and
external `$ref` are a config-profile-only feature.** The MCP profile inlines
everything and emits no `$id`. So the composer and `$id` work below cannot
pollute tool schemas, and the two efforts never collide. Named profiles to seed:
`mcp`, `config_strict`, `config_overlay` (the last two differ only in
`required_mode`).

## Required means mandatory-without-a-default (sparse authoring)

The IDL rule is: a field is required unless it is `rpc::optional<T>`. But
*mandatory* is not the same as *must appear in the JSON*. If a mandatory field
has an explicit default, the supplied JSON may omit it and the value is injected
from the default. The field is always populated; the author never has to say so.

So the set a human or LLM **must** author is narrow:

> required-in-schema = non-optional **AND** no explicit default from any source.

Everything else is omissible. The injected value comes from, in precedence
order, the config file, then the application/C++ default, then the IDL/schema
default. This is the efficiency that matters for both audiences: an LLM emits
only the fields that differ from defaults (fewer tokens, fewer hallucinated
keys), and a developer writes only what they actually want to change.

This **supersedes** the earlier idea of a wholesale `required`-stripped overlay
variant. Stripping `required` was a workaround for an *inaccurate* `required`
set. Make the set accurate instead, and the only thing an editor or MCP client
flags as missing is a field that genuinely must be supplied (an `enclave_path`,
a remote `host`) — a correct diagnostic, not a false positive.

Two precisions:

1. **A default is an *explicit* value, not zero-initialisation.** `uint32_t
   port;` with no `= value` default-constructs to `0`, but `0` is not a default
   — that field is required. `required` must therefore be driven by explicit
   default presence (IDL `= value`, or an explicit application-default entry),
   never by default-constructibility. Feeding `to_json_object(Settings{})`
   blindly into schema `default`s would inject spurious zeros; the existing
   IDL-default translator is the principled source.

2. **"By the schema *or* the C++ code" splits across two computation sites.**
   The generator sees IDL `= value` defaults only, so it emits the
   *IDL-accurate* `required` set and `default`s. The C++ application defaults —
   the hand-written overlays such as `tcp_default_options()`, and
   `typed_settings_defaults<Settings>()` (= `to_json_object(Settings{})`) in
   `connection_factory/options.h` — are known only where the factory registers
   them. The *deployment-accurate* `required`/`default`s can therefore only be
   produced by the **composer on the connection-factory side**, layering those
   defaults onto the generator's baseline. Another reason the connection factory
   is the right home for the authoring schema.

For profiles, this means the required computation and default *rendering* are
**independent levers**: whether a field sits in `required` depends on whether a
default *exists*; whether the schema *shows* the default value is a separate
choice. The MCP profile can drop default values to save tokens while still
keeping `required` minimal.

## Three component categories, not one

`dependency_injection_plan.md` already names the categories. The schema must
respect them, because they do **not** all live in `stream_layers`:

1. **Base streams, stream layers, RPC transports** — these are the discriminated
   `oneOf` branches above. `stream_layers[]` entries are layers; `transport` and
   the base of `stream_layers[0]` are streams/transports.

2. **Runtime-dependency providers** — `rpc::service`, executor/scheduler,
   io_uring controller, TLS contexts, attestation *service* (distinct from the
   attestation stream layer), SPSC queue pairs, and **the HTTP server**. These
   are not byte-stream stages. They must not be forced into `stream_layers`.
   They belong in a separate, also-discriminated section that mirrors the DI
   context — a `runtime_dependencies` / `services` block, or attached under the
   transport that needs them.

3. **Application-provided extensions** — future LoRa, custom layers. The
   declarative registry must allow third parties to contribute a `type` + a
   schema, so their config gets the same editor support. This is the strongest
   argument for the registry being data, not code.

### Attestation and HTTP server (your note)

- **Attestation** has two faces. The *stream layer* (`attestation` /
  `attestation_stream`, settings `rpc::attestation_stream::stream_settings` in
  `c++/streaming/attestation/interfaces/.../attestation_stream_config.idl`) is
  already a layer `type`, so it slots straight into category 1 once the registry
  is declarative. The *attestation service* it depends on is category 2 (a
  runtime dependency resolved via the context), and needs its own discriminated
  entry.

- **HTTP server** (`canopy::http_server`, e.g. `client_connection_limits` in
  `c++/subcomponents/http_server/interfaces/http_server/http_server_config.idl`)
  is category 2 — a runtime service, not a stream stage. It should attach as a
  runtime dependency, not as a `stream_layers[]` item. Its schema already
  exists; it is simply **not mapped to a `type` discriminator yet.**

The registry should treat "schema exists, not yet wired to a runnable factory"
as a **first-class state** (e.g. a `status: available | experimental | planned`
field). That lets the composed schema either include such a type with a note, or
list it as known-but-unconfigurable, instead of silently dropping it. Both
attestation-service and http_server are in exactly this state today.

## Supporting changes the generator/runtime need

Independent of the registry, four schema-quality items gate the tooling
experience:

1. **Stable `$id` per schema.** Currently zero generated schemas carry `$id`
   (only the draft-07 `$schema` meta-pointer). Cross-file `$ref` from the
   composed schema requires it. First change to make.

2. **An accurate `required` set, not a stripped one.** Config files are *sparse
   overlays* (`documents/configuration.md`): missing keys are filled by
   lower-precedence defaults. Rather than emit a `required`-stripped variant,
   compute `required` accurately per "Required means mandatory-without-a-default"
   above — non-optional fields with no explicit default from any visible source.
   The generator does the IDL-accurate baseline; the connection-factory composer
   refines it with the registered application defaults so the authoring schema's
   `required` matches what a real deployment will actually leave for the human.

3. **Mark config roots vs incidental schemas.** Not every generated schema is
   human-authored. `io_uring.json` (ring-data structs: `rpc_io_uring_sq_data`,
   method-parameter envelopes) is generated but nobody writes it by hand. The
   composer must pull in only factory `settings` types, so we need a way to mark
   which IDL structs are configuration entry points (an IDL attribute, or the
   `_config`/`_options`/registry-membership convention).

4. **Keep `additionalProperties: false`.** Already present, and exactly what
   turns a typo into an editor diagnostic. Do not relax it in the strict schema.

## Delivery — no custom language server

End state for a third party consuming Canopy via `add_subdirectory`:

1. The build emits one composed `connection_settings` schema (strict + relaxed
   overlay variant) from the declarative registry.
2. A generated `.vscode/settings.json` snippet (or a SchemaStore-style catalog
   entry) associates `*.canopy.json` / `*.canopy.yaml` with the composed schema.
3. The author gets full IntelliSense in any JSON-Schema-aware editor, with the
   `settings` block driven by whichever `type` they chose.

## Phased plan

1. **Declarative registry.** Replace the imperative `emplace` + `if (type ==)`
   dispatch with a data table: `{ type, role (base/layer/transport/runtime),
   settings schema id, status }`. Runtime dispatch reads it; the composer reads
   it. Start with the existing built-ins; preserve the public custom-layer
   registration hooks by feeding them into the same table.

2. **`$id` emission** in `generator/src/json_schema/generator.cpp`.

3. **Schema composer** — walks the registry, emits the discriminated
   `connection_settings` schema with `$ref`s into the per-component schemas.
   Can begin as a standalone post-build step reading existing schemas + a small
   type-map, then fold into the generator once the registry is declarative.

4. **Relaxed overlay variant** emission.

5. **Editor association artifact** + a worked example config in
   `documents/configuration.md`.

6. **Categories 2 and 3**: add the `runtime_dependencies` section (HTTP server,
   attestation service, executor, io_uring controller, TLS contexts) and the
   application-extension contribution path.

7. Verify blocking and coroutine builds; coroutine-only components must still
   surface their schema so blocking-mode tooling can describe the full topology.

## Smallest first slice

Make the stream/transport registry declarative for the built-ins already in
`src/adapters/`, add `$id`, and emit the composed `oneOf` for `stream_layers`
only (TCP base + TLS/WebSocket/compression layers). That single slice proves the
end-to-end editor experience on the most common topology before touching
runtime dependencies, SGX, or the extension path.
