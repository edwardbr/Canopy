# Shared Architecture View

These notes describe Canopy concepts that should be read as language- and
implementation-neutral unless a document explicitly says otherwise.

Best shared-concept entry points today:

- [Architecture Overview](../01-overview.md)
- [Zones](../02-zones.md)
- [Services](../03-services.md)
- [Memory Management](../04-memory-management.md)
- [Proxies and Stubs](../05-proxies-and-stubs.md)
- [Zone Hierarchies](../07-zone-hierarchies.md)
- [Core Concepts](../08-core-concepts.md)

Interpretation rule:

- if a document describes protocol, pointer semantics, zone/service concepts,
  or routing intent, it is usually shared
- if it describes concrete target names, coroutine-only behavior, SGX setup,
  specific stream classes, or exact build artifacts, it is probably C++-specific

Current limitation:

- many of the detailed documents are still written from the C++ implementation
  point of view even when the concepts are shared
- treat code snippets, target names, and build references in those documents as
  C++ examples unless the text says otherwise
