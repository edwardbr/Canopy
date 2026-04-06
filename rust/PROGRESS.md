# Rust Port Progress

This file tracks the handwritten progress of the Canopy Rust port.

Keep the completed section as a replayable migration history. The goal is not
only to show current status, but to preserve the order of implementation
decisions so the same work can be translated to other language backends.

## Current Status

Current focus:

- keep the completed migration sequence replayable for future language ports
- continue Rust generator/protobuf work from the supported service-level probe
- expand validation toward richer existing IDLs without moving
  service/transport mechanics into generated code

## Completed

Completed work is kept in implementation order. When planned work becomes
done, move the plan into this section rather than deleting it, so the migration
sequence remains reconstructable.

- Wrote the top-level Rust port plan in [`README.md`](/var/home/edward/projects/Canopy/rust/README.md).
- Wrote the first-pass `rust/rpc` scope in [`rpc/README.md`](/var/home/edward/projects/Canopy/rust/rpc/README.md).
- Captured key constraints:
  - protobuf-only initially
  - fingerprints remain generator-owned
  - `marshaller.h`, error codes, and `rpc_types.idl` are compatibility contracts
  - generated Rust must not be committed
  - dynamic-library interop needs a neutral C ABI
- Installed the Rust toolchain:
  - `rustc`
  - `cargo`
  - `clippy`
  - `rustfmt`
- Added Rust build/generated ignore rules to [`.gitignore`](/var/home/edward/projects/Canopy/.gitignore).
- Created the initial Cargo workspace:
  - [`Cargo.toml`](/var/home/edward/projects/Canopy/rust/Cargo.toml)
  - [`rpc/Cargo.toml`](/var/home/edward/projects/Canopy/rust/rpc/Cargo.toml)
  - [`rpc/src/lib.rs`](/var/home/edward/projects/Canopy/rust/rpc/src/lib.rs)
  - [`transports/README.md`](/var/home/edward/projects/Canopy/rust/transports/README.md)
  - [`tests/README.md`](/var/home/edward/projects/Canopy/rust/tests/README.md)
- Verified the initial Rust workspace with `cargo check`.
- Added the first handwritten runtime implementation chunk:
  - Rust error code module with numeric/stateful parity to the C++ runtime
  - basic parity tests for default and offset-based modes
- Added the first handwritten `rpc_types` chunk:
  - encoding and address-related enums
  - add-ref and release option flags
  - object, interface ordinal, and method wrappers
  - back-channel entry and zone-address argument data structs
  - basic parity tests for IDL-defined values/defaults
- Added the first blocking marshalling surface:
  - minimal `ZoneAddress`, `Zone`, and `RemoteObject` wrapper shells
  - parameter/result bundles matching `marshaller_params.h`
  - blocking Rust `IMarshaller` trait mirroring `marshaller.h`
  - `RetryBuffer`, `ConnectResult`, and empty back-channel helper
- Added the shared ABI home at [`../c_abi/README.md`](/var/home/edward/projects/Canopy/c_abi/README.md).
- Added the first neutral dynamic-library ABI draft under:
  - [`../c_abi/dynamic_library/README.md`](/var/home/edward/projects/Canopy/c_abi/dynamic_library/README.md)
  - [`../c_abi/dynamic_library/canopy_dynamic_library.h`](/var/home/edward/projects/Canopy/c_abi/dynamic_library/canopy_dynamic_library.h)
- Added the first substantial address/object identity port in `rpc_types`:
  - packed `ZoneAddress` creation and capability parsing
  - address getters, subnet/object setters, `zone_only`, `with_object`, and `same_zone`
  - `Zone` and `RemoteObject` helpers built on the packed address representation
  - tests covering local, IPv4, and IPv6 tunnel address behaviour
- Connected the shared `rpc_types` layer into internal runtime entry points:
  - internal address conversion helpers
  - internal string-format helpers for core wrapper types
  - runtime protocol version constants and `get_version()`
- Added the first Rust-side C ABI bridge crate:
  - [`transports/dynamic_library/Cargo.toml`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library/Cargo.toml)
  - [`transports/dynamic_library/src/lib.rs`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library/src/lib.rs)
  - raw `repr(C)` structs mirroring the shared dynamic-library ABI
  - borrowed request adapters for the first marshalling parameter types
  - copying helpers for result structs coming back from the ABI boundary
  - initial tests for request borrowing and result copying
- Tightened the Rust public wrapper semantics to better match generated C++:
  - `ConnectionSettings`
  - default local prefix helper
  - conversion impls for `Zone` and `RemoteObject`
  - cross-type `Zone` / `RemoteObject` equality semantics
  - tests covering the added public-shape behaviour
- Locked the shared dynamic-library C ABI onto packed `zone_address` blobs for
  routing and identity.
- Added the first Rust-side dynamic-library transport adapter sketch:
  - raw callback, allocator, and init-param ABI types
  - `ConnectionSettings` bridge support
  - raw-to-runtime conversion helpers for marshalling parameter structs
  - a parent-callback bridge implementing `IMarshaller` over the shared C ABI function pointers
  - `ChildTransportAdapter` decoding ABI requests into `canopy-rpc` runtime calls
  - tests covering both child-side decode/dispatch and parent-callback invocation
- Split the Rust dynamic-library bridge into:
  - [`transports/dynamic_library/src/ffi.rs`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library/src/ffi.rs)
    containing the raw ABI structs, pointer decoding helpers, and callback calls
  - [`transports/dynamic_library/src/adapter.rs`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library/src/adapter.rs)
    containing the safe child/parent transport adapters with no `unsafe`
  - [`transports/dynamic_library/src/lib.rs`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library/src/lib.rs)
    re-exporting the split modules
- Added the first safe Rust equivalent of the DLL-side context/helper layer:
  - [`transports/dynamic_library/src/context.rs`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library/src/context.rs)
  - `DllContext`
  - `InitChildZoneResult`
  - `init_child_zone(...)`
  - idempotent destroy-state tracking and focused tests
- Added a generic child-side entrypoint façade mirroring `canopy_dll_*`:
  - [`transports/dynamic_library/src/entrypoints.rs`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library/src/entrypoints.rs)
  - boxed `child_ctx` handle flow
  - `dll_init`, `dll_destroy`, `dll_send`, `dll_post`, `dll_try_cast`,
    `dll_add_ref`, `dll_release`, `dll_object_released`,
    `dll_transport_down`, and `dll_get_new_zone_id`
  - allocator-backed result writeback through the new façade
  - an end-to-end `dll_init` + `dll_send` round-trip test
- Added a handwritten Rust child DLL crate exporting real `canopy_dll_*`
  symbols:
  - [`transports/dynamic_library_test_child/Cargo.toml`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library_test_child/Cargo.toml)
  - [`transports/dynamic_library_test_child/src/lib.rs`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library_test_child/src/lib.rs)
  - `cdylib` + `rlib` outputs
  - exported `extern "C"` entrypoints built on the generic façade
  - a passing handwritten init/send/destroy round-trip test through the real exports
- Added the first C++ parent <-> Rust child interop harness:
  - new C++ transport home at [`../c++/transports/c_abi`](/var/home/edward/projects/Canopy/c++/transports/c_abi)
  - a low-level loader plus parent-side `child_transport` scaffold over the shared `c_abi`
  - a passing C++ test that loads the Rust test child DLL and performs init/send/destroy
  - expanded ABI smoke coverage for `try_cast`, `release`, and `get_new_zone_id`
  - additional ABI smoke coverage for `post`, `add_ref`, `object_released`, and `transport_down`
  - a passing reverse-direction callback test where the Rust child invokes `parent_send`
  - a passing reverse-direction callback test where the Rust child invokes `parent_get_new_zone_id`
- Added canonical raw-blob construction support for `rpc::zone_address`:
  - `zone_address::from_blob(std::vector<uint8_t>)`
  - IDL/source updates plus a passing round-trip unit test
  - the C++ `c_abi` bridge now rebuilds native addresses from the packed blob directly
- Completed the first full non-coroutine C++ `c_abi` transport path:
  - added DLL-side `parent_transport`, `dll_context`, and `init_child_zone<>`
  - added exported C++ `canopy_dll_*` entrypoints over the shared ABI
  - added a C++ child DLL target loaded through `c++/transports/c_abi`
  - added a new typed-test setup and suite in `rpc_test`
  - verified that a C++ parent can load a C++ child DLL and pass the full `c_abi`
    typed suite in `rpc_test`
- Added the first safe Rust parent-side dynamic-library loader:
  - [`transports/dynamic_library/src/platform_ffi.rs`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library/src/platform_ffi.rs)
    for raw `dlopen` / `dlsym` / `dlclose`
  - [`transports/dynamic_library/src/loader.rs`](/var/home/edward/projects/Canopy/rust/transports/dynamic_library/src/loader.rs)
    for safe `DynamicLibrary` / `LoadedChild` wrappers
  - added Rust-side function-pointer aliases for exported `canopy_dll_*`
  - added tests proving a Rust parent can load the handwritten Rust child DLL,
    drive `send`, `try_cast`, `post`, `add_ref`, `release`,
    `object_released`, `transport_down`, and `get_new_zone_id`,
    and observe callback traffic back into the parent
- Added the first IDL/parser and generator support for Rust-targeted output:
  - `#rust_quote(...)` is now recognized by the macro parser and AST parser
  - C++ generators explicitly ignore `RUSTQUOTE` content in generated C++ output
  - the initial Rust generator is wired into the main `generator` executable behind `--rust`
  - initial Rust output now emits:
    - interface fingerprint constants
    - interface method ordinals
    - struct fingerprint constants matching the existing C++ `rpc::id<>` generation surface
    - raw `#rust_quote` pass-through blocks
  - `#cpp_quote(...)` and `#rust_quote(...)` no longer contaminate fingerprints
  - verified that adding/removing `#rust_quote(...)` does not change interface IDs
  - verified the Rust generator on `interfaces/rpc/rpc_types.idl`
- Added the first Rust protobuf generator path:
  - [`generator/include/rust_protobuf_generator.h`](/var/home/edward/projects/Canopy/generator/include/rust_protobuf_generator.h)
  - [`generator/src/rust_protobuf_generator.cpp`](/var/home/edward/projects/Canopy/generator/src/rust_protobuf_generator.cpp)
  - wired into `generator` when `--protobuf --rust` are both enabled
  - emits Rust metadata pointing at the canonical generated `.proto` tree rather than
    generating committed Rust protobuf code
  - current Rust protobuf metadata includes:
    - `MASTER_PROTO`
    - `PROTO_MANIFEST`
    - `PROTO_INCLUDE_DIRS`
    - `PROTO_FILES`
  - verified on both `example.idl` and `interfaces/rpc/rpc_types.idl`
- Extended Rust generator metadata for interface-pointer semantics:
  - per-interface `interface_binding` metadata is now emitted
  - per-method `INTERFACE_PARAMS` lists now distinguish:
    - `rpc::shared_ptr`
    - `rpc::optimistic_ptr`
    - `[in]` versus `[out]`
    - target interface name
  - verified on `example.idl`, including the `set_optimistic_ptr` and
    `get_optimistic_ptr` methods
- Extended Rust interface-binding metadata to be directly method-dispatchable:
  - generated `MethodBindingDescriptor`
  - generated `METHODS` table per interface
  - generated `by_method_id(method_id)` lookup helper
  - verified on `example.idl`
- Added the first handwritten Rust ownership/binding semantics for interface
  marshalling:
  - `BoundInterface<T>` with explicit `Null` / `Gone` / `Value(T)` states
  - `InterfacePointerKind` as a shared versus optimistic distinction in the
    runtime layer
  - `ObjectProxy` with separate shared and optimistic local counts
  - `ObjectStub` with separate shared and optimistic counts plus per-zone maps
  - helpers mapping pointer kind to `AddRefOptions` / `ReleaseOptions`
  - re-exported these through `canopy-rpc`
  - verified with `cargo test -p canopy-rpc --lib`
- Added the first real handwritten Rust `service` core in
  [`rpc/src/internal/service.rs`](/var/home/edward/projects/Canopy/rust/rpc/src/internal/service.rs):
  - `Service`
  - `ServiceConfig`
  - `RemoteObjectResult`
  - `CurrentServiceGuard`
  - `DUMMY_OBJECT_ID`
  - current-service thread-local tracking
  - object ID generation
  - default encoding storage
  - stub registration / lookup / removal
  - focused unit tests for service core behavior
- Added the first Rust `local_proxy` scaffold in `remote_pointer.rs`:
  - preserves the C++ rule that both `shared_ptr` and `optimistic_ptr` can
    refer to local or remote objects
  - captures the local optimistic shape as a weak/local proxy rather than
    flattening it into a remote-object-only concept
  - adds `CreateLocalProxy` and `LocalProxy<T>` as the first handwritten
    foundation for generated local optimistic bindings
  - verified with `cargo test -p canopy-rpc --lib`
- Strengthened handwritten Rust bind-result modeling:
  - `InterfaceBindResult<T>` now carries `BoundInterface<T>` rather than
    flattening everything to `Option<T>`
  - explicit bind origin tracking distinguishes local vs remote rebinding
  - local/remote/null/gone constructor helpers added for the upcoming bind logic
  - verified with `cargo test -p canopy-rpc --lib`
- Added the first generic handwritten bind-flow helpers in `bindings.rs`:
  - incoming shared bind flow now distinguishes null, same-zone local lookup,
    and foreign-zone remote binding
  - incoming optimistic bind flow now uses the local weak-proxy path for same-zone
    bindings and preserves `OBJECT_GONE` semantics
  - outgoing bind flow now routes by locality and pointer kind while preserving
    null and gone cases
  - covered with fake-runtime tests rather than waiting on the unfinished Rust
    `service` and `service_proxy` implementations
  - verified with `cargo test -p canopy-rpc --lib`
- Threaded the Rust generator into the handwritten binding runtime:
  - added public generated-metadata runtime types in `canopy-rpc`
  - Rust generator `interface_binding` modules now emit
    `canopy_rpc::GeneratedInterfaceParamDescriptor` and
    `canopy_rpc::GeneratedMethodBindingDescriptor`
  - each generated interface now emits a `BindingMetadata` type implementing
    `canopy_rpc::GeneratedInterfaceBindingMetadata`
  - verified by rebuilding `generator` and generating fresh Rust output for
    `c++/tests/idls/example/example.idl`
- Extended generated Rust from metadata into method-shaped binding scaffolding:
  - each generated interface-param module now emits handwritten-runtime-facing
    binder wrappers such as `bind_<param>_incoming(...)` and
    `bind_<param>_outgoing(...)`
- Added a shared protobuf type-interpretation layer in:
  - [`../generator/include/proto_generator.h`](/var/home/edward/projects/Canopy/generator/include/proto_generator.h)
  - [`../generator/src/proto_generator.cpp`](/var/home/edward/projects/Canopy/generator/src/proto_generator.cpp)
  - both [`../generator/src/protobuf_generator.cpp`](/var/home/edward/projects/Canopy/generator/src/protobuf_generator.cpp)
    and [`../generator/src/rust_protobuf_generator.cpp`](/var/home/edward/projects/Canopy/generator/src/rust_protobuf_generator.cpp)
    now delegate their duplicated C++-to-protobuf type mapping through this shared layer
  - verified with `cmake --build build_debug --target generator`
- Re-enabled the real in-tree Rust protobuf runtime behind a feature-gated seam:
  - [`rpc/Cargo.toml`](/var/home/edward/projects/Canopy/rust/rpc/Cargo.toml)
    now has `protobuf-runtime`
  - [`rpc/src/serialization/protobuf/facade.rs`](/var/home/edward/projects/Canopy/rust/rpc/src/serialization/protobuf/facade.rs)
    now implements `ProtobufWireMessage` for real `protobuf::Message + Parse + Serialize` types
  - verified with `cargo check -p canopy-rpc --features protobuf-runtime`
- Added a small probe crate for real Rust protobuf codegen/runtime bring-up:
  - [`tests/protobuf_runtime_probe/Cargo.toml`](/var/home/edward/projects/Canopy/rust/tests/protobuf_runtime_probe/Cargo.toml)
  - [`tests/protobuf_runtime_probe/build.rs`](/var/home/edward/projects/Canopy/rust/tests/protobuf_runtime_probe/build.rs)
  - [`tests/protobuf_runtime_probe/src/lib.rs`](/var/home/edward/projects/Canopy/rust/tests/protobuf_runtime_probe/src/lib.rs)
  - it successfully generates Rust from Canopy's canonical `build_debug/generated/src/rpc/protobuf/schema/rpc.proto`
  - real generated Rust protobuf messages now build, link, serialize, and parse successfully in the probe crate
  - shared interface params now route through `canopy_rpc::bind_incoming_shared`
- Added generator-owned lightweight Rust protobuf build metadata:
  - `generator/src/rust_protobuf_generator.cpp` now emits sibling
    `*_protobuf_build.rs` files beside the existing Rust protobuf metadata
  - those build-metadata files expose:
    - `ProtobufBuildTarget`
    - `BUILD_TARGET`
    - `BUILD_DEPENDENCIES`
    - `all_build_targets()`
  - target metadata now uses the generated output subdirectory / crate name
    rather than blindly mirroring the IDL base filename, so `rpc_types.idl`
    correctly reports `crate::rpc`
  - non-`rpc` targets now also carry the implicit protobuf dependency on the
    shared `rpc` proto crate mapping
- Threaded the probe crate onto that generated build metadata:
  - `protobuf_runtime_probe/build.rs` now `include!`s the generated
    `rpc_types_protobuf_build.rs`
  - the probe no longer hardcodes the proto target file list
  - it reads the generated manifest listed by `BUILD_TARGET` and drives the
    in-tree Rust protobuf codegen from generator-owned metadata instead
  - `protobuf_runtime_probe/src/lib.rs` continues to validate real generated
    Rust protobuf types by round-tripping `zone_address` and nested
    `remote_object`
  - verified with `cargo test -p canopy-protobuf-runtime-probe --lib`
- Fixed a generated-Rust dispatch bug in [`../generator/src/rust_generator.cpp`](/var/home/edward/projects/Canopy/generator/src/rust_generator.cpp):
  - `__rpc_dispatch_generated(...)` no longer emits undeclared interface generic names like `TARGETIface0`
  - scalar-only generated methods still route through the generated dispatch path
  - interface-bearing generated methods now fail cleanly with `INVALID_DATA()` at the top-level raw dispatch seam until erased wire-level interface dispatch is implemented
  - this keeps generated Rust compilable while protobuf/interface dispatch is filled in properly
- Reworked [`rpc/src/internal/object_proxy.rs`](/var/home/edward/projects/Canopy/rust/rpc/src/internal/object_proxy.rs) toward the C++ remote-cast architecture:
  - one shared `ObjectProxy` now owns the remote object identity
  - added `RemoteInterfaceView<T>` so multiple interface-specific views can hang off the same object proxy
  - added cached per-interface view lookup and insertion on `ObjectProxy`
  - added `QueryInterfaceResult<T>` and a first `query_interface_view(...)` helper that reuses cached views and treats `INVALID_CAST` as an acceptable non-critical cast failure
  - verified with new unit tests that:
    - repeated casts to the same interface reuse the same cached view
    - different interface views share the same underlying `ObjectProxy`
    - failed casts do not create cached views
    and `canopy_rpc::bind_outgoing_interface(..., Shared, ...)`
  - optimistic interface params now route through
    `canopy_rpc::bind_incoming_optimistic` and preserve the `LocalProxy<T>`
    result shape for local optimistic bindings
  - generated modules also emit `<PARAM>_INTERFACE_NAME` constants to keep
    interface intent explicit beside the wrappers
  - verified by rebuilding `generator`, regenerating `example.idl`, and
    inspecting the emitted Rust wrappers
- Added the first generated Rust interface representation:
  - `canopy-rpc` now exposes minimal `CastingInterface` and
    `GeneratedRustInterface` traits
  - each generated IDL interface now emits `pub trait Interface`
  - the generated trait exposes:
    - `interface_name()`
    - `get_id(rpc_version)`
    - `binding_metadata()`
  - this gives handwritten Rust implementations a real generated trait surface
    to target before proxy/stub codegen exists
  - verified with `cargo test -p canopy-rpc --lib`, generator rebuild, and
    inspection of regenerated `example.idl` Rust output
- Extended generated interface traits with Rust method signatures:
  - added semantic `canopy_rpc::Shared<T>` and `canopy_rpc::Optimistic<T>`
    wrappers plus `canopy_rpc::OpaqueValue` for unsupported value types
  - generated trait methods now emit concrete Rust signatures for supported
    scalar/string/vector cases
  - shared interface params map to `canopy_rpc::Shared<T>`
  - optimistic interface params map to `canopy_rpc::Optimistic<T>`
  - `[out]` params map to `&mut ...`
  - unsupported non-interface value types currently fall back to
    `canopy_rpc::OpaqueValue` rather than guessing an incorrect Rust type
  - verified by rebuilding `generator`, regenerating `example.idl`, and
    inspecting the emitted signatures for both shared and optimistic methods
- Added generated request/response shape helpers beside each method binder:
  - each generated method module now emits `Request` and `Response` structs
  - request structs contain `[in]` and implicit-in parameters
  - response structs contain `[out]` parameters plus `return_value`
  - shared interface params use `canopy_rpc::Shared<T>`
  - optimistic interface params use `canopy_rpc::Optimistic<T>`
  - unsupported value types continue to use `canopy_rpc::OpaqueValue`
  - verified by rebuilding `generator`, regenerating `example.idl`, and
    inspecting both shared and optimistic method modules
- Added the first proxy/stub-oriented generated Rust scaffolding:
  - each generated interface now emits `Proxy` and `Stub` traits on top of
    the generated `Interface` trait
  - those traits currently expose request/response shape anchors via
    `std::any::type_name::<...>()`
  - each generated interface now also emits `ProxySkeleton` and `StubSkeleton`
    placeholder implementations with non-panicking transport-error fallback
    bodies
  - the skeletons explicitly implement the generated Rust interface identity
    traits so the scaffold is structurally coherent
  - verified by rebuilding `generator`, regenerating `example.idl`, and
    inspecting the emitted proxy/stub blocks
- Added generated request/response helper methods:
  - each generated `Request` now emits `from_call(...)`
  - each generated `Response` now emits `apply_to_call(...)`
  - this gives the Rust scaffold a concrete path for:
    - building request shapes from method arguments
    - applying response shapes back onto out parameters
    - returning the generated `return_value`
  - verified by rebuilding `generator`, regenerating `example.idl`, and
    inspecting both shared and optimistic method modules
- Replaced inert generated proxy/stub fallbacks with method-shaped hook points:
  - each generated `Response` now emits `transport_error()`
  - each generated `Proxy` trait now emits default `proxy_call_<method>(...)`
    hooks returning a generated response shape
  - each generated `Stub` trait now emits default `stub_dispatch_<method>(...)`
    hooks returning a generated response shape
  - generated `ProxySkeleton` and `StubSkeleton` methods now:
    - build a generated request via `Request::from_call(...)`
    - delegate through the method-specific hook
    - apply the generated response back via `Response::apply_to_call(...)`
  - verified by rebuilding `generator`, regenerating `example.idl`, and
    inspecting the emitted hook and skeleton bodies
- Added the first generated proxy-side transport seam:
  - `rust/rpc` now exposes `GeneratedProxyCallContext`
  - `rust/rpc` now exposes `GeneratedProxyTransport`, which builds real
    `SendParams` and calls `IMarshaller::send(...)`
  - generated `Proxy` traits now emit `proxy_transport(&self)`
  - generated default `proxy_call_<method>(...)` hooks now:
    - resolve interface ID from the transport protocol version
    - send a real `SendParams` through `GeneratedProxyTransport`
    - convert the `SendResult` into a generated response via
      `Response::from_send_result(...)`
  - current payload/back-channel marshalling is still placeholder-only:
    `Vec::new()` is used until protobuf request/response encoding is wired in
  - verified by Rust unit tests and regenerated `example.idl` output
- Added the first real protobuf request/response path on the Rust side:
  - added handwritten protobuf helpers in `rust/rpc/src/internal/protobuf_codec.rs`
    covering:
    - `int32`
    - `bool`
    - `uint64`
    - `string`
    - packed `repeated uint64`
    - nested `rpc.remote_object` via packed `zone_address.blob`
  - generated `Request` types now emit `encode_for_transport()`
  - generated `Response` types now emit:
    - `from_error_code(...)`
    - `from_send_result(...)`
  - generated proxy calls now:
    - encode request payloads before calling `send_generated(...)`
    - return generated error responses if encoding fails
    - decode successful `SendResult.out_buf` back into generated responses
  - current supported subset is intentional:
    - scalar-only and vector/string methods can now use real protobuf bytes
    - methods with interface params or interface out params still return
      `INVALID_DATA` on the generated encode/decode path until interface
      rebinding is threaded through protobuf serialisation
  - verified by Rust unit tests and regenerated `example.idl` output
- Aligned generated Rust protobuf metadata with the existing C++ protobuf
  generator:
  - added runtime metadata types:
    - `GeneratedProtobufFieldKind`
    - `GeneratedProtobufParamDescriptor`
    - `GeneratedProtobufMethodDescriptor`
  - removed protobuf lookup/types from the generic
    `internal/bindings_fwd.rs` path entirely
  - added a dedicated Rust serialization subtree:
    - [`rpc/src/serialization/mod.rs`](/var/home/edward/projects/Canopy/rust/rpc/src/serialization/mod.rs)
    - [`rpc/src/serialization/protobuf/mod.rs`](/var/home/edward/projects/Canopy/rust/rpc/src/serialization/protobuf/mod.rs)
    - [`rpc/src/serialization/protobuf/metadata.rs`](/var/home/edward/projects/Canopy/rust/rpc/src/serialization/protobuf/metadata.rs)
    - [`rpc/src/serialization/protobuf/facade.rs`](/var/home/edward/projects/Canopy/rust/rpc/src/serialization/protobuf/facade.rs)
  - protobuf metadata is now carried by
    `canopy_rpc::serialization::protobuf::GeneratedProtobufBindingMetadata`
    rather than by the generic `GeneratedInterfaceBindingMetadata`
  - added a dedicated protobuf helper facade analogous in role to
    [`../c++/rpc/include/rpc/serialization/protobuf/protobuf.h`](/var/home/edward/projects/Canopy/c++/rpc/include/rpc/serialization/protobuf/protobuf.h):
    - byte-field helpers
    - signed-byte helpers
    - repeated-integer-vector helpers
    - a format-specific `GeneratedProtobufCodec<Message>` trait for future
      generated request/response conversion
  - protobuf-specific Rust generation now lives in
    [`../generator/src/rust_protobuf_generator.cpp`](/var/home/edward/projects/Canopy/generator/src/rust_protobuf_generator.cpp),
    not in
    [`../generator/src/rust_generator.cpp`](/var/home/edward/projects/Canopy/generator/src/rust_generator.cpp)
  - generated `*_protobuf.rs` companion output now emits:
    - `PROTOBUF_PARAMS`
    - `PROTOBUF_REQUEST_MESSAGE`
    - `PROTOBUF_RESPONSE_MESSAGE`
    - `PROTOBUF_PACKAGE`
    - `PROTOBUF_SCHEMA_FILE`
    - `PROTOBUF_REQUEST_FULL_NAME`
    - `PROTOBUF_RESPONSE_FULL_NAME`
    - `PROTOBUF_REQUEST_RUST_TYPE`
    - `PROTOBUF_RESPONSE_RUST_TYPE`
    - `PROTOBUF_METHODS`
  - generated non-protobuf Rust interface output no longer emits `PROTOBUF_*`
    items
  - the Rust generator now follows the same protobuf type interpretation as
    `generator/src/protobuf_generator.cpp` for:
    - scalar mappings like `error_code -> int32`, `size_t -> uint64`,
      `std::string -> string`
    - interface pointers -> `rpc.remote_object`
    - pointer values -> `uint64`
    - scalar sequences -> `repeated ...`
    - scalar maps -> `map<...>`
    - byte vectors -> `bytes`
  - protobuf serialisation remains deferred until we integrate a real Rust
    protobuf backend from `submodules/protobuf`; no handwritten wire logic
    remains
  - tightened the protobuf stub-dispatch seam so it is typed rather than
    byte-only:
    - added `ProtobufWireMessage`
    - added `UnsupportedGeneratedMessage`
    - `GeneratedProtobufMethodCodec` now carries:
      - `ProtoRequest`
      - `ProtoResponse`
      - `request_from_protobuf_message(...)`
      - `response_to_protobuf_message(...)`
    - default `decode_request(...)` / `encode_response(...)` now route through
      that typed protobuf-message seam
    - generated `*_protobuf.rs` companions now emit those associated types and
      conversion hooks for each method codec
  - added local offline Rust wrapper crates for the in-tree protobuf stack:
    - `rust/third_party/paste`
    - `rust/third_party/protobuf_macros`
    - `rust/third_party/protobuf_codegen`
    - `rust/third_party/protobuf`
  - verified that the local protobuf runtime and codegen crates compile under
    Cargo without using crates.io
  - attempted to wire the protobuf runtime directly into `canopy-rpc`, but
    that is currently blocked by duplicate exported upb symbols in the
    Cargo-side native build; the wrappers were kept and the direct `canopy-rpc`
    dependency was backed out so the Rust workspace remains clean
  - verified by rebuilding `generator`, regenerating `example.idl`, and
    inspecting the emitted protobuf descriptors
- Extended the handwritten Rust `service` lifetime-management surface:
  - added `ServiceEvent` registration removal and dead-listener pruning
  - added `release(ReleaseParams)` with version/object validation
  - added `object_released(ObjectReleasedParams)` and
    `transport_down_from_params(TransportDownParams)` wrappers
  - added tests covering missing objects, invalid versions, embedded-zone
    release notifications, dead listener pruning, and zone teardown cleanup
  - verified with `cargo test -p canopy-rpc --lib`
- Added the first real local stub dispatch path in the handwritten Rust runtime:
  - `CastingInterface` is now object-safe and closer in role to the C++
    `casting_interface`
  - `ObjectStub` now owns an optional castable target and supports:
    - `call(SendParams)`
    - `try_cast(InterfaceOrdinal)`
    - `get_castable_interface(...)`
    - `keep_self_alive(...)` / `dont_keep_alive()`
  - `Service` now implements `IMarshaller` for the local-zone path:
    - `send`
    - `post`
    - `try_cast`
    - `add_ref`
    - `release`
    - `object_released`
    - `transport_down`
    - `get_new_zone_id` currently returns `ZONE_NOT_SUPPORTED`
  - added `register_local_object(...)` as the first handwritten local-object
    registration helper
  - added tests covering:
    - local object registration
    - local send/try_cast dispatch
    - add_ref/post basics through the `IMarshaller` surface
  - while doing this, corrected an important drift from C++:
    Rust stubs previously had no self-keepalive equivalent, so locally
    registered objects could disappear immediately because `Service` only held
    weak references
  - verified with `cargo test -p canopy-rpc --lib`
- Threaded the handwritten bind helpers into the real local service/stub path:
  - `ObjectStub` now keeps both:
    - a castable dispatch target
    - an erased typed target for local interface recovery
  - `Service` now has:
    - `lookup_local_interface<T>(...)`
    - `bind_incoming_shared_interface<T>(...)`
    - `bind_incoming_optimistic_interface<T>(...)`
  - added tests proving that:
    - local shared interface binds recover the original `Arc<T>`
    - local optimistic binds produce a live `LocalProxy<T>`
    - missing objects report `OBJECT_NOT_FOUND`
    - wrong-interface binds report `INVALID_INTERFACE_ID`
  - while adding this, corrected another drift bug:
    local lookup briefly keyed off the protocol version rather than the
    generated interface ID
  - verified with `cargo test -p canopy-rpc --lib`
- Restored the C++-style forwarding seams in the Rust `service` path:
  - public/inherent `Service` entrypoints now exist for:
    - `send`
    - `post`
    - `try_cast`
    - `add_ref`
    - `release`
    - `get_new_zone_id`
  - those entrypoints now forward through explicit outbound seams:
    - `outbound_send`
    - `outbound_post`
    - `outbound_try_cast`
    - `outbound_add_ref`
    - `outbound_release`
    - `outbound_get_new_zone_id`
  - `IMarshaller for Service` now delegates to those entrypoints instead of
    inlining the behavior directly
  - this preserves the same override/interception shape used in the C++
    `service` implementation rather than flattening the path
  - verified with `cargo test -p canopy-rpc --lib`
- Added the outgoing local-interface export path in the handwritten Rust
  `service`:
  - `find_local_stub_for_interface<T>(...)`
  - `get_descriptor_from_local_interface<T>(...)`
  - `bind_outgoing_local_interface<T>(...)`
  - the path now mirrors the key C++ `get_descriptor_from_interface_stub` /
    `stub_bind_out_param` intent:
    - reuse an existing local stub when one is already associated with the
      local object
    - otherwise create and register a new stub
    - increment shared vs optimistic refcounts according to the pointer kind
    - return a packed `RemoteObject` descriptor rooted in the service zone
  - added tests proving:
    - an existing registered local object reuses its stub and descriptor
    - an unregistered local object creates a new stub and returns a valid
      descriptor
    - optimistic export increments the optimistic local count
  - verified with `cargo test -p canopy-rpc --lib`
- Extended the Rust generator's method-level local binding seam:
  - generated method modules already had `IncomingLocalBindings` and
    `OutgoingLocalBindings` structs
  - they now also emit direct method-shaped entrypoints:
    - `bind_incoming_local(...)`
    - `bind_outgoing_local(...)`
  - this keeps generated call sites from needing to know the struct names and
    gives the generator a cleaner seam to grow toward real stub/proxy call
    composition
  - kept serialization concerns out of `rust_generator.cpp`
  - verified with:
    - `cmake --build build_debug --target generator`
    - `cargo test -p canopy-rpc --lib`
- Added generated local-call composition for interface-bearing methods:
  - method modules now emit `LocalCallResult`
  - method modules now emit `call_local(...)`
  - `call_local(...)`:
    - binds local incoming interface params through the handwritten `Service`
      paths
    - short-circuits to `Response::from_error_code(...)` on bind failure
    - invokes the local implementation through the generated Rust interface
      trait
    - exports local outgoing interface params through the handwritten `Service`
      paths
  - this is still local-call composition only; it does not reintroduce
    protobuf or transport logic into `rust_generator.cpp`
  - verified with:
    - `cmake --build build_debug --target generator`
    - `cargo test -p canopy-rpc --lib`
    - regenerated `example.idl` and read back the emitted `call_local(...)`
      helpers
- Corrected the generated Rust error-handling assumption for local-call
  composition:
  - generated code no longer treats `err != OK()` as an RPC-internal failure
  - method-level generated local bind paths now check
    `canopy_rpc::is_critical(...)` instead
  - exported `is_error(...)` and `is_critical(...)` from the Rust crate so
    generated code can follow the same classification model as C++
- Carried the same error-classification rule into the handwritten Rust
  runtime:
  - `Service::register_local_object(...)` no longer treats `err != OK()` as the
    control-flow rule
  - it now uses `error_codes::is_critical(...)` for the registration result,
    because `OBJECT_GONE` / `INVALID_CAST` are not acceptable there
  - current `rust/rpc` runtime control flow no longer has handwritten
    `!= OK()` checks outside tests
- Added the first handwritten Rust `rpc::base` foundation in
  `rust/rpc/src/internal/base.rs`:
  - `DispatchContext`
  - `DispatchResult`
  - `RpcObject`
  - `LocalObjectAdapter`
  - `RpcBase<Impl, Adapter>`
  - this provides:
    - one object supporting multiple interfaces through an adapter
    - a future-ready dispatch seam carrying `in_back_channel`
    - weak stub attachment mirroring the C++ `rpc::base` intent
- Threaded stub attachment into the runtime:
  - `ObjectStub` now stores `Arc<dyn RpcObject>`
  - `Service::register_stub(...)` attaches the stub back to the target object
  - `Service::remove_stub(...)` detaches it
- Marked the current `__rpc_*` Rust plumbing traits/methods as hidden/internal:
  - Rust does not reserve `__` the way C++ does, so the internal-only intent is
    now reinforced with `#[doc(hidden)]`
  - this keeps the future application-facing surface focused on generated
    interfaces plus a `make_local(...)`-style wrapper rather than low-level RPC
    plumbing
- Added the first generated `make_rpc_object(...)` application-facing wrapper:
  - generated interface modules now emit `RpcObjectAdapter`
  - generated interface modules now emit `make_rpc_object(...)`
  - this gives application code a clean entrypoint over the handwritten
    `RpcBase` seam without exposing adapter internals
- Replaced the generated adapter's hardcoded `INVALID_DATA()` dispatch body with
  real interface/method routing shape:
  - generated interfaces now emit:
    - `matches_interface_id(...)`
    - `method_metadata_for_interface(...)`
    - hidden `__rpc_dispatch_generated(...)`
  - generated `RpcObjectAdapter::dispatch(...)` now delegates to
    `implementation.__rpc_dispatch_generated(...)`
  - generated `__rpc_dispatch_generated(...)` now:
    - checks interface id
    - routes by method ordinal
    - returns `INVALID_METHOD_ID` for unknown methods in a matched interface
    - returns `INVALID_INTERFACE_ID` when the interface does not match
  - per-method `dispatch_generated(...)` hooks are still placeholders pending
    serialization-aware argument decoding
- Added a protobuf-local typed codec seam without leaking protobuf specifics
  back into the generic Rust generator:
  - `rust/rpc/src/serialization/protobuf/facade.rs` now exports
    `GeneratedProtobufMethodCodec`
  - `rust_protobuf_generator.cpp` now emits per-method `ProtobufCodec`
    companions in `*_protobuf.rs`
  - those generated codec companions:
    - point at the generic generated `Request` / `Response` shapes
    - expose the correct protobuf method descriptor
    - keep the actual decode/encode body deferred until the protobuf runtime is
      wired in
  - this establishes the correct place for future protobuf request/response
    conversion: the protobuf companion, not `rust_generator.cpp`
- Routed the generated stub-dispatch placeholder through the protobuf facade
  instead of hardcoding format behavior in generated code:
  - `rust/rpc/src/serialization/protobuf/facade.rs` now exports
    `dispatch_generated_stub_call(...)`
  - that helper owns the protobuf-specific
    decode-request / dispatch / encode-response flow
  - generated `*_protobuf.rs` method companions now call
    `dispatch_generated_stub_call::<ProtobufCodec, _>(...)`
  - the method closure still returns `INVALID_DATA()` for now, but the format
    seam is now in the right place for real protobuf-backed dispatch
- Split generated stub dispatch into explicit format and method-execution stages:
  - generic generated `*.rs` method modules now emit hidden
    `dispatch_decoded_request(...)` helpers over generated `Request` /
    `Response` shapes
  - generated `*_protobuf.rs` method companions now decode through the protobuf
    facade and then delegate to `dispatch_decoded_request(...)`
  - this keeps protobuf-specific work inside the protobuf layer while moving
    method execution onto format-agnostic generated request/response types
- Replaced the generic decoded-request placeholder with real generated method
  execution:
  - generated `dispatch_decoded_request(...)` helpers now:
    - destructure generated `Request` values
    - create default `[out]` values
    - call the generated interface method on the implementation
    - build a generated `Response`
  - this gives the Rust path a real decoded-call execution seam before any
    protobuf-specific message conversion is introduced
- Added the first service-aware stub-side generated request/response path for
  interface-bearing RPC methods:
  - `ObjectStub` now records its owning service address when registered
  - `RpcBase` now enriches `DispatchContext` from the attached object-stub weak
    pointer instead of depending on thread-local service identity
  - `DispatchContext` now exposes `current_service()` from that owner-stub path
  - generic generated method modules now emit hidden:
    - `DispatchRequest`
    - `DispatchResponse`
    - `dispatch_stub_request(...)`
  - `DispatchRequest` / `DispatchResponse` keep interface parameters in
    wire-facing `RemoteObject` form instead of application-bound interface
    wrappers
  - generated `dispatch_stub_request(...)` now:
    - uses direct method execution for scalar-only methods
    - uses `context.current_service()` plus `call_local(...)` for
      interface-bearing methods
    - checks outgoing interface bind results with `is_critical(...)`
  - generated protobuf companions now target `DispatchRequest` /
    `DispatchResponse` and delegate into `dispatch_stub_request(...)`
  - this aligns the Rust direction more closely with the C++ `__rpc_call` /
    stub-demarshaller shape instead of pretending protobuf decode can directly
    produce application-bound interface values

### Generated Protobuf Replay Milestones

- Completed the previously planned move away from per-parameter Rust protobuf
  conversion toward the C++-style per-method request/response marshalling
  model:
  - used the generated C++ protobuf shape, including richer outputs such as
    `example_shared`, as the behavioural reference for Rust companion
    generation
  - made the generated method `Request` / `Response` units the Rust-side
    marshalling seam before any byte-level protobuf encode/decode is applied
  - refactored the Rust protobuf companion so method codecs are centered on
    generated per-method protobuf request/response message types rather than
    a separate free-function byte conversion layer
  - added recursive generated conversion helpers for the currently supported
    scalar, string, bytes, vector, struct, and remote-object/interface value
    shapes
  - kept generator output scoped to message conversion and method binding while
    caller/service/transport mechanics remain in handwritten Rust runtime code
  - retained the still-open follow-up that real existing-IDL round-trip
    coverage must be expanded beyond the focused probe once unsupported field
    shapes and schema drift are handled

- Extended the Rust generator and protobuf companion to support IDL-defined
  struct types as method parameters, removing the previous scalar/string/vector
  whitelist restriction:
  - `generator/src/rust_generator.cpp` now emits actual Rust struct definitions
    (`pub struct Value`) for non-template IDL structs, with field types resolved
    through the full library struct-lookup path
  - `analyse_rust_method_params` and all downstream generated method signatures
    now use the struct-aware type resolver (`rust_value_type_for_cpp_type_with_lib`)
    so struct-typed parameters appear with correct `crate::...::Value` types
    rather than falling back to `canopy_rpc::OpaqueValue`
  - `rust_default_value_expression_for_type_name` now returns `Default::default()`
    for any `crate::...` type so `Response::from_error_code(...)` compiles for
    struct-typed out parameters
  - `generator/src/rust_protobuf_generator.cpp` now emits `from_proto_X` and
    `to_proto_X` helpers inside each interface's `interface_binding` module for
    every struct type used by a method with working codegen
  - the protobuf companion gating logic (`supports_basic_protobuf_method_codegen`)
    now recognises struct types whose fields are all scalars or strings, so those
    methods are no longer assigned `UnsupportedGeneratedMessage` proto types
  - getter and setter expression generation now delegates to the per-interface
    struct helpers via `super::from_proto_X(...)` / `super::to_proto_X(...)` when
    a struct type is detected
  - added two new methods to the probe IDL:
    - `translate_point([in] const point& p, int dx, int dy, [out] point& translated)`
    - `label_value([in] const labeled_value& input, [out] labeled_value& output)`
  - probe test verifies both methods round-trip correctly through the full
    protobuf encode/decode/dispatch path
  - `cargo test -p canopy-protobuf-runtime-probe --lib` passes with 1 test

- Added the first passing end-to-end Rust probe for a basic IDL-defined RPC
  call over generated protobuf request/response messages:
  - added a tiny dedicated probe IDL under
    [`tests/protobuf_runtime_probe/basic_rpc_probe.idl`](/var/home/edward/projects/Canopy/rust/tests/protobuf_runtime_probe/basic_rpc_probe.idl)
    with one scalar method
  - the probe build now runs the Canopy generator at build time for both
    `rpc_types.idl` and that tiny probe IDL, then invokes the in-tree Rust
    protobuf codegen on the emitted `.proto` files
  - the probe crate includes:
    - generated Rust bindings
    - generated Rust protobuf companion metadata/codecs
    - generated Rust protobuf message modules
  - the probe test now registers a local generated stub in `Service`, builds a
    generated proxy transport context, and successfully performs a real
    protobuf-encoded `add(...)` call with the expected out-param/result
    behaviour
- Kept transport concerns in the handwritten runtime rather than generator
  output:
  - added handwritten protobuf proxy-call helpers in
    [`rpc/src/serialization/protobuf/facade.rs`](/var/home/edward/projects/Canopy/rust/rpc/src/serialization/protobuf/facade.rs)
    so generated proxy methods delegate transport send/receive flow instead of
    embedding marshaller logic directly
  - renamed the generated proxy seam away from transport terminology:
    - `GeneratedProxyTransport` -> `GeneratedRpcCaller`
    - `GeneratedProxyCallContext` -> `GeneratedRpcCallContext`
    - generated proxy traits now use `proxy_caller(&self)` instead of
      `proxy_transport(&self)`
    - the marshaller/service-backed call implementation remains in handwritten
      runtime code
  - generated protobuf companions now own only protobuf message conversion for
    the supported method shapes, not transport mechanics
- Enabled the first supported generated protobuf method conversion slice for
  scalar-only methods:
  - `generator/src/rust_protobuf_generator.cpp` now emits real
    `ProtoRequest` / `ProtoResponse` bindings plus request/response conversion
    helpers for scalar-only methods with no interface parameters
  - `generator/src/rust_generator.cpp` now routes generated proxy methods
    through the handwritten protobuf proxy-call helper and fixes several older
    minimal-binding issues that blocked the probe:
    - scalar-only `call_local(...)` / `LocalCallResult`
    - correct interface scope in nested helper modules
    - `ProxySkeleton` / `StubSkeleton` now satisfy `CastingInterface`
    - out-param `Response::apply_to_call(...)` no longer self-shadows fields
- Expanded the first generated protobuf conversion slice to cover richer owned
  Rust values while keeping service/caller mechanics in handwritten runtime
  code:
  - generated protobuf companions now handle `std::string` and
    `std::vector<uint64_t>` request/response conversion in addition to the
    original scalar fields
  - generated Rust bindings no longer hardcode only `Vec<u64>` for sequence
    values; they now map scalar vectors generically for the currently
    supported scalar element types
  - byte-oriented vectors are now covered in the same slice:
    - `std::vector<uint8_t>` / `std::vector<unsigned char>` map to `Vec<u8>`
      and protobuf `bytes`
    - `std::vector<char>` / `std::vector<signed char>` map to `Vec<i8>` and
      protobuf `bytes` via signed-byte conversion helpers
  - the focused probe IDL now exercises:
    - scalar request/response
    - `std::string` request plus out-param response
    - `std::vector<uint64_t>` request plus out-param response
    - `std::vector<int>` request plus out-param response
    - `std::vector<std::string>` request plus out-param response
    - unsigned byte-vector request plus out-param response
    - signed byte-vector request plus out-param response
- Extended generated Rust and generated protobuf conversion to cover supported
  IDL-defined struct values:
  - generated Rust now emits `Value` structs for non-template IDL structs when
    every member is representable in the current Rust/protobuf slice
  - generated protobuf companions now emit `from_proto_*` / `to_proto_*`
    helpers for those supported struct types and reuse them transitively for
    nested supported structs and vectors of supported structs
  - the focused probe IDL now exercises struct-valued request/response
    round-trips through generated protobuf messages
- Tightened the generated Rust type boundary so partially supported structs do
  not leak as mixed typed/opaque bindings:
  - struct resolution now uses scope-aware parser lookup instead of matching
    only on leaf type names
  - generated Rust no longer emits a `Value` struct for an IDL struct if any
    member still falls outside the supported Rust/protobuf conversion slice
  - unsupported struct-bearing methods now fall back cleanly to
    `canopy_rpc::OpaqueValue` at the method boundary rather than embedding
    `OpaqueValue` inside a partially typed generated struct
  - protobuf struct helper emission now deduplicates and names helpers using
    qualified type identity so same-name structs in different namespaces do not
    collide
- Moved the Rust protobuf companion onto generated per-method request/response
  shapes as the marshalling unit:
  - `GeneratedProtobufMethodCodec` now owns both directions of generated
    request/response protobuf message conversion:
    - `request_to_protobuf_message(...)`
    - `request_from_protobuf_message(...)`
    - `response_to_protobuf_message(...)`
    - `response_from_protobuf_message(...)`
  - the handwritten protobuf facade now owns the byte-level proxy/stub flow
    around that codec:
    - proxy path encodes a generated `Request` through the codec and decodes a
      generated `Response`
    - stub path decodes a generated `Request`, then routes through
      `dispatch_decoded_request(...)` before encoding the generated `Response`
  - generated protobuf companions no longer need a separate free-function
    byte-conversion layer beside the codec for the supported methods
  - verified by rebuilding `generator`, rerunning
    `cargo test -p canopy-protobuf-runtime-probe --lib`, and regenerating
    `example_shared` to confirm the emitted Rust now centers on the generated
    method `Request` / `Response` units
- Split the generated Rust protobuf codec seam so proxy-facing application
  shapes and stub-facing dispatch shapes are no longer conflated:
  - `GeneratedProtobufMethodCodec` now distinguishes:
    - `ProxyRequest` / `ProxyResponse`
    - `DispatchRequest` / `DispatchResponse`
  - this lets interface-bearing methods demarshal protobuf `rpc.remote_object`
    values into wire-facing dispatch requests without pretending the proxy can
    skip the normal bind/marshalling path from application `Shared<T>` /
    `Optimistic<T>` values
  - generated Rust dispatch request/response structs for interface parameters
    stay `RemoteObject`-based and do not carry unused interface generics
  - generated protobuf companions now decode interface parameters from
    `rpc.remote_object` into `DispatchRequest` and encode interface out params
    from `DispatchResponse` back into `rpc.remote_object`
  - proxy-side conversion for interface-bearing generated requests remains
    unsupported until the outgoing bind/back-channel path is added; this
    deliberately avoids a proxy-to-stub shortcut or any direct transport
    knowledge in generated code
- Added focused Rust probe coverage for shared versus optimistic interface
  demarshalling:
  - `basic_rpc_probe.idl` now includes `i_peer` plus
    `accept_shared_peer(...)` and `accept_optimistic_peer(...)`
  - the probe validates service-level protobuf dispatch for both pointer kinds
    over the same `rpc.remote_object` wire shape
  - the shared case binds to `Shared<Arc<T>>`
  - the optimistic case binds to `Optimistic<LocalProxy<T>>` and keeps a shared
    registered peer object alive for the duration of the call, matching the C++
    lifetime rule that optimistic pointers do not own the remote object
  - verified with `cargo test -p canopy-protobuf-runtime-probe --lib`
- Removed the generated top-level `INVALID_DATA()` fallback for interface-
  bearing methods:
  - generated interface traits now emit associated target-interface types for
    each interface parameter used by service-level dispatch
  - the generated method table substitutes those associated types into the
    method-specific protobuf dispatch call, so service-level dispatch can route
    interface-bearing methods without knowing a transport or guessing an
    application implementation type
  - probe implementations set those associated types to the concrete peer
    implementation used by the test, and the probe now calls through
    `Service::send(...)` rather than directly invoking a method-specific
    dispatch helper
- Added the first C++ -> Rust generated-protobuf interop proof over the
  dynamic-library C ABI:
  - the Rust dynamic-library test child now depends on the generated protobuf
    runtime probe crate and embeds a tiny generated `i_math` implementation
  - `TestChildMarshaller::send(...)` detects protobuf `i_math` calls and routes
    them through generated Rust `__rpc_dispatch_generated(...)` /
    `GeneratedProtobufMethodCodec` dispatch rather than the previous raw echo
    payload path
  - the CMake rule for the Rust test child now passes the active CMake-built
    `generator` and `protoc` paths to Cargo so generated Rust protobuf code is
    rebuilt from the same repository tools
  - `rust_dynamic_library_abi_test` now includes
    `cxx_payload_can_call_generated_rust_protobuf_method`, where C++ sends the
    canonical protobuf wire bytes for `i_math.add(20, 22)` through
    `canopy_dll_send(...)` and validates the generated Rust response decodes to
    `c = 42` and `result = OK`
  - this is intentionally not yet a generated C++ proxy call; it proves the
    cross-language ABI/protobuf/generated-Rust dispatch seam before wiring the
    full generated C++ proxy surface into the test
  - verified with `cmake --build build_debug --target
    rust_dynamic_library_abi_test`, `./build_debug/output/rust_dynamic_library_abi_test`,
    `cargo fmt --package canopy-transport-dynamic-library-test-child`, and
    `cargo build --package canopy-transport-dynamic-library-test-child`
- Added the first generated C++ proxy -> generated Rust protobuf object proof
  over the dynamic-library C ABI:
  - `c++/tests/rust_dynamic_library_abi` now generates a C++ `basic_rpc_probe`
    IDL target from the same probe IDL used by the Rust protobuf runtime probe,
    so the host-side test uses generated C++ `probe::i_math` bindings rather
    than manually assembled protobuf bytes
  - `rust_dynamic_library_abi_test` now includes
    `generated_cxx_proxy_can_call_generated_rust_protobuf_method`, which builds
    a `rpc::root_service`, attaches a `rpc::c_abi::child_transport`, calls
    `connect_to_zone<probe::i_peer, probe::i_math>(...)`, and invokes the
    generated C++ proxy method `add(20, 22, c)`
  - the Rust test child now returns its output object descriptor in the
    C++-assigned child zone when the transport provides a usable child-zone
    address, while preserving the previous sample output object for direct
    low-level ABI init tests that do not provide object-id-capable zone
    metadata
  - the C++ protobuf generator now treats byte-vector spellings such as
    `std::vector<signed char>`, `std::vector<char>`, and compact/spaced
    unsigned/signed-char variants as protobuf `bytes`, matching the generated
    schema and avoiding repeated-field API emission for byte buffers
  - this proves the path
    `generated C++ proxy -> C ABI child transport -> generated Rust protobuf
    dispatch -> generated Rust response -> generated C++ proxy`
    for a scalar method without introducing a proxy-to-stub shortcut
  - verified with `cmake --build build_debug --target
    rust_dynamic_library_abi_test`,
    `./build_debug/output/rust_dynamic_library_abi_test
    --gtest_filter=rust_dynamic_library_abi_test.generated_cxx_proxy_can_call_generated_rust_protobuf_method`,
    `./build_debug/output/rust_dynamic_library_abi_test`,
    `cargo fmt --package canopy-transport-dynamic-library-test-child`, and
    `clang-format -i c++/tests/rust_dynamic_library_abi/rust_dynamic_library_abi_test.cpp`
- While proving the round-trip, fixed an important handwritten runtime
  deadlock:
  - `Service::outbound_send(...)` previously called into the target while still
    holding the `ObjectStub` mutex
  - `RpcBase::__rpc_call(...)` then tried to lock the same stub again to
    recover the owner service pointer, deadlocking local dispatch
  - the runtime now clones the dispatch target under the mutex and releases the
    lock before invoking `__rpc_call(...)`

## Notes

- `rustup --version` printed its version but panicked under the sandbox; this did not block `rustc`, `cargo`, `clippy`, or `rustfmt`.
- Generated Rust should only appear in ignored/generated output paths.
- The handwritten `rust/rpc` module tree now mirrors the current `c++/rpc/include/rpc/internal` file layout.
- Shared C ABI specifications belong under `c_abi/`; language-specific implementations stay under their own language roots.
- The dynamic-library C ABI currently carries packed `zone_address` blobs for
  routing and identity rather than a flattened POD address struct.
- While wiring generator-owned protobuf build metadata into the probe, the
  current C++ protobuf generator still exposed a separate drift on some
  non-`rpc` targets: several shared-pointer outputs are emitted as
  `rpc.shared_ptr_*` message types instead of the intended
  `rpc.remote_object` form. That needs to be corrected before the same
  build-metadata path can be expanded to those richer generated targets.
- Next implementation step:
  - replace the remaining field-by-field message population in
    `generator/src/rust_protobuf_generator.cpp` with conversion that is driven
    directly from the canonical generated protobuf request/response message
    types and their schema shape, so adding new supported IDL fields does not
    require duplicating per-field set/get emission in multiple places
  - extend the current interface-pointer support from the focused service-level
    probe toward richer existing IDLs while keeping the C++ IDL distinction
    between `rpc::shared_ptr<T>` and `rpc::optimistic_ptr<T>` as seen in
    [`../c++/tests/idls/example/example.idl`](/var/home/edward/projects/Canopy/c++/tests/idls/example/example.idl):
    both pointer kinds can use `rpc.remote_object` as the protobuf wire value,
    but generated Rust metadata, request/response bindings, local bind helpers,
    add-ref/release semantics, and collection handling must preserve whether
    each value came from a shared or optimistic IDL pointer
  - add validation coverage over real `example.idl` methods such as
    `set_optimistic_ptr(...)` / `get_optimistic_ptr(...)` plus comparable
    `rpc::shared_ptr<T>` methods, beyond the current focused
    `basic_rpc_probe.idl` coverage
  - add proxy-side outgoing interface binding / back-channel support for
    interface-bearing generated proxy requests; this remains intentionally
    separate from stub demarshalling and must stay in handwritten runtime seams
    rather than direct proxy-to-stub calls
  - add the reverse cross-language proof:
    generated Rust proxy -> dynamic-library C ABI transport -> generated C++
    protobuf object, starting with scalar methods and then expanding to richer
    IDL values
  - expand validation from structural regeneration to a richer existing IDL
    round-trip, such as `example_shared`, once the current protobuf schema drift
    and unsupported field shapes are handled
  - keep generator output limited to protobuf message conversion and method
    binding; caller/service/transport mechanics remain handwritten runtime
    concerns
- Planned Rust application ergonomics should stay close to the current C++
  `rpc::base` experience:
  - application structs implement generated interface traits
  - a generated `make_rpc_object(...)`-style helper wraps the object for
    Canopy
  - future back-channel-aware application entrypoints should be supported via
    a dispatch context rather than leaking low-level RPC plumbing into app code
