#!/usr/bin/env bash
set -e

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

git clone "$SPIKE_TEST_TRACES_URL" ${ROOT}/spike-traces

mkdir ${ROOT}/data/cvs

cp ${ROOT}/spike-traces/* ${ROOT}/data/cvs/
