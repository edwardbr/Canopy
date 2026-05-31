<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Design Change: Schema Profiles, Stable `$id`, and a Connection-Factory Composer

Implementable companion to `config_topology_schema_proposal.md`. The proposal is
the "why"; this is the "what and where". It is scoped so the first landing is
**behaviour-preserving** for every schema emitted today.

## Summary

Three coordinated changes, in dependency order:

1. **`schema_profile`** — a small options struct threaded through the existing
   JSON-schema emitter so one canonical walker can emit either a minimal
   MCP/tool schema or a full config-authoring schema. Default value reproduces
   today's output byte-for-byte.
2. **Stable `$id`** — emitted only under the config profile, from a deterministic
   URI derived from the schema's output location.
3. **A declarative component registry + schema composer** in
   `connection_factory` that turns the per-component schemas into one
   discriminated topology schema, and refines `required`/`default`s with the
   factory's registered application defaults.

Nothing below changes the typed C++ converters or the runtime overlay/merge
path. It only changes what schema *documents* are emitted and adds a composer.

## Change 1 — `schema_profile`

New header `generator/include/json_schema/schema_profile.h`:

```cpp
namespace json_schema
{
    struct schema_profile
    {
        enum class required_policy { idl_accurate, none };
        enum class enum_form       { string_only, string_and_int };
        enum class ref_form        { inline_all, external_id };

        bool          include_descriptions = true;
        bool          include_defaults     = true;
        required_policy required             = required_policy::idl_accurate;
        enum_form     enums                = enum_form::string_and_int;
        ref_form      refs                 = ref_form::external_id;
        bool          emit_id              = true;
        bool          additional_properties_false = true;

        // Stable base for $id; ignored unless emit_id && refs == external_id.
        std::string   id_base;   // e.g. "https://schemas.canopy.dev/"
        std::string   id_path;   // e.g. "connection_factory_config/connection_factory_config.json"
    };

    schema_profile config_strict_profile();   // == today's output (the default)
    schema_profile config_overlay_profile();  // strict, required = none (fallback only)
    schema_profile mcp_profile();              // minimal, inline, string enums, no defaults/$id
}
```

Preset definitions:

| Field | `config_strict` (default) | `mcp` |
|-------|---------------------------|-------|
| `include_descriptions` | true | true (callers may truncate) |
| `include_defaults` | true | false |
| `required` | `idl_accurate` | `idl_accurate` |
| `enums` | `string_and_int` | `string_only` |
| `refs` | `external_id` | `inline_all` |
| `emit_id` | true | false |
| `additional_properties_false` | true | true |

Note both profiles keep `required = idl_accurate`: per the proposal, the set is
always minimal (non-optional, no explicit default). The profiles differ in
whether the *default value* is rendered, not in which fields are required.
`config_overlay` exists only as a fallback for tools that cannot supply the
application-default layer the composer normally injects.

### Threading

Add a `const schema_profile& = config_strict_profile()` trailing parameter to
the public entry points in `generator/include/json_schema/generator.h`:

- `write_json_schema(...)`
- `write_cpp_schema_accessors(...)`

`write_cpp_convert_accessors` is unaffected (it emits converters, not schema).

Internally, pass the profile by const-ref into the existing private functions
that already exist in `generator/src/json_schema/generator.cpp`:

- `write_json_schema_document` (line 1787)
- `write_schema_document_start` (line 2034)
- `write_definition_set`
- `map_idl_type_to_json_schema`
- the struct-member loop around lines 895–966

The default argument means **all existing call sites compile unchanged and emit
identical output** until a caller passes a non-default profile.

### Gate points (all already present in the emitter)

| Profile field | Existing site to gate | Behaviour |
|---------------|-----------------------|-----------|
| `include_defaults` | `default` emission at lines 442, 916, 1511, 1546 | skip when false |
| `enums` | enum `oneOf`(string\|integer) at lines 1017–1034 | when `string_only`, emit only the string `enum` branch, drop the integer branch and the wrapping `oneOf` |
| `required` | required loop at lines 926–928 / 956–965 | when `none`, suppress the `required` array |
| `include_descriptions` | every `write_string_property("description", ...)` | skip when false |
| `additional_properties_false` | line 966 | emit only when true |
| `refs` / `emit_id` | see Change 2 and the composer | — |

The existing IDL-accurate required logic at line 926
(`if (var->get_default_value().empty()) required_fields.push_back(...)`) is
exactly the rule the proposal wants; it stays, now gated by `required_policy`.

## Change 2 — Stable `$id`

`$id` is emitted only when `emit_id && refs == external_id` (config profile).
Two emission sites, both already writing `$schema`/`title`:

- `write_json_schema_document` — after line 1812, add
  `writer.write_string_property("$id", profile.id_base + profile.id_path);`
- `write_schema_document_start` — after line 2042, emit the same `$id` line into
  the raw-string literal.

Both must produce the **same** `$id` for the same schema, because
`materialise_settings<T>` validates against the embedded-string schema while
editors read the `.json` file; a mismatch makes cross-file `$ref` silently
unresolvable.

`id_path` is the generated relative path (`<component>/<name>.json`), which is
already unique. It must be threaded from the CMake `CanopyGenerate` invocation /
`main.cpp` (line ~820) down into the profile, since the emitter functions
currently receive only `schema_title`. `id_base` is a single project-wide
constant (a CMake cache variable, default `https://schemas.canopy.dev/`).

The MCP profile sets `emit_id = false` and `refs = inline_all`, so tool schemas
remain self-contained with no `$id` — the composer/`$id` work cannot leak into
them.

## Change 3 — Declarative registry + composer (connection_factory)

### 3a. Make the dispatch table data

Today `type` -> settings-type is imperative (`emplace("tcp_blocking", …)` plus
`materialise_settings<rpc::tcp_blocking_stream::endpoint>` in
`src/adapters/streams/*.cpp`, and `if (layer.type == "…")` in
`streaming/layer_factory/src/factory.cpp`). Replace the registration shape with
a descriptor the runtime *and* the composer can both read:

```cpp
namespace rpc::connection_factory::detail
{
    enum class component_role { base_stream, stream_layer, transport, runtime_dependency };
    enum class component_status { available, experimental, planned };

    struct component_descriptor
    {
        std::string      type;            // discriminator value, e.g. "tcp_blocking"
        component_role   role;
        component_status status;
        // Schema id of the settings type (matches the generated $id).
        std::string      settings_schema_id;     // e.g. "tcp_blocking_stream/tcp_blocking_stream_config.json"
        std::string      settings_definition;    // "#/definitions/<root>"
        // Effective application defaults to inject (may be empty).
        std::function<const json::v1::object&()> default_overlay;
        // The existing runtime builder(s) — unchanged behaviour.
        /* base_stream_connect_builder / stream_layer_builder / ... */
    };
}
```

The `register_*` functions populate `component_descriptor`s instead of bare
`emplace`. Runtime dispatch reads `type` -> builder exactly as before; the new
fields are inert at runtime. `status` lets attestation-service and
`http_server` (category: `runtime_dependency`, settings IDL exists, not yet
wired to a builder) appear as `planned`/`experimental` without a runnable
factory.

### 3b. The composer

A build step (initially standalone, later foldable into the generator) that:

1. Reads the registry descriptors.
2. For each envelope (`stream_layer_settings`, `typed_settings`) emits a
   discriminated schema: `properties.type.enum` = the registered `type`s, plus
   an `allOf` of `if (type const) then settings $ref settings_schema_id +
   settings_definition`.
3. Adds the `runtime_dependencies` section from `role == runtime_dependency`
   descriptors (HTTP server, attestation service, executor, io_uring controller,
   TLS contexts).
4. Merges each descriptor's `default_overlay` into the composed schema so
   `required`/`default`s are **deployment-accurate** — the refinement the
   generator alone cannot do (it sees IDL defaults only).
5. Emits strict and `config_overlay` documents and a VS Code
   `json.schemas` association snippet.

Because every branch `$ref`s by `$id`, Change 2 is a hard prerequisite.

## Migration & verification

Behaviour-preserving order:

1. Land `schema_profile` with the default = `config_strict` and thread it
   through. **Golden test:** every currently generated `.json` and
   `*_schema.h` is byte-identical (the default profile changes nothing).
2. Add `$id` threading; golden test updates to expect the new `$id` line only.
3. Make the registry declarative; runtime behaviour unchanged — existing
   `rpc_test` / transport tests must pass with no edits.
4. Add the composer + a test that authors a sparse topology
   (`tcp_blocking` base + `tls` + `websocket`) and validates it against the
   composed schema, asserting: omitted defaulted fields validate, and a wrong
   key under `settings` for the chosen `type` is rejected.
5. Migrate the MCP path (`per_function_generator.cpp`) onto the shared walker
   with `mcp_profile()` last, since that is where behavioural risk to existing
   tool schemas lives; gate behind a golden comparison of current tool schemas.

Both blocking and coroutine builds must be checked at steps 1, 3, 5.

## File-by-file

| File | Change |
|------|--------|
| `generator/include/json_schema/schema_profile.h` | **new** — struct + presets |
| `generator/src/json_schema/schema_profile.cpp` | **new** — preset definitions |
| `generator/include/json_schema/generator.h` | add trailing `schema_profile` param to two entry points |
| `generator/src/json_schema/generator.cpp` | thread profile; gate sites listed above; add `$id` at lines 1812 & 2042 |
| `generator/src/main.cpp` | build `id_path`; pass profile (line ~820, ~136) |
| `generator/src/json_schema/per_function_generator.cpp` | (phase 5) route through shared walker with `mcp_profile` |
| `c++/connection_factory/src/connection_factory_components.h` | `component_descriptor`, role/status enums |
| `c++/connection_factory/src/adapters/**/*.cpp` | register descriptors instead of bare `emplace` |
| `c++/connection_factory/src/.../compose_schema.cpp` | **new** — the composer |
| `cmake/*` | `CANOPY_SCHEMA_ID_BASE` cache var; wire composer into the build |

## First commit (smallest safe slice)

Change 1 only: `schema_profile` + threading + the golden byte-identical test.
It introduces the seam everything else hangs off, with zero output change.
