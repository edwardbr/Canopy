<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# REST Clients

Scope note:

- this document describes the current C++ REST caller support
- generated REST callers are clients for external HTTP/JSON services
- a generated REST caller is also a normal local Canopy object, so it can be
  exported to another Canopy zone through the usual RPC transports

## What Is Generated

`CanopyGenerate(... yas_json rest_client <file> ...)` adds a
`rest_caller` nested class to each interface listed in the REST metadata.

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
```

The generated methods:

- serialise IDL input parameters through the existing YAS JSON proxy serialiser
- build a real HTTP request from REST metadata
- encode path and query parameters
- send JSON request bodies when the operation has a body parameter
- add JSON `Accept`/`Content-Type` headers as required
- apply default REST request settings
- send the request with `streaming_http_client`
- wrap the REST JSON response back into the generated Canopy out-parameter
  envelope before deserialising it

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

## Creating A REST Client Without An LLM

You do not need an LLM to create a REST-backed Canopy object. Treat the external
REST documentation as an engineering input and write two small files:

- a Canopy IDL file that describes the typed interface you want in Canopy
- a `.rest.meta` file that maps each IDL method to the HTTP operation

If you already have a supported OpenAPI/Swagger JSON document, you can use
`CanopyGenerateRest(...)` instead. That shortcut is described below; the manual
workflow is still useful because it shows the exact information the converter
must derive.

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
  - request body schema, if any
  - success response schema, if the operation returns a JSON body
  - which 2xx response body should be treated as the method output, if any
- Authentication and request defaults:
  - headers such as `Authorization` or API-key headers
  - query-string API keys
  - cookies
  - whether a per-request signing step is needed
- Runtime connection requirements:
  - TLS peer verification policy and trust anchors
  - connect and receive timeouts
  - maximum expected response body size
  - whether the build should prefer `tcp_coroutine` or `tcp_blocking`

### Convert That Information Into IDL

Create normal Canopy structs and an interface. Path/query/body inputs become
IDL input parameters. A successful REST JSON response becomes an out parameter.
Operations with no response body, such as a typical `204 No Content` `DELETE`
or metadata-only `HEAD`, should have no `[out]` parameter; the returned
`error_code` is the success/failure result.

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
- Use one out parameter for the successful response body when the operation
  returns one.
- Use no out parameter for no-body responses.
- Use `json::v1::object` only when the REST response is too loose to model
  safely yet.
- Model optional REST inputs as optional IDL inputs only when the IDL supports
  the optional shape you need; otherwise start with the required subset.

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

### Write The REST Metadata

The `.rest.meta` file connects the IDL method to the wire operation. It is
tab-separated, not space-separated.

For the IDL above:

```text
# Canopy REST caller metadata v1
interface	airportsapi::v1::i_airportsapi	airport-web.appspot.com	/_ah/api
method	airportsapi::v1::i_airportsapi	get_airport	GET	/airportsapi/v1/airports/{icao_code}		airport
param	airportsapi::v1::i_airportsapi	get_airport	icao_code	path	icao_code	true
```

The fields are deliberately direct:

- `interface`: `interface`, qualified IDL interface, host, base path.
- `method`: `method`, qualified IDL interface, IDL method, HTTP method,
  operation path, optional body input parameter, optional response out
  parameter.
- `param`: `param`, qualified IDL interface, IDL method, IDL input parameter,
  REST location, wire name, required flag.

For a `POST` with JSON body, put the IDL input parameter name in the body field:

```text
method	example::i_items	create_item	POST	/items	item	created
```

For a no-body operation with no response body, omit both optional method
fields:

```text
method	example::i_items	delete_item	DELETE	/items/{id}
param	example::i_items	delete_item	id	path	id	true
```

The body field on the `method` line is what drives the generated JSON body.
Some converter-generated metadata also includes a matching `param` row with
location `body`; the generator accepts that row and ignores it because the body
has already been identified by the `method` line.

For a query parameter:

```text
param	example::i_items	list_items	limit	query	limit	false
```

### Enable Generation

Add `yas_json` and `rest_client <metadata-file>` to `CanopyGenerate`:

```cmake
CanopyGenerate(
  example_api
  example/example_api.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  rest_client ${CMAKE_CURRENT_SOURCE_DIR}/example/example_api.rest.meta
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR} ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})
```

Build the IDL target. The generated header will contain
`i_your_interface::rest_caller`, and the generated REST implementation will be
compiled into the IDL library.

`rest_client` is a single-value CMake argument whose value is the metadata file
path. It is not a boolean flag, and there is no separate `rest_metadata`
keyword. Unknown `CanopyGenerate` arguments fail at configure time so old or
misspelled REST generation calls do not turn into confusing generator file
errors later.

Generated schema/sample directories from examples or smoke tools are disposable
outputs. REST caller generation consumes the IDL file and `.rest.meta` file,
or the OpenAPI JSON passed to `CanopyGenerateRest(...)`; it does not consume a
`generated_schema/` directory.

### Shortcut From OpenAPI JSON

If the input is a Swagger/OpenAPI JSON file, use `CanopyGenerateRest(...)`.
This runs `tools/openapi_to_canopy_idl.py` during CMake configure, writes the
derived `.idl` and `.rest.meta` beside the JSON spec, then calls
`CanopyGenerate(...)` internally with `yas_json` and
`rest_client <generated-metadata-file>`.

```cmake
CanopyGenerateRest(
  my_api
  my_api/my_api.openapi.json
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR} ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})
```

Input names ending in `.openapi.json` or `.swagger.json` produce the same-stem
`.idl` and `.rest.meta` files. A plain `.json` input also works and produces a
same-stem `.idl`. The JSON spec and converter script are added to CMake
configure dependencies, so changing the spec reruns conversion at configure
time.

### Provider Specs, Overlays, And Committed IDL

The usual source inputs for an OpenAPI-backed endpoint are:

- the provider OpenAPI/Swagger JSON, kept unchanged where practical
- an optional Canopy overlay JSON for Canopy-specific details such as namespace,
  interface name, endpoint correction, or generated-client schema hints
- the generated `.idl` and `.rest.meta` files

For third-party interfaces it is reasonable to commit the generated `.idl` and
`.rest.meta` files. They are generated artifacts, but they are also the Canopy
ABI that other zones or offline clients need in order to call the REST-backed
object without direct internet access to the provider service.

The source-of-truth relationship is still the provider JSON plus any Canopy
overlay. Regenerate the IDL and metadata when either input changes. The
converter supports repeated overlays:

```bash
python3 tools/openapi_to_canopy_idl.py \
  --input third_party_interfaces/rest/1forge-finance-apis/idl/oneforge/oneforge.openapi.json \
  --overlay third_party_interfaces/rest/1forge-finance-apis/idl/oneforge/oneforge.canopy.overlay.json \
  --output third_party_interfaces/rest/1forge-finance-apis/idl/oneforge/oneforge.idl \
  --metadata third_party_interfaces/rest/1forge-finance-apis/idl/oneforge/oneforge.rest.meta
```

`CanopyGenerateRest(...)` currently consumes the JSON spec directly. If an
endpoint needs an overlay, run the converter or the third-party generation
script to refresh the checked-in `.idl` and `.rest.meta`, then use
`CanopyGenerate(... rest_client <metadata-file> ...)` on those files.

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

## REST Metadata

REST metadata is a tab-separated file consumed by the generator. The current
format is intentionally small:

```text
# Canopy REST caller metadata v1
interface	qualified::i_interface	host.example.com	/base/path
method	qualified::i_interface	get_item	GET	/items/{id}		item
param	qualified::i_interface	get_item	id	path	id	true
param	qualified::i_interface	get_item	filter	query	filter	false
```

Line meanings:

- `interface`: qualified Canopy interface name, default host, and default base
  path. An empty base path is valid, but the line must still contain the fourth
  tab-separated field. In practice that means a trailing tab after the host.
- `method`: interface name, IDL method name, HTTP method, operation path, and
  then optional body and output parameter names. If there is neither a request
  body nor a response body, stop after the operation path. If there is a
  request body but no response body, include only the body parameter. If there
  is no request body but there is a response body, leave the body field empty
  with two adjacent tab separators before the output parameter.
- `param`: interface name, IDL method name, IDL input parameter name, REST
  location (`path`, `query`, or converter-emitted `body`), wire name, and
  required flag. `body` rows are accepted for converter output but the generated
  request body is selected by the `method` line's body parameter field.

An empty or omitted body parameter means the HTTP request has no JSON body. An
omitted output parameter means the generated IDL method has no `[out]`
parameter and the REST caller returns `rpc::error::OK()` after any 2xx HTTP
response.

Example from `third_party_interfaces/rest/airportsapi`:

```text
interface	third_party_interfaces::airportsapi::v1::i_airportsapi	airport-web.appspot.com	/_ah/api
method	third_party_interfaces::airportsapi::v1::i_airportsapi	get_airport	GET	/airportsapi/v1/airports/{icao_code}		airport
param	third_party_interfaces::airportsapi::v1::i_airportsapi	get_airport	icao_code	path	icao_code	true
```

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
  rest_client ${CMAKE_CURRENT_SOURCE_DIR}/airportsapi/airportsapi.rest.meta
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR} ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})
```

`rest_client` requires `yas_json`. REST-enabled IDL targets link the generated
library to the REST support libraries needed by the generated caller.

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

- The low-level generator consumes explicit REST metadata. `CanopyGenerateRest`
  is the supported CMake wrapper for JSON OpenAPI/Swagger inputs; it runs the
  converter at configure time and then invokes the metadata-driven generator.
- YAML specs, remote `$ref`, multipart upload, full polymorphic schema
  modelling, and generated REST server dispatch are not part of the current
  generated caller path.
- Authentication is configured through `default_headers`,
  `default_query_parameters`, `default_cookies`, or `before_send`; there is not
  yet a first-class auth-schema generator.

## REST To-Do

- Add a first-class REST auth subsystem later. The current request injection
  hooks are enough for manual bearer tokens, API keys, cookies, and custom
  signing, but OAuth token acquisition/refresh, JWT creation/signing, generated
  OpenAPI `securitySchemes`, retry-on-401 handling, and shared token caching
  should live behind a common `canopy::rest` abstraction rather than in each
  generated client.
