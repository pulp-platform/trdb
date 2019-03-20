#!/usr/bin/env bash
set -e

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

cd ${ROOT}
make spike-traces-32
make spike-traces-64
