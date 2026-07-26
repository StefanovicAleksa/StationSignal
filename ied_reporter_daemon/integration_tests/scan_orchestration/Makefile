# E2E test of the scan_orchestration service API: drives real
# ScanOrchestration_startScan/_stopScan against the real "lo" interface with
# two different mmsPorts, proving the refcounted shared-websocket sequencing
# (0->1 starts the dispatcher, 1->2 shares it, 2->1/1->0 tear-down, a
# subsequent scan cleanly rebinds the same port) via a hand-rolled minimal
# websocket test client (same as integration_tests/scan_dispatcher/'s own -
# deliberately not libwebsockets client mode, see that test's header comment).
#
# Sweeps against "lo" are EXPECTED to fail with IED_DISCOVERY_ERR_SUBNET_TOO_LARGE
# (loopback's real netmask is /8, deterministically exceeding
# IedDiscoveryConfig.maxHosts - see tests/ied_discovery/test_ied_discovery_api.c's
# own proof of this) - tolerated gracefully by the worker's own sweepLoop.
# This test proves sequencing/refcounting/threading, not sweep success -
# same reasoning integration_tests/ied_discovery/ already documents for why
# it doesn't exercise a real scanSubnet over a real interface either.
#
# No sudo, no ied_simulator needed - plain TCP/loopback only, and this
# feature has no external IED to talk to (same reasoning
# integration_tests/scan_dispatcher/ and integration_tests/ipc_dispatcher/
# already document).

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
SCAN_ORCHESTRATION := $(SRC)/scan_orchestration
SCAN_DISPATCHER := $(SRC)/features/scan_dispatcher
IED_DISCOVERY := $(SRC)/features/ied_discovery
SCL_BOOTSTRAP := $(SRC)/features/scl_bootstrap

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lcjson -lwebsockets -lunity -lpthread

BIN := e2e_test_scan_orchestration

.PHONY: all run clean

all: run

$(BIN): e2e_test_scan_orchestration.c \
		$(SCAN_ORCHESTRATION)/domain/scan_orchestration_usecases.c \
		$(SCAN_ORCHESTRATION)/data/scan_orchestration_worker.c \
		$(SCAN_ORCHESTRATION)/data/scan_orchestration_registry.c \
		$(SCAN_ORCHESTRATION)/service/scan_orchestration_api.c \
		$(SCAN_DISPATCHER)/domain/scan_dispatcher_usecases.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_ring_buffer.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_ws_server.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_json_writer.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_adapter.c \
		$(SCAN_DISPATCHER)/service/scan_dispatcher_api.c \
		$(IED_DISCOVERY)/service/ied_discovery_api.c \
		$(IED_DISCOVERY)/domain/ied_discovery_cidr.c \
		$(IED_DISCOVERY)/data/ied_discovery_netif.c \
		$(IED_DISCOVERY)/data/ied_discovery_mms_probe.c \
		$(IED_DISCOVERY)/data/ied_discovery_auth.c \
		$(IED_DISCOVERY)/utils/ied_discovery_utils.c \
		$(SCL_BOOTSTRAP)/service/scl_bootstrap_api.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_tcp_probe.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_mms_session.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_auth.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_file_download.c \
		$(SCL_BOOTSTRAP)/domain/scl_bootstrap_usecases.c \
		$(SCL_BOOTSTRAP)/utils/scl_bootstrap_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
