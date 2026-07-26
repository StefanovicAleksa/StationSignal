# E2E test of the scan_dispatcher service API: real bind, real libwebsockets
# service thread, a hand-rolled minimal websocket test client (deliberately
# NOT libwebsockets client mode - see e2e_test_scan_dispatcher.c's header
# comment, same rationale integration_tests/ipc_dispatcher/ already applies),
# driving hand-built ScanDispatcher_publishDeviceFound calls and asserting
# real JSON delivery. No ied_simulator needed - this feature's job starts
# after a scan has already found something, same reasoning ipc_dispatcher's
# own E2E test uses hand-built fixtures rather than a real producer. Scoped
# to this directory only - does not imply or replace a root daemon build
# command. Unity is vendored as a prebuilt static lib in third_party/, same
# as libiec61850/cJSON/libwebsockets - linked via -lunity.

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
SCAN_DISPATCHER := $(SRC)/features/scan_dispatcher

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -lhal -lcjson -lwebsockets -lunity -lpthread

BIN := e2e_test_scan_dispatcher

.PHONY: all run clean

all: run

$(BIN): e2e_test_scan_dispatcher.c \
		$(SCAN_DISPATCHER)/domain/scan_dispatcher_usecases.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_ring_buffer.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_ws_server.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_json_writer.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_adapter.c \
		$(SCAN_DISPATCHER)/service/scan_dispatcher_api.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
