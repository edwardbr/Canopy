# Transport Documentation

The transport documents currently describe Canopy transport concepts through the
primary C++ implementation.

Use the following split when reading them:

- [Shared Transport View](shared/README.md)
- [C++ Transport View](cpp/README.md)
- [TCP Transport](tcp.md)
- [io_uring Stream Factories](io_uring.md)
- [SPSC Queues and IPC](spsc_and_ipc.md)
- [Streaming Result Listener](streaming-result-listener.md)
- [Security Notes](../security/README.md), especially for hostile transports,
  SGX boundaries, IPC, and network input

The existing transport documents remain the detailed source material, but the
index pages above describe how to interpret them.
