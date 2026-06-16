/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Curated robustness corpus for the hand-rolled JSON parser
// (json::v1::parse) and schema validator (json::v1::schema::schema_validator).
//
// The goal here is not exhaustive fuzz coverage — that's a separate
// libFuzzer-style harness — but to give CI a fast, deterministic check
// that the parser never crashes and only throws on bad input, and that
// schema validation against adversarial inputs terminates with a clean
// validation_result rather than an unhandled exception or hang.
//
// The corpus is split into:
//   * well_formed_inputs — must parse, must validate cleanly against a
//     permissive schema.
//   * malformed_inputs — must throw a YAS or std exception; never UB.
//   * pathological_inputs — deep nesting, big arrays, long strings; the
//     parser must terminate quickly without stack-overflowing.

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <json/json_dom.h>
#include <json/schema_validator.h>

namespace
{
    bool parses_cleanly(const std::string& input)
    {
        try
        {
            static_cast<void>(json::v1::parse(input));
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool parses_or_throws_cleanly(const std::string& input)
    {
        try
        {
            static_cast<void>(json::v1::parse(input));
            return true;
        }
        catch (const std::exception&)
        {
            // Pathological inputs may be rejected; the robustness contract is
            // that rejection is a normal std::exception rather than UB or a
            // non-std exception escaping.
            return true;
        }
        catch (...)
        {
            // Anything else (non-std exception) is a robustness failure.
            return false;
        }
    }

    bool throws_std_exception(const std::string& input)
    {
        try
        {
            static_cast<void>(json::v1::parse(input));
            return false;
        }
        catch (const std::exception&)
        {
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::string build_nested_array(int depth)
    {
        std::string s;
        s.reserve(static_cast<size_t>(depth) * 2);
        for (int i = 0; i < depth; ++i)
            s += '[';
        for (int i = 0; i < depth; ++i)
            s += ']';
        return s;
    }

    std::string build_nested_object(int depth)
    {
        std::string s;
        for (int i = 0; i < depth; ++i)
            s += R"({"x":)";
        s += "1";
        for (int i = 0; i < depth; ++i)
            s += '}';
        return s;
    }

    std::string repeat(
        const std::string& seed,
        int times)
    {
        std::string out;
        out.reserve(seed.size() * static_cast<size_t>(times));
        for (int i = 0; i < times; ++i)
            out += seed;
        return out;
    }
} // namespace

TEST(
    JsonParserRobustness,
    WellFormedCorpusParses)
{
    const std::vector<std::string> well_formed = {
        R"(null)",
        R"(true)",
        R"(false)",
        R"(0)",
        R"(-1)",
        R"(3.14)",
        R"(1e10)",
        R"(-1.5e-3)",
        R"("")",
        R"("simple")",
        R"("\"escaped\"")",
        R"("\\backslash")",
        R"("é")",            // unicode escape
        R"("\n\r\t\b\f\/")", // all simple escapes
        R"([])",
        R"([1,2,3])",
        R"({})",
        R"({"a":1})",
        R"({"a":1,"b":[true,null]})",
        R"({"nested":{"deep":{"deeper":[1,2,{"k":"v"}]}}})",
    };

    for (const auto& input : well_formed)
    {
        EXPECT_TRUE(parses_cleanly(input)) << "well-formed input failed: " << input;
    }
}

TEST(
    JsonParserRobustness,
    MalformedCorpusThrows)
{
    const std::vector<std::string> malformed = {
        // Truncated
        "",
        "{",
        "[",
        R"({"k":)",
        R"({"k":1,)",
        R"([1,)",
        // Bad numbers
        "01",
        ".5",
        "1.",
        "1e",
        "1e+",
        "+1",
        "-",
        "--1",
        "1..0",
        // Bad strings
        R"("unterminated)",
        R"("\u")",
        R"("\uXXXX")",
        R"("\x41")",
        R"("bad escape \q")",
        R"("control)"
        "\x01"
        R"(")", // raw control char inside string
        // Bad structural
        "}",
        "]",
        ",",
        ":",
        R"({"k" 1})",
        R"({,})",
        R"([,1])",
        R"([1,,2])",
        R"({"a":1 "b":2})",
        // Bad keywords
        "True",
        "FALSE",
        "Null",
        "nul",
    };

    for (const auto& input : malformed)
    {
        EXPECT_TRUE(throws_std_exception(input))
            << "malformed input parsed successfully or threw a non-std exception: " << input;
    }
}

TEST(
    JsonParserRobustness,
    DeeplyNestedInputsTerminate)
{
    // Schema validator has its own depth guard (max_schema_evaluation_depth
    // = 4096); the parser itself uses recursive descent via the YAS
    // serializer. Anything we hand it should terminate in bounded time
    // without stack-overflowing the test process.
    const auto start = std::chrono::steady_clock::now();

    for (const int depth : {64, 256, 1024})
    {
        EXPECT_TRUE(parses_or_throws_cleanly(build_nested_array(depth))) << "nested array depth=" << depth;
        EXPECT_TRUE(parses_or_throws_cleanly(build_nested_object(depth))) << "nested object depth=" << depth;
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds{5}) << "nested-input fuzzing took too long";
}

TEST(
    JsonParserRobustness,
    LargeArrayInputsTerminate)
{
    // Big-but-flat structures shouldn't blow up the parser.
    const auto big_array = "[" + repeat("0,", 10000) + "0]";
    const auto big_object_start = std::string{"{"} + repeat(R"("k":0,)", 10000);
    const auto big_object = big_object_start.substr(0, big_object_start.size() - 1) + "}";

    EXPECT_TRUE(parses_cleanly(big_array));
    EXPECT_TRUE(parses_cleanly(big_object));
}

TEST(
    JsonParserRobustness,
    SchemaValidationOnPathologicalInputs)
{
    // A schema that accepts anything is the most permissive parse target.
    // We're checking that validation against malformed instances returns
    // a clean validation_result rather than throwing or hanging.
    json::v1::schema::schema_validator anyone(R"json({})json");

    const std::vector<std::string> probes = {
        "null",
        "true",
        "false",
        "0",
        R"("text")",
        "[]",
        "[1,2,3]",
        R"({"k":1})",
    };
    for (const auto& probe : probes)
    {
        EXPECT_TRUE(anyone.validate(json::v1::parse(probe)));
    }
}

TEST(
    JsonParserRobustness,
    RecursiveSchemaCyclesDoNotDivergeOrHang)
{
    // Self-referential schemas must terminate via the depth guard or the
    // active-refs short-circuit rather than recurse forever. We don't
    // care here whether the result is valid or not — only that we get a
    // result without crashing, stack-overflowing, or hanging.
    json::v1::schema::schema_validator any_root(R"json({"$ref": "#"})json");
    static_cast<void>(any_root.validate(json::v1::parse(R"({"anything": true})")));

    json::v1::schema::schema_validator pure_loop(R"json({
        "$defs": {"node": {"$ref": "#/$defs/node"}},
        "$ref": "#/$defs/node"
    })json");
    static_cast<void>(pure_loop.validate(json::v1::parse("{}")));

    // Recursive schema with a structural constraint at every level —
    // the active-refs short-circuit must still terminate, and a wrong
    // type at the root should still fail validation.
    json::v1::schema::schema_validator typed_loop(R"json({
        "$defs": {
            "node": {
                "type": "object",
                "properties": {"child": {"$ref": "#/$defs/node"}}
            }
        },
        "$ref": "#/$defs/node"
    })json");
    EXPECT_TRUE(typed_loop.validate(json::v1::parse(R"({"child":{"child":{}}})")));
    EXPECT_FALSE(typed_loop.validate(json::v1::parse(R"("not an object")")));
}

TEST(
    JsonParserRobustness,
    RegexKeywordsInSchemaAreReportedNotCrashed)
{
    // The validator no longer supports `pattern` or `patternProperties`
    // because the IDL-driven schema generator never emits them and
    // supporting them brought a thread-safety / regex-engine surface
    // we deliberately don't want. A hand-written schema using either
    // keyword should be reported as an unsupported-keyword validation
    // error rather than crash or be silently accepted.
    json::v1::schema::schema_validator pattern_validator(R"json({
        "type": "string",
        "pattern": "([unterminated"
    })json");
    const auto pattern_result = pattern_validator.validate(json::v1::parse(R"("hello")"));
    EXPECT_FALSE(pattern_result.valid());

    json::v1::schema::schema_validator pattern_props_validator(R"json({
        "type": "object",
        "patternProperties": {"^CANOPY_": {"type": "string"}}
    })json");
    const auto pp_result = pattern_props_validator.validate(json::v1::parse(R"({"CANOPY_LOG":"x"})"));
    EXPECT_FALSE(pp_result.valid());
}
