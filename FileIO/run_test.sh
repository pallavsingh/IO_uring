#!/bin/bash

set -e

APP_NAME="uring_benchmark"
SRC_NAME="main.cc"
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


# 1. Dependency Check - Direct user to setup_deps.sh if missing
if ! pkg-config --exists liburing 2>/dev/null && [ ! -f /usr/include/liburing.h ]; then
    fail "'liburing' development headers not found."
    echo "Please install dependencies first by running: sudo ./setup_deps.sh"
    exit 1
fi

pass "'liburing' dependency check passed."

# 2. Build Step
echo "================================================="
echo "  1. Compiling io_uring Engine (${SRC_NAME})"
echo "================================================="

if g++ -O3 -std=c++20 -Wall "${SRC_NAME}" -o "${APP_NAME}" -luring -lpthread; then
    pass "Compilation successful."
else
    fail "Compilation failed."
    exit 1
fi

echo ""
echo "================================================="
echo "  2. Running Benchmark Workloads"
echo "================================================="

WORKERS=4
OPS_PER_WORKER=50000

for MODE in rand_read seq_read rand_write seq_write copy; do
    echo ""
    echo "-------------------------------------------------"
    echo " Executing Workload: [${MODE^^}]"
    echo "-------------------------------------------------"

    if ./"${APP_NAME}" "${TARGET_FILE}" "${MODE}" "${WORKERS}" "${OPS_PER_WORKER}"; then
        pass "${MODE^^} workload completed."
    else
        fail "${MODE^^} workload failed."
        exit 1
    fi
done

echo ""
echo "================================================="
pass "All benchmarks completed successfully."
echo "================================================="
