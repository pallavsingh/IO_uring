#!/bin/bash

set -e

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
echo "   Installing Dependencies for io_uring Engine"
echo "================================================="

# 1. Ensure script is run with root privileges
if [ "$EUID" -ne 0 ]; then
    fail "This script requires root privileges to install packages."
    echo "Please re-run using: sudo ./setup_deps.sh"
    exit 1
fi

pass "Root privilege check passed."

# 2. Detect Package Manager & Install Dependencies
if command -v apt-get &>/dev/null; then
    echo "Detected Debian/Ubuntu system."
    export DEBIAN_FRONTEND=noninteractive

    if apt-get update -qq && apt-get install -y build-essential liburing-dev pkg-config; then
        pass "Dependencies installed successfully."
    else
        fail "Failed to install dependencies."
        exit 1
    fi

elif command -v dnf &>/dev/null; then
    echo "Detected RHEL/Fedora/CentOS system."

    if dnf install -y gcc-c++ liburing-devel pkgconf-pkg-config; then
        pass "Dependencies installed successfully."
    else
        fail "Failed to install dependencies."
        exit 1
    fi

elif command -v pacman &>/dev/null; then
    echo "Detected Arch Linux system."

    if pacman -S --noconfirm --needed base-devel liburing pkgconf; then
        pass "Dependencies installed successfully."
    else
        fail "Failed to install dependencies."
        exit 1
    fi

elif command -v zypper &>/dev/null; then
    echo "Detected openSUSE system."

    if zypper install -y gcc-c++ liburing-devel pkg-config; then
        pass "Dependencies installed successfully."
    else
        fail "Failed to install dependencies."
        exit 1
    fi

else
    fail "Unsupported package manager."
    echo "Please manually install 'liburing-dev' and 'g++'."
    exit 1
fi

echo ""
echo "================================================="
pass "All dependencies installed successfully!"
echo "================================================="
