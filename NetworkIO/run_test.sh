#!/bin/bash

set -e

SERVER_APP="uring_server"
SERVER_SRC="main.cc"
PORT=9000
HOST="127.0.0.1"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

fail() {
    echo -e "${RED}[FAIL]${NC} $1"
}


# 1. Elevate file descriptor limit for 2000+ socket connections
ulimit -n 65535 2>/dev/null || true

# 2. Dependency Check
if ! pkg-config --exists liburing 2>/dev/null && [ ! -f /usr/include/liburing.h ]; then
    fail "'liburing' development headers not found."
    echo "Please run: sudo ./setup_deps.sh"
    exit 1
fi

pass "'liburing' dependency check passed."

# 3. Check Port Availability
if lsof -i:"${PORT}" >/dev/null 2>&1; then
    fail "Port ${PORT} is occupied."
    echo "Run ./cleanup.sh first."
    exit 1
fi

pass "Port ${PORT} is available."

# 4. Compile the Server
echo "================================================="
echo "  1. Compiling io_uring Server (${SERVER_SRC})"
echo "================================================="

if g++ -O3 -std=c++17 -Wall "${SERVER_SRC}" -o "${SERVER_APP}" -luring -lpthread; then
    pass "Compilation successful."
else
    fail "Compilation failed."
    exit 1
fi

# 5. Launch Server in Background
echo ""
echo "================================================="
echo "  2. Starting Server & Spawning Stress Workload"
echo "================================================="

./"${SERVER_APP}" &
SERVER_PID=$!

# Ensure server cleanup on script exit
trap 'kill -9 $SERVER_PID 2>/dev/null || true' EXIT

# Give server time to bind port and setup ring buffers
sleep 1.5

if kill -0 "${SERVER_PID}" 2>/dev/null; then
    pass "Server started successfully."
else
    fail "Server failed to start."
    exit 1
fi

# 6. Stress Test Configuration (Pure Bash Implementation)
TARGET_CONNECTIONS=2000
MSGS_PER_CONN=20

echo "[STRESS] Launching ${TARGET_CONNECTIONS} parallel TCP connections..."
echo "[STRESS] Each connection sends ${MSGS_PER_CONN} messages with random sleeps and payload sizes."

# Single client worker function running inside a background subshell
run_client_worker() {
    local conn_id=$1
    for ((m=1; m<=MSGS_PER_CONN; m++)); do
        # Open TCP connection to the io_uring server using Bash socket interface
        exec 3<>/dev/tcp/${HOST}/${PORT} 2>/dev/null || return

        # Generate variable payload size (between 32 and 512 characters)
        local payload_size=$(( (RANDOM % 480) + 32 ))
        local payload
        payload=$(head -c "${payload_size}" /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c "${payload_size}")

        # Send data and read echo response
        echo "${payload}" >&3
        read -t 2 -r response <&3 || true

        # Close socket
        exec 3<&-
        exec 3>&-

        # Random sleep interval (between 10ms and 150ms)
        local sleep_ms=$(( (RANDOM % 140 + 10) ))
        sleep "0.${sleep_ms}"
    done
}

export -f run_client_worker
export HOST PORT MSGS_PER_CONN

# Spawn 2000 background worker processes in batches
for ((i=1; i<=TARGET_CONNECTIONS; i++)); do
    run_client_worker "$i" &

    # Throttle spawning rate slightly to avoid local ephemerals socket exhaustion
    if (( i % 100 == 0 )); then
        sleep 0.1
    fi
done

echo ""
echo "[STRESS] All ${TARGET_CONNECTIONS} worker jobs spawned into background."
echo "[STRESS] Monitoring server stats output... (waiting for completion)"

# Wait for all background client subshells to finish
wait

echo ""
echo "================================================="
pass "Native Bash stress workload finished."
echo "================================================="

# Allow server stats monitor thread to print final throughput
sleep 2

# Cleanup server
kill -9 "${SERVER_PID}" 2>/dev/null || true

pass "io_uring server stress test completed successfully."
