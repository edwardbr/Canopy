<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# optimistic_ptr Race Conditions — Root Causes and Fixes

This document records three race conditions that were identified and fixed in the
`rpc::optimistic_ptr` control-block implementation in
`rpc/include/rpc/internal/remote_pointer.h`.  The fixes landed together as part of
the refactor that merged `shared_count_` and `optimistic_count_` into a single
`std::atomic<int64_t> combined_count_`.

---

## Background: the control block

Every `rpc::shared_ptr<T>` owns a **control block** (`control_block_base`) that
tracks three reference counts:

| Field | Meaning |
|---|---|
| `combined_count_` (high 32 bits) | number of live `shared_ptr` instances (`shared_count`) |
| `combined_count_` (low 32 bits) | number of live `optimistic_ptr` instances (`optimistic_count`) |
| `weak_count_` | number of live `weak_ptr` instances + 1 for the "shared group" |

The control block is destroyed when both `weak_count_` reaches 0 **and**
`shared_count` (high 32 bits of `combined_count_`) is also 0.

---

## Issue 1 — use-after-free on the control block in `try_increment_optimistic`

### Location

`control_block_base::try_increment_optimistic()` — called on the 0→1 transition
when the first `optimistic_ptr` is created from a `shared_ptr`.

### The race

The function has to call `CO_AWAIT control_block_call_add_ref()` on the 0→1
transition to notify the remote stub.  At that `CO_AWAIT` point the coroutine
suspends and may be resumed on a different thread.  A first attempt used a RAII
guard to keep the control block alive:

```cpp
// BROKEN - guard is a local variable; it is destroyed when the coroutine suspends
struct keep_alive_guard {
    control_block_base* cb;
    keep_alive_guard(control_block_base* c) : cb(c) { cb->increment_weak(); }
    ~keep_alive_guard() { cb->decrement_weak_and_destroy_if_zero(); }
};
keep_alive_guard guard(this);          // increment weak_count_
auto err = CO_AWAIT control_block_call_add_ref(...);  // coroutine suspends here
// guard destructor runs at suspension, not at the end of the scope!
```

Local variables (including RAII guards) in a coroutine frame are destroyed when
the coroutine suspends, not when the enclosing scope ends.  Another thread could
therefore decrement both counts to zero and call `destroy_self_actual()` while
the coroutine was still suspended, giving a use-after-free on resumption.

### The fix

Instead of a guard, `weak_count_` is incremented at the top of
`try_increment_optimistic()` and is intentionally **not** decremented until the
last `optimistic_ptr` is released via `decrement_optimistic_and_dispose_if_zero`.
This single extra weak reference covers the entire lifetime of the first
optimistic group, across any number of `CO_AWAIT` suspensions.

If the 0→1 add_ref fails the weak reference must be released on the error path:

```cpp
if (err)
{
    combined_count_.fetch_sub(1, ...);   // rollback optimistic count
    decrement_weak_and_destroy_if_zero(); // release the weak ref taken above
    CO_RETURN err;
}
```

Without this call, a failed add_ref would permanently leak a weak reference and
prevent the control block from ever being destroyed.

---

## Issue 2 — double-free when `shared_ptr` and `optimistic_ptr` are released concurrently

### Location

`control_block_base::decrement_shared_and_dispose_if_zero()` and
`control_block_base::decrement_optimistic_and_dispose_if_zero()`.

### The race

With two independent atomics (`shared_count_` and `optimistic_count_`) two
threads could interleave as follows:

1. Thread A decrements `shared_count_` to 0, then reads `optimistic_count_` → 1
   (still alive, so A does not dispose).
2. Thread B decrements `optimistic_count_` to 0, then reads `shared_count_` → 0
   (already gone) → B disposes the object.
3. Thread A resumes; believing optimistic refs still existed it now also
   disposes the object — **double-free**.

The window exists because reading the *other* count after a `fetch_sub` is not
atomic with the decrement itself.

### The fix

`shared_count` and `optimistic_count` are packed into a single
`std::atomic<int64_t> combined_count_` (high 32 bits = shared, low 32 bits =
optimistic).  A single `fetch_sub` produces an atomic snapshot of both halves:

```cpp
// In decrement_shared_and_dispose_if_zero:
std::int64_t prev = combined_count_.fetch_sub(1LL << 32, std::memory_order_acq_rel);
long prev_shared = prev >> 32;
long prev_opt    = prev & 0xFFFFFFFF;

if (prev_shared == 1 && prev_opt == 0)
    dispose_object_actual();   // only this thread can reach this branch

// In decrement_optimistic_and_dispose_if_zero:
std::int64_t prev = combined_count_.fetch_sub(1, std::memory_order_acq_rel);
long prev_shared = prev >> 32;
long prev_opt    = prev & 0xFFFFFFFF;

if (prev_opt == 1 && prev_shared == 0)
    dispose_object_actual();   // only this thread can reach this branch
```

Because both decrements touch the same atomic, exactly one thread will observe
the transition to all-zeros.  The other thread's snapshot will show a non-zero
count in the other half, so it will skip disposal.

The `combined_count_` field is unconditional (not guarded by
`#ifndef TEST_STL_COMPLIANCE`).  In STL-compliance mode the low 32 bits
(`optimistic_count`) are always zero, so `combined_count_ >> 32` behaves
identically to the old standalone `shared_count_` atomic.

---

## Issue 3 — `weak_ptr::lock()` load-then-CAS race (deferred)

### Location

`weak_ptr<T>::lock()`.

### The race

The standard CAS-retry loop in `lock()` checks whether `shared_count` is still
positive before atomically incrementing it.  With the introduction of
`optimistic_ptr`, the managed object can be disposed even while `shared_count`
is non-zero if `optimistic_count` drove the disposal decision.  A thread that
loaded a positive `shared_count` just before another thread disposed the object
could produce a `shared_ptr` pointing to freed memory.

### Status

This issue has been identified but **not yet fixed**.  A tracked follow-up item
covers the correct mitigation (making the lock operation aware of
`optimistic_count` when deciding whether the managed object is still valid).

---

## Callable accessor pattern — mitigating the check-then-call race

### The problem

`optimistic_ptr` is non-owning.  A check-then-call sequence is racy for local
objects:

```cpp
if (opt)               // thread 1: object is alive
    opt->do_something(); // thread 2: object was destroyed between check and call
```

For local objects the old `operator->()` returned a raw pointer.  If the
`shared_ptr` was destroyed between the `if` check and the call, the pointer
dangled.

### The fix: `callable_accessor`

`optimistic_ptr::get_callable()` returns a `callable_accessor<T>` that pins the
local object for the duration of the accessor's lifetime:

```cpp
auto acc = opt->get_callable();  // lock() + pin for local, copy ptr_ for remote
if (!acc)
    return;  // null or local-gone

// For local: pin_ holds shared_ptr, object stays alive
// For remote: ptr_ is kept alive by optimistic count on the parent optimistic_ptr
acc->do_something();
```

#### Local-alive

`pin_` holds a `shared_ptr<T>` from `lock()`.  The object cannot be destroyed
while `acc` exists.  Calls go through the actual object.

#### Local-gone

`pin_` is null.  `ptr_` holds the `local_proxy` pointer (which is always alive
because it lives inside `local_proxy_holder_`).  Calls through the proxy return
`OBJECT_GONE`.

#### Remote

`pin_` is null.  `ptr_` holds a copy of the parent's `interface_proxy*`.  The
proxy is kept alive by the optimistic count on the parent `optimistic_ptr`.  If
the parent `optimistic_ptr` is destroyed concurrently, the race persists — but
this is the same inherent racy behaviour as before, and is by design for an
"optimistic" (non-owning) pointer.

### State table

| State       | `pin_`             | `ptr_` (dispatch)          | `acc.operator bool()` |
|-------------|--------------------|----------------------------|-----------------------|
| Local alive | locked `shared_ptr`| `pin_.get()` — actual obj  | `true`                |
| Local gone  | `nullptr`          | `local_proxy_holder_.get()`| `false`               |
| Remote      | `nullptr`          | `interface_proxy*`         | `true` iff non-null   |

### Key properties

- Wire protocol unchanged.
- `OBJECT_GONE` preserved for local-gone calls.
- The accessor is cheap to copy and can be cached on the stack for reuse.
- `operator bool()` on the accessor reflects actual liveness, not just proxy presence.
- `local_proxy<T>` remains fully intact and necessary — it is the fallback
  dispatch target for the gone case.

---

## Regression guard — `decrement_weak_and_destroy_if_zero` placement

During the refactor a latent indentation error moved
`decrement_weak_and_destroy_if_zero()` outside the `if (prev_shared == 1)`
block in `decrement_shared_and_dispose_if_zero`.  This caused it to be called
for **every** `shared_ptr` release rather than only the last one, which would
decrement `weak_count_` below its initial value of 1 and trigger the assertion
on the second release of any object held by more than one `shared_ptr`.

The call must remain inside the `if (prev_shared == 1)` block — it represents
the "shared group" reference being retired when the last `shared_ptr` goes away.
