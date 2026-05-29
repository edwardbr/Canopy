<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Building Canopy

Scope note:

- this is the build and test guide for the primary C++ implementation
- preset names, target names, test executables, and output paths in this file
  refer to the C++ tree unless explicitly stated otherwise

This guide covers building Canopy from source, including build configuration options and CMake presets.

## 1. Prerequisites

### Required Tools

- **CMake**: 3.24 or higher
- **C++ Compiler**: Clang 10+, GCC 9.4+, or MSVC 2019+
- **Build System**: Ninja (recommended)
- **Git**: For fetching submodules

### Optional Dependencies

- **libcoro**: For coroutine support (`CANOPY_BUILD_COROUTINE=ON`)
- **Intel SGX SDK**: For enclave support (`CANOPY_BUILD_ENCLAVE=ON`)
- **Python**: For protobuf JavaScript generation (demo only)

### Platform-Specific Requirements

**Linux**:
```bash
sudo apt-get install cmake ninja-build clang-10 g++-9
```

**macOS**:
```bash
brew install cmake ninja clang-format
```

**Windows**:
- Visual Studio 2019 or later
- CMake 3.24+
- Ninja (via vcpkg or chocolatey)

## 2. Getting the Source Code

```bash
# Clone the repository
git clone https://github.com/edwardbr/Canopy.git
cd Canopy

# Initialize submodules
git submodule update --init --recursive
```

## 3. Build Configuration

### CMake Presets

Canopy uses CMake presets for common build configurations:

```bash
# List available presets
cmake --list-presets
```

| Preset | Description |
|--------|-------------|
| `Debug` | Standard debug build with logging/telemetry |
| `Debug_ASAN` | Debug build with AddressSanitizer |
| `Debug_Coroutine` | Debug with C++20 coroutines enabled |
| `Debug_Coroutine_ASAN` | Coroutine debug with AddressSanitizer |
| `Debug_Coroutines_With_No_Logging` | Coroutines without logging overhead |
| `Debug_Coverage` | Debug build with gcov coverage flags enabled |
| `Debug_Coroutine_Coverage` | Coroutine debug build with gcov coverage flags enabled |
| `Release` | Optimized production build |
| `Debug_SGX` | SGX hardware enclave support (debug) |
| `Debug_SGX_Sim` | SGX simulation mode (debug) |
| `Debug_Coroutine_Fake_SGX` | Coroutine SGX transport built against the fake SGX backend for sanitizer/debug testing |
| `Release_SGX` | SGX hardware release build |
| `Release_SGX_Sim` | SGX simulation release build |
| `Release_Coroutine_Fake_SGX` | Optimized coroutine SGX transport built against the fake SGX backend |

### Local Custom Presets

You can keep machine-specific presets in `CMakeUserPresets.json` without editing tracked project presets.

```bash
# Create local presets from the provided template
cp CMakeUserPresets.json.example CMakeUserPresets.json

# Verify both project and user presets
cmake --list-presets
```

Example local preset names from the template:
- `My_Debug`
- `My_Coroutine_Coverage`

### Base Configuration

The tracked repository presets inherit these defaults from `Base`:

```cmake
CANOPY_BUILD_COROUTINE=OFF              # Coroutines disabled by default
CANOPY_BUILD_ENCLAVE=OFF                # SGX enclave support disabled
CANOPY_BUILD_TEST=ON                    # Test building enabled
CANOPY_BUILD_DEMOS=ON                   # Demo building enabled in repository presets
CANOPY_BUILD_BENCHMARKING=ON            # Benchmark building enabled in repository presets
CANOPY_BUILD_RUST=OFF                   # Rust workspace disabled by default
CANOPY_VERBOSE_GENERATOR=OFF            # Debug code generation disabled
CMAKE_EXPORT_COMPILE_COMMANDS=ON        # Export compile commands
```

These are repository preset values. The raw CMake option defaults are listed
below and are more conservative for downstream `add_subdirectory()` consumers.

### CMake Module Structure

The build configuration is modularized into separate files in the `cmake/` directory:

| File | Description |
|------|-------------|
| `Canopy.cmake` | Main configuration - options, shared defines, includes platform files |
| `Linux.cmake` | Linux-specific compiler flags, warnings, sanitizers |
| `Windows.cmake` | Windows/MSVC-specific settings and libraries |
| `SGX.cmake` | SGX enclave support (only included when `CANOPY_BUILD_ENCLAVE=ON`) |
| `CanopyGenerate.cmake` | IDL code generation macros |
| `FindSGX.cmake` | CMake find module for Intel SGX SDK |

This modular structure allows:
- Non-SGX users to work with simpler configuration
- Platform-specific settings to be maintained independently
- Easy extension for new platforms or enclave technologies

## 4. Building

### Standard Build

```bash
# Configure with Debug preset
cmake --preset Debug

# Build the project
cmake --build build_debug --parallel $(nproc)

# Or for a specific target
cmake --build build_debug --target rpc        # Core library only
cmake --build build_debug --target generator  # IDL generator only
```

### Coroutine Build

```bash
# Configure with coroutines enabled
cmake --preset Debug_Coroutine

# Build
cmake --build build_debug_coroutine --parallel $(nproc)
```

### SGX Enclave Build

```bash
# Hardware mode (requires SGX-capable CPU)
cmake --preset Debug_SGX

# Simulation mode (no hardware required)
cmake --preset Debug_SGX_Sim

cmake --build build_debug_sgx
# or
cmake --build build_debug_sgx_sim
```

### Fake SGX Coroutine Build

The fake SGX backend builds the coroutine SGX transport and enclave runtime as
ordinary shared libraries. It does not provide SGX security. Its purpose is to
exercise `c++/transports/sgx_coroutine` and the Canopy SGX coroutine/polyfill
code under normal host debuggers and sanitizers.

```bash
cmake --preset Debug_Coroutine_Fake_SGX
cmake --build build_debug_coroutine_fake_sgx --target rpc_test
```

Use `Release_Coroutine_Fake_SGX` for an optimized fake-backend build. The fake
backend selects `CANOPY_SGX_BACKEND=Fake`, requires coroutines and Nanopb, and
keeps full Google protobuf out of enclave targets. It provides only the narrow
host-side SGX shims needed for testing: enclave create/destroy, ECALL dispatch,
basic enclave/outside memory checks, and the minimum runtime headers needed when
Intel's enclave C++ support is unavailable or unsuitable for this path.

Fake SGX tests should exercise the same coroutine SGX runtime shutdown contract
as SGX simulation: release interface pointers, keep driving the scheduler until
the service shutdown event fires, then release the scheduler/runtime. The
shutdown event is the completion signal; `check_is_empty()` is diagnostic, not a
fixture control-flow mechanism.

The shared typed transport fixtures centralise the host/root-service part of
that teardown in `transport_setup_base`. SGX coroutine fixtures also wait for
their created stream transports to release final self-references before dropping
the fixture scheduler, because stream cleanup may complete on a scheduler worker.

### Release Build

```bash
cmake --preset Release
cmake --build build_release
```

### Coverage HTML Reports

Coverage targets are available when `CANOPY_ENABLE_COVERAGE=ON` and tests are enabled.

```bash
# Configure coverage build (debug)
cmake --preset Debug_Coroutine_Coverage

# Build tests and runtime
cmake --build build_debug_coroutine --parallel $(nproc)

# Run single-process rpc_test and generate HTML report
cmake --build build_debug_coroutine --target coverage-html
```

Available targets:
- `coverage-reset` - remove stale `.gcda` files in the active build directory
- `coverage-run-rpc_test` - run `rpc_test --telemetry-console` in single-process mode
- `coverage-html-gcovr` - generate `build_*/coverage/gcovr/index.html`
- `coverage-html-lcov` - generate `build_*/coverage/lcov/html/index.html`
- `coverage-html` - default HTML target (`gcovr` when available, otherwise `lcov`)

## 5. Build Options

### Key CMake Options

```bash
# Execution mode
cmake --preset Debug -DCANOPY_BUILD_COROUTINE=ON/OFF

# Features
cmake --preset Debug -DCANOPY_BUILD_ENCLAVE=ON/OFF
cmake --preset Debug -DCANOPY_BUILD_TEST=ON/OFF
cmake --preset Debug -DCANOPY_BUILD_DEMOS=ON/OFF
cmake --preset Debug -DCANOPY_BUILD_BENCHMARKING=ON/OFF
cmake --preset Debug -DCANOPY_BUILD_RUST=ON/OFF
cmake --preset Debug -DCANOPY_BUILD_PROTOCOL_BUFFERS=ON/OFF
cmake --preset Debug -DCANOPY_BUILD_NANOPB=ON/OFF

# Debugging
cmake --preset Debug -DCANOPY_USE_LOGGING=ON/OFF
cmake --preset Debug -DCANOPY_USE_TELEMETRY=ON/OFF
cmake --preset Debug -DCANOPY_VERBOSE_GENERATOR=ON/OFF

# Sanitizers (when CANOPY_BUILD_TEST=ON)
cmake --preset Debug -DCANOPY_DEBUG_ADDRESS=ON/OFF
cmake --preset Debug -DCANOPY_DEBUG_THREAD=ON/OFF
cmake --preset Debug -DCANOPY_DEBUG_UNDEFINED=ON/OFF
cmake --preset Debug -DCANOPY_DEBUG_LEAK=ON/OFF
```

### Common Active Options

The raw CMake option defaults are conservative for downstream consumers.
Repository presets may override these values for local development builds.

```cmake
# Core options
option(CANOPY_BUILD_COROUTINE "Enable C++20 coroutine support" OFF)
option(CANOPY_BUILD_ENCLAVE "Enable SGX enclave support" OFF)
option(CANOPY_BUILD_TEST "Build test code" OFF)
option(CANOPY_BUILD_DEMOS "Build demo code" OFF)
option(CANOPY_BUILD_BENCHMARKING "Build benchmarking code" OFF)
option(CANOPY_BUILD_RUST "Build the Rust workspace alongside the C++ build" OFF)
option(CANOPY_BUILD_PROTOCOL_BUFFERS "Include full Google C++ Protocol Buffers support" ON)
option(CANOPY_BUILD_NANOPB "Include Nanopb protobuf-compatible support" ON)
option(CANOPY_BUILD_CANONICAL_CRYPTO "Include deterministic canonical_crypto serialization support" ON)
option(CANOPY_BUILD_WEBSOCKET "Include websocket support" ${CANOPY_BUILD_WEBSOCKET_DEFAULT})

# Debug options
option(CANOPY_VERBOSE_GENERATOR "Enable debug output for code generation" OFF)
option(CANOPY_USE_LOGGING "Enable RPC logging" OFF)
option(CANOPY_USE_TELEMETRY "Enable RPC telemetry" OFF)
option(CANOPY_USE_CONSOLE_TELEMETRY "Enable console telemetry output" OFF)
option(CANOPY_USE_TELEMETRY_RAII_LOGGING "Enable RAII telemetry logging" OFF)
option(CANOPY_HANG_ON_FAILED_ASSERT "Hang on failed assert" OFF)

# Sanitizers
option(CANOPY_DEBUG_ADDRESS "Enable address sanitizer" OFF)
option(CANOPY_DEBUG_THREAD "Enable thread sanitizer" OFF)
option(CANOPY_DEBUG_UNDEFINED "Enable undefined behavior sanitizer" OFF)
option(CANOPY_DEBUG_LEAK "Enable leak sanitizer" OFF)
option(CANOPY_DEBUG_ALL "Enable all sanitizers" OFF)

# SGX options
set(SGX_MODE "debug" CACHE STRING "SGX mode: debug or release")
set(SGX_HW "OFF" CACHE BOOL "Enable SGX hardware (vs simulation)")
set(CANOPY_SGX_BACKEND "Intel" CACHE STRING "SGX backend: Intel or Fake")
set(CANOPY_ATTESTATION_BACKEND "NULL" CACHE STRING "Attestation backend")
set(CANOPY_SECURE_STREAM_BACKEND "OPENSSL" CACHE STRING "Secure stream backend")
option(CANOPY_BUILD_MBEDTLS "Build bundled Mbed TLS support" ${CANOPY_BUILD_MBEDTLS_DEFAULT})
option(CANOPY_BOOTSTRAP_SGX_SDK "Build and install the Intel SGX SDK from submodule source when missing" OFF)
option(CANOPY_SGX_BOOTSTRAP_UPDATE_SUBMODULES "Run the Intel SGX SDK preparation step, which updates nested SGX SDK submodules" ON)
```

`CANOPY_BUILD_WEBSOCKET` can be explicitly enabled in both blocking and
coroutine builds. Its default remains tied to coroutine demo/test presets, so
blocking consumers should set it to `ON` when they need WebSocket support.

### Protobuf-Compatible Serialization Options

`CANOPY_BUILD_PROTOCOL_BUFFERS` enables the full Google C++ protobuf runtime and generated C++ protobuf API. It is useful for ordinary host processes that need the full protobuf feature set.

`CANOPY_BUILD_NANOPB` enables the Nanopb-backed protobuf-compatible runtime. It uses `.proto` files and protobuf wire bytes, but links the small Nanopb C runtime plus Canopy-generated adapters instead of `protobuf::libprotobuf`. This is the intended protobuf-compatible path for SGX enclave builds.

These options are independent.  `CanopyGenerate(... protocol_buffers ...)`
requests protobuf-compatible schema/wire support for an IDL target; CMake then
generates the full protobuf backend when `CANOPY_BUILD_PROTOCOL_BUFFERS=ON` and
the Nanopb backend when `CANOPY_BUILD_NANOPB=ON`.  If both are ON, both generated
backends can coexist and `rpc::encoding::protocol_buffers` and
`rpc::encoding::nanopb` use their respective backends.

If only one protobuf-compatible backend is enabled, the other encoding is treated
as an alias.  With `CANOPY_BUILD_PROTOCOL_BUFFERS=OFF` and `CANOPY_BUILD_NANOPB=ON`,
`rpc::encoding::protocol_buffers` routes through Nanopb.  With
`CANOPY_BUILD_NANOPB=OFF` and `CANOPY_BUILD_PROTOCOL_BUFFERS=ON`,
`rpc::encoding::nanopb` routes through the full protobuf backend.

SGX enclave targets never link the full Google C++ protobuf runtime.  When
`CANOPY_BUILD_ENCLAVE=ON`, host-side targets may still build full protobuf
support, but enclave compile definitions remove `CANOPY_BUILD_PROTOCOL_BUFFERS`
and enable the `protocol_buffers` to Nanopb alias.  Typical SGX release-style
configurations therefore use:

```cmake
CANOPY_BUILD_ENCLAVE=ON
CANOPY_BUILD_NANOPB=ON
CANOPY_BUILD_PROTOCOL_BUFFERS=OFF
```

Nanopb still needs protobuf tooling at build time so Canopy can compile the generated `.proto` files. That build-time dependency does not imply that enclave targets link the full protobuf runtime.

Shared objects that link the full Google C++ protobuf runtime must run module
cleanup before unload.  The dynamic-library transports provide a required
shutdown hook for this: `canopy_dll_shutdown` for the non-coroutine C++ and C ABI
DLL transports, and before the coroutine dynamic-library entry point returns.  These
hooks call `google::protobuf::ShutdownProtobufLibrary()` only when
`CANOPY_BUILD_PROTOCOL_BUFFERS` is enabled.

`CANOPY_BOOTSTRAP_SGX_SDK=ON` allows CMake to install the SGX SDK from `submodules/confidential-computing.sgx` when no SDK is already available. `CANOPY_SGX_BOOTSTRAP_UPDATE_SUBMODULES=OFF` keeps bootstrap enabled but skips SGX SDK source preparation when an SDK installer or already-prepared source tree is present, avoiding repeated nested SGX submodule updates after the first preparation.

## 6. Build Targets

### Core Library

```bash
cmake --build build_debug --target rpc          # Core RPC library
cmake --build build_debug_sgx --target rpc_enclave   # Enclave library
```

### Generator

```bash
cmake --build build_debug --target generator  # IDL code generator
```

### Transports

```bash
cmake --build build_debug --target transport_local                # Local transport
cmake --build build_debug --target transport_dynamic_library      # Blocking DLL transport
cmake --build build_debug --target transport_c_abi                # C ABI DLL transport
cmake --build build_debug --target transport_streaming            # Stream-backed transport, blocking mode
cmake --build build_debug_coroutine --target transport_streaming  # Stream-backed transport, coroutine mode
cmake --build build_debug --target connection_factory_config_idl  # Generated connection factory options
cmake --build build_debug_coroutine --target transport_ipc_transport
cmake --build build_debug_coroutine --target transport_libcoro_host_scheduled_dynamic_library
cmake --build build_debug_coroutine --target transport_libcoro_dll_scheduled_dynamic_library
```

`connection_factory` is an interface library target used from
`target_link_libraries`, not a standalone build target.

### Tests

These targets are available only when `CANOPY_BUILD_TEST=ON`; turning that
option off also skips the integration fuzz test subdirectory. The optional Rust
child used by some fuzz parity scenarios is only wired into `fuzz_test_gtest`
when `CANOPY_BUILD_RUST=ON`, Protocol Buffers are enabled, the build is
non-coroutine, and `cargo` is found.

```bash
cmake --build build_debug --target rpc_test
cmake --build build_debug --target serialiser_test
cmake --build build_debug --target fuzz_test_main
cmake --build build_debug --target fuzz_test_gtest
cmake --build build_debug --target member_ptr_test
cmake --build build_debug --target passthrough_test
cmake --build build_debug --target zone_address_test
```

### Benchmarks

Benchmark targets are available when `CANOPY_BUILD_BENCHMARKING=ON`. The main
fullstack executable is `benchmark`:

```bash
cmake --build build_debug --target benchmark
cmake --build build_debug_coroutine --target benchmark

./build_debug/output/benchmark --html-report build_debug/blocking-fullstack.html
./build_debug_coroutine/output/benchmark --html-report build_debug_coroutine/coroutine-fullstack.html
```

The fullstack benchmark can be filtered with:

```bash
./build_debug/output/benchmark \
  --transport local,tcp,dll \
  --size 64,4096 \
  --format yas_binary,protocol_buffers \
  --passes 5 \
  --html-report report.html
```

Blocking fullstack builds cover `local`, `dll`, `tcp`, and, when the relevant
features are enabled, `tls+tcp`, `ws+tcp`, and `tls+ws+tcp`.

Coroutine fullstack builds cover `local`, TCP/TLS/WebSocket stream transports,
`libcoro_dll_scheduled`, `libcoro_host_scheduled`, `ipc_direct`, `ipc_dll`,
`spsc`, the `io_uring_*` variants, and SGX io_uring variants when enclave
support is enabled.

### Demos

```bash
cmake --build build_debug --target websocket_server             # Blocking calculator-only WebSocket demo
cmake --build build_debug_coroutine --target websocket_server   # Coroutine WebSocket demo server
```

## 7. AddressSanitizer Support

Canopy has comprehensive AddressSanitizer (ASan) support for detecting memory safety issues during development and testing.

### Available ASan Presets

| Preset | Description | Binary Directory |
|--------|-------------|------------------|
| `Debug_ASAN` | Standard debug build with ASan | `build_debug` |
| `Debug_Coroutine_ASAN` | Coroutine build with ASan | `build_debug_coroutine` |

### Building with AddressSanitizer

```bash
# Standard debug build with ASan
cmake --preset Debug_ASAN
cmake --build build_debug --parallel $(nproc)

# Coroutine build with ASan
cmake --preset Debug_Coroutine_ASAN
cmake --build build_debug_coroutine --parallel $(nproc)
```

### Running Tests with AddressSanitizer

**Individual test run:**
```bash
./build_debug/output/rpc_test --gtest_filter="*test_name*"
```

**Run all tests individually (recommended for ASan):**
```bash
# Standard build
c++/tests/scripts/run_asan_tests.sh

# Coroutine build
c++/tests/scripts/run_asan_tests_coroutine.sh
```

These scripts run each test individually and report any AddressSanitizer errors, creating detailed logs for failed tests in `/tmp/`.

### ASan Configuration

AddressSanitizer is configured in `cmake/Linux.cmake` with the following flags:

```cmake
CANOPY_DEBUG_ADDRESS=ON  # Enables AddressSanitizer
Compiler flags: -fsanitize=address -fno-omit-frame-pointer
Linker flags: -fsanitize=address
```

### Environment Variables

For specialized testing scenarios, you can configure ASan behavior:

```bash
# Suppress specific checks (useful for known false positives)
export ASAN_OPTIONS="detect_leaks=0:detect_container_overflow=0"
export LSAN_OPTIONS="detect_leaks=0"

./build_debug/output/rpc_test
```

### Known Issues and Suppressions

**Protobuf Container-Overflow False Positive:**
- **Issue**: Protobuf's static initialization triggers container-overflow during test discovery
- **Suppression**: Set `ASAN_OPTIONS=detect_container_overflow=0`
- **Impact**: Only affects coroutine builds that use protobuf serialization

**SGX simulation builds:**
- Standard Clang sanitizers apply to the host/non-enclave objects in the SGX
  simulation build. They do not instrument enclave code as native SGX
  sanitizers.
- For focused SGX simulation ASan runs, use
  `ASAN_OPTIONS=detect_container_overflow=0:detect_leaks=0`. The first option
  avoids the libstdc++/protobuf static-registration container annotation issue;
  the second avoids LeakSanitizer shutdown hangs seen in SGX simulation tests.
- UBSan and TSan can still be useful for host-side SGX transport code. Keep
  console telemetry disabled for TSan unless the race investigation explicitly
  needs telemetry output.

**Fake SGX coroutine builds:**
- `Debug_Coroutine_Fake_SGX` is the preferred preset when the goal is to run the
  coroutine SGX transport/runtime code under ordinary host sanitizers.
- The fake backend compiles the enclave runtime sources as a normal shared
  object, so ASan/UBSan/TSan can see more of the code than Intel SGX simulation
  builds can.
- TSan fake-backend runs intentionally preserve copied enclave images under
  `/tmp` instead of removing them in `sgx_destroy_enclave`. The sanitizer
  symbolizer needs those paths to remain readable for frames from `dlopen`ed
  fake enclave objects.
- Treat fake-backend failures as functional or lifetime bugs to investigate, but
  do not treat fake-backend success as a hardware SGX security validation.

**Microsoft STL Compliance Tests:**
- **Test**: `Dev10_445289_make_shared` overrides global `operator new`/`delete`
- **Suppression**: `ASAN_OPTIONS=alloc_dealloc_mismatch=0` is set for this specific test
- **Location**: Configured in `c++/tests/std_test/CMakeLists.txt`

### Test Results

The exact test count changes over time. Treat the ASan expectation as:

- `Debug_ASAN` should configure and run the current debug test set cleanly
- `Debug_Coroutine_ASAN` should configure and run the current coroutine test
  set cleanly

### What AddressSanitizer Detects

- ❌ Use-after-free
- ❌ Buffer overflows (heap and stack)
- ❌ Heap corruption
- ❌ Double-free
- ❌ Memory leaks (when enabled)
- ❌ Use-after-scope
- ❌ Container overflow

### Performance Impact

AddressSanitizer adds overhead:
- **Memory**: 2-3x memory usage
- **CPU**: 2x slower execution
- **Binary size**: Larger executables (~160MB for rpc_test)

**Recommendation**: Use ASan builds only for testing and development, not production.

### Continuous Integration

For CI/CD pipelines, run ASan tests on critical paths:

```bash
# Quick smoke test
cmake --preset Debug_ASAN
cmake --build build_debug --target rpc_test
./build_debug/output/rpc_test --gtest_filter="*standard_tests*"

# Full validation (takes longer)
c++/tests/scripts/run_asan_tests.sh
```

## 8. Running Tests

### Run All Tests

```bash
# From build directory
ctest --output-on-failure

# Or from project root
ctest --test-dir build_debug --output-on-failure
ctest --test-dir build_debug_coroutine --output-on-failure
```

### Run Specific Tests

```bash
# Run SGX tests
ctest --test-dir build_debug_sgx --tests-regex enclave

# Run fuzz tests
ctest --test-dir build_debug --tests-regex fuzz

# Run a specific gtest suite inside rpc_test
./build_debug/output/rpc_test --gtest_filter='*remote_type*'
```

### Test Categories

| Category | Description | Current Executable |
|----------|-------------|--------------------|
| Main typed host tests | Core C++ RPC transport and type suites | `rpc_test` |
| JSON schema tests | Metadata extraction | `json_schema_metadata_test` |
| Serializer tests | Serialization-focused checks | `serialiser_test` |
| Fuzz harness | Iterative fuzz-style runner, when `CANOPY_BUILD_TEST=ON` | `fuzz_test_main` |
| Fuzz gtests | Discoverable fuzz scenarios, when `CANOPY_BUILD_TEST=ON`; Rust child parity is wired only for non-coroutine protobuf builds with `CANOPY_BUILD_RUST=ON` and `cargo` available | `fuzz_test_gtest` |
| Unit tests | Focused standalone executables | `member_ptr_test`, `passthrough_test`, `zone_address_test` |

## 9. Build Output

### Directory Structure

```
build_<preset>/
├── output/              # Executables and libraries for that preset
├── generated/           # IDL-generated code for that preset
├── CMakeFiles/          # CMake state for that preset
└── ...                  # Other preset-local build products
```

Typical examples:

- `build_debug/output/rpc_test`
- `build_debug/output/fuzz_test_gtest`
- `build_debug_coroutine/output/websocket_server`

### Generated Files

When you build with IDL files:

```
build_<preset>/generated/
├── include/
│   └── {project}/
│       ├── {name}.h
│       └── {name}_stub.h
├── src/
│   └── {project}/
│       ├── {name}_proxy.cpp
│       └── {name}_stub.cpp
└── protobuf/
    └── {name}.proto
```

## 10. Development Workflow

### Quick Rebuild After IDL Changes

```bash
# Regenerate code
cmake --build build_debug --target {interface_name}_idl

# Or rebuild everything
cmake --build build_debug
```

### Clean Build

```bash
# Remove build directory and rebuild
rm -rf build_debug
cmake --preset Debug
cmake --build build_debug
```

### Incremental Build

```bash
# Standard incremental build
cmake --build build_debug

# Parallel build
cmake --build build_debug --parallel $(nproc)
```

## 11. Docker Builds

### Dockerfile Example

```dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    cmake \
    ninja-build \
    clang-10 \
    g++-9 \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /rpc
COPY . .
RUN git submodule update --init --recursive

RUN cmake --preset Debug && \
    cmake --build build_debug --parallel $(nproc)

CMD ["ctest", "--test-dir", "build_debug", "--output-on-failure"]
```

## 12. Troubleshooting

### CMake Configuration Fails

```bash
# Check CMake version
cmake --version

# Ensure Ninja is installed
which ninja

# Clean and retry
rm -rf build_debug
cmake --preset Debug
```

### Header Not Found

```bash
# Ensure submodules are initialized
git submodule update --init --recursive

# Check include paths in compile_commands.json
cat build/compile_commands.json | head -100
```

### Linker Errors

```bash
# Rebuild from scratch
rm -rf build_debug
cmake --preset Debug
cmake --build build_debug
```

### SGX Build Fails

```bash
# Verify SGX SDK is installed
ls /opt/intel/sgxsdk/include

# Use simulation mode if no hardware
cmake --preset Debug_SGX_Sim
```

## 13. Building Demo Applications

### Demo CMakeLists.txt Structure

When building demo applications that use Canopy, follow the structure from `c++/demos/websocket/server/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_demo)

add_executable(my_demo demo.cpp)

# Define compile definitions
target_compile_definitions(my_demo PRIVATE ${CANOPY_DEFINES})

# Include directories - use generator expressions for portability
target_include_directories(
    my_demo
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>"
    PRIVATE ${CANOPY_INCLUDES}
)

# Link libraries - order matters for static linking
target_link_libraries(my_demo
    PUBLIC
        my_idl                   # Your generated IDL library
        transport_local          # Local transport (if using)
        connection_factory       # Typed/JSON stream/RPC helpers (if using connection factories)
        rpc::rpc                 # Core RPC library
        ${CANOPY_LIBRARIES}        # System libraries
)

# Compiler and linker options
target_compile_options(my_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(my_demo PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

# Set PDB name for debugging
set_property(TARGET my_demo PROPERTY COMPILE_PDB_NAME my_demo)

# Optional: Clang-tidy
if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(my_demo PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
```

### Common Demo Library Names

| Library | CMake Target | Purpose |
|---------|--------------|---------|
| Local transport | `transport_local` | In-process communication |
| Blocking DLL transport | `transport_dynamic_library` | In-process child-zone loading |
| Streaming transport | `transport_streaming` | Stream-backed RPC transport |
| Connection factory | `connection_factory` | Interface target for typed and JSON-configured stream/RPC helper APIs |
| IPC transport | `transport_ipc_transport` | Coroutine process-owned transport |
| Core RPC | `rpc::rpc` | Base RPC functionality |
| Logging IDL | `logging_idl` | Generated `rpc::log_record` type |
| Telemetry | `rpc::rpc_telemetry` | Optional telemetry sinks and reports |

### Build Order Issues

**Problem**: Linker complains about undefined references to `service::service()` or `create_interface_stub<>()`

**Cause**: Missing libraries or wrong library order

**Solution**: Ensure all required libraries are linked:
```cmake
target_link_libraries(my_demo
    PUBLIC
        my_idl
        transport_local  # Required for local transport
        rpc::rpc
        ${CANOPY_LIBRARIES}
)
```

### Transport Include Path Issues

**Problem**: `rpc::local::child_transport` not found

**Cause**: Missing transport include path

**Solution**: Link the transport target and include `${CANOPY_INCLUDES}`:
```cmake
target_include_directories(my_demo PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(my_demo PRIVATE transport_local)
```

## 14. Next Steps

- [Getting Started](02-getting-started.md) - Follow a tutorial
- [Transports and Passthroughs](architecture/06-transports-and-passthroughs.md) - Choose the right transport
- [IDL Guide](03-idl-guide.md) - Define your interfaces
- [Examples](10-examples.md) - Explore demo code
