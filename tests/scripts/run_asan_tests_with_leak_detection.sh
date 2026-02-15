#!/bin/bash
cd /var/home/edward/projects/Canopy/build_debug

# Enable leak detection with suppressions
export ASAN_OPTIONS="detect_leaks=1"
export LSAN_OPTIONS="suppressions=../tests/lsan.supp:print_suppressions=0"

# Get all test names properly
CURRENT_SUITE=""
declare -a TEST_ARRAY

echo "Collecting test list..."
while IFS= read -r line; do
    # Check if it's a test suite line (e.g., "type_test/0.  # TypeParam...")
    if [[ $line =~ ^([A-Za-z0-9_]+)/([0-9]+)\. ]]; then
        CURRENT_SUITE="${BASH_REMATCH[1]}/${BASH_REMATCH[2]}"
    # Check if it's a test case (starts with spaces)
    elif [[ $line =~ ^[[:space:]]+([A-Za-z0-9_]+)$ ]]; then
        TEST_NAME="${BASH_REMATCH[1]}"
        if [ -n "$CURRENT_SUITE" ]; then
            TEST_ARRAY+=("${CURRENT_SUITE}.${TEST_NAME}")
        fi
    fi
done < <(./output/rpc_test --gtest_list_tests 2>&1)

TOTAL_COUNT=${#TEST_ARRAY[@]}
PASSED_COUNT=0
FAILED_COUNT=0
LEAK_COUNT=0
FAILED_TESTS=()
LEAK_TESTS=()

echo "========================================"
echo "Running AddressSanitizer Tests with Leak Detection"
echo "Total tests to run: $TOTAL_COUNT"
echo "========================================"
echo ""

for i in "${!TEST_ARRAY[@]}"; do
    FULL_TEST="${TEST_ARRAY[$i]}"
    TEST_NUM=$((i + 1))

    echo -n "[$TEST_NUM/$TOTAL_COUNT] Running: $FULL_TEST ... "

    # Run the test and capture output
    if timeout 30 ./output/rpc_test --gtest_filter="$FULL_TEST" > /tmp/test_output_leak.log 2>&1; then
        # Check for memory leaks
        if grep -q "ERROR: LeakSanitizer: detected memory leaks" /tmp/test_output_leak.log; then
            echo "MEMORY LEAK"
            LEAK_TESTS+=("$FULL_TEST")
            LEAK_COUNT=$((LEAK_COUNT + 1))
            cp /tmp/test_output_leak.log "/tmp/leak_${FULL_TEST//\//_}.log"
        # Check if AddressSanitizer found any other errors
        elif grep -q "AddressSanitizer.*ERROR" /tmp/test_output_leak.log; then
            echo "ASAN ERROR"
            FAILED_TESTS+=("$FULL_TEST")
            FAILED_COUNT=$((FAILED_COUNT + 1))
            cp /tmp/test_output_leak.log "/tmp/asan_leak_error_${FULL_TEST//\//_}.log"
        else
            echo "PASSED"
            PASSED_COUNT=$((PASSED_COUNT + 1))
        fi
    else
        echo "FAILED/TIMEOUT"
        FAILED_TESTS+=("$FULL_TEST")
        FAILED_COUNT=$((FAILED_COUNT + 1))
        cp /tmp/test_output_leak.log "/tmp/asan_leak_error_${FULL_TEST//\//_}.log"
    fi
done

echo ""
echo "========================================"
echo "Summary (Leak Detection Enabled)"
echo "========================================"
echo "Total tests: $TOTAL_COUNT"
echo "Passed: $PASSED_COUNT"
echo "Failed (ASan errors): $FAILED_COUNT"
echo "Memory leaks detected: $LEAK_COUNT"
echo ""

if [ ${#FAILED_TESTS[@]} -gt 0 ]; then
    echo "Failed tests (ASan errors):"
    for test in "${FAILED_TESTS[@]}"; do
        echo "  - $test"
        echo "    Log: /tmp/asan_leak_error_${test//\//_}.log"
    done
    echo ""
fi

if [ ${#LEAK_TESTS[@]} -gt 0 ]; then
    echo "Tests with memory leaks:"
    for test in "${LEAK_TESTS[@]}"; do
        echo "  - $test"
        echo "    Log: /tmp/leak_${test//\//_}.log"
    done
fi
