#!/usr/bin/env bash
# Manual stopgap build for the daemon (no CMakeLists.txt/root Makefile yet — see CLAUDE.md
# "Commands" section). Wraps the exact documented gcc invocation; does not run the daemon,
# since that needs sudo (raw GOOSE socket access) if a real device is ever actually reported on.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

OUT="${1:-/tmp/ied_reporter_daemon}"

gcc -g -Wall -Isrc -idirafter third_party/include \
    src/main.c \
    src/orchestration/*/*.c \
    src/scan_orchestration/*/*.c \
    src/device_manager/*/*.c \
    src/features/*/*/*.c \
    -o "$OUT" \
    -Lthird_party/lib -liec61850 -lhal -lmxml -lwebsockets -lcjson -lpthread

echo "Built: $OUT"
