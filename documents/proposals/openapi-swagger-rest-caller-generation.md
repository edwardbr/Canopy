# OpenAPI/Swagger REST Caller Generation

This proposal has been partially implemented. The current developer-facing
documentation is now:

- [REST Clients](../rest.md)

The implemented path is:

- structured `.rest.json` REST binding files as the canonical binding IR
- `CanopyGenerate(... <idl> ... yas_json rest_client <file> ...)`;
  `rest_client` is the single current CMake keyword for REST caller/handler
  binding data
- OpenAPI/Swagger JSON inputs passed to `rest_client` regenerate the derived
  `.idl` and `.rest.json` before the normal generator runs; the IDL path remains
  the second `CanopyGenerate` argument
- generated `Interface::rest_caller`
- common `canopy::rest::connection_settings`
- common REST connection factory support for TCP/TLS stream construction
- generated REST callers that derive from `rpc::base`, so they can be held in
  `rpc::shared_ptr<Interface>` and exported to other Canopy zones
- generated inbound REST handlers that crack HTTP method, path, query, header,
  and JSON body data before dispatching through the normal generated RPC stub
  path to a local `rpc::base` object
- binding constants for fixed REST binding values such as AWS Query
  `Action`/`Version` or AWS JSON-RPC `X-Amz-Target`, keeping those selectors
  out of the IDL method signature
- TLS peer verification defaults to enabled for REST connection settings
- provider OpenAPI/Swagger JSON can be combined with Canopy overlay JSON during
  conversion, while generated `.idl` and `.rest.json` files may be kept as
  optional review/ABI snapshots for offline clients
- deterministic namespace generation from provider identity, OpenAPI paths, or
  the source directory name when provider metadata is too generic
- `rpc::nullable_optional<T>` for optional nullable JSON fields that need to
  preserve absent versus explicit `null` versus concrete value
- generated caller-handler roundtrip tests and a Schemathesis loopback runner
  for black-box OpenAPI validation without live provider credentials

The motivation is to make REST services first-class typed Canopy interfaces:
direct C++ bindings for HTTP/JSON APIs, local replication of Swagger/OpenAPI
services, gatewaying for components without direct network access, structured
inspection/routing of REST calls, and future translation to other protocols or
serialisation formats such as gRPC, YAS, Protocol Buffers, and nanopb.

Hard requirement: the HTTP/JSON wire shape must match the provider
Swagger/OpenAPI specification. Generated REST code must not add Canopy RPC
envelope fields such as `in` or `out` to REST request or response bodies unless
those fields are explicitly present in the provider schema. Internal RPC
serialisation is an implementation detail and must not leak onto the REST wire.

Generated inbound handlers should not bypass the RPC serialized entry point.
They crack HTTP method, path, query, header, cookie, and body data into the
canonical generated IDL/YAS JSON input, then call the registered object through
`rpc::casting_interface::call(...)`. URL-derived strings are converted into
schema-appropriate JSON values by the REST runtime and generated binding. This
same canonical JSON form is intended to be reusable by future non-HTTP bridges,
including a WebSocket-to-REST bridge.

IDL names and REST wire names are distinct. Generated IDL uses stable
Canopy/C++ identifiers; generated REST code must preserve provider
Swagger/OpenAPI names on the HTTP wire. The structured `.rest.json` binding is
the source for IDL-name to wire-name mappings, including JSON body fields,
nested JSON fields, elided top-level body parameters, query/header/cookie
styles, binary codecs, custom status codes, and provider error objects.

Swagger/OpenAPI defaults are part of the REST conversion contract. Missing
request or response fields with provider defaults should be resolved at the
REST boundary before entering or leaving the canonical IDL/YAS JSON form.
Missing required values without defaults are errors, optional missing values
without defaults remain unset/omitted, and explicit JSON `null` is accepted
only when the schema or IDL type is nullable/optional.

The generated IDL mapping distinguishes the common JSON presence cases:
`rpc::optional<T>` for optional non-nullable values, `rpc::nullable<T>` for
present nullable values, and `rpc::nullable_optional<T>` for optional nullable
values. The nested spelling `rpc::optional<rpc::nullable<T>>` should not be
generated because it is not portable across all Canopy serialisation formats.

Path, query, header, and cookie values should be decoded by common REST runtime
logic using binding-provided OpenAPI `style`, `explode`, and schema metadata.
Declared `in: cookie` parameters are normal IDL parameters. When a service
implementation needs ambient cookie visibility and there is no declared cookie
parameter or request-context mechanism yet, generation may add a reserved
`[in] json::v1::object& _cookie` parameter whose keys and values are the HTTP
cookies for the request.

Documented operation status codes can be represented by generated IDL `error`
types. When Swagger/OpenAPI clearly describes custom success or error statuses,
the generated method should return an operation-specific error type instead of
plain `error_code`; callers map HTTP status codes to that return value and
handlers map returned values back to HTTP status codes. Success response bodies
remain output parameters, with status-aware JSON fallbacks only for genuinely
polymorphic success bodies.

When a success response body is a JSON object with clear top-level properties,
the converter should prefer explicit IDL out parameters for those properties
instead of a synthetic wrapper response object. For example, a provider body
such as `{ "Entities": [...], "Metadata": {...} }` can map to `[out] entities`
and `[out] metadata`. The `.rest.json` binding records the provider wire names,
and generated callers/handlers must still send and receive the exact provider
JSON object on the HTTP wire. Primitive, array, map, opaque, or ambiguous
success bodies should stay as one whole-body out parameter.

Non-JSON and binary bodies should map to `std::vector<uint8_t>` when the
provider schema clearly indicates bytes. OpenAPI `format: binary` is raw HTTP
body bytes; OpenAPI `format: byte` is base64 on the REST wire and byte vectors
inside the IDL. Plain JSON strings remain strings unless the schema gives a
clear binary/base64 signal.

Generated conversion and typed deserialisation are the production validation
path. Full OpenAPI/Swagger validation of raw request/response traffic is useful
for generated-code tests, diagnostics, and firewall/DPI experiments, but should
not be required for normal production use.

Remaining proposal items include fuller OpenAPI/Swagger conversion coverage,
YAML/external `$ref` handling, auth-schema generation, multipart upload,
runtime OpenAPI validation for layer-7 inspection experiments, broader
Schemathesis and caller-handler roundtrip coverage across the third-party
corpus, generated-name rationalisation for operation-selector-heavy specs, and
more complete polymorphic schema support.

The current test direction is generated caller to loopback HTTP request to
generated handler to local `rpc::base` implementation. That tests outgoing and
incoming bindings together without depending on live third-party credentials or
side effects. Schemathesis can also drive the loopback HTTP server from the
effective OpenAPI document. That black-box validation supplements the C++
roundtrip tests rather than replacing them, because it bypasses Canopy's
generated caller and RPC dispatch.

The current composition decision is documented in `documents/rest.md`:
multi-interface REST callers should use `rpc::base` and normal Canopy interface
casting, while OpenAPI body composition is handled separately. Safe `allOf`
schemas should be flattened, clear discriminated `oneOf` schemas may use
`rpc::variant`, and ambiguous `oneOf`/`anyOf`/complex `allOf` should fall back
to `json::v1::object` for broad generated-client builds.
