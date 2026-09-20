#!/bin/bash

set -e

SERVER_SRC="tcp_server.cc"
CLIENT_SRC="tcp_client.cc"
SERVER_APP="tcp_server"
CLIENT_APP="tcp_client"

PORT=9000
HOST="127.0.0.1"

SERVER_LOG="server.log"
CLIENT_LOG="client.log"

GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

fail() {
    echo -e "${RED}[FAIL]${NC} $1"
}

info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

cleanup() {
    info "Cleaning up processes and temporary logs..."
    if [ -n "${SERVER_PID:-}" ]; then
        kill -9 "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -f "${SERVER_LOG}" "${CLIENT_LOG}" "${SERVER_APP}" "${CLIENT_APP}"
}

trap cleanup EXIT INT TERM

# 1. Elevate file descriptor limit
ulimit -n 65535 2>/dev/null || true

# 2. Dependency Check
if ! pkg-config --exists liburing 2>/dev/null && [ ! -f /usr/include/liburing.h ]; then
    fail "'liburing' development headers not found."
    exit 1
fi
pass "'liburing' dependency check passed."

# 3. Check Port Availability
if lsof -i:"${PORT}" >/dev/null 2>&1; then
    fail "Port ${PORT} is currently occupied."
    exit 1
fi
pass "Port ${PORT} is available."

# 4. Compile Server and Client
echo "================================================="
echo " 1. Compiling TCP Server & Client"
echo "================================================="

if g++ -O3 -std=c++17 -Wall "${SERVER_SRC}" -o "${SERVER_APP}" -luring -lpthread; then
    pass "TCP Server compilation successful."
else
    fail "TCP Server compilation failed."
    exit 1
fi

if g++ -O3 -std=c++17 -Wall "${CLIENT_SRC}" -o "${CLIENT_APP}" -luring; then
    pass "TCP Client compilation successful."
else
    fail "TCP Client compilation failed."
    exit 1
fi

# 5. Launch Server in Background with unbuffered output (stdbuf -oL)
echo ""
echo "================================================="
echo " 2. Starting TCP Server"
echo "================================================="

# stdbuf -oL forces line-buffering on stdout even when redirected to a file
stdbuf -oL ./"${SERVER_APP}" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

sleep 1.5

if kill -0 "${SERVER_PID}" 2>/dev/null; then
    pass "Server started successfully (PID: ${SERVER_PID})."
else
    fail "Server failed to start."
    cat "${SERVER_LOG}"
    exit 1
fi

# 6. Execute TCP Client
echo ""
echo "================================================="
echo " 3. Running TCP Client Test"
echo "================================================="

if ./"${CLIENT_APP}" > "${CLIENT_LOG}" 2>&1; then
    pass "TCP Client process exited cleanly."
else
    fail "TCP Client failed during execution."
    cat "${CLIENT_LOG}"
    exit 1
fi

# Give small delay to ensure all buffers flush to log files
sleep 0.5

# 7. Print Log Summaries & Validate Payload
echo ""
echo "================================================="
echo " 4. Verifying Log Outputs"
echo "================================================="

echo "--- Client Output ---"
cat "${CLIENT_LOG}"
echo "---------------------"

echo "--- Server Output ---"
cat "${SERVER_LOG}"
echo "---------------------"

if grep -q "Received Echo back from server: \"Hello TCP Server via io_uring!\"" "${CLIENT_LOG}" && \
   grep -q "Received from client" "${SERVER_LOG}"; then
    echo ""
    pass "TCP Client and Server io_uring communication test PASSED."
else
    echo ""
    fail "Message exchange verification failed."
    exit 1
fi
