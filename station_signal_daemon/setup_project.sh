#!/usr/bin/env bash
# Rebuilds every one of station_signal_daemon/third_party/{include,lib}'s vendored static
# archives (libiec61850.a, libhal.a, libcjson.a, libmxml.a, libwebsockets.a) from the vendored
# source in third_party_src/ (git submodules - run `git submodule update --init --recursive`
# first if not yet checked out; the root repo's .gitmodules governs these, since the gitlinks
# live under this directory but are recorded in the top-level station_signal tree).
#
# NOT part of the normal build and not run automatically - rebuild_proj.sh and every Makefile
# in this repo build against the already-committed third_party/{include,lib}, which is the
# source of truth (see CLAUDE.md's "Don't touch third_party/" Hard Rule). Only run this if you
# genuinely need to regenerate those archives (different build flags, a version bump, targeting a
# new architecture such as Raspberry Pi's ARM, or to step into library source while debugging a
# link/runtime issue) - then review the diff in third_party/ before committing it. Runs natively
# against whatever architecture it's invoked on - no cross-compilation - so for a new target
# architecture, run this script on that machine directly.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

echo "[1/5] Installing OS build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y build-essential cmake git -qq

echo "[2/5] Building libiec61850 + hal from third_party_src/libiec61850..."
git submodule update --init third_party_src/libiec61850
(
    cd third_party_src/libiec61850
    mkdir -p build && cd build
    cmake -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF ..
    make -j"$(nproc)"
)
cp third_party_src/libiec61850/build/src/libiec61850.a third_party/lib/
cp third_party_src/libiec61850/build/hal/libhal.a third_party/lib/
find third_party_src/libiec61850/src -name "*.h" -type f -exec cp {} third_party/include/ \;
find third_party_src/libiec61850/hal -name "*.h" -type f -exec cp {} third_party/include/ \;

echo "[3/5] Building cJSON from third_party_src/cJSON..."
git submodule update --init third_party_src/cJSON
(
    cd third_party_src/cJSON
    rm -rf build && mkdir -p build && cd build
    cmake -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF ..
    make -j"$(nproc)"
)
cp third_party_src/cJSON/build/libcjson.a third_party/lib/
cp third_party_src/cJSON/cJSON.h third_party/include/

echo "[4/5] Building mxml from third_party_src/mxml..."
git submodule update --init third_party_src/mxml
(
    cd third_party_src/mxml
    ./configure --disable-shared
    make libmxml.a
)
cp third_party_src/mxml/libmxml.a third_party/lib/
cp third_party_src/mxml/mxml.h third_party/include/

echo "[5/5] Building libwebsockets from third_party_src/libwebsockets..."
git submodule update --init third_party_src/libwebsockets
(
    cd third_party_src/libwebsockets
    rm -rf build && mkdir -p build && cd build
    # Flags match how the currently-vendored archive was built (confirmed via its lws_config.h
    # and nm symbol inspection: no OpenSSL symbols, only the *_no_ssl stubs) - built without TLS,
    # static only, no test apps/extensions.
    cmake -DLWS_WITH_SSL=OFF -DLWS_WITH_SHARED=OFF -DLWS_WITH_STATIC=ON \
          -DLWS_WITHOUT_TESTAPPS=ON -DLWS_WITHOUT_EXTENSIONS=ON ..
    make -j"$(nproc)"
)
cp third_party_src/libwebsockets/build/lib/libwebsockets.a third_party/lib/
cp third_party_src/libwebsockets/include/libwebsockets.h third_party/include/
cp -r third_party_src/libwebsockets/include/libwebsockets third_party/include/
cp third_party_src/libwebsockets/build/lws_config.h third_party/include/

echo "=========================================================================="
echo "[DONE] third_party/lib/{libiec61850,libhal,libcjson,libmxml,libwebsockets}.a"
echo "and third_party/include/ regenerated from third_party_src/. Review"
echo "'git diff third_party/' before committing - this replaces the currently-"
echo "vendored archives. Confirm the daemon still links (rebuild_proj.sh) before"
echo "trusting the result."
echo "=========================================================================="
