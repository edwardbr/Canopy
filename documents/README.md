<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Canopy Documentation

Use this directory by task, not by file number.

## Start Here

- New to Canopy:
  - [Introduction](01-introduction.md)
  - [Getting Started](02-getting-started.md)
  - [External Project Guide](external-project-guide.md)
- Best first hands-on path:
  - [External Project Guide](external-project-guide.md)
  - then the example consumer app linked from the root [README](../README.md)
- Building and testing the primary implementation:
  - [Build And Test](build-and-test/README.md)
  - [C++ Build And Test Guide](build-and-test/cpp.md)
- Understanding the runtime:
  - [Architecture](architecture/README.md)
  - [Transports](transports/README.md)
- Looking up APIs and patterns:
  - [API Reference](09-api-reference.md)
  - [Examples](10-examples.md)
  - [Best Practices](11-best-practices.md)

## Implementation Status

- [C++ Status](status/cpp.md)
- [Rust Status](status/rust.md)
- [JavaScript Status](status/javascript.md)

## Language Port Material

- [Rust Port Documentation](language-ports/rust/README.md)

## How To Read These Docs

- The primary implementation is C++.
- Many documents describe shared Canopy concepts through the C++ implementation.
- When a document is implementation-specific, it should say so near the top.
- If a document and the code disagree, prefer the live code and CMake files.
