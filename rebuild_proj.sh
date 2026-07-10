#!/usr/bin/env bash
# Manual stopgap build for the daemon (no CMakeLists.txt/root Makefile yet — see CLAUDE.md
# "Commands" section). Wraps the exact documented gcc invocation; does not run the daemon,
# since that needs sudo plus per-IED runtime args (host/mmsPort/iedName/interface/...).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

OUT="${1:-/tmp/ied_reporter_daemon}"

gcc -g -Wall -Isrc -idirafter third_party/include \
    src/main.c src/main_discovery_prompt.c \
    src/orchestration/*/*.c \
    src/features/*/*/*.c \
    -o "$OUT" \
    -Lthird_party/lib -liec61850 -lhal -lmxml -lwebsockets -lcjson -lpthread

echo "Built: $OUT"
