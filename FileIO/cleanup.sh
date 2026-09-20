#!/bin/bash

APP_NAME="uring_benchmark"
TARGET_FILE="io_engine_target.dat"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

fail() {
    echo -e "${RED}[FAIL]${NC} $1"
}


echo "================================================="
echo "  Cleaning up Benchmark Artifacts & Data Files"
echo "================================================="

# Remove compiled binary
if [ -f "${APP_NAME}" ]; then
    if rm -f "${APP_NAME}"; then
        pass "Removed executable: ${APP_NAME}"
    else
        fail "Failed to remove executable: ${APP_NAME}"
    fi
fi

# Remove target dummy data files
if [ -f "${TARGET_FILE}" ]; then
    if rm -f "${TARGET_FILE}"; then
        pass "Removed benchmark file: ${TARGET_FILE}"
    else
        fail "Failed to remove benchmark file: ${TARGET_FILE}"
    fi
fi

if [ -f "${TARGET_FILE}.copy" ]; then
    if rm -f "${TARGET_FILE}.copy"; then
        pass "Removed copy benchmark file: ${TARGET_FILE}.copy"
    else
        fail "Failed to remove copy benchmark file: ${TARGET_FILE}.copy"
    fi
fi

# Clean any leftover objects or crash logs
rm -f *.o *.log core

pass "Cleanup complete."
