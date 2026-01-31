<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Code Quality Review & Modernization Plan

**Document Type:** Project Management & Task Tracking
**Created:** 2026-01-10
**Last Updated:** 2026-01-10
**Project:** Canopy Framework Modernization
**Scope:** `rpc/src`, `rpc/include`, `generator`, test infrastructure

## Status Legend
- ✅ **Completed** - Implementation finished and verified
- 🚧 **In Progress** - Currently being worked on
- 📋 **Planned** - Scheduled for implementation
- ⏸️ **Blocked** - Waiting on dependencies or decisions
- ❌ **Not Started** - Not yet begun

## Executive Summary
The Canopy project has successfully migrated to C++20 and demonstrates modern C++ usage (smart pointers, lambda expressions, structured bindings). Ongoing modernization efforts focus on improving thread safety, reducing code complexity, enhancing static analysis coverage, and refactoring the monolithic code generator.

## 1. Modern C++ & Standards Compliance

### 1.1 ✅ `rpc::span` Implementation for i_marshaller Interface
**Status:** Completed
**Completed Date:** 2026-01-10
**Priority:** High
**Owner:** System Migration

**Implementation Details:**
- Replaced separate `size_t in_size` and `const char* in_buf` parameters with `const rpc::span& in_data` throughout the i_marshaller interface
- Updated `i_marshaller::send()` and `i_marshaller::post()` method signatures
- Modified all implementing classes: `service`, `service_proxy`, `pass_through`, `transport`, `object_proxy`
- Updated code generator to emit new signatures for generated proxy and stub classes
- Migrated all test files to use new span-based interface
- Created custom `rpc::span` struct (lines 41-110 in `serialiser.h`) with constructors for various container types to support C++17 compatibility
- Added `data()` and `size()` accessor methods to `rpc::span`

**Files Modified:**
- `rpc/include/rpc/internal/marshaller.h` - Interface definitions
- `rpc/include/rpc/internal/service.h` - Service implementations
- `rpc/include/rpc/internal/service_proxy.h` - Service proxy
- `rpc/include/rpc/internal/pass_through.h` - Pass-through marshaller
- `rpc/include/rpc/internal/transport.h` - Transport layer
- `rpc/include/rpc/internal/serialiser.h` - Span definition and serialization utilities
- `rpc/src/*.cpp` - Implementation updates for all affected classes
- `generator/src/*.cpp` - Code generator updates for proxy/stub generation
- `tests/test_host/post_functionality_test_suite.cpp` - Test infrastructure updates

**Benefits Achieved:**
- Improved type safety by eliminating error-prone pointer+size pairs
- Enhanced code readability with self-documenting span parameters
- Reduced parameter count in marshalling functions
- Better C++ standard library compatibility

### 1.2 📋 C++20 Concepts Implementation
**Status:** Planned
**Priority:** Medium
**Estimated Effort:** 2-3 weeks

**Objective:** Use C++20 Concepts to constrain template parameters, replacing SFINAE/`enable_if` patterns.

**Target Areas:**
- Generator template logic
- Serialization template constraints
- Type trait utilities

**Benefits:**
- Clearer compiler error messages
- More readable template code
- Better documentation of template requirements

### 1.3 📋 `std::format` Migration
**Status:** Planned
**Priority:** Low
**Estimated Effort:** 1 week

**Objective:** Replace `std::to_string` and `fmt::format` with standard `std::format` for logging.

**Benefits:**
- Reduced allocation overhead
- Standard library usage
- Consistent formatting approach

### 1.4 📋 Attributes Enhancement
**Status:** Planned
**Priority:** Medium
**Estimated Effort:** 1 week

**Tasks:**
- Add `[[nodiscard]]` to functions returning error codes (e.g., `service::send`, `service::add_ref`)
- Replace `(void)var;` suppressions with `[[maybe_unused]]` attribute

**Benefits:**
- Compile-time detection of ignored error codes
- Cleaner code without manual void casts

## 2. Code Safety & Concurrency

### 2.1 📋 Thread Safety & Race Conditions
**Status:** Planned (Analysis Complete - Needs Implementation)
**Priority:** High
**Estimated Effort:** 8-12 days
**Analysis Date:** January 2025

**Overview:**
Comprehensive static analysis identified 24 potential race conditions across transport layer and core components. Most use atomic operations or mutexes, but edge cases need verification and fixes.

**Summary Statistics:**
- 🔴 **CRITICAL**: 0 issues (1 verified already fixed)
- 🟠 **HIGH**: 4 issues (keep_alive access, shutdown races, map operations)
- 🟡 **MEDIUM**: 9 issues remaining (2 fixed today, 9 need verification)
- 🟢 **LOW**: 8 issues (verified correct or by design)
- ✅ **VERIFIED FIXED**: 3 (SPSC cancel flags + 2 memory ordering issues)

**Status Breakdown:**
- ✅ **Verified Correct/Fixed**: 11 items (8 low priority + 3 fixed today)
- ⚠️ **Needs Verification**: 10 items
- ❌ **Needs Fix**: 3 items (down from 5)

#### 2.1.1 🔴 CRITICAL Issues (✅ All Resolved)

**SPSC Transport: Non-Atomic Cancel Flags** ✅
- **Location**: `/transports/spsc/transport.h:59-60`, `/transports/spsc/transport.cpp`
- **Issue**: Originally identified as plain booleans, but code review shows already atomic
- **Current Implementation**:
```cpp
// Header declaration:
std::atomic<bool> cancel_sent_{false};
std::atomic<bool> cancel_confirmed_{false};

// Usage with proper memory ordering:
cancel_sent_.load(std::memory_order_acquire);    // Lines 552, 805
cancel_sent_.store(true, std::memory_order_release);  // Line 561
cancel_confirmed_.load(std::memory_order_acquire);    // Lines 597, 728
cancel_confirmed_.store(true, std::memory_order_release);  // Line 570
```
- **Status**: ✅ ALREADY FIXED - Atomic with proper acquire/release ordering
- **Verification Date**: 2026-01-10
- **Impact**: No action required - correct implementation already in place

#### 2.1.2 🟠 HIGH Priority Issues (Phase 1)

**TCP Transport: keep_alive_ Shared Pointer Access**
- **Location**: `/transports/tcp/transport.cpp:463` - `pump_messages()` loop
- **Issue**: `keep_alive_` shared_ptr accessed in loop condition without synchronization
- **Problem**: Multiple coroutines might modify causing undefined behavior
- **Recommended Fix Options**:
  1. Use `std::atomic<std::shared_ptr<T>>` (C++20)
  2. Mutex-protected access
  3. Load once before loop and use local copy
- **Estimated Effort**: 4 hours
- **Status**: NEEDS FIX

**Pending Transmits Shutdown Race**
- **Location**: Both `tcp_transport` and `spsc_transport`
- **Issue**: `pending_transmits_` map cleared during shutdown while requests might be completing
- **Problem**: Race between pending request completion and map clearing
- **Recommended Fix**:
  1. Set shutdown flag atomically
  2. Drain pending requests with timeout
  3. Only then clear map
  4. Handle late completions gracefully
- **Estimated Effort**: 8 hours
- **Status**: NEEDS VERIFICATION

**Transport Destination Management**
- **Location**: `/rpc/src/transport.cpp:249-298` - `create_pass_through()`
- **Issue**: Nested map `pass_thoughs_` with complex locking
- **Current State**: Already implements deadlock prevention via consistent lock ordering (zone ID order)
- **Note**: May still have performance issues with coarse-grained locking
- **Status**: ✅ DEADLOCK PREVENTION IMPLEMENTED - Performance optimization opportunity

**Service Proxy Map Operations**
- **Location**: `/rpc/src/service_proxy.cpp`
- **Issue**: `proxies_` map operations in `on_object_proxy_released()` and `get_or_create_object_proxy()` may have races
- **Problem**: Race between creation, release, and cleanup
- **Recommended Fix**: Ensure consistent locking pattern (lock → check → operate → unlock)
- **Estimated Effort**: 6 hours
- **Status**: NEEDS VERIFICATION

#### 2.1.3 🟡 MEDIUM Priority Issues (Phase 2)

**TCP Transport: Send Queue Lock Scope**
- **Location**: `/transports/tcp/transport.cpp:463`
- **Issue**: Lock might be released during coroutine suspension points within processing loop
- **Fix**: Verify lock is held across entire queue processing
- **Status**: NEEDS VERIFICATION

**TCP Transport: shutdown_event_ Access**
- **Location**: `/transports/tcp/transport.cpp`
- **Issue**: `shutdown_event_` set from multiple places without clear synchronization
- **Fix**: Ensure atomic or mutex-protected, consider single shutdown coordination point
- **Status**: NEEDS VERIFICATION

**SPSC Transport: Send Queue Lock Scope**
- **Location**: `/transports/spsc/transport.cpp`
- **Issue**: Similar to TCP - lock acquired, released, queue accessed again
- **Fix**: Ensure atomic queue operations or proper lock scope
- **Status**: NEEDS VERIFICATION

**SPSC Transport: peer_cancel_received_ Memory Ordering** ✅
- **Location**: `/transports/spsc/transport.cpp`
- **Issue**: Atomic bool had mixed access patterns (plain assignment, implicit conversion in assertions)
- **Fix Applied**:
```cpp
// All stores now use explicit memory ordering:
peer_cancel_received_.store(true, std::memory_order_release);  // Lines 505, 574

// All loads (including assertions) use explicit memory ordering:
peer_cancel_received_.load(std::memory_order_acquire);  // Lines 458, 463, 468, 477, 482, 487, 492, 597
```
- **Status**: ✅ FIXED - Consistent acquire/release memory ordering throughout
- **Fix Date**: 2026-01-10

**SPSC Transport: close_ack_queued_ Memory Ordering** ✅
- **Location**: `/transports/spsc/transport.cpp`
- **Issue**: Atomic flag had plain assignment without explicit memory ordering
- **Fix Applied**:
```cpp
// Store now uses explicit memory ordering:
close_ack_queued_.store(true, std::memory_order_release);  // Line 504

// Load already had correct memory ordering:
close_ack_queued_.load(std::memory_order_acquire);  // Line 728
```
- **Status**: ✅ FIXED - Consistent acquire/release memory ordering
- **Fix Date**: 2026-01-10

**Object Proxy Reference Counting**
- **Location**: `/rpc/src/object_proxy.cpp`
- **Issue**: `shared_count_` and `optimistic_count_` atomics in complex remote reference logic
- **Problem**: Complex state machine involving local/remote operations may have races
- **Fix**: Review `add_ref()` and `release()` for proper sequencing
- **Status**: NEEDS VERIFICATION

**Service Proxy Version Negotiation**
- **Location**: `/rpc/src/service_proxy.cpp`
- **Issue**: `version_` atomic modified during version negotiation
- **Problem**: Race during concurrent version negotiation attempts
- **Fix**: Ensure idempotent negotiation and use compare-and-swap where needed
- **Status**: NEEDS VERIFICATION

**Thread-Local Logger Circular Buffer**
- **Location**: `/rpc/src/thread_local_logger.cpp`
- **Issue**: `write_index_` atomic in circular buffer
- **Problem**: Multiple threads could overwrite entries if not properly synchronized
- **Fix**: Verify buffer is thread-local OR has proper CAS operations
- **Status**: NEEDS VERIFICATION

**Concurrent Transport Access**
- **Location**: `/rpc/src/service.cpp`
- **Issue**: `transports_` map protected by `service_proxy_control_` mutex with complex operations
- **Problem**: Race between transport creation, access, and removal
- **Fix**: Audit all access patterns, consider `std::shared_ptr` in map values
- **Status**: NEEDS VERIFICATION

**Service Proxy Management**
- **Location**: `/rpc/src/service.cpp`
- **Issue**: `service_proxies_` map protected by `service_proxy_control_` mutex
- **Problem**: Similar to transport management - race between creation, access, removal
- **Fix**: Ensure consistent locking and reference counting
- **Status**: NEEDS VERIFICATION

**Service Proxy Map Operations (Detailed)**
- **Location**: `/rpc/src/service_proxy.cpp`
- **Issue**: Complex operations with `proxies_` map and `insert_control_` mutex
- **Fix**: Avoid unlocking and re-locking within single logical operation
- **Estimated Effort**: 6 hours
- **Status**: NEEDS VERIFICATION

#### 2.1.4 🟢 LOW Priority Issues (Verified Correct or Optimization Opportunities)

**Transport Status Modification** ✅
- **Location**: `/rpc/src/transport.cpp:303-320`
- **Status**: CORRECTLY IMPLEMENTED - Uses proper acquire/release ordering
- **Implementation**:
```cpp
std::atomic<transport_status> status_{transport_status::CONNECTING};
transport_status get_status() const {
    return status_.load(std::memory_order_acquire);
}
void set_status(transport_status new_status) {
    status_.store(new_status, std::memory_order_release);
}
```

**SPSC Transport: shutdown_sequence_completed_ Coordination** ✅
- **Location**: `/transports/spsc/transport.cpp`
- **Status**: LIKELY CORRECT - Atomic counter for tracking completion
- **Recommendation**: Document memory ordering rationale

**Sequence Number Increment** ✅
- **Location**: Both `tcp_transport` and `spsc_transport`
- **Status**: LIKELY CORRECT - Uses default `memory_order_seq_cst`
- **Optimization Opportunity**: Could use `memory_order_relaxed` for performance:
```cpp
auto seq = sequence_number_.fetch_add(1, std::memory_order_relaxed);
```

**Thread-Local Service Variable** ✅
- **Location**: `/rpc/src/service.cpp` - `current_service_`
- **Status**: CORRECT - Thread-local variables are thread-safe by definition
- **Implementation**: `thread_local service* current_service_ = nullptr;`

**Zone ID Generator** ✅
- **Location**: `/rpc/src/service.cpp` - `generate_new_zone_id()`
- **Status**: CORRECT - Sequential consistency appropriate for ID generation
- **Implementation**: `static std::atomic<uint64_t> zone_id_generator_{1};`
- **Optimization**: Could use `memory_order_relaxed` for performance

**Object ID Generator** ✅
- **Location**: `/rpc/src/service.cpp` - `generate_new_object_id()`
- **Status**: CORRECT - Same as Zone ID Generator

**Stub Reference Counting** ✅
- **Location**: `/rpc/src/stub.cpp`
- **Status**: CORRECTLY IMPLEMENTED - Atomics for counts, mutex for map modifications
- **Implementation**:
```cpp
std::atomic<uint64_t> shared_count_ = 0;
std::atomic<uint64_t> optimistic_count_ = 0;
std::unordered_map<caller_zone, std::atomic<uint64_t>> optimistic_references_;
std::mutex references_mutex_;
```

**Service Event Notification** ✅
- **Location**: `/rpc/src/service.cpp:1161-1174` - `notify_object_gone_event()`
- **Status**: CORRECTLY IMPLEMENTED - Copies weak references, iterates outside lock
- **Implementation**: Creates local copy of `service_events_` before iteration

#### 2.1.5 Implementation Plan

**Phase 1: Critical Fixes (1-2 days)**
1. Fix `cancel_sent_`/`cancel_confirmed_` - Make atomic (SPSC transport)
2. Fix `keep_alive_` access - Add synchronization or local copies
3. Review `pending_transmits_` shutdown - Ensure proper drain sequence

**Phase 2: High-Priority Review (2-3 days)**
4. Audit transport destination management lock ordering
5. Review service proxy map race conditions
6. Verify object_proxy reference counting state machine

**Phase 3: Memory Ordering Audit (2 days)**
7. Verify all atomic operations have appropriate memory ordering
8. Document memory ordering choices
9. Add static assertions or comments explaining ordering

**Phase 4: Testing (3-5 days)**
10. ThreadSanitizer runs on all tests
11. Stress tests with multiple concurrent transports
12. Race condition reproduction tests

**Total Estimated Effort:** 8-12 days

#### 2.1.6 Testing Strategy

**ThreadSanitizer (TSan)**:
```bash
# Build with ThreadSanitizer
cmake --preset Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build
./build/tests/rpc_test
```

**Helgrind (Valgrind)**:
```bash
valgrind --tool=helgrind ./build/tests/rpc_test
```

**Stress Testing**:
- Run tests with many concurrent zones (100+)
- Rapid connection/disconnection cycles
- Simultaneous shutdown from multiple threads

#### 2.1.7 Documentation Requirements

**Required Documentation**:
1. Memory ordering rationale for all atomic operations
2. Lock ordering policies to prevent deadlocks
3. Coroutine suspension behavior with regards to locks
4. Thread safety guarantees for each public API

**References**:
- [C++ Reference: std::memory_order](https://en.cppreference.com/w/cpp/atomic/memory_order)
- "C++ Concurrency in Action" by Anthony Williams, Chapter 5
- [Herb Sutter: atomic<> Weapons](https://herbsutter.com/2013/02/11/atomic-weapons-the-c-memory-model-and-modern-hardware/)

### 2.2 📋 Deadlock Risk Mitigation
**Status:** Planned (Partially Addressed in Thread Safety Analysis)
**Priority:** High
**Estimated Effort:** 3-4 weeks

**Current State:**
- `service.cpp` contains explicit warnings about deadlock risks
- Uses scoped blocks to manage lock lifetimes (fragile approach)
- Complex interactions between `stub_control_` and `zone_control_` mutexes
- Transport destination management already implements deadlock prevention via lock ordering (see Section 2.1.2)

**Objective:** Eliminate deadlock risks through architectural improvements.

**Proposed Actions:**
1. Review and document all mutex acquisition patterns
2. Analyze `stub_control_` and `zone_control_` lock interactions
3. Consider lock hierarchy to prevent circular dependencies
4. Evaluate alternative concurrency models (e.g., actor model for internal state management)
5. Reduce scope of critical sections where possible

**Risk Assessment:** High priority due to potential runtime failures

**Note:** Section 2.1 Thread Safety analysis covers some deadlock prevention aspects (transport lock ordering)

### 2.3 📋 Raw Pointer Audit
**Status:** Planned
**Priority:** Medium
**Estimated Effort:** 1-2 weeks

**Current Issues:**
- `service::clean_up_on_failed_connection` takes `rpc::casting_interface* input_interface`
- `service::get_object_id` takes `rpc::shared_ptr<casting_interface>& ptr`
- Various other raw pointer usages throughout codebase

**Objective:** Eliminate ambiguous ownership semantics.

**Guidelines:**
- Use references (`&`) for non-null observers
- Use `std::optional<T*>` or nullable pointers for optional parameters
- Ensure ownership is never transferred via raw pointers
- Document ownership semantics in function contracts

**Benefits:**
- Clearer ownership semantics
- Reduced risk of dangling pointers
- Better static analysis results

## 3. Architecture & Complexity

### 3.1 📋 `service.cpp` Refactoring
**Status:** Planned
**Priority:** Medium
**Estimated Effort:** 2 weeks

**Target Functions:**

#### 3.1.1 `check_is_empty()` Decomposition
**Current Issues:**
- Does too many things (logging, state verification, resource cleanup)
- Complex control flow with multiple responsibilities
- Hard to test in isolation

**Proposed Refactoring:**
- Extract logging logic into separate function
- Create dedicated state verification function
- Separate resource cleanup concerns
- Apply Single Responsibility Principle

#### 3.1.2 `release()` Simplification
**Current Issues:**
- Nested scopes with complex lock management
- Intricate logic for handling optimistic referencing
- Difficult to understand control flow

**Proposed Refactoring:**
- Extract optimistic reference handling into helper functions
- Reduce nesting depth through early returns
- Document state transitions clearly
- Create unit tests for each code path

### 3.2 📋 Code Generator Modernization
**Status:** Planned
**Priority:** High
**Estimated Effort:** 4-6 weeks

**Current State:**
- `generator/src/protobuf_generator.cpp` is ~3000 lines
- "Stringly typed" C++ code generation approach
- Monolithic structure makes maintenance difficult
- Hard to add new features or modify existing output

**Objective:** Refactor generator into maintainable, testable components.

**Phase 1: Component Extraction (2 weeks)**
- Create `TypeMapper` class for type conversions
- Create `MessageGenerator` class for message/struct generation
- Create `ServiceGenerator` class for interface generation
- Create `CodeFormatter` utility for consistent output

**Phase 2: Template System Evaluation (1 week)**
- Evaluate template engines (Mustache, Jinja2-style, custom)
- Create proof-of-concept with selected engine
- Compare maintenance burden vs. current approach

**Phase 3: Implementation (2-3 weeks)**
- Implement chosen approach
- Migrate existing generation logic
- Add comprehensive unit tests
- Update documentation

**Benefits:**
- Easier to maintain and extend
- Better separation of concerns
- More testable code
- Reduced risk of code generation bugs

## 4. Build & Tooling

### 4.1 📋 Static Analysis Enhancement
**Status:** Planned
**Priority:** Medium
**Estimated Effort:** 1 week

**Current State:**
- Minimal `.clang-tidy` configuration
- Limited static analysis coverage
- No automated quality gates

**Objective:** Comprehensive static analysis coverage with automated enforcement.

**Proposed Configuration Updates:**
1. Enable `bugprone-*` checks for common programming errors
2. Enable `performance-*` checks for optimization opportunities
3. Enable `modernize-*` checks (selective) for C++20 idioms
4. Enable `cppcoreguidelines-*` checks for best practices
5. Configure appropriate severity levels and exclusions

**Integration:**
- Add clang-tidy to CI pipeline
- Set up pre-commit hooks (optional)
- Create baseline for existing issues
- Establish zero-new-issues policy

### 4.2 📋 Compiler Warning Enhancement
**Status:** Planned
**Priority:** High
**Estimated Effort:** 1 week

**Objective:** Maximize compiler-based error detection.

**Actions:**
- Ensure `-Wall -Wextra -Wpedantic` enabled (GCC/Clang)
- Enable equivalent MSVC warnings (`/W4`, `/WX`)
- Treat all warnings as errors in CI builds
- Document warning suppression policy
- Audit and justify any warning suppressions

## 5. Documentation

### 5.1 📋 Public API Documentation
**Status:** Planned
**Priority:** Medium
**Estimated Effort:** 2 weeks

**Current State:** Sparse comments in public headers

**Objective:** Comprehensive Doxygen-style API documentation.

**Target Files:**
- `rpc/include/rpc/rpc.h` - Main public API
- `rpc/include/rpc/internal/service.h` - Service interface
- `rpc/include/rpc/internal/marshaller.h` - Marshalling interface
- All other public headers

**Required Documentation:**
- Function purpose and behavior
- Parameter descriptions
- Return value semantics
- Thread-safety guarantees
- Exception specifications
- Usage examples
- Pre/post-conditions

### 5.2 📋 Architecture Documentation
**Status:** Planned
**Priority:** High
**Estimated Effort:** 1-2 weeks

**Objective:** High-level architectural documentation for complex subsystems.

**Required Documents:**
1. **Locking Strategy** (`docs/architecture/locking_strategy.md`)
   - Document mutex hierarchy
   - Explain deadlock prevention strategies
   - Describe critical section patterns

2. **Zone Lifecycle** (`docs/architecture/zone_lifecycle.md`)
   - Document zone creation and destruction
   - Explain parent-child relationships
   - Describe reference counting semantics

3. **Marshalling Architecture** (`docs/architecture/marshalling.md`)
   - Document serialization approach
   - Explain encoding negotiation
   - Describe version compatibility

**Benefits:**
- Easier onboarding for new developers
- Better understanding of complex interactions
- Foundation for future refactoring efforts

## 6. Project Status Summary

### Completed Tasks (Q1 2026)
| Task | Completion Date | Impact |
|------|----------------|---------|
| ✅ rpc::span migration for i_marshaller | 2026-01-10 | High - Improved type safety across entire marshalling interface |

### In Progress Tasks
| Task | Owner | Expected Completion | Status |
|------|-------|-------------------|---------|
| (None currently) | - | - | - |

### Upcoming Tasks (Priority Order)
| Priority | Task | Estimated Effort | Dependencies |
|----------|------|-----------------|--------------|
| High | Thread Safety & Race Conditions | 8-12 days | None |
| High | Deadlock Risk Mitigation | 3-4 weeks | Thread Safety (partial overlap) |
| High | Code Generator Modernization | 4-6 weeks | None |
| High | Compiler Warning Enhancement | 1 week | None |
| High | Architecture Documentation | 1-2 weeks | Thread Safety completion |
| Medium | C++20 Concepts Implementation | 2-3 weeks | None |
| Medium | Static Analysis Enhancement | 1 week | None |
| Medium | service.cpp Refactoring | 2 weeks | None |
| Medium | Raw Pointer Audit | 1-2 weeks | None |
| Medium | Public API Documentation | 2 weeks | None |
| Medium | Attributes Enhancement | 1 week | None |
| Low | std::format Migration | 1 week | None |

### Total Estimated Effort
- **Completed:** 1 task (rpc::span migration)
- **Remaining High Priority:** ~12-18 weeks (includes 8-12 days thread safety)
- **Remaining Medium Priority:** ~8-11 weeks
- **Remaining Low Priority:** ~1 week
- **Total Remaining:** ~21-30 weeks

## 7. Success Metrics

### Code Quality
- [ ] Zero `clang-tidy` violations in new code
- [ ] Zero compiler warnings with `-Wall -Wextra -Wpedantic`
- [ ] 100% public API documentation coverage
- [ ] All error-returning functions marked `[[nodiscard]]`

### Thread Safety & Concurrency
- [ ] Zero ThreadSanitizer violations
- [ ] Zero Helgrind violations
- [ ] All atomic operations have documented memory ordering rationale
- [ ] All mutex acquisition patterns follow documented lock hierarchy
- [ ] No data races on shared state (verified by TSan)
- [ ] All 🔴 CRITICAL race conditions fixed
- [ ] All 🟠 HIGH priority race conditions verified or fixed
- [ ] Stress tests pass with 100+ concurrent zones

### Architecture
- [ ] No mutex acquisition patterns with deadlock potential
- [ ] All raw pointers documented with ownership semantics
- [ ] All functions under 100 lines
- [ ] All classes under 1000 lines
- [ ] Lock ordering policy documented and enforced

### Testing
- [ ] All refactored code has unit tests
- [ ] Critical path code coverage > 90%
- [ ] All architectural documents validated by implementation
- [ ] Concurrent stress tests integrated into CI pipeline

## 8. Notes and Decisions

**2026-01-10:** Completed rpc::span migration
- Used custom `rpc::span` struct instead of `std::span` to maintain C++17 compatibility
- Custom span provides constructors for various container types
- Maintains compatibility with existing serialization infrastructure
- All tests updated and passing

**2026-01-10:** Integrated Race Conditions Analysis
- Completed comprehensive static analysis of transport layer and core components
- Identified 24 potential race conditions categorized by priority
- Merged findings into project management framework with 4-phase implementation plan
- Critical issues: 1 (SPSC cancel flags - verified already fixed with proper atomic + acquire/release ordering)
- High priority issues: 4 (keep_alive access, shutdown races, map operations)
- Medium priority issues: 11 (lock scopes, memory ordering)
- Low priority/verified correct: 8 (documented for future reference)
- Testing strategy includes ThreadSanitizer, Helgrind, and stress testing
- Total estimated effort: 8-12 days

**2026-01-10:** Verified SPSC Transport Cancel Flags Implementation
- Code review confirmed `cancel_sent_` and `cancel_confirmed_` are already `std::atomic<bool>`
- Proper acquire/release memory ordering already in place:
  - All loads use `std::memory_order_acquire`
  - All stores use `std::memory_order_release`
- Implementation follows textbook pattern for atomic flag synchronization
- No action required - critical race condition already resolved

**2026-01-10:** Fixed SPSC Transport Memory Ordering Issues
- Fixed `peer_cancel_received_` inconsistent memory ordering:
  - Updated plain assignments to use `.store(true, std::memory_order_release)`
  - Updated all assertions to use explicit `.load(std::memory_order_acquire)`
  - Changed 9 locations to use consistent acquire/release semantics
- Fixed `close_ack_queued_` plain assignment:
  - Updated to use `.store(true, std::memory_order_release)` on line 504
  - Load operations already had correct memory ordering
- Files modified: `/transports/spsc/transport.cpp`
- Lines changed: 458, 463, 468, 477, 482, 487, 492, 504, 505, 574
- Result: All SPSC transport atomic flags now have consistent, explicit acquire/release memory ordering
