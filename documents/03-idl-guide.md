<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# IDL Guide

The Interface Definition Language (IDL) is used to define interfaces, structures, and data types that Canopy uses to generate type-safe proxy and stub code.

## 1. IDL Syntax Overview

Canopy IDL uses a C++-like syntax with additional attributes for RPC-specific metadata.

### Basic Structure

```idl
// Comments use C++ style
#include "other_file.idl"  // Copy/paste content (caution - causes duplicate symbols)
#import "other_file.idl"    // Reference external IDL types (preferred)

namespace my_namespace
{
    [attribute=value, ...]
    interface my_interface
    {
        [attribute=value]
        return_type method_name(param_type param_name, [out] param_type& out_param);
    };

    [attribute=value]
    struct my_struct
    {
        type member_name;
        type member_name = default_value;
    };
}
```

### File Organization

IDL files should be organized logically:

```
idl/
├── my_project/
│   ├── main.idl          // Main file with interfaces
│   ├── types.idl         // Shared struct definitions
│   └── import.idl        // External imports
```

## 2. Namespaces

Namespaces organize interfaces and structs into logical groups.

### Basic Namespace

```idl
namespace calculator
{
    interface i_calculator
    {
        error_code add(int a, int b, [out] int& result);
    };
}
```

### Nested Namespaces

```idl
namespace outer
{
    namespace inner
    {
        interface i_service
        {
            error_code method();
        };
    }
}
```

### Inline Namespace

Inline namespaces allow seamless API evolution by treating nested namespaces as a single namespace. This is essential for version management.

**Key Benefits**:
- **C++ access without version prefix**: `calculator::i_calculator` instead of `calculator::v1::i_calculator`
- **Seamless version upgrades**: When adding `v2`, existing code continues to work
- **Explicit version access available**: Can still use `calculator::v1::i_calculator` or `calculator::v2::i_calculator`

```idl
namespace calculator
{
    [inline] namespace v1
    {
        interface i_calculator
        {
            error_code add(int a, int b, [out] int& result);
        };
    }
}
```

**C++ Usage**:
```cpp
// Direct access (inline namespace means v1 is transparent)
calculator::i_calculator calc;

// Explicit version access (still works)
calculator::v1::i_calculator calc_v1;
```

**Version Migration Example**:
```idl
// Original version
namespace comprehensive
{
    [inline] namespace v1
    {
        interface i_service { ... };
    }
}

// Adding v2 with new features
namespace comprehensive
{
    [inline] namespace v2
    {
        interface i_service : v1::i_service
        {
            error_code new_feature();
        };
    }
}
```

### Namespace Attributes

```idl
[status=production]  // development, production, deprecated
namespace stable
{
    interface i_service { };
};
```

## 3. Structures

Structs define complex data types with member variables.

### Basic Struct

```idl
namespace xxx
{
    struct person
    {
        std::string name;
        int age;
        std::vector<std::string> hobbies;
    };
}
```

### Struct with Defaults

```idl
struct config
{
    std::string name = "default_name";
    int timeout = 30;
    bool enabled = true;
};
```

### Struct with Static Members

```idl
struct constants
{
    static std::string prefix = "RPC_";
    static int max_value = 1000;
};
```

### Nested Structs

```idl
struct address
{
    std::string street;
    std::string city;
    int zip_code;
};

struct employee
{
    std::string name;
    address home_address;
    address work_address;
};
```

### Vector and Map Types

```idl
struct collection
{
    std::vector<int> numbers;
    std::vector<std::string> strings;
    std::map<std::string, int> name_to_id;
    std::map<int, std::string> id_to_name;
};
```

### Template Structs

```idl
template<typename T>
struct box
{
    T value;
};

template<typename T>
struct result
{
    T data;
    bool success;
    std::string error_message;
};
```

## 4. Interfaces

Interfaces define RPC service contracts with method signatures.

### Basic Interface

```idl
namespace yyy
{
    interface i_example
    {
        error_code add(int a, int b, [out] int& c);
        error_code multiply(int a, int b, [out] int& c);
    };
}
```

### Interface with Attributes

```idl
[status=production, description="Calculator service for mathematical operations"]
interface i_calculator
{
    [description="Adds two integers"] error_code add(int a, int b, [out] int& result);
    [description="Subtracts two integers"] error_code subtract(int a, int b, [out] int& result);
    [description="Multiplies two integers"] error_code multiply(int a, int b, [out] int& result);
};
```

### Interface with Interface Parameters

```idl
namespace yyy
{
    interface i_host
    {
        error_code look_up_app(const std::string& name, [out] rpc::shared_ptr<i_example>& app);
        error_code set_app(const std::string& name, [in] const rpc::shared_ptr<i_example>& app);
    };

    interface i_example
    {
        error_code create_foo([out] rpc::shared_ptr<xxx::i_foo>& target);
        error_code receive_interface([out] rpc::shared_ptr<xxx::i_foo>& val);
        error_code give_interface([in] const rpc::shared_ptr<xxx::i_baz> val);
    };
}
```

### Interface with Multiple Inheritance

```idl
namespace xxx
{
    interface i_foo
    {
        error_code do_something(int val);
    };

    interface i_bar
    {
        error_code do_something_else(int val);
    };

    // Multiple inheritance
    interface i_baz : i_foo, i_bar
    {
        error_code callback(int val);
    };
}
```

### Function Tags for Special Processing

The `[tag=...]` attribute passes metadata to the service proxy or transport for special processing:

```idl
// Define tag values (typically an enum)
namespace comprehensive::v1
{
    enum class tags
    {
        none = 0,
        include_certificate,
        require_auth,
        high_priority
    };
}

interface i_demo_service
{
    // Normal call - no special processing
    error_code get_name([out] std::string& name);

    // Tagged call - transport/proxy can handle specially
    [tag=comprehensive::v1::tags::include_certificate]
    error_code create_object([out] rpc::shared_ptr<i_managed_object>& obj);

    [tag=comprehensive::v1::tags::require_auth]
    error_code delete_object(uint64_t object_id);
}
```

**How Tags Work**:
1. The `[tag=...]` attribute is stored in the generated interface metadata
2. When the function is called via the service proxy, the tag is passed to `i_marshaller::send()` and `i_marshaller::post()`
3. The transport or marshaller can inspect the tag and apply special processing

**Common Use Cases**:
- **Authentication**: Tag sensitive operations requiring extra verification
- **Priority**: High-priority vs normal calls in network transport
- **Encryption**: Request certificate inclusion for certain operations
- **Logging**: Tag operations for audit trails
- **Routing**: Direct specific calls through different paths

**Accessing Tags in Generated Code**:
```cpp
// Tags are accessible via function_info
auto& info = i_demo_service::function_info<create_object>();
auto tag = info.tag;  // comprehensive::v1::tags::include_certificate
```

## 5. Parameter Direction and Passing

Parameters in Canopy have direction attributes that control data marshalling across the transport.

### Default Behavior ([in])

If no attribute is specified, the parameter is assumed to be `[in]`:

```idl
// All three are equivalent - data sent TO the object
error_code process_value(int value);
error_code process_value([in] int value);
error_code process_value([in] const int& value);
```

### Input Parameters ([in])

Data is marshalled FROM the caller TO the remote object:

```idl
// Pass by value (copy)
error_code process_value(int value);

// Pass by const reference
error_code process_ref([in] const int& value);

// Pass by move
error_code process_move([in] int&& value);
```

### Output Parameters ([out])

Data is marshalled FROM the remote object BACK to the caller:

```idl
// Output by reference (caller provides storage)
error_code get_value([out] int& value);
```

### Input-Output Parameters ([in, out])

**Note**: This feature has limited testing, particularly with interface parameters. Use with caution and verify behavior in your specific use case.

Data is marshalled IN BOTH DIRECTIONS - first to the object, then back:

```idl
// Modify in place
error_code modify([in, out] int& value);
```

### Pointer Types in IDL

**Recommendation**: Avoid using raw pointer types (`T*`, `T*&`) in IDL interfaces.

**Reason**: Pointers represent memory addresses in a specific address space. When marshalling data between different processes or machines, these addresses have no meaning in the remote address space.

```idl
// Not recommended - pointers only valid in shared memory scenarios
error_code get_optional([out] int*& value);  // Address not valid remotely
error_code process_ptr([in] const int* value);  // Address not valid remotely
```

**Use instead**: Value types, references, or smart pointers:

```idl
// Good - values are copied across address spaces
error_code get_value([out] int& value);
error_code get_values([out] std::vector<int>& values);

// Use rpc::shared_ptr for interface references
error_code get_service([out] rpc::shared_ptr<i_service>& service);
```

**Exception**: Pointer types may be useful only when both objects share the same memory address space (e.g., shared memory regions between processes).

**Use Cases** (rare):
Marshalling pointers only use within zones that share the same address space, not recommended for handles
- `[in]` parameters: For pointer types (`T*`) where you need to serialize the pointer address
- `[out]` parameters: For double pointers (`T**`) or pointer references (`T*&`) to receive an address

**Example**:
```idl
// Pointer to single value - serializes the address
error_code process_value([in] const int* value);

// Pointer reference - serializes the address
error_code allocate_value([out] int*& value);
```

Node the [by_value] attribute is now deprecated, and this feature will be removed.

**Security Warning**: Raw pointer values are memory addresses that should **never** be used for unrestricted environments (e.g., web clients, untrusted networks). Pointer serialization only makes sense when both caller and callee exist in the same address space or have carefully controlled shared memory access.

## Pointer Type Restrictions for Smart Pointers

**Important**: `rpc::shared_ptr` and `rpc::optimistic_ptr` can only be `[in]` OR `[out]`, never `[in, out]`:

```idl
// Valid - shared_ptr only in
error_code set_app([in] const rpc::shared_ptr<i_service> app);

// Valid - shared_ptr only out
error_code get_app([out] rpc::shared_ptr<i_service>& app);

// Invalid - shared_ptr cannot be [in, out]
// error_code transfer_app([in, out] rpc::shared_ptr<i_service>& app);  // ERROR!
```

### Data Transfer Patterns

IDL interfaces define how data is marshalled between address spaces. Consider your transfer patterns:

```idl
// Get entire blob at once - good for bulk operations
error_code get_config([out] std::vector<char>& config_data);

// Navigate and extract - good for selective access
error_code get_value(const std::string& key, [out] std::string& value);
error_code get_count([out] int& count);
```

**Trade-offs**:
- Single large transfer: One round-trip, may transfer unused data
- Selective access: Multiple round-trips, but transfers only needed data

Choose based on your use case and network characteristics.

## 6. Enumerations

```idl
namespace rpc
{
    enum encoding : uint64_t
    {
        yas_binary = 1,
        yas_compressed_binary = 2,
        yas_json = 8,
        protocol_buffers = 16
    };

    enum status : uint32_t
    {
        success = 0,
        pending = 1,
        failed = 2
    };
}
```

## 7. Attributes

Attributes provide metadata for interfaces, methods, and structs.

### Interface Attributes

```idl
[status=production]                    // development, production, deprecated
[description="Service description"]
interface i_service
{
    // ...
};
```

### Method Attributes

```idl
[description="What the method does"]
[deprecated="Use new_method instead"]
error_code old_method(int input, [out] int& output);
```

### Struct Attributes

```idl
[status=production, description="Data structure"]
struct my_data
{
    int value;
};
```

## 8. Imports

Use `#import` to reference types from other IDL files:

```idl
// Main IDL file
#import "shared/types.idl"
#import "common/interfaces.idl"

namespace my_project
{
    // Uses types and interfaces from imports
}
```

### Import vs Include

Canopy IDL supports two ways to include external definitions:

| Directive | Behavior | Use Case |
|-----------|----------|----------|
| `#import` | Makes IDL aware of types without regenerating them | **Preferred** - Use for referencing external IDL definitions |
| `#include` | Copies and pastes IDL content (like C++ #include) | **Useful for #defines etc** - Can cause duplicate symbol errors |

### #include

Using `#include` can lead to duplicate symbol problems:

```idl
// types.idl
namespace shared
{
    struct data { int value; };
}

// Avoid this - causes duplicate symbols when types.idl is included multiple times
#include "types.idl"

// Use this instead - safe reference without duplication
#import "types.idl"
```

**Best Practice**: Always use `#import` to reference types from other IDL files. Only use `#include` if you specifically need to inline content and understand the duplication risks.

### How #import Works

When you `#import` another IDL file:
1. The generator recognizes the types defined in that file
2. No code is regenerated for the imported types
3. The imported types can be used in your interfaces and structs
4. Marshalling logic for imported types is reused from their original definition

## 9. Error Codes

Define a return type for methods that indicates success or failure:

```idl
// Define error_code as a simple typedef (typically 0 = OK, non-zero = error)
typedef int error_code;

interface i_calculator
{
    error_code add(int a, int b, [out] int& result);

    // Common error code values (defined in your IDL or C++):
    // 0 = OK
    // Non-zero = error
};
```

**Note**: `error_code` is not a built-in Canopy type. Define it in your IDL using `typedef int error_code;` or a similar pattern that works for your use case.

Canopy provides a flexible error code system designed for seamless integration with legacy applications:

```cpp
// Configure Canopy error codes to avoid conflicts with your application
rpc::error::set_offset_val(10000);           // Base offset
rpc::error::set_offset_val_is_negative(false); // Positive offset direction
rpc::error::set_OK_val(0);                   // Success value (typically 0)
```

**Key Points**:
- **RPC error codes** are reserved for internal RPC operations (transport errors, object not found, etc.)
- **Application error codes** are defined by you in your IDL (`typedef int error_code;`)
- Canopy error codes can be offset to coexist with existing application error codes
- Use RPC error codes for **inspection purposes** only, not as your application error codes

**Standard RPC Error Codes** (may be offset):
| Code | Meaning |
|------|---------|
| 0 | OK (success) |
| 1 | OUT_OF_MEMORY |
| 4 | INVALID_DATA |
| 5 | TRANSPORT_ERROR |
| 12 | OBJECT_NOT_FOUND |
| 23 | OBJECT_GONE |

## 10. Raw C++ Code Injection

Inject raw C++ code using `#cpp_quote`:

```idl
namespace rpc
{
    // Template specialization that can't be expressed in IDL
    #cpp_quote(R^__(
    template<typename T>
    class id
    {
        static constexpr uint64_t get(uint64_t rpc_version);
    };
    )__^)
}
```

## 11. Complete Example

```idl
// First define error_code (typically at top of IDL file)
typedef int error_code;

#import "example_shared/example_shared.idl"

namespace yyy
{
    [status=production]
    interface i_example
    {
        [description="Adds two integers and returns the result"]
        error_code add(int a, int b, [out] int& c);

        [description="Creates a new foo object instance"]
        error_code create_foo([out] rpc::shared_ptr<xxx::i_foo>& target);

        [description="Creates an example instance in a subordinate zone"]
        error_code create_example_in_subordinate_zone(
            [out] rpc::shared_ptr<yyy::i_example>& target,
            const rpc::shared_ptr<i_host>& host_ptr,
            uint64_t new_zone_id);

        [description="Receives an interface object (can be null)"]
        error_code receive_interface([out] rpc::shared_ptr<xxx::i_foo>& val);

        [description="Gets the current host instance"]
        error_code get_host([out] rpc::shared_ptr<i_host>& app);
    };

    [status=production]
    interface i_host
    {
        [description="Creates a new enclave instance"]
        error_code create_enclave([out] rpc::shared_ptr<i_example>& target);

        [description="Looks up an application by name from the registry"]
        error_code look_up_app(const std::string& name, [out] rpc::shared_ptr<i_example>& app);

        [description="Sets an application instance in the registry with the given name"]
        error_code set_app(const std::string& name, [in] const rpc::shared_ptr<i_example>& app);

        [description="Unloads an application by name from the registry"]
        error_code unload_app(const std::string& name);
    };
}
```

## 12. Generated Code

When you compile IDL files, Canopy generates:

| File | Purpose |
|------|---------|
| `{name}.h` | Interface declarations |
| `{name}_proxy.cpp` | Client-side proxy implementation |
| `{name}_stub.cpp` | Server-side stub implementation |
| `{name}_stub.h` | Stub declarations |
| `{name}.json` | JSON schema for introspection |

### Generated Interface

```cpp
// Generated header
namespace yyy {

class i_example : public rpc::interface<i_example>
{
public:
    virtual CORO_TASK(error_code) add(int a, int b, [out] int& c) = 0;
    virtual ~i_example() = default;

    static std::vector<rpc::function_info> get_function_info();
};

} // namespace yyy
```

### Generated Proxy

```cpp
// Generated proxy implementation
class i_example_proxy : public rpc::interface_proxy<i_example>
{
public:
    virtual CORO_TASK(error_code) add(int a, int b, [out] int& c) override
    {
        // Serialization and network send
        // Deserialization of response
    }
};
```

## 13. IDL Best Practices

1. **Use descriptive names**: `i_calculator_service` not `calc`
2. **Add descriptions**: Always include `[description="..."]` attributes
3. **Version with namespaces**: Use `[inline] namespace v1` for API versioning
4. **Keep interfaces focused**: Single responsibility per interface
5. **Use output parameters**: For large data, use `[out]` to avoid copying
6. **Document error codes**: Explain what each method returns on error
7. **Organize in files**: Group related interfaces in separate files
8. **Use #import for external types**: Avoid `#include` to prevent duplicate symbols
9. **Understand marshalling**: `[in]` sends to object, `[out]` receives back
10. **No [in,out] with pointers**: `rpc::shared_ptr` and `rpc::optimistic_ptr` cannot be `[in, out]`
11. **Avoid raw pointers**: Use references, value types, or smart pointers instead

## 14. IDL Generator Quirks and Known Issues

When using the Canopy IDL generator, be aware of the following behaviors and workarounds:

### Parameter Attributes in Implementations

**Note**: Parameter attributes (`[in]`, `[out]`, `[in, out]`) are instructions to the IDL generator about marshalling direction. They are reflected in the generated proxy/stub code but do not appear in the C++ method signature as attributes.

```idl
interface i_example
{
    int process([in] const std::string& input, [out] std::string& output);
};

// C++ implementation - attributes determine marshalling but are not C++ attributes
CORO_TASK(int) process(const std::string& input, std::string& output) override
{
    output = "Processed: " + input;
    CO_RETURN rpc::error::OK();
}
```

### Reserved Method Names

**Issue**: Method names like `get_id` may conflict with interface ID getter methods.

**Workaround**: Use alternative names:

```idl
// Avoid
interface i_object
{
    int get_id([out] uint64_t& id);  // Conflicts with interface ID getter
};

// Use instead
interface i_object
{
    int get_object_id([out] uint64_t& id);  // Unique method name
};
```

### Struct Serialization

**Issue**: Some struct patterns may not serialize correctly due to generator limitations.

**Workaround**: Test struct serialization early and avoid complex nested patterns. If serialization fails, simplify the struct or use alternative data representation.

### Duplicate Parameter Names

**Note**: Multiple methods in the same interface can use the same parameter names (e.g., `result`) without issue. The IDL generator should handle this correctly.

If you encounter conflicts with duplicate parameter names, this indicates a bug in the code generator that should be reported.

```idl
// This is valid IDL - parameter names can repeat across methods
interface i_calculator
{
    int add(int a, int b, [out] int& result);
    int multiply(int a, int b, [out] int& result);  // 'result' is fine here
    int divide(int a, int b, [out] int& result);    // 'result' is fine here
};
```

## 15. Next Steps

- [Transports](04-transports.md) - Learn about communication channels
- [Getting Started](06-getting-started.md) - Follow a tutorial
- [API Reference](12-api-reference.md) - Complete API documentation
