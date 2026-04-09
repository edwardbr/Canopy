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
| `Release_SGX` | SGX hardware release build |
| `Release_SGX_Sim` | SGX simulation release build |

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

All presets inherit these defaults:

```cmake
CANOPY_BUILD_COROUTINE=OFF              # Coroutines disabled by default
CANOPY_BUILD_ENCLAVE=OFF                # SGX enclave support disabled
CANOPY_BUILD_TEST=ON                    # Test building enabled
CANOPY_VERBOSE_GENERATOR=OFF                # Debug code generation disabled
CMAKE_EXPORT_COMPILE_COMMANDS=ON # Export compile commands
```

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

### Full Option List

```cmake
# Core options
option(CANOPY_BUILD_COROUTINE "Enable C++20 coroutine support" OFF)
option(CANOPY_BUILD_ENCLAVE "Enable SGX enclave support" OFF)
option(CANOPY_BUILD_TEST "Build tests" ON)
option(CANOPY_BUILD_DEMOS "Build demos" ON)
option(BUILD_GENERATOR "Build IDL generator" ON)

# Debug options
option(CANOPY_VERBOSE_GENERATOR "Enable debug output for code generation" OFF)
option(CANOPY_USE_LOGGING "Enable RPC logging" OFF)
option(CANOPY_USE_TELEMETRY "Enable RPC telemetry" OFF)
option(CANOPY_USE_CONSOLE_TELEMETRY "Enable console telemetry output" OFF)
option(CANOPY_USE_THREAD_LOCAL_LOGGING "Enable thread-local logging" OFF)
option(CANOPY_USE_TELEMETRY_RAII_LOGGING "Enable RAII telemetry logging" OFF)

# Sanitizers
option(CANOPY_DEBUG_ADDRESS "Enable address sanitizer" OFF)
option(CANOPY_DEBUG_THREAD "Enable thread sanitizer" OFF)
option(CANOPY_DEBUG_UNDEFINED "Enable undefined behavior sanitizer" OFF)
option(CANOPY_DEBUG_LEAK "Enable leak sanitizer" OFF)
option(CANOPY_DEBUG_ALL "Enable all sanitizers" OFF)

# SGX options
set(SGX_MODE "debug" CACHE STRING "SGX mode: debug or release")
set(SGX_HW "OFF" CACHE BOOL "Enable SGX hardware (vs simulation)")
```

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
cmake --build build_debug_coroutine --target transport_streaming  # Stream-backed transport
cmake --build build_debug_coroutine --target transport_ipc_transport
cmake --build build_debug_coroutine --target transport_libcoro_dynamic_library
```

### Tests

```bash
cmake --build build_debug --target rpc_test
cmake --build build_debug --target serialiser_test
cmake --build build_debug --target fuzz_test_main
cmake --build build_debug --target fuzz_test_gtest
cmake --build build_debug --target member_ptr_test
cmake --build build_debug --target passthrough_test
cmake --build build_debug --target zone_address_test
```

### Demos

```bash
cmake --build build_debug_coroutine --target websocket_server   # WebSocket demo server
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
| Fuzz harness | Iterative fuzz-style runner | `fuzz_test_main` |
| Fuzz gtests | Discoverable fuzz scenarios | `fuzz_test_gtest` |
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
        my_idl_host              # Your generated IDL library
        transport_local          # Local transport (if using)
        transport_streaming      # Stream transport (if using)
        rpc::rpc                 # Core RPC library
        ${CANOPY_LIBRARIES}        # System libraries
)

# Compiler and linker options
target_compile_options(my_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(my_demo PRIVATE ${HOST_LINK_EXE_OPTIONS})

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
| IPC transport | `transport_ipc_transport` | Coroutine process-owned transport |
| Core RPC | `rpc::rpc` | Base RPC functionality |
| Telemetry | `rpc::rpc_telemetry` | Logging/metrics |

### Build Order Issues

**Problem**: Linker complains about undefined references to `service::service()` or `create_interface_stub<>()`

**Cause**: Missing libraries or wrong library order

**Solution**: Ensure all required libraries are linked:
```cmake
target_link_libraries(my_demo
    PUBLIC
        my_idl_host
        transport_local  # Required for local transport
        rpc::rpc
        ${CANOPY_LIBRARIES}
)
```

### Transport Include Path Issues

**Problem**: `rpc::local::child_transport` not found

**Cause**: Missing transport include path

**Solution**: CMake's `${CANOPY_INCLUDES}` should include transport paths. If manual configuration is needed:
```cmake
target_include_directories(my_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../transports/local/include
)
```

## 14. Next Steps

- [Getting Started](02-getting-started.md) - Follow a tutorial
- [Transports and Passthroughs](architecture/06-transports-and-passthroughs.md) - Choose the right transport
- [IDL Guide](03-idl-guide.md) - Define your interfaces
- [Examples](10-examples.md) - Explore demo code
