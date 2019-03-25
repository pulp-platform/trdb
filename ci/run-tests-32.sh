#!/usr/bin/env bash
set -e

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# 32-bit tests
make -C ${ROOT} clean all test

./run-cli-tests.sh
