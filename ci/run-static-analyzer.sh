#!/usr/bin/env
set -e

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

"$SCAN_BUILD" make -C ${ROOT} clean all
