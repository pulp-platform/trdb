#!/usr/bin/env bash
set -e

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# 64-bit tests
make -C ${ROOT} clean all test CFLAGS=-DTRDB_ARCH64
