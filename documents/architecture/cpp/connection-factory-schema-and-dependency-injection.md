<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Connection Factory Schema And Dependency Injection

This note preserves the durable design intent from the old planning notes that
lived under `c++/connection_factory`. It is background architecture material,
not the source of truth. Validate details against the live CMake files, IDL
files, and implementation before changing behavior.

## Current Shape

The connection factory is the configuration and dependency-injection boundary
for configured transports and stream stacks. Public JSON describes topology
through generic envelopes such as `connection_settings`, `typed_settings`, and
`stream_layer_settings`. The selected component, named by `type`, owns the
schema and typed materialisation of its `settings` object.

This keeps component-specific fields out of the generic connection envelope.
For example, TCP endpoint fields belong to the TCP stream settings type, TLS
fields belong to the TLS stream settings type, and SGX fields belong to the SGX
transport settings type. The connection factory validates and routes the opaque
JSON settings object to the registered component factory, which materialises it
with the component's generated IDL schema.

The factory has four broad component categories:

- Base streams, such as blocking TCP, coroutine TCP, and in-process SPSC queues.
- Stream layers, such as WebSocket, TLS, compression, SPSC buffering, and
  attestation.
- RPC transports, such as stream RPC, local, DLL, IPC/SPSC, and SGX transports.
- Runtime dependencies, such as executor configuration, named SPSC queue pairs,
  attestation services, and other executable-owned services.

Attestation is intentionally split by role. The stream layer configures the
attestation protocol applied to a link. The executable-owned configuration owns
attestation services and named service instances that the layer can reference.
HTTP servers, executor pools, TLS contexts, I/O contexts, and similar process
resources should be treated as runtime dependencies, not as stream layers.

## Schema Composition

Generated IDL schemas can describe the generic envelopes, but those raw schemas
cannot know that `settings` must change shape when `type` changes. The
connection schema composer fills that gap by combining the application schema
with built-in component descriptors.

The composer should:

- Keep one stable top-level application schema for editor and tooling use.
- Patch each typed envelope so `type` is restricted to the component values
  available in the current build.
- Add conditional branches that map each `type` value to the selected
  component's generated settings schema.
- Reference imported component schemas instead of copying their definitions into
  the application schema.
- Preserve accurate IDL required-field semantics: required means non-optional
  and no explicit default. Defaults are applied by config materialisation, not
  by requiring sparse JSON files to spell every default.

The current implementation has a `config_schema_compose` tool and the
`CanopyGenerateConnectionConfigSchema` CMake helper. Demos can use those to emit
a composed schema beside their sample configs, while the runtime still uses the
same generated IDL materialisation path as application code.

Component descriptors distinguish role, status, type name, schema id, and the
definition inside the component schema. Status is descriptive. It should not be
confused with build availability: a strict schema for a binary should only list
components that are actually built and registered.

## Runtime Schema Reflection

The same schema model also supports runtime reflection for agents and MCP-style
tool discovery. Generated interfaces expose stable names, fingerprints, method
metadata, and schema accessors. At runtime, `casting_interface::get_schema` and
`i_marshaller::get_schema` can enumerate the interfaces implemented by an RPC
object and return method schemas that an agent can use to construct JSON calls.

The useful distinction is audience, not data ownership:

- `schema_flavor::config` is the full authoring/configuration schema.
- `schema_flavor::mcp` is intended to be a compact tool-call schema for LLMs.

The profile hook exists in the generator. Keep MCP-specific transforms isolated
from the config path so editor/config schemas do not silently lose defaults,
external references, or enum forms.

Interface names and method ids must remain stable enough for agents and tools to
replay calls correctly. The generated fingerprint is still the wire identity;
qualified interface name plus method name is the human and agent-facing identity.

## Design Rules

- Keep implementation-specific config in the implementation's IDL settings type.
- Prefer generated schema and typed materialisation over ad hoc JSON parsing.
- Let the connection factory own bootstrap dependencies and pass typed settings
  to component factories.
- Keep local-only pointers and process objects inside local boundaries. Across a
  process, enclave, machine, or C ABI boundary, use strings, ids, or generated
  serialisable structures.
- Treat application config as scoped to the module or executable that receives
  it. One module must not see another module's application settings.
- Avoid a custom language server until JSON Schema is proven insufficient.

## Remaining Work

The old planning notes also contained future-hardening work. These items are not
requirements that blocked the current cleanup, but they remain useful design
markers:

- Add production-grade ABI or capability gating for schema-introspection calls so
  older marshallers and transports can be detected deliberately.
- Harden the generated checksum/status workflow for production interfaces.
- Add an explicit runtime-discovery exposure policy if not every interface
  should be visible to agents.
- Complete any future type-query registry or name-to-interface cache only when a
  caller needs schema for an interface that is not attached to a live object.
- Continue moving transport-specific instantiation details into the relevant
  transport implementation while keeping reusable configuration and schema
  concepts transport-neutral.
