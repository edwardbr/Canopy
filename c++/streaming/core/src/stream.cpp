// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// stream.cpp - Default stream method implementations
//
// All three public streaming headers are pulled in so any compile error in
// the dual-mode interface surfaces directly in the streaming library build
// (both Debug and Debug_Coroutine) rather than only at downstream consumer
// build time.
#include <streaming/stream.h>
#include <streaming/stream_acceptor.h>
#include <streaming/listener.h>

namespace streaming
{
} // namespace streaming
