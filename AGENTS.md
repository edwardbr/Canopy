<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Canopy Development Guide

## Git Policy

**IMPORTANT: DO NOT perform any git operations in this project without user authorization.**

- **DO NOT** run git commands without explicit authorization

## Overview

Canopy is a Remote Procedure Call library for modern C++ that enables type-safe communication across different execution contexts (in-process, inter-process, remote machines, embedded devices, SGX enclaves). The system uses Interface Definition Language (IDL) files to generate C++ proxy/stub code with full JSON schema generation capabilities.

## Project Structure

### Core Directories
- **`/rpc/`** - Core RPC library implementation
  - Headers in `rpc/include/rpc`
  - Implementations in `rpc/src` (mirror the API tree)
- **`/generator/`** - IDL code generator (creates C++ from .idl files)
- **`/submodules/idlparser/`** - IDL parsing engine
  - AST Classes in `coreclasses.h` (entity, interface, attributes)
  - String extraction functions: `extract_string_literal()`, `extract_multiline_string_literal()`
- **`/transport/`** - Transport implementations (tcp, spsc, etc.)
- **`/tests/`** - Test suite
  - `tests/common` - Shared fixtures
  - `tests/test_enclave` + `tests/test_host` - SGX orchestration
  - `tests/fuzz_test` - Fuzz assets
  - `tests/idls/` - Test IDL files
- **`/telemetry/`** - Optional telemetry/logging subsystem
- **`/cmake/`** - CMake build configuration modules
  - `Canopy.cmake` - Main build configuration
  - `Linux.cmake` - Linux-specific settings
  - `Windows.cmake` - Windows-specific settings
  - `SGX.cmake` - SGX enclave support (included when CANOPY_BUILD_ENCLAVE=ON)
  - `CanopyGenerate.cmake` - IDL code generation macros
  - `FindSGX.cmake` - SGX SDK finder module
- **`/documents/`** - Documentation (see Documentation Structure below)
- **`/build/`** - Build output (disposable per preset)
  - `build/generated` - IDL outputs and proxies

## Documentation Structure
There is detailed documentation in the documents folder if you need more information

### Key Files
- **`CMakeLists.txt`** - Main build configuration (version 2.2.0)
- **`README.md`** - Project documentation and build instructions
- **`CMakePresets.json`** - CMake preset configurations
- **`.vscode/settings.json`** - VSCode configuration (file associations only)
- **`.clang-format`** - WebKit base, 4-space indent, type-aligned pointers

## Build System

### CMake Configuration
- **Generator**: Ninja
- **C++ Standard**: C++20 when working with coroutines, otherwise C++17
- **Toolchain Requirements**: Clang ≥10, GCC ≥9.4, MSVC 2019
- **Presets**: Defined in `CMakePresets.json`

### Base Configuration
The "Base" preset provides default settings inherited by other presets:
```cmake
CANOPY_BUILD_COROUTINE=OFF            # Coroutines disabled by default
CANOPY_BUILD_ENCLAVE=OFF              # SGX enclave support disabled
CANOPY_BUILD_TEST=ON                  # Test building enabled
CANOPY_DEBUG_GEN=OFF              # Debug code generation disabled by default
CMAKE_EXPORT_COMPILE_COMMANDS=ON # Export compile commands for tooling
CANOPY_STANDALONE=ON              # Standalone build mode
```

### Available CMake Presets
- **`Debug`** - Standard debug build (no coroutines) with logging/telemetry hooks enabled
- **`Coroutine_Debug`** - Debug build with coroutine pipeline (proxies emit `coro::task<T>` signatures)
- **`SGX_Debug`** - Debug build with SGX hardware enclave support
- **`SGX_Sim_Debug`** - Debug build with SGX simulation mode
- **`Release`** - Optimized release build
- **`SGX_Release`** - Release build with SGX hardware support


## Blocking and Coroutine support
- Supports both blocking and coroutine functionallity 
- macros CORO_TASK CO_RETURN CO_AWAIT are there to resolve to blocking and coroutines depending on the build flags 

## Coding Style & Naming Conventions

### Code Formatting
- Apply `.clang-format` (WebKit base, 4-space indent, type-aligned pointers) via `clang-format -i` each time you finish making changes to a file
- Baseline code targets C++17; coroutine builds rely on C++20 toolchain
- Lowercase camel back with curly brackets on a new line, being dyslexic it makes it easier to read

### Naming Conventions
- **Interfaces**: Follow `Name` pattern
- **Generated Proxies**: Become `Name_proxy`
- **Generated Stubs**: Become `Name_stub`
- **Services**: Use `Name_service` pattern
- **Telemetry Components**: Use `*_telemetry_service` pattern
- **IDL Interfaces**: By convention all interfaces are named with a leading `i_` in IDL (not used for non-marshalled interfaces)

**IDL-Derived Interfaces** (using `rpc::shared_ptr` or `rpc::optimistic_ptr`):
- All interfaces defined in `.idl` files (e.g., `i_host`, `i_example`) 
- Generated proxy/stub classes for RPC marshalling
- User-implemented interface classes

## Architecture

### Zone and Transport Lifecycle Management

**Key Principles**:
1. **Parent Transport Lifetime**: Must remain alive as long as there's a positive reference count between zones in either direction
2. **Single Parent Transport**: Only one parent transport per zone
3. **Child Service Ownership**: Must have strong reference to parent transport to keep parent zone alive if the transport is hierarchical
4. **Transport Lifetime**: All transports and services must keep parent transport alive until they die
5. **Stub Ownership**: Stubs instantiated in service must keep service alive via `std::shared_ptr`
6. **Zone Lifecycle Management**: Zones kept alive through `shared_ptr` to objects, not service storage
7. **Hidden Service Principle**: Each object only interacts with current service via `get_current_service()`

### Hierarchical Transport Circular Dependency Pattern

All hierarchical transports (local, SGX enclave, DLL) implement an intentional circular dependency for zone lifetime management.

**Applicable to:**
- Local transport (in-process parent/child zones)
- SGX enclave transport (host/enclave communication)
- DLL transport (cross-DLL boundary communication)

**Design Pattern:**
```
Parent Zone: child_transport → stdex::member_ptr<parent_transport> (to child zone)
Child Zone:  parent_transport → stdex::member_ptr<child_transport> (to parent zone)
```

**Stack-Based Lifetime Protection:**
When calls cross zone boundaries, stack-based `shared_ptr` protects transport lifetime:

```cpp
// In child_transport (parent zone), calling into child zone:
CORO_TASK(int) child_transport::outbound_send(...) {
    auto child = child_.get_nullable();  // Stack-based shared_ptr<parent_transport>
    if (!child) CO_RETURN rpc::error::ZONE_NOT_FOUND();

    // child shared_ptr keeps parent_transport alive during entire call
    CO_RETURN CO_AWAIT child->inbound_send(...);
}
```

**Safe Disconnection Protocol:**
1. `child_service` destructor calls `parent_transport->set_status(DISCONNECTED)`
2. `parent_transport::set_status()` override propagates to parent zone
3. `child_transport::on_child_disconnected()` breaks its circular reference
4. Both transports break their references, circular dependency resolved
5. Stack-based protection ensures no use-after-free during active calls

**Critical Safety Properties:**
- Zone boundaries respected: child_service only touches its own transport
- Status propagation handles cross-zone coordination
- Stack protection prevents destruction during active calls
- Natural cleanup when call stack unwinds

See `documents/transports/hierarchical.md` and `documents/architecture/07-zone-hierarchies.md` for comprehensive details.

### Smart Pointer Architecture
- `rpc::shared_ptr` - used for remote RAII
- `rpc::optimistic_ptr` - used for remote interfaces not managed by RAII

**Core Components** (using `std::shared_ptr`):
- `rpc::service` - Main service management class
- `rpc::service_proxy` - Service communication proxy (holds strong reference to transport via `member_ptr`)
- `rpc::object_proxy` - Object reference proxy
- `rpc::child_service` - Child zone service management (hierarchical transports only: local, SGX, DLL)
- `rpc::pass_through` - Routes communication between non-adjacent zones through intermediary (holds strong references to both transports and the intermediary service, see `documents/architecture/06-transports-and-passthroughs.md`)
- `rpc::transport` - A base class for linking two zones together e.g. TCP and SPSC

**Transport Lifetime Management**:
As documented in `rpc/include/rpc/internal/service.h`, transports are owned by:
- **Service proxies** - Hold strong references (`member_ptr<transport>`) to route calls
- **Passthroughs** - Hold strong references to both transports and the intermediary service, keeping entire routing paths alive
- **Child services** - Hold strong reference to parent transport
- **Active stubs** - May cause transports to hold references to adjacent transports during calls
- **Services** - Hold only weak references (registry for lookup, doesn't keep alive)

**Service Lifetime Management**:
Services are kept alive by:
- **Local objects** (stubs) - Objects living in the zone
- **Child services** - Hold strong reference to parent service (via parent transport)
- **Passthroughs** - Hold strong reference to intermediary service, allowing zones to function purely as routing hubs

**rpc::shared_ptr std::shared_ptr are not the same**:
- **NEVER** cast between `rpc::shared_ptr` and `std::shared_ptr`
- **NEVER** use raw pointer conversion between the two types
- Use proper type-specific containers and member pointers

**Type Ownership Patterns**:
- Core components OWN `std::shared_ptr` objects
- Core components can REFERENCE `rpc::shared_ptr` objects (IDL interfaces)
- IDL interfaces only work with `rpc::shared_ptr` for marshalling compatibility

### Transport Layer
- **In-Memory**: Direct function calls
- **SPSC**: Single-producer single-consumer queues
- **TCP**: Network transport
- **Arena-based**: Different memory spaces
- **SGX Enclaves**: Secure execution environments
- **Serialization**: YAS library for data marshalling

**Supported Encoding Formats**:
- `enc_default` - Default encoding (typically yas_binary)
- `yas_binary` - Binary serialization
- `yas_compressed_binary` - Compressed binary serialization
- `yas_json` - JSON serialization (universal fallback format)
- `protocol_buffers` - Protocol buffers

**Format and Version Negotiation**:
- Invalid encoding formats automatically fall back to `yas_json`

### Key Dependencies
- **YAS**: Serialization framework
- **libcoro**: Coroutine support (when CANOPY_BUILD_COROUTINE=ON)
- **c-ares**: DNS resolution (configured in submodules)
- **range-v3**: Range library support
- **spdlog**: Logging framework for telemetry
- **CMake 3.24+**: Build system requirement

### Design Characteristics
- **Type Safety**: Full C++ type system support
- **Transport Independence**: Protocol-agnostic design
- **Modern C++**: Leverages C++17/C++20 features
- **Performance**: Zero-copy serialization where possible
- **Extensibility**: Plugin-based transport system
- **Automatic Fallback**: Format and version negotiation for compatibility

## IDL System

### Supported IDL Features
- Interfaces (pure virtual base classes)
- Structures with C++ STL types
- Attributes with name=value pairs
- Namespaces
- Basic types: int, string, vector, map, optional, variant

### Code Generation Process
1. Parse .idl files using idlparser
2. Extract attributes (including descriptions)
3. Generate C++ headers with proxy/stub code
4. Include JSON schema metadata in function_info structures
5. Compile generated code with main project

Generator code: `/generator/src/synchronous_generator.cpp`

### Adding New IDL Features
1. Update parser in `/submodules/idlparser/`
2. Modify code generator in `/generator/src/`
3. Add test IDL files in `/tests/idls/`
4. Create corresponding tests
5. Update documentation

## Development Workflow


### Build Commands
```bash
# Configure with a preset
cmake --preset Debug                    # Host build with logging/telemetry
cmake --preset Coroutine_Debug          # Enable coroutine pipeline
cmake --preset SGX_Debug                # SGX enclave support

# Build core library
cmake --build build --parallel 32 --

# Regenerate interfaces after IDL edits
cmake --build build --target rpc_generator
cmake --build build --target <interface_name>_idl

# Build and run tests
cmake --build build --target <test_name>
./build/output/debug/<test_name>

# Execute all tests
ctest --test-dir build --output-on-failure

# Run SGX tests
ctest --tests-regex enclave

# Enable coverage (review build/coverage before large merges)
cmake --preset Debug -DENABLE_COVERAGE=ON
```

### Testing Guidelines

**Host Tests**:
- Adopt GoogleTest with filenames suffixed `_test.cpp`
- Reflect runtime topology in namespaces and fixture names
- Test types:
  - **Unit Tests**: Individual component testing
  - **Integration Tests**: Full IDL→C++ generation pipeline
  - **JSON Schema Tests**: Metadata extraction and schema validation
  - **Hierarchical Zone Tests**: Multi-level zone creation and cross-zone marshalling

**SGX Tests**:
- Run `cmake --preset SGX_Debug` then `ctest --tests-regex enclave`
- Never gate enclave tests into default CI

### Coroutine Support

**Template-Based Test Infrastructure**:
Coroutine tests use a template-based approach for improved debugging:

```cpp
// Define coroutine test function
template<class T> CORO_TASK(bool) coro_test_name(T& lib) {
    // test implementation
    CO_RETURN true;
}

// Invoke via run_coro_test dispatcher
TYPED_TEST(remote_type_test, test_name) {
    run_coro_test(*this, [](auto& lib) {
        return coro_test_name<TypeParam>(lib);
    });
}
```

**Benefits**:
- Template functions can be debugged with standard debuggers (macros cannot)
- Clean function signatures with proper return types
- Unified testing approach across all coroutine tests

### Common Development Tasks

**This should build everything including any proxies and stubs as a result to idl changes**:
```bash
cmake --build build
```

**Generate Code from IDL**:
```bash
cmake --build build --target <interface_name>_idl
```

**Run JSON Schema Tests**:
```bash
cmake --build build --target simple_json_schema_metadata_test
./build/output/debug/simple_json_schema_metadata_test
```

**Run Hierarchical Zone Tests**:
```bash
cmake --build build --target fuzz_test_main
./build/output/debug/fuzz_test_main 3  # Run 3 iterations
```

Tests multi-level zone hierarchies, cross-zone `rpc::shared_ptr` marshalling, `place_shared_object` functionality, and property-based testing with telemetry visualization.


### Logging and Telemetry Services
- Prefer structured logging macros: `RPC_INFO`, `RPC_WARNING`, etc.
- Telemetry defaults to console output
- Enable `CANOPY_USE_TELEMETRY_RAII_LOGGING` only during investigations and scrub traces prior to upload
- Canopy provides comprehensive telemetry services for debugging and visualization. They will harm performance.

### Documentation
- Avoid using the word Critical, when amendments are asked for.  Blend the amendments into the documentation as if they were there all the time.

### Beads Issue Tracking
Quick commands for `bd` (beads):
- Create: `bd create "Title" -t task -d "Description" --acceptance "Criteria"`
- Quick create (ID only): `bd q "Title"`
- List/search: `bd list`, `bd search "text"`
- Show/edit: `bd show <id>`, `bd edit <id>` or `bd update <id> --description "..."`
- If `bd create` hangs, retry with `bd --no-daemon create ...`

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds.  Be sure that the code compiles in coroutine and non coroutine modes
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd sync
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
