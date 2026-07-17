#!/usr/bin/env bash
# Rebuilds ied_reporter_daemon/third_party/{include,lib}'s libiec61850.a + libhal.a from the
# vendored source in third_party_src/libiec61850 (a git submodule - run
# `git submodule update --init third_party_src/libiec61850` first if it's not checked out yet;
# it builds hal/ automatically via its own CMakeLists.txt's add_subdirectory(hal)).
#
# NOT part of the normal build and not run automatically - rebuild_proj.sh and every Makefile
# in this repo build against the already-committed third_party/{include,lib}, which is the
# source of truth (see CLAUDE.md's "Don't touch third_party/" Hard Rule). Only run this if you
# genuinely need to regenerate those archives (different CMake flags, a version bump, or to step
# into library source while debugging a link/runtime issue) - then review the diff in
# third_party/ before committing it.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

echo "[1/4] Installing OS build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y build-essential cmake git -qq

echo "[2/4] Building libiec61850 + hal from third_party_src/libiec61850..."
git submodule update --init third_party_src/libiec61850
cd third_party_src/libiec61850
mkdir -p build && cd build
cmake -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF ..
make -j"$(nproc)"
cd ../../..

echo "[3/4] Copying compiled static archives into third_party/lib..."
cp third_party_src/libiec61850/build/src/libiec61850.a third_party/lib/
cp third_party_src/libiec61850/build/hal/libhal.a third_party/lib/

echo "[4/4] Flattening headers into third_party/include..."
find third_party_src/libiec61850/src -name "*.h" -type f -exec cp {} third_party/include/ \;
find third_party_src/libiec61850/hal -name "*.h" -type f -exec cp {} third_party/include/ \;

echo "=========================================================================="
echo "[DONE] third_party/lib/libiec61850.a + libhal.a and third_party/include/"
echo "regenerated from third_party_src/libiec61850. Review 'git diff third_party/'"
echo "before committing - this replaces the currently-vendored archives."
echo "=========================================================================="
