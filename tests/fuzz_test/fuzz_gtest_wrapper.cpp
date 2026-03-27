// GTest wrapper for fuzz_test_main so TestMate can discover and run it.
// fuzz_test_main.cpp is compiled with -Dmain=fuzz_test_entry; this file
// provides the GTest TEST case that drives it with default arguments.
#include <gtest/gtest.h>

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern int fuzz_test_entry(int argc, char** argv);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
TEST(FuzzTest, BasicCycles) // NOLINT
{
    // Run 3 cycles with default settings (no args needed beyond argc=0)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
    static const char* argv[] = {"fuzz_test_gtest", "--cycles", "3", nullptr};
    static int argc = 3;
    EXPECT_EQ(0, fuzz_test_entry(argc, const_cast<char**>(argv)));
}
