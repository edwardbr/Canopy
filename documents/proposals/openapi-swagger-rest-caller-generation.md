# OpenAPI/Swagger REST Caller Generation

This proposal has been partially implemented. The current developer-facing
documentation is now:

- [REST Clients](../rest.md)

The implemented path is:

- explicit tab-separated REST metadata
- `CanopyGenerate(... yas_json rest_client <metadata-file> ...)`; `rest_client`
  is the single current CMake keyword for REST caller metadata
- generated `Interface::rest_caller`
- common `canopy::rest::connection_settings`
- common REST connection factory support for TCP/TLS stream construction
- generated REST callers that derive from `rpc::base`, so they can be held in
  `rpc::shared_ptr<Interface>` and exported to other Canopy zones
- TLS peer verification defaults to enabled for REST connection settings
- provider OpenAPI/Swagger JSON can be combined with Canopy overlay JSON during
  conversion, while generated `.idl` and `.rest.meta` files may be committed as
  the shareable Canopy ABI for offline clients

Remaining proposal items include fuller OpenAPI/Swagger conversion coverage,
YAML/external `$ref` handling, auth-schema generation, multipart upload,
polymorphic schemas, and generated REST server dispatch.

The current composition decision is documented in `documents/rest.md`:
multi-interface REST callers should use `rpc::base` and normal Canopy interface
casting, while OpenAPI body composition is handled separately. Safe `allOf`
schemas should be flattened, clear discriminated `oneOf` schemas may use
`rpc::variant`, and ambiguous `oneOf`/`anyOf`/complex `allOf` should fall back
to `json::v1::object` for broad generated-client builds.
