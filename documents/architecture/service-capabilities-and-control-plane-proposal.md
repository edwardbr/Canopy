# Service Capabilities And Control Plane Proposal

Status: proposal / discussion draft.

This note records a possible direction for service-scoped runtime capabilities,
attestation, io_uring, and service-level RPC discovery. It is not a statement
of current implementation behavior.

## Problem

Several pieces of runtime authority are currently supplied through different
paths:

- `rpc::service` owns an executor/scheduler.
- SGX enclave services own an attestation service and an io_uring controller.
- `connection_factory::context` can inject SPSC queues and attestation services.
- Stream layers can name an attestation service, but the name is resolved from
  connection factory context rather than from the owning service.
- There is a parked protocol proposal for a service-owned object at
  `object_id == 0`.

This creates two related problems.

First, objects such as attestation services and io_uring controllers are
service/runtime capabilities, not per-connection settings. They should be
owned by the app or runtime that creates the service, and local child services
should inherit them where that is safe.

Second, service-level RPC features such as named object lookup need a control
plane. That control plane should be remotely callable, but it should not expose
every local runtime capability by accident.

## Proposed Model

Use three separate concepts:

```text
Service capabilities
  Local, in-process runtime objects owned by a service.

Service control plane
  Well-known RPC objects exposed by a service or zone.

Application objects
  Ordinary generated RPC objects created by application code.
```

Keeping these concepts separate prevents the capability registry from becoming
a remote service locator and prevents service-control RPC objects from becoming
a container for sensitive local state.

## Service Capabilities

A service capability is a runtime object that the service owns or is allowed to
use. It is local to the process unless explicit bootstrap logic reconstructs an
equivalent capability in a child process or enclave.

Candidate capabilities:

| Capability | Named? | Same-process child inheritance | Cross-process/enclave behavior |
| --- | --- | --- | --- |
| executor / scheduler | usually no | inherit | child creates or receives its own runtime |
| io_uring controller | initially no | inherit where same runtime owns the ring | pass bootstrap settings, not the live controller |
| attestation service | yes | inherit unless overridden | child constructs/provisions its own service |
| zone security policy | maybe | inherit or snapshot | pass policy/config only if trusted |
| credential / trust provider | yes | inherit where safe | provision in child, do not blindly copy secrets |
| child process / enclave launch policy | maybe | inherit for local launchers | parent uses policy to start child |
| SPSC queue broker | maybe | inherit if it brokers queues | individual queue pairs remain per connection |

Individual SPSC queue pairs are not good service capabilities. They are usually
connection rendezvous objects. A service-owned queue broker may be a capability,
but a single queue pair belongs in transport config or explicit context
injection.

### Placement Rule

A value belongs on the service when it is:

- constructed by the app/runtime rather than by one connection,
- reused across multiple connections or transports,
- security or runtime sensitive,
- needed by local child services,
- optional by build or platform,
- and possibly selected by logical name.

A value belongs in connection config when it is:

- specific to one connection,
- safe to describe declaratively,
- not inherited by child services,
- or describes transport shape rather than authority.

`connection_factory::context` should remain an escape hatch for tests, demos,
and programmatic injection. It should not be the primary source of authority
when an owning service already exists.

### API Sketch

The preferred API shape is a typed, named registry attached to `rpc::service`.
The exact names are illustrative:

```cpp
service->set_capability<T>(std::shared_ptr<T> value);
service->set_capability<T>("name", std::shared_ptr<T> value);

auto default_value = service->get_capability<T>();
auto named_value = service->get_capability<T>("name");
```

To avoid turning `rpc::service` into an unstructured service locator, capability
types should be deliberately registered by the component that owns the feature.
Core RPC should not need to include attestation, io_uring, TLS, or SGX headers
just to store an optional capability.

Possible implementation approaches:

- a private typed map keyed by `std::type_index` and optional name,
- a small `service_capabilities` helper owned by `rpc::service`,
- feature-specific wrapper functions in optional components, for example
  `attach_attestation_service(service, ...)`.

The registry should hold shared ownership only for local runtime capabilities.
It should not automatically expose capabilities remotely.

## Inheritance

Same-process child services should inherit capabilities from the parent service
by default. This matches the desired behavior for:

- executor/scheduler,
- io_uring controller inside one runtime,
- attestation service,
- security policy,
- credential providers that are safe to share in that process.

Inheritance should allow explicit override. A child service may need a stricter
policy, a different named attestation service, or a different controller.

Crossing a process or enclave boundary is different. A live C++ object cannot
be inherited across that boundary. The parent service may own the launch policy
and bootstrap settings, but the child process or enclave must construct its own
service capabilities from provisioned or explicitly passed inputs.

## Attestation Direction

Attestation is a forcing case for the capability model.

Desired lookup order for stream attestation:

1. Resolve the named/default attestation service from the owning `rpc::service`.
2. Fall back to `connection_factory::context` for compatibility.
3. Fail clearly if the layer requires attestation and no service is available.

The `attestation_stream::stream_settings::service_name` field can remain a
logical selector. Its meaning should become "select a named attestation
capability from the owning service", not "look only in connection factory
context".

When connection factory creates the root service itself from config, any
configured runtime attestation services should be attached to that service.
When the app supplies an existing service, the app should attach the
attestation capability before asking connection factory to build streams.

## io_uring Direction

The enclave io_uring controller is also a service capability.

The live controller should be attached to the service that owns the runtime.
Local child services can inherit it. Connection config should not construct an
independent controller for each stream layer.

Separate the live capability from bootstrap settings:

```text
io_uring settings
  queue depth, SQPOLL, fixed-file policy, and other startup config

io_uring controller
  live runtime object used by services and streams
```

For a new binary, process, or enclave, the parent can pass settings or launch
policy. The child creates its own live controller.

## Service Control Plane

Some service-level features should be remotely callable. A named object lookup
service is the clearest example: a root zone may act like a DNS service for RPC
objects.

This should be modeled as a service control-plane object, not as a local
capability.

Possible directory interface shape:

```cpp
lookup(name, interface_id) -> remote_object
list(prefix) -> vector<object_entry>
describe(remote_object) -> object_metadata
```

The directory can use local capabilities internally, but it should expose only
the narrow RPC surface the service deliberately publishes.

### Object Ids

Keep `object_id == 0` as "zone-only / no specific object" unless a deeper
protocol audit decides otherwise. Current code paths treat zero this way in
many places.

Prefer a reserved framework object-id range:

```text
object_id 0
  zone-only address, not a callable object

object_id 1..N
  framework/service control-plane objects

object_id N+1..
  application objects allocated by the normal service allocator
```

For example:

```text
object_id 1 = service directory
```

This means the service object-id allocator must start after the reserved range.
The reserved range must be part of the protocol contract and language bindings,
not just a C++ convention.

An alternative is to advertise control-plane object descriptors during a
transport handshake. That is more flexible, but the reserved-id approach is
simpler for bootstrapping a directory lookup object.

## Directory Inheritance And Delegation

Child services need explicit rules for directory behavior.

Possible policies:

- local only: the child directory publishes only child-local objects,
- delegate unknown names to the parent directory,
- inherit selected parent entries at child creation,
- shadow parent entries with child-local entries of the same name.

The safest initial model is local-only plus explicit delegation. Implicitly
publishing all parent names through every child can leak capabilities across
zones that should have different security policy.

## Security

Service capabilities are authority. Service control-plane objects are RPC
surfaces. They need different security rules.

Guidelines:

- Do not expose local capabilities remotely by default.
- Directory lookup should be subject to zone security policy.
- A lookup result should not bypass normal `add_ref`, protected-RPC, or route
  attestation checks.
- Attestation and credential capabilities should not be built from untrusted
  host JSON inside a release enclave.
- Cross-process bootstrap should pass only the minimum settings needed for the
  child to construct its own capabilities.

## Migration Plan

1. Add the service capability registry with no behavior change.
2. Add tests proving local child services inherit generic capabilities.
3. Attach attestation services to services when connection factory creates a
   service from config.
4. Change stream layer context creation to prefer service capabilities and fall
   back to connection factory context.
5. Move enclave io_uring controller access onto the generic capability path, or
   provide a typed helper backed by that path.
6. Keep `connection_factory::context::set_attestation_service` and
   `register_attestation_service` as compatibility helpers, but document them
   as fallback injection.
7. Reserve framework object ids and update the service object allocator.
8. Add a service directory control-plane interface at a reserved object id.
9. Audit generated bindings, Rust/C ABI, and protocol docs for reserved object
   id handling before exposing the directory cross-language.

## Open Questions

- Should capability inheritance be copy-on-child-create, parent-linked lookup,
  or a hybrid?
- Should every capability type declare inheritance policy, or should the
  parent service choose at child creation?
- Is a fixed reserved object-id range sufficient, and how large should it be?
- Should the service directory be present on every service or opt-in?
- How should directory lookup interact with protected RPC and route
  attestation?
- Should directory entries be plain names, hierarchical paths, or structured
  service records?
- Should config-created attestation services become service capabilities in the
  first implementation, or should io_uring be the first capability migrated?

## Relationship To Existing Proposals

This proposal supersedes the preferred direction in
`documents/protocol/service-object-singleton-proposal.md` for new work. That
older note proposed using `object_id == 0` as a service-owned singleton object.
This proposal recommends keeping `object_id == 0` as zone-only and reserving a
small nonzero framework range instead.
