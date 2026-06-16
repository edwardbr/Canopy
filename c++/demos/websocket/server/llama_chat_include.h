// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

// chat.h declares a global `json` alias, while Canopy generated headers
// forward-declare a top-level `namespace json`. Keep the workaround local.
#include <common.h>
#include <jinja/caps.h>
#include <jinja/parser.h>
#include <jinja/runtime.h>
#include <nlohmann/json_fwd.hpp>
#include <peg-parser.h>

#pragma push_macro("json")
#undef json
#define json canopy_llama_cpp_ordered_json
#include <chat.h>
#pragma pop_macro("json")
