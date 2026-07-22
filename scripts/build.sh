#!/bin/bash
set -euo pipefail
rm -rf build || true
mkdir build && pushd build
cmake ..
make
popd
