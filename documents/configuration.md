# Canopy Configuration

Canopy components — connection factories, services, transports, demos — read
their settings as sparse JSON overlays. Those overlays are combined with
schema and application defaults, the effective JSON is validated against an
IDL-generated schema, and only then is it converted to a typed C++ options
struct. The same IDL declaration drives the C++ struct, schema, converter, and
schema-visible defaults so they cannot drift.

This document covers the layering policy, null semantics, and how to add a
new options IDL. For the design history and the issues that drove each
decision, see `documents/reviews/2026-05-28-json-config-staging-review.md`.

## Layering order

```
schema defaults < application/component defaults < config-file values < CLI overrides
```

Each layer is merged onto the one above using
`json::v1::merge_overlay`. After merging, the result is validated against
the schema once and converted to the typed options struct via
`json::v1::convert::from_json_object<T>(...)`.

Configuration fragments are **not** complete configurations. They are
overlays. Do not validate config-file values, CLI overrides, or application
defaults in isolation:

- Missing values are expected and should be filled by lower-precedence
  defaults.
- Users should not have to specify values that are already assumed by the
  library or application.
- Unknown keys and invalid values are rejected when they survive into the
  final effective object.
- Code below the configuration boundary receives concrete generated C++ option
  types, not raw JSON and not partially populated JSON DOMs.

The merge is configuration-style, **not** RFC 7396 JSON Merge Patch:

- Object members merge recursively (the override's keys win on conflict,
  unspecified keys are kept from the base).
- Scalar and array overrides **replace** the base value entirely; there is
  no automatic array append.

| Layer                 | Source                                                     |
|-----------------------|------------------------------------------------------------|
| Schema defaults       | IDL `= value` initialisers, materialised by `json::v1::schema_default_values` |
| Application/component defaults | C++ code (e.g. `tcp_default_options()` in `connection_factory/tcp.h`) |
| Config-file values    | `parse_file(path)` then `apply_schema_overlay`             |
| CLI overrides         | Parsed by the caller into a `json::v1::object` and passed as the last argument |

`json::v1::apply_schema_overlay` (in `json/json_utils.h`) is the all-in-one
helper. The connection factories wrap it with their typed materialisers
(`materialise_tcp_options` etc.) so a single boundary call gives you a
typed struct ready to use.

## Null semantics

In overlay layers, JSON `null` follows the selected `overlay_null_policy`.
The default policy is `ignore`, so a `null` override means "not supplied" and
does not force users to restate default values.

If a `null` value reaches typed conversion, the exact behaviour depends on the
field's declared type:

- **`rpc::optional<T>` field** — `null` clears the optional (no value).
  Field absent ⇒ also no value. Field present with a T-typed value ⇒
  populated.
- **`json::v1::object` passthrough field** — `null` is a legitimate JSON
  value; the field holds `null`. Only absence triggers the IDL default /
  required error.
- **Other field types** — `null` is treated the same as field absence:
  the IDL default applies if one was declared; otherwise the converter
  throws `json::v1::config_error`.

`overlay_null_policy` in `json/json_utils.h` lets configuration tools choose
whether an override `null` should be ignored (default), kept, or erase the
underlying field. Most callers want the default `ignore` behaviour.

## IDL default translation policy

The IDL parser captures the verbatim text after `=`. The schema
generator translates a subset of those expressions to a JSON literal so
external clients (MCP, json-schema validators) can see them:

| IDL form                                          | JSON output            |
|---------------------------------------------------|------------------------|
| `true` / `false`                                  | `true` / `false`       |
| `nullptr` / `NULL` / `std::nullopt`               | `null`                 |
| `"literal"`                                       | `"literal"`            |
| Numeric literal (decimal, with C++ suffixes stripped) | the number         |
| `A::B::value` on a field of enum type `A` (or `B`) | `"value"`             |
| `{}`, function call, expression, hex literal, scoped int constexpr | omitted |

Untranslatable defaults are silently dropped from the schema; the C++
struct and converter still apply the original IDL expression. Schemas
remain self-consistent because the converter's else-branch and the
schema's `default` always agree by construction (or neither exists).

A field with **any** IDL default — translatable or not — is omitted
from the schema's `required` list. The converter's "missing key" branch
either applies the IDL fallback or, when used together with overlay,
the schema default.

When a default lives on a struct- or enum-typed field, the schema emits
`{"default": value, "allOf": [{"$ref": ...}]}` rather than putting
`default` next to `$ref`. Draft-07 processors ignore siblings of `$ref`,
so the `allOf` wrapping is what makes the default visible to external
validators. `json::v1::schema_default_values` already walks `allOf`
recursively, so the in-tree overlay is unaffected.

## `rpc::variant` JSON wire format

`rpc::variant<T1, T2, ...>` serialises as a JSON object with a single
key naming the active alternative:

```json
{ "<tag>": <value> }
```

Tag names are derived from the alternative's IDL type:

- Primitives use canonical short names: `"bool"`, `"int8"`...`"int64"`,
  `"uint8"`...`"uint64"`, `"float"`, `"double"`, `"string"`.
- IDL struct and enum types use their unqualified name.
- Anything else (template instantiations, `json::v1::object`, typedef
  aliases) is rejected at code-generation time; structs that would have
  contained such variants are filtered out of converter emission.

The schema emits `oneOf` of one-key objects keyed by tag, with `not: {}`
on any variant the runtime cannot tag — fail-closed so a non-runtime
schema document can't quietly accept arbitrary input.

The clean break from the older index-keyed `{"caseN": value}` shape
means any persisted JSON variant data needs reformatting from `caseN`
to `<tag>`. The YAS JSON path and the DOM-based converter both follow
the same shape.

## Per-connection validation cost

Connection-factory entry points that take `json::v1::object` overlay defaults,
validate the effective JSON, and convert exactly once per call (via
`materialise_tcp_options` and its siblings), then dispatch to the typed-options
overload of the same factory. Internal helpers (`ensure_service`,
`connect_stream`, `connect_rpc_stream`, `configured_name`,
`transport_options`, ...) operate only on the typed struct and never re-walk
the schema.

Public factories that already accept a typed
`rpc::connection_factory_config::stream_factory_options` skip the
validation step entirely; structural validity is guaranteed by the
struct's type.

## Typed envelopes with app-owned JSON

Some transports need a shared root configuration that contains opaque
application fragments. The root object should still be an IDL-defined
struct so the runtime-owned fields, defaults, schema, converter, and
fingerprint all come from the same declaration. App-owned fragments can
remain `std::map<std::string, json::v1::object>` or another explicit
passthrough field inside that typed root.

The runtime validates and converts only the root envelope it understands.
Each application validates its own JSON fragment later against that
application's IDL schema. Resource checks that are security policy, such
as maximum app count, nesting depth, key length, or total byte budget,
belong in C++ after the root has been materialised.

If the runtime does not own any fields beyond dispatching applications,
do not introduce a generated wrapper only to hold the map. The boundary can
be the map itself: the host converts the JSON object to
`std::map<std::string, json::v1::object>`, the runtime applies only its
resource limits, and each application uses its private IDL to validate and
deserialize its own entry.

## Adding a new options IDL

The same boundary pattern applies to connection factories and any component
that exposes JSON configuration. To add a new options group:

1. **Declare the IDL.** Create
   `interfaces/<name>/<name>.idl` with a struct holding the fields you
   need. Prefer concrete fields with `= value` defaults for values the
   component always assumes; use `rpc::optional<T>` only for fields that are
   genuinely nullable or absent as part of the domain model. Example:

   ```idl
   namespace canopy { namespace mything {
       struct mything_options
       {
           rpc::optional<std::string> endpoint;
           rpc::optional<uint16_t> port;
           rpc::optional<bool> enable_tracing;
           uint64_t retry_timeout = 5000;
       };
   } }
   ```

2. **Wire `CanopyGenerate`** in the component's `CMakeLists.txt` so the
   IDL is built. Enable at minimum `yas_json`. The build will produce
   `<name>.h`, `<name>_schema.h`, and the JSON schema document under
   `generated/json_schema/`.

3. **Write a default-options helper only for real application policy.** If the
   component defaults are already expressed in the IDL, prefer converting a
   default-constructed generated object with `to_json_object` rather than
   spelling the same values again in C++. A handwritten default overlay is only
   for application/component defaults that intentionally supplement the schema.
   This object may be partial; it is an overlay layer, not a complete
   configuration:

   ```cpp
   inline const json::v1::object& mything_default_options()
   {
       static const json::v1::object options = []
       {
           return json::v1::object(
               json::v1::map{{"endpoint",
                              json::v1::map{{"host", std::string("127.0.0.1")},
                                            {"port", uint16_t{8080}}}}});
       }();
       return options;
   }
   ```

4. **Add a materialiser** that runs the overlay, validates the effective
   object, and converts:

   ```cpp
   struct materialise_mything_options_result
   {
       int error_code{rpc::error::OK()};
       canopy::mything::mything_options options;
   };

   inline materialise_mything_options_result materialise_mything_options(
       const json::v1::object& client_options)
   {
       try
       {
           const auto schema = json::v1::parse(
               canopy::mything::mything_options::get_schema(rpc::encoding::yas_json));
           const auto effective = json::v1::apply_schema_overlay(
               schema, mything_default_options(), client_options);
           return {rpc::error::OK(),
                   json::v1::convert::from_json_object<canopy::mything::mything_options>(
                       effective)};
       }
       catch (const std::exception&)
       {
           return {rpc::error::INVALID_DATA(), {}};
       }
   }
   ```

5. **Public entry points** take either the typed options directly or a
   raw `json::v1::object`. The raw variant calls the materialiser and
   delegates to the typed overload.

That's it. The schema is built from the IDL automatically; defaults
flow through `schema_default_values`; the converter is generated
alongside the IDL header. No hand-written DOM walks are needed for
the common case.

## Pointers to the implementation

- `c++/subcomponents/json/include/json/config_loader.h` — boundary
  helpers (`load_typed_config`).
- `c++/subcomponents/json/include/json/json_utils.h` — overlay
  (`merge_overlay`, `schema_default_values`, `apply_schema_overlay`).
- `c++/subcomponents/json/include/json/convert.h` — DOM to typed
  converter (`from_json_object<T>`, `to_json_object`).
- `c++/subcomponents/json/include/json/schema_validator.h` —
  Draft-07-ish schema validator.
- `generator/src/json_schema/generator.cpp` — IDL to JSON schema
  emitter, including the IDL default translator.
- `c++/connection_factory/include/connection_factory/options.h` and
  `tcp.h` — connection factory boundary helpers (the materialisers).
