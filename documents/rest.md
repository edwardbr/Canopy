<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# REST Clients And Handlers

Scope note:

- this document describes the current C++ REST caller and inbound handler support
- generated REST callers are clients for external HTTP/JSON services
- a generated REST caller is also a normal local Canopy object, so it can be
  exported to another Canopy zone through the usual RPC transports
- generated REST handlers expose a local Canopy object through HTTP/JSON
  request cracking

## Why REST Integration Matters

REST integration is not just a convenience wrapper around HTTP. The generated
REST caller and handler path gives Canopy a typed bridge between provider
Swagger/OpenAPI contracts and normal C++ RPC interfaces.

The practical benefits are:

- Direct C++ bindings to REST services. Application code can call a generated
  Canopy interface instead of hand-building URLs, headers, JSON bodies, and
  response parsers.
- Replication of remote services from Swagger/OpenAPI files. A provider
  specification can generate both a REST-backed caller and a loopback/local
  handler, which is useful for gateways, tests, local substitutes, and offline
  development.
- Network boundary control. A component without direct network access can call a
  local Canopy interface while a gateway component owns the outbound REST stream,
  credentials, policy, and lifecycle.
- Deep packet inspection, routing, and request parsing. Generated REST handlers
  crack HTTP method, path, query, header, cookie, and body data into a typed
  Canopy call boundary, so policy or routing layers can inspect structured
  operations instead of raw HTTP text.
- Protocol upgrade paths. Once an external REST service is represented as a
  Canopy interface, the same typed contract can be exported over other Canopy
  transports and can later be mapped to protocols such as gRPC.
- Serialization translation. REST JSON can be converted at the boundary into
  Canopy's internal representations and then moved through YAS, Protocol
  Buffers, nanopb, or other supported formats where those formats are enabled.

## REST JSON Wire Contract

The HTTP/JSON wire shape is defined by the provider Swagger/OpenAPI
specification, or by the OpenAPI document generated from an IDL-led service.
Generated REST callers and handlers must not add Canopy RPC envelope fields to
the HTTP request or response body.

In particular:

- request bodies must match the operation request-body schema exactly
- response bodies must match the selected 2xx response schema exactly
- synthetic Canopy RPC fields such as `in`, `out`, parameter envelopes, or
  method-call envelopes must not appear on the REST wire unless those fields are
  explicitly present in the Swagger/OpenAPI schema itself
- if the REST schema contains a field named `in`, `out`, or any other name that
  happens to have meaning inside Canopy RPC, it is treated as an ordinary REST
  field because the provider schema is authoritative
- no-body responses, including `204 No Content` and `205 Reset Content`, must
  produce no response body and no generated `[out]` parameter
- response schemas that explicitly define a JSON body must be represented by an
  IDL `[out]` parameter, regardless of HTTP method
- when a response body is a JSON object with clear top-level properties, those
  properties may be represented as multiple IDL `[out]` parameters; generated
  REST code must still emit and accept the original provider JSON object with
  the provider field names

Internal Canopy RPC serialisation may use whatever generated buffer shape the
RPC stub requires, but that shape is an implementation detail behind the REST
handler and caller. It must not leak into HTTP headers, query strings, paths, or
JSON bodies.

The generated REST adapter has a stable conversion boundary:

```text
HTTP method/path/query/header/cookie/body
  -> generated REST binding
  -> canonical IDL/YAS JSON input
  -> rpc::casting_interface::call(...)
```

Path, query, header, and cookie values are HTTP strings on the wire, but the
REST runtime should convert them into sensible JSON values according to the
generated binding and schema metadata. For example, an integer path component
becomes a JSON number, a boolean query component becomes a JSON boolean, and a
repeated query parameter can become a JSON array when the schema requires an
array. The request body remains the provider-defined JSON body and is inserted
into the generated IDL/YAS input as the configured body parameter.

This canonical IDL/YAS JSON input is the common serialized entry point for
generated REST handlers. It also gives future bridges, such as a WebSocket to
REST bridge, a transport-neutral JSON form that can enter the same
`rpc::casting_interface::call(...)` path without reimplementing per-method
dispatch.

IDL identifiers and REST wire names are separate. The IDL must use stable
Canopy/C++ identifiers, while HTTP paths, query parameters, headers, cookies,
and JSON object fields must use the provider Swagger/OpenAPI names. Generated
REST code must therefore consult the structured REST binding for every
operation and field whose IDL name differs from the wire name. For example, an
IDL field named `engine_serial_number` may still be emitted and accepted as
`engineSerialNumber` on the REST wire.

The structured `.rest.json` binding is the REST binding IR for generated
callers and handlers.

Missing, `null`, and default handling follows the provider schema:

- a required value that is missing and has no Swagger/OpenAPI default is a bad
  REST request
- a missing value with a Swagger/OpenAPI default is defaulted at the REST
  conversion boundary before building the canonical IDL/YAS JSON input, and the
  same rule applies when a caller deserialises a REST response
- an optional value that is missing and has no default remains unset or omitted
- an explicit JSON `null` is accepted only when the schema or IDL type is
  nullable/optional; a default does not silently replace an explicit `null`
  unless the provider schema clearly defines that behaviour
- generated REST callers should preserve the provider wire contract when
  sending: if the provider schema permits omission, optional/defaulted values
  may be omitted on the HTTP wire even though generated deserialisation resolves
  missing defaulted values for local typed use

The current IDL type mapping for JSON presence and nullability is:

- `T`: required and not nullable
- `rpc::optional<T>`: optional and not nullable; the member may be absent but
  explicit JSON `null` is not a separate provider value
- `rpc::nullable<T>`: present but nullable; the containing object or parameter
  supplies presence, and the value itself is either JSON `null` or `T`
- `rpc::nullable_optional<T>`: optional and nullable; the value can be absent,
  present as JSON `null`, or present as `T`

Do not model optional nullable REST fields as
`rpc::optional<rpc::nullable<T>>`. That nested representation loses portability
across generated serialisation formats such as Protocol Buffers and nanopb.
Use the single tri-state `rpc::nullable_optional<T>` type instead.
`rpc::nullable<T>` remains useful for required nullable fields and should not
be removed.

OpenAPI `style` and `explode` settings are part of the binding for path, query,
header, and cookie parameters. Generated endpoint code should not hand-roll URL
or header parsing. The REST runtime owns percent decoding, query parsing, cookie
header parsing, repeated value handling, and schema-driven string-to-JSON
conversion. `.rest.json` should carry enough style, explode, and schema
information for the runtime to convert URL/header/cookie values into the
canonical IDL/YAS JSON input.

Cookies have two supported forms:

- declared OpenAPI `in: cookie` parameters are normal generated IDL parameters
  with `location: "cookie"` binding metadata
- when a service implementation needs ambient cookie visibility and there is no
  declared cookie parameter or request-context mechanism available, the
  generated interface may include a reserved `[in] json::v1::object& _cookie`
  parameter; the object keys are cookie names and the values are cookie values

The reserved `_cookie` parameter is a transitional explicit data path until
thread-local or coroutine-local request context exists. It should be generated
only when the service interface needs cookie visibility and there is no better
Swagger/OpenAPI mapping. It is not part of the provider JSON request body.

Documented HTTP status codes should be reflected in the generated IDL when the
provider schema is clear. For operations with custom success or error status
semantics, the converter may generate an operation-specific `error` type and
use it as the method return type:

```cpp
error create_user_error
{
    ok = 0,
    bad_request = 400,
    unauthorized = 401,
    conflict = 409,
    rate_limited = 429,
    provider_error = 500
};

create_user_error create_user(
    [in] std::string name,
    [in] std::string email,
    [out] std::string& id);
```

The return value is the operation status channel. Success response bodies remain
IDL `[out]` parameters. Generated callers map documented HTTP status codes to
the generated error value. Generated handlers map returned error values back to
HTTP status codes when exposing a local object as REST. If multiple documented
success responses have incompatible bodies, the generator should prefer the
clearest typed representation and fall back to status-aware outputs such as
`_status` plus `json::v1::object` only when the provider schema cannot be
represented cleanly.

Non-JSON bodies are represented explicitly rather than forced through JSON
objects. OpenAPI `type: string, format: binary` and opaque binary HTTP bodies
map to `std::vector<uint8_t>` and are sent or received as raw body bytes.
OpenAPI `type: string, format: byte` maps to `std::vector<uint8_t>` with base64
encode/decode performed at the REST boundary. JSON fields that clearly declare
base64 data, such as `format: byte`, may also map to `std::vector<uint8_t>`;
plain JSON strings remain `std::string` unless the provider schema gives a
clear binary/base64 signal.

Generated REST conversion is the normal production validation boundary. Callers
and handlers should reject values that cannot be converted according to the
generated binding, required/default/nullable rules, schema-driven URL/header
conversion, or binary/base64 codecs. Full OpenAPI/Swagger validation of raw
HTTP traffic is primarily a testing and inspection facility, not a requirement
for normal production calls.

## What Is Generated

`CanopyGenerate(... yas_json rest_client <file> ...)` adds a `rest_caller`,
`rest_handler`, and `rest_handler_info` to each interface listed in the REST
metadata.

For an interface `api::i_service`, the generated header contains:

```cpp
using rest_settings = ::canopy::rest::connection_settings;

class i_service::rest_caller final
    : public rpc::base<rest_caller, i_service>
{
public:
    explicit rest_caller(
        std::shared_ptr<::streaming::stream>&& stream,
        rest_settings settings = rest_settings{});

    static CORO_TASK(::canopy::rest::connect_result<i_service>) connect(
        rest_settings settings,
        std::shared_ptr<rpc::service> service = {},
        ::rpc::connection_factory::context factory_context =
        ::rpc::connection_factory::default_context());
};

class i_service::rest_handler
{
public:
    explicit rest_handler(
        rpc::shared_ptr<i_service> object,
        std::string base_path = {});

    [[nodiscard]] std::string_view base_path() const noexcept;

    CORO_TASK(std::optional<::canopy::rest::server_response>) handle(
        const ::canopy::rest::server_request& request) const;
};

struct i_service::rest_handler_info
{
    using interface_type = i_service;

    [[nodiscard]] static std::string_view default_name() noexcept;
    [[nodiscard]] static std::string_view default_host() noexcept;
    [[nodiscard]] static std::string_view default_base_path() noexcept;
};
```

`CanopyGenerate(... rest_client <openapi.json> ...)` derives the IDL and
`.rest.json` binding from an OpenAPI/Swagger JSON file before calling the same
generator. The normal IDL path is still the second `CanopyGenerate` argument;
`rest_client` is the OpenAPI input only when it points at a `.openapi.json` or
`.swagger.json` file. For an IDL-led server surface,
`CanopyGenerateRestOpenApi(...)` publishes an OpenAPI document from the IDL plus
the REST binding.

The generated methods:

- serialise IDL input parameters through the existing YAS JSON proxy serialiser
- build a real HTTP request from the REST binding
- encode path and query parameters
- send JSON request bodies when the operation has a body parameter
- add JSON `Accept`/`Content-Type` headers as required
- apply default REST request settings
- send the request with `streaming_http_client`
- deserialise the REST JSON response into the generated Canopy out parameter
  without changing the HTTP response body shape

`rest_caller` derives from `rpc::base`, not just the interface. That is what
makes it a local RPC object that can be stored in `rpc::shared_ptr<api::i_service>`
and passed to, or returned from, another zone.

`rpc::base` also supports objects that implement more than one Canopy
interface. That is the right model when an OpenAPI document is split into
multiple resources, tags, or service surfaces: one REST-backed object can
derive from `rpc::base<rest_caller, i_accounts, i_payments, i_reports>`, and
callers can use `rpc::dynamic_pointer_cast` to query the interface they need.
The chosen Canopy interface and method then select the generated JSON
serialisation path for that call.

That interface casting mechanism does not solve OpenAPI schema composition
inside a request or response body. `oneOf`, `anyOf`, and `allOf` describe JSON
value shapes, not RPC object identities.

`rest_handler` is the inbound counterpart to `rest_caller`. It is transport
neutral: a server can adapt its own request type into
`canopy::rest::server_request`, call `Interface::rest_handler::handle(...)`,
and convert the optional `server_response` back into its own HTTP response
type. If the path does not match the REST binding, the handler returns
`std::nullopt` so the caller can try another route.

When a route does match, `rest_handler` cracks the HTTP request into the
canonical generated IDL/YAS JSON input, builds `rpc::send_params`, and calls the
registered object through `rpc::casting_interface::call(...)`. For local
`rpc::base` objects the implementation sees normal typed IDL parameters through
the generated stub path. Any RPC buffer shape needed to make that call is
internal to generated code; the HTTP request body remains the provider-defined
REST body, and the HTTP response body remains the provider-defined REST
response body.

For HTTP servers, prefer registering generated handlers in
`canopy::rest::endpoint_registry` and using the rest subcomponent HTTP adapter
rather than writing per-endpoint HTTP parsing code. The registry can hold
multiple generated handlers for multiple `rpc::base` objects:

```cpp
canopy::rest::endpoint_registry endpoints;
endpoints.add_object("accounts", accounts_object, "/api/accounts");
endpoints.add_object("payments", payments_object, "/api/payments");

auto response = CO_AWAIT canopy::rest::handle_http_request(
    http_request,
    endpoints);
```

The interface type is normally deduced from the `rpc::shared_ptr`. If that is
not enough at a call site, use the explicit form:
`endpoints.add_object<accounts::v1::i_accounts>("accounts", accounts_object,
"/api/accounts")`.

Each registered object carries the effective base path from the generated
metadata when the caller does not override it. The generated handler still owns
route matching, HTTP method matching, JSON decoding, and the call into the RPC
object.

## Creating A REST Client Without An LLM

You do not need an LLM to create a REST-backed Canopy object. Treat the external
REST documentation as an engineering input and write two small files:

- a Canopy IDL file that describes the typed interface you want in Canopy
- a `.rest.json` binding file that maps each IDL method and parameter to the
  HTTP operation

If you already have a supported OpenAPI/Swagger JSON document, `CanopyGenerate`
can derive the IDL and binding from it. That shortcut is described below; the
manual workflow is still useful because it shows the exact information the
converter must derive.

### Information To Collect First

From the REST API documentation, Swagger, or OpenAPI file, collect:

- Service authority:
  - scheme: usually `https`
  - host: for example `airport-web.appspot.com`
  - default port if not standard
  - base path: for example `/_ah/api`
- For each operation:
  - HTTP method: `GET`, `POST`, `PUT`, `DELETE`, and so on
  - operation path relative to the base path
  - path parameters, including exact placeholder names
  - query parameters, including required/optional status
  - header parameters, including required/optional status
  - fixed operation selectors that are part of the REST binding rather than the
    business interface, such as a fixed `Action`, `Version`, or
    `X-Amz-Target` value
  - request body schema, if any
  - success response schema, if the operation returns a JSON body
  - which 2xx response body should be treated as the method output, if any
- Authentication and request defaults:
  - headers such as `Authorization` or API-key headers
  - query-string API keys
  - cookies
  - whether a per-request signing step is needed
  - which values are dynamic credentials or signatures, and therefore must stay
    in runtime settings/auth hooks rather than the IDL
- Runtime connection requirements:
  - TLS peer verification policy and trust anchors
  - connect and receive timeouts
  - maximum expected response body size
  - whether the build should prefer `tcp_coroutine` or `tcp_blocking`

### Convert That Information Into IDL

Create normal Canopy structs and an interface. Path, query, header, and body
inputs become IDL input parameters when the caller supplies them as part of the
business operation. A successful REST JSON response becomes an out parameter
when the provider schema describes a useful response body.

Do not infer response shape from the HTTP method. Some `POST` calls return a
body and some do not; the provider response schema and status code are the
source of truth. `204 No Content`, `205 Reset Content`, and metadata-only
`HEAD` responses should have no `[out]` parameter; the returned `error_code` is
the success/failure result. An explicit success response schema, including an
empty JSON object schema, is still a response body and should be represented by
an `[out]` parameter, usually `json::v1::object` when there are no named
properties. Opaque or intentionally loose JSON responses can also use
`json::v1::object` until the endpoint has a stronger model.

Example:

```idl
#import "rpc/rpc_types.idl"

namespace airportsapi
{
    namespace v1
    {
        error airportsapi_error
        {
            AIRPORT_NOT_FOUND,
            UPSTREAM_ERROR
        };

        struct airport_response
        {
            std::string ICAO;
            std::string last_update;
            std::string name;
            std::string url;
        };

        interface i_airportsapi
        {
            airportsapi_error get_airport(
                [in] const std::string& icao_code,
                [out] airport_response& airport);
        };
    }
}
```

Rules of thumb:

- Keep the Canopy method name stable and C++ friendly.
- Use explicit out parameters for top-level response object properties when
  that maps cleanly to the provider schema. For example, a response body shaped
  as `{ "Entities": [...], "Metadata": {...} }` can become `[out] entities`
  and `[out] metadata`, with `.rest.json` preserving `Entities` and `Metadata`
  as the wire names.
- Use one out parameter for primitive, array, map, opaque, or otherwise
  non-elided successful response bodies.
- Use no out parameter for no-body responses.
- Use `json::v1::object` only when the REST response is too loose to model
  safely yet.
- Model optional REST inputs as `rpc::optional<T>` when omission is the only
  extra state, `rpc::nullable<T>` when a present value may be JSON `null`, and
  `rpc::nullable_optional<T>` when the provider distinguishes absent, explicit
  `null`, and concrete values.

### OpenAPI Composition Policy

OpenAPI schema composition needs a separate decision from the REST connection
and interface generation model:

- `allOf` should be flattened into a generated Canopy `struct` when every branch
  is an object schema and fields do not conflict. Complex `allOf` should fall
  back to `json::v1::object` in broad-generation mode.
- `oneOf` can map to `rpc::variant<Ts...>` when the alternatives have clear,
  unique tags or a discriminator and the generated REST caller knows how to
  render the external REST JSON shape.
- `anyOf` is usually not a good variant fit because more than one alternative
  may match the same JSON value. Prefer `json::v1::object` unless a specific
  endpoint has a stronger rule.

The broad one-shot REST corpus generator should therefore be permissive by
default: flatten safe `allOf`, use `json::v1::object` for ambiguous
`oneOf`/`anyOf`/complex `allOf`, and keep a strict mode that fails when the
schema cannot be represented as a strong Canopy type.

Use `rpc::variant`, not `std::variant`, for IDL sum types. `rpc::variant` is the
Canopy-supported variant type and has generated schema/serialisation support.
It should still be used deliberately for REST schemas: the normal Canopy JSON
representation is a tagged union, while many REST APIs expect a bare JSON
object selected by a discriminator field. When a REST `oneOf` is represented as
`rpc::variant`, the REST caller must preserve the external wire shape rather
than leaking an internal Canopy wrapper into the HTTP body.

### Write The REST Binding

The `.rest.json` file connects IDL methods and parameters to the provider wire
operation. It is the canonical binding IR for generated callers and handlers.

For the IDL above:

```json
{
  "schema": "canopy.rest.binding.v1",
  "interfaces": [
    {
      "qualified_name": "airportsapi::v1::i_airportsapi",
      "host": "airport-web.appspot.com",
      "base_path": "/_ah/api",
      "methods": [
        {
          "name": "get_airport",
          "http_method": "GET",
          "path": "/airportsapi/v1/airports/{icao_code}",
          "parameters": [
            {
              "name": "icao_code",
              "location": "path",
              "wire_name": "icao_code",
              "required": true,
              "style": "simple",
              "explode": false
            }
          ],
          "request": {
            "body_param": ""
          },
          "response": {
            "out_param": "airport",
            "fields": [],
            "success_statuses": [200]
          }
        }
      ]
    }
  ]
}
```

The important fields are:

- `qualified_name`: the generated Canopy interface.
- `host` and `base_path`: default authority values used when settings do not
  override them.
- `name`, `http_method`, and `path`: the IDL method and HTTP operation.
- `parameters`: path, query, header, cookie, and synthetic transport
  parameters. `name` is the IDL name; `wire_name` is the provider name.
- `style`, `explode`, schema/default/nullability fields, and codec fields:
  binding details used by the REST runtime to convert HTTP strings and bodies
  into canonical IDL/YAS JSON.
- `request.body_param`: the IDL input parameter that supplies the request body,
  or empty for no body.
- `response.out_param`: the IDL output parameter that receives the whole
  success response body, or empty when there is no body or when
  `response.fields` maps top-level object fields.
- `response.fields`: top-level response object field mappings. Each item has an
  IDL output `name`, provider `wire_name`, and required/default/nullability
  metadata. Generated callers map HTTP response fields into the canonical RPC
  out parameters; generated handlers map RPC out parameters back to the
  provider response object.
- `constants`: fixed REST binding values such as AWS Query `Action`/`Version`
  fields or AWS JSON-RPC `X-Amz-Target` headers.
- status mappings: documented success and error status codes, used when the
  IDL method has an operation-specific `error` return type.

Fixed operation selectors are binding data, not IDL business parameters. For
example:

```json
{
  "constants": [
    {
      "location": "header",
      "wire_name": "X-Amz-Target",
      "value": "AWSEvents.ActivateEventSource"
    }
  ]
}
```

The generated caller emits constants on every matching request. The generated
handler also checks them before dispatch, which is important for provider
styles that use the same path and method for many operations.

### Enable Generation

Add `yas_json` and `rest_client <binding-file>` to `CanopyGenerate`:

```cmake
CanopyGenerate(
  example_api
  example/example_api.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  rest_client ${CMAKE_CURRENT_SOURCE_DIR}/example/example_api.rest.json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR} ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})
```

Build the IDL target. The generated header will contain
`i_your_interface::rest_caller`, and the generated REST implementation will be
compiled into the IDL library.

`rest_client` is a single-value CMake argument whose value is the binding file
path. It is not a boolean flag, and there is no separate `rest_metadata`
keyword. Unknown `CanopyGenerate` arguments fail at configure time so old or
misspelled REST generation calls do not turn into confusing generator file
errors later.

Generated schema/sample directories from examples or smoke tools are disposable
outputs. REST caller/handler generation consumes the IDL file and `.rest.json`
binding file, or the OpenAPI JSON passed through `rest_client`; it does
not consume a `generated_schema/` directory.

### Shortcut From OpenAPI JSON

If the input is a Swagger/OpenAPI JSON file, pass the IDL path as the normal
second `CanopyGenerate` argument and pass the JSON spec to `rest_client`. This
runs `tools/openapi_to_canopy_idl.py` at build time, writes the derived `.idl`
and `.rest.json` beside the JSON spec, and then generates from the IDL with
`yas_json` and `rest_client <generated-binding-file>`. The generated C++
includes both the outbound `rest_caller` and inbound `rest_handler`, so
Swagger/OpenAPI can drive both the client side and a local REST-compatible
implementation.

```cmake
CanopyGenerate(
  my_api
  my_api/my_api.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  rest_client ${CMAKE_CURRENT_SOURCE_DIR}/my_api/my_api.openapi.json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR} ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})
```

Input names ending in `.openapi.json` or `.swagger.json` produce the same-stem
`.idl` and `.rest.json` files. A plain `.json` binding file is treated as an
already-generated binding and does not trigger OpenAPI conversion. The JSON spec
and converter script are build dependencies, so changing either input
regenerates the IDL and binding before the generated C++ is compiled.

If the Swagger/OpenAPI document does not provide enough stable identity for a
namespace, the converter falls back to the source directory name. This keeps
generated third-party interfaces deterministic even when provider metadata is
generic, missing, or reused across many operations.

### Provider Specs, Overlays, And Committed IDL

The usual source inputs for an OpenAPI-backed endpoint are:

- the provider OpenAPI/Swagger JSON, kept unchanged where practical
- an optional Canopy overlay JSON for Canopy-specific details such as namespace,
  interface name, endpoint correction, or generated-client schema hints
- optional generated `.idl` and `.rest.json` snapshots

For third-party interfaces it is reasonable to commit generated `.idl` and
`.rest.json` snapshots when they are useful for review, offline clients, or ABI
inspection. They are not the primary source of truth. When the provider JSON and
any Canopy overlays are available, the CMake path can regenerate the IDL and
binding from scratch.

The source-of-truth relationship is still the provider JSON plus any Canopy
overlay. Regenerate the IDL and binding when either input changes. The
converter supports repeated overlays:

```bash
python3 tools/openapi_to_canopy_idl.py \
  --input third_party_interfaces/rest/1forge-finance-apis/idl/oneforge/oneforge.openapi.json \
  --overlay third_party_interfaces/rest/1forge-finance-apis/idl/oneforge/oneforge.canopy.overlay.json \
  --output third_party_interfaces/rest/1forge-finance-apis/idl/oneforge/oneforge.idl \
  --binding third_party_interfaces/rest/1forge-finance-apis/idl/oneforge/oneforge.rest.json
```

If an endpoint needs an overlay, keep the overlay beside the OpenAPI file. The
CMake path automatically applies a same-stem `.canopy.overlay.json` file when it
exists.

If the OpenAPI snapshot itself is not committed, the build needs some other
local or network step to obtain the provider spec before `CanopyGenerate` runs.
The `source.json` records in the third-party corpus are provenance metadata, not
compiled REST bindings.

### Publishing OpenAPI From IDL

If the Canopy IDL is the lead interface specification, publish an OpenAPI view
with `CanopyGenerateRestOpenApi(...)` or by running
`tools/canopy_rest_to_openapi.py` directly:

```cmake
CanopyGenerateRestOpenApi(
  my_api_openapi
  my_api/my_api.idl
  my_api/my_api.rest.json
  my_api/my_api.openapi.json
  ${CMAKE_CURRENT_SOURCE_DIR}
  title my_api
  scheme https)
```

This is the reverse direction from OpenAPI-backed generation: the IDL and
`.rest.json` stay authoritative and the OpenAPI JSON is generated for external
users, testing tools, or documentation.

### Create The REST Base Object

The generated `rest_caller` is already a `rpc::base`-derived object. The simple
path is to use the static factory:

```cpp
using api = airportsapi::v1::i_airportsapi;

api::rest_settings settings;
settings.endpoint.scheme = "https";
settings.endpoint.host = "airport-web.appspot.com";
settings.endpoint.base_path = "/_ah/api";
settings.default_headers.push_back({"User-Agent", "Canopy-example/1.0"});

auto connected = CO_AWAIT api::rest_caller::connect(settings, service);
if (connected.error_code != rpc::error::OK() || !connected.object)
    CO_RETURN connected.error_code;

rpc::shared_ptr<api> object = connected.object;
```

`object` is now a local Canopy object that implements `api`. It happens to call
HTTP behind the scenes, but other Canopy code just sees `rpc::shared_ptr<api>`.

Keep the whole `connect_result`, or at least both `connected.object` and
`connected.stream`, in the same owning component. `connected.object` is the RPC
object; `connected.stream` is the underlying REST TCP/TLS stream and should be
closed explicitly during shutdown.

## REST Binding Compatibility

`.rest.json` is the supported REST binding format. The older tab-separated
`.rest.meta` format is no longer generated or consumed by the current REST
generation tools. Do not add `.rest.meta` files to new REST targets.

## CMake

Enable REST generation on the IDL target:

```cmake
CanopyGenerate(
  airportsapi
  airportsapi/airportsapi.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  rest_client ${CMAKE_CURRENT_SOURCE_DIR}/airportsapi/airportsapi.rest.json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR} ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})
```

`rest_client` requires `yas_json`. REST-enabled IDL targets link the generated
library to the REST support libraries needed by the generated caller and
handler.

## Hosting A Local REST Implementation

Use `rest_handler` when you have a local Canopy object and want an HTTP server
to expose it as REST:

```cpp
class echo_service final
    : public rpc::base<echo_service, websocket_demo::rest::v1::i_echo>
{
public:
    CORO_TASK(error_code) echo(
        const std::string& message,
        std::string& response) override
    {
        response = message;
        CO_RETURN rpc::error::OK();
    }
};

rpc::shared_ptr<i_echo> object(new echo_service());

canopy::rest::server_request request{
    "POST",
    "/api/echo",
    R"("hello")"};

websocket_demo::rest::v1::i_echo::rest_handler handler(object, "/api");
auto response = CO_AWAIT handler.handle(request);
```

The concrete HTTP server should only adapt request and response types. The
generated handler owns REST route matching, path and query decoding, JSON body
selection, and conversion into `rpc::send_params`. The object call then goes
through `rpc::casting_interface::call`, which dispatches through the generated
`stub_caller` for the interface.

For an HTTP server, register one or more generated handlers with the common
registry and inject that registry into the server component:

```cpp
canopy::rest::endpoint_registry endpoints;
endpoints.add_object("echo", object, "/api");

auto response = CO_AWAIT canopy::rest::handle_http_request(
    http_request,
    endpoints);
```

The websocket demo uses this dependency-injection pattern in
`c++/demos/websocket/server/main.cpp`: it creates the `i_echo` implementation,
registers it with `canopy::rest::endpoint_registry::add_object(...)`, and then
injects that registry into each HTTP connection. Only the `i_echo`
implementation is demo-specific. The HTTP adapter and endpoint multiplexing
live in the rest subcomponent.

## Connection Settings

Generated callers use `canopy::rest::connection_settings`:

```cpp
airports::i_airportsapi::rest_settings settings;
settings.endpoint.scheme = "https";
settings.endpoint.host = "airport-web.appspot.com";
settings.endpoint.base_path = "/_ah/api";
settings.connect_timeout = std::chrono::milliseconds{10000};
settings.receive_timeout = std::chrono::milliseconds{10000};
settings.default_headers.push_back({"User-Agent", "Canopy-airportsapi/1.0"});
```

Important fields:

- `endpoint.scheme`, `host`, `port`, `base_path`, `ipv6`: REST authority and
  HTTP host construction. Scheme checks are case-insensitive. Port defaults to
  `443` for HTTPS and `80` for HTTP.
- `tls`: peer verification and optional trust anchors. `verify_peer`
  defaults to `true`; OpenSSL builds load the system trust store by default.
  Use `trust_anchor` or `trust_anchor_file` for private certificate
  authorities. Set `verify_peer` to `false` only for explicit local
  diagnostics.
- `base_stream_type`: force a derived base stream such as `tcp_blocking` or
  `tcp_coroutine`. Leave empty to use the best available target in the current
  build.
- `stream_connection`: explicit `rpc::connection_factory::connection_settings`.
  If this contains stream layers, the REST factory uses it instead of deriving
  TCP/TLS layers from the endpoint.
- `default_headers`, `default_query_parameters`, `default_cookies`: request
  defaults injected for every generated call. Cookie names and values are
  validated before the request is sent so configured defaults cannot split the
  generated `Cookie` header.
- `before_send`: last-chance mutator for auth signatures, custom headers,
  tracing, or validation.
- `connect_timeout`, `receive_timeout`, `max_response_bytes`: stream and HTTP
  limits.

`settings` is runtime connection configuration. It is not the same thing as
`rpc::connection_factory::context`.

## JSON Conversion And Schema Processing

Generated REST code uses the normal YAS JSON path internally, but it must still
preserve the provider HTTP/JSON shape at the REST boundary. The shared JSON
conversion helpers support the IDL-facing REST types used by the generator:

- scalar JSON values, `std::string`, integral and floating-point types
- `std::vector`, `std::list`, `std::deque`, fixed-size `std::array`
- `std::set` and `std::unordered_set`; duplicate JSON array items are rejected
  on input rather than silently deduplicated
- `std::map<std::string, T>` and `std::unordered_map<std::string, T>` for JSON
  object maps; JSON object keys are strings, so other key types are not
  generated for JSON map bindings
- `rpc::optional<T>`, `rpc::nullable<T>`, and
  `rpc::nullable_optional<T>` for absence/null/value modelling
- `rpc::variant<Ts...>` for Canopy tagged unions
- `json::v1::object` as the escape hatch for schemas that are intentionally
  loose, ambiguous, or deferred to runtime

`rpc::variant` has a Canopy tagged-object JSON representation. That is correct
for internal YAS JSON, but a REST `oneOf` body may have a different provider
wire shape. When the REST binding represents a provider `oneOf` as a variant,
the generated REST caller/handler must still emit and accept the provider shape
rather than leaking the internal tagged wrapper onto the HTTP wire.

Schema defaults are materialised by the JSON overlay helpers, not by the
validator itself. JSON Schema `default` is an annotation; Canopy applies defaults
at the configuration or REST conversion boundary when that is the selected
policy, then validates the effective object. Schema-aware overlays only seed
defaults into child objects that are actually present or required by the
effective schema, so optional nested objects are not accidentally created just
because their children have defaults.

## Connection Factory Context

`rest_caller::connect(settings, service, factory_context)` takes both
`settings` and `factory_context` because they describe different things:

- `settings` describes the REST endpoint, request defaults, TLS options,
  timeouts, and stream stack.
- `factory_context` supplies process-local connection-factory dependencies:
  custom stream components, SPSC queues, attestation services, or test-only
  scripted streams.

Most production callers pass no context and use
`rpc::connection_factory::default_context()`. Tests and applications with
custom stream components pass a context explicitly.

The `service` argument owns the executor used by stream layers. TLS and some
other layers need that executor for asynchronous cleanup. Production code should
normally pass the service that owns the caller.

## Calling A REST Service

```cpp
auto executor = rpc::make_executor();
auto service = rpc::root_service::create(
    "airportsapi_gateway",
    rpc::zone{rpc::DEFAULT_PREFIX},
    executor);

airports::i_airportsapi::rest_settings settings;
settings.endpoint.scheme = "https";
settings.endpoint.host = "airport-web.appspot.com";
settings.endpoint.base_path = "/_ah/api";
settings.default_headers.push_back({"User-Agent", "Canopy-airportsapi/1.0"});

auto connected = CO_AWAIT airports::i_airportsapi::rest_caller::connect(
    settings,
    service);

if (connected.error_code != rpc::error::OK() || !connected.object)
    CO_RETURN connected.error_code;

airports::airport_response airport;
auto call_error = CO_AWAIT connected.object->get_airport("EDDF", airport);
if (call_error != rpc::error::OK())
    CO_RETURN call_error;

// On application shutdown, close the stream before shutting down the executor.
if (connected.stream)
    CO_AWAIT connected.stream->set_closed();
connected.object = nullptr;
connected.stream.reset();
rpc::shutdown_executor(executor);
```

Use `CO_AWAIT` inside a coroutine. In a plain `main()` or other non-coroutine
entry point, use `SYNC_WAIT(...)` around the same asynchronous call:

```cpp
auto connected = SYNC_WAIT(airports::i_airportsapi::rest_caller::connect(
    settings,
    service));
```

For a complete production smoke example, see:

- `third_party_interfaces/rest/airportsapi/src/remote_main.cpp`
- target `airportsapi_remote`

## Making A REST Client Callable From Another Zone

The pattern is:

1. Generate a REST caller for the IDL interface.
2. Create the REST caller with `rest_caller::connect(...)`.
3. Keep the returned `rpc::shared_ptr<Interface>` alive.
4. Export that pointer through a normal Canopy transport.

From the remote zone's perspective, this is just a Canopy interface. The remote
zone does not know that the implementation is backed by HTTP.

Server/gateway side:

```cpp
using rest_api = airports::i_airportsapi;
using peer_api = my_gateway::i_peer_context;

auto executor = rpc::make_executor();
auto gateway_service = rpc::root_service::create(
    "rest_gateway",
    rpc::zone{rpc::DEFAULT_PREFIX},
    executor);

rest_api::rest_settings rest_settings;
rest_settings.endpoint.scheme = "https";
rest_settings.endpoint.host = "airport-web.appspot.com";
rest_settings.endpoint.base_path = "/_ah/api";

auto rest = CO_AWAIT rest_api::rest_caller::connect(
    rest_settings,
    gateway_service);
if (rest.error_code != rpc::error::OK() || !rest.object)
    CO_RETURN rest.error_code;

rpc::connection_factory::connection_settings listen_settings;
// Fill listen_settings with a stream_rpc transport and listening stream layers.
// See documents/transports/tcp.md and documents/external-project-guide.md.

auto accepted = CO_AWAIT rpc::connection_factory::accept_rpc<peer_api, rest_api>(
    rest.object,
    listen_settings,
    gateway_service);
if (accepted.error_code != rpc::error::OK())
    CO_RETURN accepted.error_code;

// Keep these alive for as long as peers may call the REST-backed object:
// - rest.object
// - rest.stream
// - accepted.listener or accepted.connection
// - gateway_service
// - executor
```

Client/other-zone side:

```cpp
rpc::shared_ptr<peer_api> peer_context(new peer_context_impl());

auto connected = CO_AWAIT rpc::connection_factory::connect_rpc<peer_api, rest_api>(
    peer_context,
    connect_settings,
    client_service);

if (connected.error_code != rpc::error::OK() || !connected.output_interface)
    CO_RETURN connected.error_code;

airports::airport_response airport;
auto error = CO_AWAIT connected.output_interface->get_airport("EDDF", airport);
```

Template parameters follow the normal Canopy convention:

- `accept_rpc<Remote, Local>`: `Remote` is the interface supplied by the
  connecting peer; `Local` is the interface this gateway exports.
- `connect_rpc<In, Out>`: `In` is the caller's local input interface; `Out` is
  the interface returned by the gateway.

Use a small peer-context/noop interface if the gateway does not need callbacks
from the connecting zone.

## Shutdown

For process-owned REST clients, close the underlying stream before shutting
down the executor:

```cpp
if (rest.stream)
    CO_AWAIT rest.stream->set_closed();
rest.object = nullptr;
rest.stream.reset();
accepted.listener.reset();
accepted.connection.reset();
gateway_service.reset();
rpc::shutdown_executor(executor);
```

When the REST object is exported to another zone, stop accepting new peers
first, release remote connection handles/listeners, let remote references drain,
then close the REST stream. This follows the same lifetime rules as any other
local Canopy object exported across a transport.

## Current Limits

- `.rest.json` is the supported REST binding format. JSON OpenAPI/Swagger
  inputs are passed to `CanopyGenerate` with `rest_client <openapi.json>`,
  which runs the converter before invoking the REST generator.
- YAML specs, remote `$ref`, multipart upload, full polymorphic schema
  modelling, and auth policy generation are not part of the current generated
  path.
- Authentication is configured through `default_headers`,
  `default_query_parameters`, `default_cookies`, or `before_send`; there is not
  yet a first-class auth-schema generator.
- Generated REST and Schemathesis loopback tests deliberately strip or bypass
  authentication by default. Real provider authentication, licensing, quota, and
  side-effect control remain application responsibilities.

## Schema Validation And Loopback Tests

The third-party REST tests use local loopback services rather than commercial
provider endpoints. The normal generated test shape is:

```text
generated rest_caller
  -> loopback HTTP request
  -> generated rest_handler
  -> local rpc::base implementation
```

The generated roundtrip tests exercise Canopy's own caller, HTTP request
construction, handler route matching, path/query/header/body cracking, YAS JSON
conversion, RPC dispatch, output marshalling, and no-body handling. They also
validate generated JSON request and response bodies against the schema metadata
embedded in the generated test where that schema is available.

`tools/run_rest_schemathesis.py` provides a black-box OpenAPI validation layer
for the same loopback executables. It builds an effective OpenAPI document from
the provider spec plus any Canopy overlays, strips security requirements by
default, filters unsupported request media unless requested otherwise, launches
the generated loopback server, and runs Schemathesis against it. The default
mode is intentionally light: positive examples/fuzzing, one example per
operation, no coverage phase, and authentication out of scope.

Use Schemathesis failures as evidence that the generated loopback HTTP contract
does not satisfy the effective OpenAPI document. They do not prove that live
commercial endpoints work, because those endpoints usually require credentials,
licenses, quota, provider-specific test data, and side-effect controls.

## REST Plan And To-Do

- Keep the provider OpenAPI/Swagger JSON unchanged where practical. Use Canopy
  overlay JSON only for Canopy-specific namespace, interface, endpoint, or
  schema-hint decisions. The generated `.idl` and `.rest.json` may still be
  committed as review/ABI snapshots, but provider JSON plus overlays are the
  reproducible source of truth.
- Continue expanding generated caller and generated handler roundtrip coverage.
  The primary test shape is:
  generated `rest_caller` -> loopback HTTP request -> generated
  `rest_handler` -> local `rpc::base` implementation. This verifies method
  matching, path/query/header cracking, JSON body marshalling, output
  marshalling, and no-body responses through the same code paths used by real
  clients and servers.
- When a generated endpoint exposes a generator issue, fix it with the smallest
  focused build/test for that endpoint first, then broaden to the whole REST
  corpus after the focused target passes. This keeps the large third-party
  corpus useful without making every iteration expensive.
- Cover the common REST methods explicitly in roundtrip tests, including `GET`,
  `POST`, `PUT`, `DELETE`, and any other method that appears in the provider
  specs. No-body operations must verify that the generated IDL method has no
  fake empty out parameter.
- Treat fixed operation selectors as REST binding constants, not IDL
  parameters. The caller should emit them, and the inbound handler should use
  them for dispatch before deserialising a body.
- Continue using Schemathesis as the optional black-box OpenAPI validation
  layer. It should supplement, not replace, the C++ caller-handler roundtrip
  tests because external validators bypass Canopy's generated caller, YAS
  serialisation, and RPC dispatch when they drive the loopback HTTP server
  directly.
- Add runtime validation against the provider OpenAPI schema later where it is
  useful for layer-7 inspection or firewall proof-of-concept work. The first
  target should be local handler validation with deterministic examples; live
  third-party services often require credentials, signatures, rate limits, or
  side-effect control.
- Improve generated namespace policy for operation-selector-heavy specs. Some
  provider specs encode selectors in paths or pseudo-path fragments, which can
  produce very long Canopy namespaces. A later migration should either split
  those selectors into clearer sub-namespaces or keep selectors out of the
  namespace and rely on method names plus `.rest.json` constants. This will
  change generated type names and should be planned as a deliberate ABI change.
- Improve scalability for very large OpenAPI specs by concentrating shared
  caller/handler logic in the `canopy::rest` target and, where needed, splitting
  generated sources so macro and compile-time limits do not dominate endpoint
  testing.
- Add a first-class REST auth subsystem later. The current request injection
  hooks are enough for manual bearer tokens, API keys, cookies, and custom
  signing, but OAuth token acquisition/refresh, JWT creation/signing, generated
  OpenAPI `securitySchemes`, retry-on-401 handling, and shared token caching
  should live behind a common `canopy::rest` abstraction rather than in each
  generated client.
