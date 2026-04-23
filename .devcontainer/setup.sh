#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

echo "==> Installing system dependencies..."
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential \
    gcc \
    lcov \
    python3 \
    python3-pip \
    python3-venv \
    git \
    make \
    curl \
    wget \
    ca-certificates \
    libffi-dev \
    libssl-dev \
    # cocotb dependencies
    python3-dev \
    iverilog \
    verilator

echo "==> Installing Python packages (cocotb + coverage)..."
pip3 install --upgrade pip
pip3 install \
    cocotb \
    cocotb-bus \
    pytest \
    pytest-cocotb \
    coverage \
    lcov_cobertura

echo "==> Cloning upstream ref model (as git submodule target)..."
# The actual clone is done via `git submodule update --init`
# This script just validates the environment.

echo ""
echo "==> Environment ready."
echo "    - gcc + gcov/lcov : C coverage"
echo "    - cocotb           : Python-based hardware testbench"
echo "    - iverilog         : Verilog simulator (for cocotb RTL sim)"
echo "    - verilator        : Fast Verilog simulator (optional)"
