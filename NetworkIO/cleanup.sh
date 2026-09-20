#!/bin/bash

SERVER_APP="uring_server"
PORT=9000

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
echo "  Cleaning up io_uring Server Artifacts"
echo "================================================="

# Kill server process running on port 9000
SERVER_PID=$(lsof -t -i:"${PORT}" 2>/dev/null || true)

if [ -n "${SERVER_PID}" ]; then
    if kill -9 "${SERVER_PID}" 2>/dev/null; then
        pass "Stopped server process on port ${PORT}."
    else
        fail "Failed to stop server process on port ${PORT}."
    fi
fi

pkill -9 -x "${SERVER_APP}" 2>/dev/null || true

# Remove binaries
if [ -f "${SERVER_APP}" ]; then
    if rm -f "${SERVER_APP}"; then
        pass "Removed executable: ${SERVER_APP}"
    else
        fail "Failed to remove executable: ${SERVER_APP}"
    fi
fi

rm -f *.o *.log core

pass "Cleanup complete."
