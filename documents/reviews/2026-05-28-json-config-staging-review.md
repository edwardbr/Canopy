# JSON Configuration & Schema Staging Review

Date: 2026-05-28
Branch: DCAP
Reviewer: Claude (Opus 4.7)
Scope: staged changes introducing JSON configurability, the hand-rolled schema validator, the new `connection_factory/` module, the `rpc::optional` / `rpc::variant` IDL primitives, and the replacement of the `nlohmann/json` and `pboettch/json-schema-validator` submodules.

This review is design-focused. A separate security pass is planned and is **not** covered here.

## What this change set is doing

Three intertwined moves:

1. **JSON-driven configuration boundary.** A canonical layering order:
   schema `default` annotations → component defaults → config-file values → CLI overrides → validate → convert via YAS JSON to a typed IDL struct.
   Implemented in `c++/subcomponents/json/include/json/config_loader.h`,
   `json_utils.h`, and `config.h`.
2. **New `connection_factory/` module.** Unifies TCP / SPSC / io_uring stream-and-RPC wiring behind helpers that accept either raw `json::v1::object` *or* the IDL-generated `rpc::connection_factory_config::stream_factory_options`. Adds opaque handle types (`listener_handle`, `rpc_connection_handle`, `stream_accept_handle`).
3. **Submodule removal** of `nlohmann/json` and `pboettch/json-schema-validator`, replaced by hand-rolled, SGX-friendly equivalents:
   `json/json_dom.h`, `json/schema_validator.h`, plus new IDL primitives `rpc::optional<T>` (`c++/rpc/include/rpc/internal/optional.h`) and `rpc::variant<...>` (`c++/rpc/include/rpc/internal/variant.h`).

The intent is good and the layering is internally consistent. Several design choices below are worth revisiting before the surface ossifies.

## Significant design issues

### 1. The two-overload API in `connection_factory` undercuts the typed-config goal

Every helper in `connection_factory/include/connection_factory/options.h`, `tcp.h`, `stream_rpc.h`, `service.h`, and `handles.h` has near-identical `json::v1::object` and `stream_factory_options` overloads — `validate_options`, `make_effective_options`, `configured_name`, `transport_options`, `encoding_option`, `endpoint_from_options`, `connect_timeout_from_options`, `ensure_service`, `accept_rpc`, `connect_rpc`, `accept_rpc_stream`, `accept_rpc_listener`, `connect_rpc_stream`, etc.

`config_loader.h` explicitly says:
> Code below this boundary should accept the typed options object rather than repeatedly probing raw JSON.

The connection_factory code then violates that boundary all the way down.

**Recommendation:** one boundary helper, `load_typed_config<stream_factory_options>(schema, defaults, json_input, cli)`; every public factory takes only `stream_factory_options`. The implementation becomes roughly half the lines, and "where does validation happen" stops being ambiguous.

### 2. Two sources of truth for defaults

`tcp_default_options()` in `connection_factory/tcp.h` and `io_uring_default_options()` in `connection_factory/io_uring_options.h` build a `json::v1::object` from C++ struct defaults at runtime, then `apply_schema_overlay` is asked to merge it with schema `default` annotations, then the result is read back into a struct via `at(...).require<T>()`.

The C++ struct defaults can silently drift from schema `default` annotations.

**Recommendation:** make the schema (driven by IDL `default = …` annotations once those exist) the single source. If C++ struct defaults are kept, add a startup-time assertion that they equal the schema defaults.

### 3. `rpc::variant` JSON encoding is index-keyed

The wire/JSON shape is `{"caseN": value}` where N is the storage index
(`c++/rpc/include/rpc/internal/variant.h:496-501`).
**Reordering alternatives in an IDL silently breaks compatibility.**
Today nothing in the codebase prevents that.

**Recommendation:** either

- key by the alternative's IDL type tag (e.g. `{"endpoint_options": …}`) — needs the generator to emit a name table; or
- document a "variant alternatives are append-only, never reorder" rule and add a stable-index assertion in the generator output.

### 4. DOM equality and schema equality disagree on numbers

`object::operator==` in `json/json_dom.h` compares the underlying `std::variant<int64_t, uint64_t, double>` directly, so `1` (int64) ≠ `1.0` (double). `schema_validator::schema_equal` in `json/schema_validator.h` calls `number_equal`, which treats them as equal.

They are used in different contexts (DOM diffing vs `const`/`enum`/`uniqueItems`), but the divergence is a footgun. The existing test `TreatsJsonNumbersByValueForSchemaEquality` documents the schema-side behaviour; the DOM side will silently disagree.

**Recommendation:** route `object::operator==` for numbers through `number_equal`. DOM equality should match the schema's behaviour.

### 5. Hand-rolled JSON parser/validator: scope is undeclared

The schema validator covers most of Draft 7 (plus Draft 4 `exclusiveMinimum` legacy and some Draft 2019 keys), but omits:

- `format` (date-time, uri, email, ipv4, …)
- `dependencies` / `dependentRequired` / `dependentSchemas`
- `unevaluatedProperties` / `unevaluatedItems`
- `contains` / `minContains` / `maxContains`
- `$dynamicRef` / `$dynamicAnchor`

`$ref` is local-only (`#/...`). None of this is stated in code or docs.

**Recommendation:** pick a draft, add `$schema` to every generated schema, and explicitly document the supported keyword set (and what happens for unsupported keywords — currently silently ignored or accepted).

### 6. Locale and regex fragility

- `number::parse_floating` (`json/json_dom.h`) uses `std::strtod`, which is locale-dependent. On a `de_DE` locale `1.5` could be misparsed.
- `schema_validator::validate_pattern_properties` constructs `std::regex(pattern)` *per validation call* and uses the default ECMAScript-ish flavour without `std::regex::optimize`. For long-lived validators this is wasteful and the regex flavour is not exactly the JSON Schema ECMA-262 dialect.

**Recommendation:** use `std::from_chars` for floats; precompile pattern regexes at validator construction with `std::regex::ECMAScript | std::regex::optimize`.

### 7. Map ordering is non-deterministic in JSON output

`json::v1::map` derives from `std::unordered_map`. The YAS JSON serialiser in `json_dom.h` iterates in hash order; only the protobuf and canonical-crypto writers sort. So `dump(...)` output is not byte-stable across runs or libstdc++ versions.

If any tests, hashes, signatures, or diffs depend on the JSON byte sequence, this will bite.

**Recommendation:** sort keys in the YAS JSON writer (matches the protobuf and canonical-crypto paths) or replace the storage with an order-preserving container.

### 8. Validation cost on hot paths

Most factories call `validate_options(raw)` *and* then `make_effective_options(...)`, which calls `apply_schema_overlay`, which calls the validator again. For configuration-time use this is fine, but `connect_rpc` / `accept_rpc` are per-connection paths — that's a full schema walk per call.

**Recommendation:** validate-and-convert once at the public boundary, pass the typed struct down. (Same as #1.)

### 9. `connection_factory_options_schema()` parses a JSON literal at first call

The generator emits the schema as a string literal that gets re-parsed via YAS at runtime and cached in `static const`. Acceptable, but if you want startup-cost reductions later, consider emitting a `const json::v1::object` builder directly from the generator instead of a JSON string.

## Smaller observations

- `inline namespace v1` is opened in every header, then call sites still write `json::v1::object` everywhere. Either commit to the inline-namespace style (call sites write `json::object`) or drop the inline-namespace and version explicitly — the current mix gives neither benefit.
- `$ref` cycle detection in `schema_validator::validate_ref` silently *accepts* on re-entry. Standard "bottom out at OK", but it interacts oddly with `oneOf`/`anyOf`: a cyclic ref inside a oneOf branch can produce a false positive match.
- `connection_factory/tcp.h` defines `connect_socket_blocking` inline in a header and pulls `<arpa/inet.h>`/`<poll.h>`/etc. into every TU that includes it. Move to a `.cpp` in a non-INTERFACE library.
- `optional`'s JSON load uses `peekch()` after `json_skipws` — fine, but `peekch()` on an empty buffer behaviour should be checked against YAS's implementation.
- The new `schema_cycle` IDL test exercises recursive `$ref` codegen end-to-end — good addition.
- Schema-validator error reporting is path-aware (good), but tests assert `errors().size() >= 4` rather than specific paths/messages. A few tests pinning exact paths/messages would catch regressions in error strings downstream tooling might depend on.
- `protobuf_deserialise` for `map` uses `(*this)[key] = value` (silently overwrites duplicate keys), while `canonical_crypto_read_from` uses `emplace` (returns false on duplicates). Inconsistent — pick one policy.
- `schema_default_values` walks `properties`, `allOf`, `$ref` but **not** `oneOf` / `anyOf` / `if`/`then`/`else`. That is correct (no way to know which branch will match), but means defaults inside `oneOf` alternatives are not auto-injected. Document this.
- The `inline namespace v1` headers all rely on `fmt` (`schema_validator.h`, `json_dom.h`). Confirm that's available in all enclave configurations, since fmt is now load-bearing for JSON error messages.

## Suggested next steps, in order

1. Pick a JSON Schema draft, add `$schema` to every generated schema, and document the supported keyword set.
2. Collapse the dual `json::v1::object` / `stream_factory_options` API surface in `connection_factory/` — only the boundary helper accepts JSON.
3. Establish a single source for defaults (schema `default` is canonical; C++ struct defaults asserted equal at startup, or removed).
4. Decide and document the variant-evolution rule (or change the encoding to type-tag-keyed).
5. Make `object::operator==` and `schema_equal` agree on numeric comparison.
6. Stabilise JSON serialisation by sorting map keys.
7. Cache regexes at validator construction; switch `parse_floating` to `std::from_chars`.
8. Move per-connection validation out of the hot path.
9. Fuzz the hand-rolled YAS-based JSON parser and the schema validator on malformed inputs — this is the highest-risk new surface, and overlaps with the planned security scan.
10. Add `documents/configuration.md` describing the layering, null-policy, and how to add a new options IDL.

## Files inspected for this review

- `c++/subcomponents/json/include/json/config.h`
- `c++/subcomponents/json/include/json/config_loader.h`
- `c++/subcomponents/json/include/json/json_dom.h`
- `c++/subcomponents/json/include/json/json_utils.h`
- `c++/subcomponents/json/include/json/schema_validator.h`
- `c++/subcomponents/json/CMakeLists.txt`
- `c++/connection_factory/CMakeLists.txt`
- `c++/connection_factory/include/connection_factory/handles.h`
- `c++/connection_factory/include/connection_factory/io_uring_options.h`
- `c++/connection_factory/include/connection_factory/options.h`
- `c++/connection_factory/include/connection_factory/service.h`
- `c++/connection_factory/include/connection_factory/stream_rpc.h`
- `c++/connection_factory/include/connection_factory/tcp.h`
- `c++/connection_factory/interfaces/connection_factory_config/connection_factory_config.idl`
- `c++/rpc/include/rpc/internal/optional.h`
- `c++/rpc/include/rpc/internal/variant.h`
- `c++/subcomponents/network_config/interfaces/network_config/network_config.idl`
- `c++/tests/idls/schema_cycle/schema_cycle.idl`
- `c++/tests/unit_tests/json_schema_validator/json_schema_validator_test.cpp`
- `generator/src/json_schema/generator.cpp` (schema emission)
- `.gitmodules` (submodule removals)

## Status update — 2026-05-29

Work that's landed since the original review, with the affected files and the
test fixtures that pin the new behaviour.

### Generator-side JSON converter pass

A new code generation pass emits ADL `from_json_object<T>` / `to_json_object`
overloads for every IDL struct and enum reachable from an IDL, alongside the
existing `get_schema` accessors in the generated `<name>_schema.h`. The
boundary helper described in next-step #2 now exists, even though
`connection_factory/` still exposes the dual API surface; the typed-only
path is available end-to-end through the converters.

- New runtime header `c++/subcomponents/json/include/json/convert.h` with
  ADL overloads for `bool`, all integer widths, `float`, `double`,
  `std::string`, `std::vector`, `std::list`, `std::deque`, `std::set`,
  `std::unordered_set`, `std::array`, `std::map<std::string,V>`,
  `std::unordered_map<std::string,V>`, `rpc::optional`, `rpc::variant`, and
  raw `json::v1::object` passthrough.
- Per-struct `from_json_object`/`to_json_object` emitted via
  `json_schema::write_cpp_convert_accessors` (`generator/src/json_schema/generator.cpp`).
- Per-enum string↔value tables emitted so enum fields participate in the
  same ADL chain.
- Each IDL header now declares the converters as `friend` of every
  non-template struct that gets a converter, granting access to private
  fields (e.g. `rpc::typed_payload`, `rpc::interface_ordinal`). The friend
  emission is gated on the same supportability predicate as the converter
  pass, so a declared friend never references a non-existent ADL overload.
- IDL fields with an `= value` initialiser fall back to that value when the
  JSON key is missing; required fields without a default raise
  `json::v1::config_error("…: required field 'X' is missing")`.
- `rpc::optional<T>` fields are omitted from `to_json_object` when empty
  (matching the field-omission semantics in `<rpc/internal/optional.h>`)
  and read absent-or-null as "no value".
- `json::v1::object` fields treat absence — not `null` — as the missing
  marker, so a non-optional raw-JSON field can legitimately hold `null`.

### Schema and converter contract parity

Several places where schema and converter disagreed were fixed together so
typed JSON validates against its own IDL-generated schema:

- **Map shape.** String-keyed `std::map`/`std::unordered_map` emit
  `{"type":"object","additionalProperties":<V>}` matching the
  converter's JSON-object wire shape. Non-string-keyed maps keep the
  legacy array-of-`{k,v}` schema and are filtered out of converter
  emission.
- **Enum shape.** Enums emit `oneOf [string-name enum, integer enum]` so
  the canonical string form `to_json_object` writes validates against
  the schema. Converters accept either string or integer on read.
- **Set duplicates.** `std::set`/`std::unordered_set` schemas carry
  `uniqueItems: true`; the converters throw on duplicate input rather
  than silently deduplicating.
- **Static members.** Static struct fields no longer appear in
  `properties`/`required` — they were schema-required but the converter
  (correctly) didn't read or write them.
- **Container coverage.** `std::queue`/`std::stack` are filtered out of
  converter emission since `convert.h` has no overloads (they would
  generate uncompilable code otherwise).

### `rpc::variant` JSON wire format — clean break

The wire shape moved from index-keyed `{"caseN": value}` to canonical
type-tag-keyed `{"<tag>": value}` across all three layers (YAS JSON DOM,
`convert.h` DOM, generated schema). This closes the IDL-evolution
footgun originally listed as next-step #4 — alternative reordering is
no longer a silent wire-format break, because the tag is bound to the
alternative's type rather than its position.

- New trait `rpc::variant_alternative_tag<T>` in
  `c++/rpc/include/rpc/internal/variant.h` with primitive
  specializations (`"bool"`, `"int8"`…`"int64"`, `"uint8"`…`"uint64"`,
  `"float"`, `"double"`, `"string"`).
- IDL struct and enum specializations emitted by the synchronous
  generator in the final `namespace rpc { … }` block alongside the
  existing `rpc::id<T>` specializations.
- YAS JSON save/load now uses the trait; the binary path is unchanged.
  The YAS key buffer was widened from 64 bytes to 1024 (matching
  YAS's own object readers) so legitimate long IDL type names round-trip.
- DOM-side `from_json_object`/`to_json_object` use the same trait via
  compile-time recursion through alternatives.
- The schema generator emits `oneOf` alternatives keyed by the canonical
  tag; both the global schema generator and the per-function schema
  generator share a `variant_alternative_tag_for` /
  `variant_alternatives_have_unique_tags` helper.
- Tag resolution is constrained to runtime-supported types: only the
  primitive table or actual IDL-defined non-template struct/enum names
  succeed. `json::v1::object`, typedef aliases (`int`, `error_code`),
  and template instantiations (`std::vector<int>`) are explicitly
  rejected so the schema can never claim a tag the runtime has no
  specialization for.
- When an alternative cannot be tag-resolved, the schema emits
  `{"not": {}}` (fail-closed) rather than an empty permissive schema,
  and the enclosing struct is filtered out of converter emission.

### Cross-IDL schema includes

Generated `_schema.h` headers now `#include` the schema header of every
imported IDL. Without this, the ADL overload for a cross-IDL type
(e.g. `rpc::zone_address_args` referenced from `network_config`) was
declared but unresolved.

### Connection-factory parity helpers

The `connection_factory` boundary now has both a JSON-object and a typed
`stream_factory_options` overload for every helper, with a shared
implementation. Next-step #2 (collapse the dual API) remains open as a
deliberate design call by the project owner — late-binding JSON parsing
needs a `json::v1::object` entry point so the discriminator can be read
before the typed conversion. The generator pass makes the typed path
zero-cost, so further collapse is a separable decision.

### Test fixtures

`json_schema_metadata_test` is the converter+schema parity test target.
It now covers, end-to-end, the following converter behaviour:

- `optional` round-trip including absent / null / explicit value
- `optional` field-omission in `to_json_object`
- nested optional struct parity vs. the schema
- raw `json::v1::object` accepting `null` (not "missing")
- IDL `= default` fallback
- required-missing throws (string, vector)
- recursive `$ref` struct via ADL
- enum round-trip by name, IDL-default for enum field
- enum schema accepts the string form `to_json_object` writes
- `rpc::variant` tag-keyed round-trip and schema validation
- legacy `caseN` shape rejected by both converter and schema
- set/unordered_set round-trip, `uniqueItems` schema, duplicate-input
  rejection, schema-side rejection of duplicates
- string-keyed map round-trip and schema validation
- map schema agreement with converter wire shape

Total: ~36 tests in this target, plus the original 10 schema-validator
tests. Full repo build and `serialiser_test` + `rpc_test` also stay
green after the wire-format change.

### Correctness & determinism follow-ups (second pass)

- **DOM `==` numeric parity (#5).** `number::operator==` now compares by
  mathematical value rather than active variant alternative: `1` (int64),
  `1u` (uint64), and `1.0` (double) all compare equal in the DOM, matching
  what `schema_equal` already did. Strict identity-of-representation is
  still available via `number::get_type()` and the typed accessors. Test:
  `JsonConvert.DomNumericEqualityIsLenientAcrossSignedness`.
- **Deterministic JSON output (#6).** The YAS JSON map writer now sorts
  keys alphabetically, matching the protobuf and canonical_crypto paths
  that already sorted. `dump()` output is byte-stable across runs and
  standard-library versions. Test:
  `JsonConvert.JsonDumpIsKeySortedAndStable`.
- **Locale-safe float parse (#7a).** `number::parse_floating` prefers
  `std::from_chars<double>` when available (libstdc++ 11+ / libc++ 14+ /
  modern MSVC), falling back to the older `std::strtod` path on toolchains
  without floating-point `from_chars`. `from_chars` is locale-independent,
  so JSON `1.5` no longer misparses on locales whose decimal point is `,`.
- **Regex support removed (originally #7b).** A first pass added a
  per-validator regex cache, but a follow-up confirmed that the
  IDL-to-schema generator never emits `pattern` or `patternProperties`
  and the runtime cost of `std::regex` plus its thread-safety
  implications outweighed the value of supporting hand-written
  pattern-bearing schemas. The validator now rejects schemas that use
  either regex keyword with a clear "not supported" error, and the
  `<regex>` and `<unordered_map>` includes are gone. See the resolved
  P2 entry below for the full story.
- **`$schema` annotation (#1).** Top-level generated schemas already
  emitted `"$schema": "http://json-schema.org/draft-07/schema#"`. The
  per-function input/output parameter schemas
  (`function_info::in_json_schema` / `out_json_schema`) now also emit
  this annotation so all generated schemas declare their dialect.

### Known regressions and follow-ups from the second-pass review

External review found four issues with the second-pass changes; all four
are real and have not yet been fixed. Each is captured here so it can be
picked up in a later round.

- **(P1) SGX enclave build broken by the regex cache — fixed.** The
  three references to `regex_cache_` in `schema_validator.h`
  (`set_root_schema(object)`, `set_root_schema(string_view)`,
  `validate()`) are now gated on `#if !defined(FOR_SGX)`. The SGX path
  uses a `validation_context` constructed without the regex cache;
  pattern-using schemas already report a "regex not available"
  validation error there. `cmake --build build_debug_coroutine_sgx_sim
  --target transport_sgx_coroutine_enclave` now succeeds.
- **(P2) Mixed integer/floating equality is lossy near the double
  mantissa limit.** The current pattern in `number::operator==` (and
  the corresponding helper inside `schema_validator::number_equal`)
  falls back to `as_double()` when the two sides have different active
  types. For values above 2⁵³ (e.g. `9007199254740993 == 9007199254740992.0`)
  the double conversion silently rounds and the comparison reports
  equality. Affects DOM equality and schema `const`/`enum`/`uniqueItems`.
  Fix: range-check the integer side against `2^53` before falling
  through to double, and factor the comparison into a shared helper so
  the DOM and schema paths can't drift again.
- **(P2) Regex cache concurrency — resolved by removing regex support.**
  After confirming that no IDL anywhere emits `pattern` or
  `patternProperties` and the generator has no code path that would, the
  validator no longer supports the regex keywords. The cache, the
  `mutable` member, the `<regex>` / `<unordered_map>` includes, the
  `get_or_compile_regex` helper, and the SGX gating that existed solely
  for regex are all gone. Hand-written schemas that use either keyword
  now produce a clear "`<keyword>` is not supported" validation error so
  the contract is explicit rather than silently dropped. Tests:
  `JsonSchemaValidator.PatternKeywordIsRejectedAsUnsupported`,
  `JsonSchemaValidator.PatternPropertiesKeywordIsRejectedAsUnsupported`,
  `JsonParserRobustness.RegexKeywordsInSchemaAreReportedNotCrashed`.
- **(P3) `parse_floating` strtod fallback is still locale-sensitive.**
  On toolchains without floating-point `std::from_chars` the code path
  falls back to `std::strtod`, which honours `LC_NUMERIC` despite the
  surrounding comment. Fix: use `strtod_l` with a cached classic locale
  on POSIX (`_strtod_l` on Windows) or an RAII `std::setlocale` scope.

### Schema-default reconciliation (#3) — landed

The IDL `= default_value` text is now translated to a JSON literal and
emitted as `"default": <value>` on the field's schema. The same IDL
expression is still pasted verbatim into the C++ struct (member
initialiser) and the converter's fallback (else branch), so a single
source — the IDL — produces all three.

Translation policy in
`generator/src/json_schema/generator.cpp:translate_idl_default_to_json`:

| IDL form                       | JSON output            |
|--------------------------------|------------------------|
| `true` / `false`               | `true` / `false`       |
| `nullptr` / `NULL` / `std::nullopt` | `null`            |
| `"literal"`                    | `"literal"`            |
| Numeric literal (`0`, `8080`, `1.5`, `0.5f`, `1e3`, with C++ suffixes) | the number |
| `A::B::value` (enum constant)  | `"value"`              |
| `{}`, function call, expression, hex literal | omitted  |

Untranslatable defaults are silently skipped — the C++ struct and
converter still honour them, just not the schema. That keeps the
"single source" promise without forcing the IDL into a JSON-only
subset.

**`required` rule.** A field with any IDL default (translatable or
not) is no longer listed in `required`. The converter's behaviour and
the schema's contract now agree: a missing key triggers either the
converter's IDL fallback or the overlay's schema default, and the
field stays accepted by validation either way.

Tests in `c++/tests/json_schema_test/json_convert_test.cpp`:

- `SchemaCarriesDefaultsTranslatedFromIdl` — `tcp_endpoint.port = 0`
  and `tcp_endpoint.family = ip_address_family::ipv4` show up as
  `"default": 0` and `"default": "ipv4"`; `addr = {}` is omitted.
- `FieldsWithIdlDefaultsAreNotMarkedRequired` — `tcp_endpoint`'s
  `required` list is just `["name"]`, the only field without a default.
- `SchemaDefaultsOverlayPopulatesIdlDefaults` —
  `json::v1::schema_default_values` walks the generated schema and
  populates a config; `apply_schema_overlay` + converter produces the
  expected typed result.
- `SchemaDefaultsAndConverterFallbackAgree` — converting the same
  user input with and without overlay produces the same typed struct;
  the three layers (C++ defaults, converter fallback, schema overlay)
  cannot drift.

**Tightening pass (post-review).** Three follow-on issues were found in
the first cut and fixed:

- *Scoped integer constants no longer translate as enum strings.* The
  earlier translator turned any `A::B`-shaped default into the JSON
  string `"B"`, which was correct for enum constants like
  `ip_address_family::ipv4` but wrong for scoped `static constexpr`
  values like `default_values::version_3` on a `uint8_t` field —
  validators would reject the value because `"version_3"` doesn't
  satisfy `"type": "integer"`. The translator now takes the field's
  declared type and the IDL's enum-name set and only translates the
  scoped form when the field is actually enum-typed. Anything else is
  refused; the C++ path still pastes the original constexpr verbatim.
  Test: `ScopedIntegerConstantIsNotTranslatedAsEnumName`.
- *Strict JSON-literal validation.* Earlier character-class checks
  passed candidates like `+1`, `.5`, `1.f`, and `1+2` that would have
  been written through `write_raw_property` and produced invalid JSON.
  Two strict validators (`is_strict_json_number`, `is_strict_json_string`)
  now run on every candidate before it lands as a `default`. Anything
  that doesn't match the JSON grammar is refused.
- *`default` beside `$ref` would be ignored by Draft-07 processors.*
  When a struct/enum-typed field carries a default, the schema now
  emits `{default, allOf:[{$ref:…}]}` instead of `{default, $ref:…}`.
  External MCP/schema clients honour the default; `schema_default_values`
  already walks `allOf` recursively so the in-tree overlay path is
  unchanged. Test: `EnumDefaultIsWrappedInAllOfBesideDefault`.

### Per-connection validation cost (#8) — landed

A single `connect_rpc(json::v1::object, ...)` previously walked the
schema validator on the order of 7–10 times per connection because
`validate_options`, `make_effective_options`, `ensure_service`,
`connect_stream`, `connect_rpc_stream`, `configured_name`,
`transport_options`, and `encoding_option` all re-validated the same
options object.

A boundary materialiser
(`rpc::tcp::materialise_tcp_options` in
`c++/connection_factory/include/connection_factory/tcp.h`) now runs
validation + overlay + DOM→typed conversion exactly once. Each public
JSON-taking entry point — `connect_stream`, `accept_stream`,
`connect_rpc`, `accept_rpc` (factory and local-interface variants) —
calls it and delegates to the typed overload of the same factory, so
every downstream helper operates on the typed struct and never re-walks
the schema. The typed-overload helpers' existing `validate_options(typed)`
calls are no-ops.

Tests in `c++/tests/json_schema_test/json_convert_test.cpp`:

- `TcpOptionsMaterialiserAppliesDefaultsAndPreservesOverrides`
- `TcpOptionsMaterialiserRejectsInvalidJson`

### `documents/configuration.md` (#10) — landed

`documents/configuration.md` describes the layering policy, null
semantics, IDL default translation rules, the `rpc::variant` tag-keyed
wire format, per-connection validation cost, and a step-by-step walk
through adding a new options IDL.

### JSON parser fuzz coverage (#9) — landed (CI-runnable subset)

A focused robustness test suite at
`c++/tests/unit_tests/json_parser_robustness/` exercises the
hand-rolled `json::v1::parse` and `schema_validator` against a curated
corpus of well-formed, malformed, and adversarial inputs:

- `WellFormedCorpusParses` — all standard JSON forms accepted.
- `MalformedCorpusThrows` — truncated, bad-number, bad-string, bad-key,
  bad-structural inputs all throw cleanly (no UB, no non-std exception
  propagation).
- `DeeplyNestedInputsTerminate` — nested arrays/objects up to depth
  1024 terminate within 5 seconds.
- `LargeArrayInputsTerminate` — 10 000-element flat arrays/objects
  parse cleanly.
- `SchemaValidationOnPathologicalInputs` — accept-anything schema
  produces a clean validation_result for every probe.
- `RecursiveSchemaCyclesDoNotDivergeOrHang` — self-referential `$ref`
  schemas terminate via the active-refs short-circuit; structural
  validation still works.
- `MaliciousPatternRegexIsRejectedNotCrashed` — invalid regex in a
  schema `pattern` is reported as a validation error rather than
  throwing.

A full libFuzzer / AFL harness for continuous fuzzing remains a
separate infrastructure piece; the suite above runs in CI on every
build and pins the parser's robustness contract.

### Migration note for callers

Any persisted JSON that contains `rpc::variant` values needs reformatting
from `{"caseN": value}` to `{"<tag>": value}`. Configuration files and
on-the-wire JSON between Canopy peers using YAS JSON must update
together; the binary YAS path is unaffected.
