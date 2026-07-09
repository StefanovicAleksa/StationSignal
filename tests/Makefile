# Scoped to tests/ only - does not imply or replace a root daemon build command
# (see CLAUDE.md: no root build system exists yet, and this doesn't invent one).
#
# Strictly unit tests: no file I/O beyond one self-contained temp-file case in
# test_ied_model_api (proving IedModel_loadFromFile's own wiring, not the
# loader's real-world parsing breadth - that's covered by the E2E test in
# integration_tests/ instead).
#
# Unity is vendored as a prebuilt static lib in third_party/, same as
# libiec61850/libmxml - linked via -lunity, not compiled from source here.

ROOT        := ..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
IED_MODEL   := $(SRC)/features/ied_model
IED_MODEL_ONLINE_LOADER := $(SRC)/features/ied_model_online_loader
MMS_REPORT_CLIENT := $(SRC)/features/mms_report_client
GOOSE_SUBSCRIBER := $(SRC)/features/goose_subscriber
SCL_BOOTSTRAP := $(SRC)/features/scl_bootstrap
ORCHESTRATION := $(SRC)/orchestration
IPC_DISPATCHER := $(SRC)/features/ipc_dispatcher
IED_DISCOVERY := $(SRC)/features/ied_discovery

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -lunity

TESTS := test_ied_model_utils test_ied_model_usecases test_ied_model_api \
         test_mms_report_client_utils test_mms_report_client_usecases test_mms_report_client_api \
         test_goose_subscriber_utils test_goose_subscriber_usecases test_goose_subscriber_api \
         test_scl_bootstrap_usecases test_scl_bootstrap_utils \
         test_orchestration_staging test_orchestration_usecases test_orchestration_utils test_orchestration_api \
         test_ipc_dispatcher_usecases test_ipc_dispatcher_value_codec test_ipc_dispatcher_ring_buffer \
         test_ipc_dispatcher_json_writer test_ipc_dispatcher_api \
         test_ied_discovery_cidr test_ied_discovery_netif test_ied_discovery_api

.PHONY: all clean run

all: run

# utils.c makes no calls into compiled libiec61850 functions - only mxml + header
# constants - so this binary only needs mxml linked (plus unity, from LDFLAGS).
test_ied_model_utils: ied_model/test_ied_model_utils.c $(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lmxml

# usecases.c builds/reads a real IedModel via the dynamic model API but never
# touches mxml.
test_ied_model_usecases: ied_model/test_ied_model_usecases.c $(IED_MODEL)/domain/ied_model_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# api.c pulls in the real loader (for the one wiring test) plus usecases and
# utils - needs the full stack.
test_ied_model_api: ied_model/test_ied_model_api.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

# utils.c only touches MmsValue construction/cloning - needs iec61850+hal, no mxml.
test_mms_report_client_utils: mms_report_client/test_mms_report_client_utils.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# usecases.c builds MmsReportRecord from real MmsValue instances (public
# constructors) but never opens a socket or touches IedConnection/ClientReport.
test_mms_report_client_usecases: mms_report_client/test_mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# api.c pulls in the full stack for argument-validation-only wiring tests
# (never calls _start() with real report targets, no real connection attempted).
test_mms_report_client_api: mms_report_client/test_mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/service/mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_connection.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_auth.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_report_adapter.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

# utils.c only touches MmsValue construction/cloning - needs iec61850+hal, no mxml.
test_goose_subscriber_utils: goose_subscriber/test_goose_subscriber_utils.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# usecases.c builds GooseSubscriberRecord from real MmsValue instances (public
# constructors) but never opens a socket or touches GooseSubscriber/GooseReceiver.
test_goose_subscriber_usecases: goose_subscriber/test_goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# api.c pulls in the full stack for argument-validation-only wiring tests
# (never calls _start() with real GOOSE targets, no real raw socket opened).
test_goose_subscriber_api: goose_subscriber/test_goose_subscriber_api.c \
		$(GOOSE_SUBSCRIBER)/service/goose_subscriber_api.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_connection.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_frame_adapter.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

# usecases.c is pure logic (extension matching/priority/host-list validation)
# - needs no libiec61850 symbols, just LinkedList (declared in a vendored
# header but implemented entirely in libiec61850.a, hence still -liec61850).
test_scl_bootstrap_usecases: scl_bootstrap/test_scl_bootstrap_usecases.c \
		$(SCL_BOOTSTRAP)/domain/scl_bootstrap_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# utils.c is plain string helpers - no libiec61850 symbols touched at all,
# but scl_bootstrap_types.h (transitively included) pulls in iec61850_client.h
# for IedClientError, so the same link line is needed to resolve LinkedList.
test_scl_bootstrap_utils: scl_bootstrap/test_scl_bootstrap_utils.c \
		$(SCL_BOOTSTRAP)/utils/scl_bootstrap_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# staging.c is pure libc I/O (mkstemp/write/unlink) - deliberately decoupled
# from every LinkedList/IedModelHandle type, so this is the only orchestration
# test that needs neither -liec61850 nor -lhal.
test_orchestration_staging: orchestration/test_orchestration_staging.c \
		$(ORCHESTRATION)/data/orchestration_staging.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# usecases.c is pure logic over SclBootstrapResult/LinkedList - no libiec61850
# symbols touched beyond LinkedList itself, same situation as
# test_scl_bootstrap_usecases (see that rule's comment).
test_orchestration_usecases: orchestration/test_orchestration_usecases.c \
		$(ORCHESTRATION)/domain/orchestration_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# utils.c touches no libiec61850 symbols directly, but orchestration_types.h
# (pulled in transitively via orchestration_api.h in the test file) pulls in
# all four sibling features' _api.h headers - same link line as every other
# feature's utils test.
test_orchestration_utils: orchestration/test_orchestration_utils.c \
		$(ORCHESTRATION)/utils/orchestration_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# api.c pulls in the full stack (all 5 sibling features, fully) for
# argument-validation-only wiring tests - never calls Orchestration_run
# against a real reachable host, no real connection/socket opened (mirrors
# test_goose_subscriber_api/test_mms_report_client_api's own convention).
# Needs ipc_dispatcher's full source set + cjson/lws now too:
# orchestration_api.c calls real IpcDispatcher_create/_start/_stop/_destroy
# (the create-time ring-buffer allocation and stop-on-never-started paths are
# real, hermetic, no socket ever opened by these particular tests - see
# test_ipc_dispatcher_api.c's own rule for why a real bind is fine in a unit
# test here, just never reached by this binary's own test cases).
test_orchestration_api: orchestration/test_orchestration_api.c \
		$(ORCHESTRATION)/service/orchestration_api.c \
		$(ORCHESTRATION)/domain/orchestration_usecases.c \
		$(ORCHESTRATION)/data/orchestration_staging.c \
		$(ORCHESTRATION)/utils/orchestration_utils.c \
		$(SCL_BOOTSTRAP)/service/scl_bootstrap_api.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_tcp_probe.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_mms_session.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_auth.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_file_download.c \
		$(SCL_BOOTSTRAP)/domain/scl_bootstrap_usecases.c \
		$(SCL_BOOTSTRAP)/utils/scl_bootstrap_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c \
		$(IED_MODEL_ONLINE_LOADER)/service/ied_model_online_loader_api.c \
		$(IED_MODEL_ONLINE_LOADER)/data/ied_model_online_loader_connection.c \
		$(IED_MODEL_ONLINE_LOADER)/data/ied_model_online_loader_auth.c \
		$(IED_MODEL_ONLINE_LOADER)/domain/ied_model_online_loader_usecases.c \
		$(MMS_REPORT_CLIENT)/service/mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_connection.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_auth.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_report_adapter.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c \
		$(GOOSE_SUBSCRIBER)/service/goose_subscriber_api.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_connection.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_frame_adapter.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c \
		$(IPC_DISPATCHER)/service/ipc_dispatcher_api.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_ring_buffer.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_ws_server.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_mms_adapter.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_goose_adapter.c \
		$(IPC_DISPATCHER)/domain/ipc_dispatcher_usecases.c \
		$(IPC_DISPATCHER)/utils/ipc_dispatcher_value_codec.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_json_writer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lcjson -lwebsockets -lpthread

# usecases.c is pure logic (reference splitting/pairing/message assembly) -
# zero third-party includes, needs no libiec61850/cjson/lws symbols at all,
# notably lighter than every other feature's usecases test.
test_ipc_dispatcher_usecases: ipc_dispatcher/test_ipc_dispatcher_usecases.c \
		$(IPC_DISPATCHER)/domain/ipc_dispatcher_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# value_codec.c touches MmsValue/Quality directly - needs iec61850+hal, no mxml/cjson/lws.
test_ipc_dispatcher_value_codec: ipc_dispatcher/test_ipc_dispatcher_value_codec.c \
		$(IPC_DISPATCHER)/utils/ipc_dispatcher_value_codec.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# ring_buffer.c only touches hal_thread.h's Semaphore - needs hal+pthread, no libiec61850/mxml/cjson/lws.
test_ipc_dispatcher_ring_buffer: ipc_dispatcher/test_ipc_dispatcher_ring_buffer.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_ring_buffer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lhal -lpthread

# json_writer.c only touches cJSON - needs cjson, no libiec61850/mxml/lws.
test_ipc_dispatcher_json_writer: ipc_dispatcher/test_ipc_dispatcher_json_writer.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_json_writer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lcjson

# api.c pulls in the full stack (real bind/start/stop on loopback for real -
# unlike mms_report_client/goose_subscriber's own api tests, there's no
# external IED dependency to avoid here) plus both sibling features' record
# types for the callback-adapter signatures.
test_ipc_dispatcher_api: ipc_dispatcher/test_ipc_dispatcher_api.c \
		$(IPC_DISPATCHER)/service/ipc_dispatcher_api.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_ring_buffer.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_ws_server.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_mms_adapter.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_goose_adapter.c \
		$(IPC_DISPATCHER)/domain/ipc_dispatcher_usecases.c \
		$(IPC_DISPATCHER)/utils/ipc_dispatcher_value_codec.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_json_writer.c \
		$(MMS_REPORT_CLIENT)/service/mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_connection.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_auth.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_report_adapter.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c \
		$(GOOSE_SUBSCRIBER)/service/goose_subscriber_api.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_connection.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_frame_adapter.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lcjson -lwebsockets -lpthread

# cidr.c is pure IPv4/CIDR arithmetic - no third-party includes at all, but
# needs LinkedList (declared in a vendored header, implemented in
# libiec61850.a), same situation as test_scl_bootstrap_usecases.
test_ied_discovery_cidr: ied_discovery/test_ied_discovery_cidr.c \
		$(IED_DISCOVERY)/domain/ied_discovery_cidr.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# netif.c is plain POSIX getifaddrs()/freeifaddrs() - no libiec61850 symbols
# touched at all, no extra link flags beyond unity itself.
test_ied_discovery_netif: ied_discovery/test_ied_discovery_netif.c \
		$(IED_DISCOVERY)/data/ied_discovery_netif.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# api.c pulls in ied_discovery's full stack plus scl_bootstrap's full source
# set (the TCP-probe delegate created in IedDiscovery_create calls into
# scl_bootstrap_api.c, which as one translation unit references
# SclBootstrapMmsSession_run etc. regardless of runtime reachability - same
# situation as test_orchestration_api's own build rule). Argument-validation-
# only plus the one deterministic real-getifaddrs assertion (loopback's
# maxHosts ceiling) - never a real subnet scan or MMS association, matching
# every other feature's own api-test convention.
test_ied_discovery_api: ied_discovery/test_ied_discovery_api.c \
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
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

run: $(TESTS)
	@status=0; \
	for t in $(TESTS); do \
		echo "=== $$t ==="; \
		./$$t || status=1; \
	done; \
	exit $$status

clean:
	rm -f $(TESTS)
