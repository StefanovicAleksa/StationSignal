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
MMS_DATASET_MANAGER := $(SRC)/features/mms_dataset_manager
GOOSE_SUBSCRIBER := $(SRC)/features/goose_subscriber
SCL_BOOTSTRAP := $(SRC)/features/scl_bootstrap
ORCHESTRATION := $(SRC)/orchestration
IPC_DISPATCHER := $(SRC)/features/ipc_dispatcher
IED_DISCOVERY := $(SRC)/features/ied_discovery
SCAN_DISPATCHER := $(SRC)/features/scan_dispatcher
SCAN_ORCHESTRATION := $(SRC)/scan_orchestration
DEVICE_MANAGER := $(SRC)/device_manager
CONTROL_DISPATCHER := $(SRC)/features/control_dispatcher

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -lunity

TESTS := test_ied_model_utils test_ied_model_usecases test_ied_model_api \
         test_ied_model_online_loader_usecases \
         test_mms_report_client_utils test_mms_report_client_usecases test_mms_report_client_api \
         test_mms_dataset_manager_usecases \
         test_goose_subscriber_utils test_goose_subscriber_usecases test_goose_subscriber_api \
         test_scl_bootstrap_usecases test_scl_bootstrap_utils \
         test_orchestration_staging test_orchestration_usecases test_orchestration_utils test_orchestration_api \
         test_ipc_dispatcher_usecases test_ipc_dispatcher_value_codec test_ipc_dispatcher_ring_buffer \
         test_ipc_dispatcher_json_writer test_ipc_dispatcher_api \
         test_ied_discovery_cidr test_ied_discovery_netif test_ied_discovery_api \
         test_scan_dispatcher_usecases test_scan_dispatcher_json_writer test_scan_dispatcher_ring_buffer \
         test_scan_dispatcher_api \
         test_scan_orchestration_usecases test_scan_orchestration_api \
         test_device_manager_port_allocator test_device_manager_registry test_device_manager_api \
         test_control_dispatcher_ring_buffer test_control_dispatcher_request_queue \
         test_control_dispatcher_json_writer test_control_dispatcher_json_parser \
         test_control_dispatcher_usecases test_control_dispatcher_api

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

# usecases.c (convertAcsiRefToWireRef) is pure string manipulation - no
# third-party includes at all, per its own header doc comment - so this is
# the one test binary in this Makefile needing no third-party lib beyond unity.
test_ied_model_online_loader_usecases: ied_model_online_loader/test_ied_model_online_loader_usecases.c \
		$(IED_MODEL_ONLINE_LOADER)/domain/ied_model_online_loader_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# utils.c only touches MmsValue construction/cloning - needs iec61850+hal, no mxml.
test_mms_report_client_utils: mms_report_client/test_mms_report_client_utils.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# usecases.c builds MmsReportRecord from real MmsValue instances (public
# constructors) but never opens a socket or touches IedConnection/ClientReport.
# Pulls in ied_model too: the Gap-4 decomposition wire-type check calls
# IedModel_dataAttributeTypeMatchesMmsType (see decomposedLeafTypesMatch).
test_mms_report_client_usecases: mms_report_client/test_mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

# api.c pulls in the full stack for argument-validation-only wiring tests
# (never calls _start() with real report targets, no real connection attempted).
test_mms_report_client_api: mms_report_client/test_mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/service/mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_connection.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_auth.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_report_adapter.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c \
		$(MMS_DATASET_MANAGER)/service/mms_dataset_manager_api.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_discovery.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_provisioning.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_naming.c \
		$(MMS_DATASET_MANAGER)/domain/mms_dataset_manager_usecases.c \
		$(MMS_DATASET_MANAGER)/utils/mms_dataset_manager_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

# usecases.c here is pure string manipulation and arithmetic over plain
# reference strings - no IedConnection, no MmsValue, no mxml. Needs LinkedList
# (declared in a vendored header but implemented entirely in libiec61850.a,
# hence still -liec61850), same situation as test_scl_bootstrap_usecases.
test_mms_dataset_manager_usecases: mms_dataset_manager/test_mms_dataset_manager_usecases.c \
		$(MMS_DATASET_MANAGER)/domain/mms_dataset_manager_usecases.c \
		$(MMS_DATASET_MANAGER)/utils/mms_dataset_manager_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# utils.c only touches MmsValue construction/cloning - needs iec61850+hal, no mxml.
test_goose_subscriber_utils: goose_subscriber/test_goose_subscriber_utils.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# usecases.c builds GooseSubscriberRecord from real MmsValue instances (public
# constructors) but never opens a socket or touches GooseSubscriber/GooseReceiver.
# Pulls in ied_model too: the Gap-4 decomposition wire-type check calls
# IedModel_dataAttributeTypeMatchesMmsType (see decomposedLeafTypesMatch).
test_goose_subscriber_usecases: goose_subscriber/test_goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

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
		$(MMS_DATASET_MANAGER)/service/mms_dataset_manager_api.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_discovery.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_provisioning.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_naming.c \
		$(MMS_DATASET_MANAGER)/domain/mms_dataset_manager_usecases.c \
		$(MMS_DATASET_MANAGER)/utils/mms_dataset_manager_utils.c \
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
		$(IPC_DISPATCHER)/data/ipc_dispatcher_dedup_cache.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_conn_state_adapter.c \
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
		$(IPC_DISPATCHER)/data/ipc_dispatcher_dedup_cache.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_conn_state_adapter.c \
		$(IPC_DISPATCHER)/domain/ipc_dispatcher_usecases.c \
		$(IPC_DISPATCHER)/utils/ipc_dispatcher_value_codec.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_json_writer.c \
		$(MMS_REPORT_CLIENT)/service/mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_connection.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_auth.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_report_adapter.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c \
		$(MMS_DATASET_MANAGER)/service/mms_dataset_manager_api.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_discovery.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_provisioning.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_naming.c \
		$(MMS_DATASET_MANAGER)/domain/mms_dataset_manager_usecases.c \
		$(MMS_DATASET_MANAGER)/utils/mms_dataset_manager_utils.c \
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

# netif.c itself is plain POSIX getifaddrs()/freeifaddrs(), but it now calls
# IedDiscoveryCidr_isLinkLocal to decide which of an interface's addresses to
# derive a sweep range from, so domain/ied_discovery_cidr.c comes with it - and
# that file's buildCandidateList pulls in LinkedList (implemented in
# libiec61850.a), so this rule needs the same extra link flags as
# test_ied_discovery_cidr above even though nothing here calls that function.
test_ied_discovery_netif: ied_discovery/test_ied_discovery_netif.c \
		$(IED_DISCOVERY)/data/ied_discovery_netif.c \
		$(IED_DISCOVERY)/domain/ied_discovery_cidr.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

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

# usecases.c is pure logic (deep-copy event assembly) - no libiec61850/cjson
# symbols touched at all, same situation as test_ipc_dispatcher_usecases.
test_scan_dispatcher_usecases: scan_dispatcher/test_scan_dispatcher_usecases.c \
		$(SCAN_DISPATCHER)/domain/scan_dispatcher_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# json_writer.c only touches cJSON - needs cjson, no libiec61850/mxml/lws.
test_scan_dispatcher_json_writer: scan_dispatcher/test_scan_dispatcher_json_writer.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_json_writer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lcjson

# ring_buffer.c only touches hal_thread.h's Semaphore - needs hal+pthread, no libiec61850/mxml/cjson/lws.
test_scan_dispatcher_ring_buffer: scan_dispatcher/test_scan_dispatcher_ring_buffer.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_ring_buffer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lhal -lpthread

# api.c pulls in the full stack (real bind/start/stop on loopback for real,
# same reasoning as test_ipc_dispatcher_api - no external IED dependency to
# avoid here) plus hal (Thread/Semaphore/Hal_getTimeInMs), cjson, lws - no
# libiec61850 symbols touched anywhere in this feature.
test_scan_dispatcher_api: scan_dispatcher/test_scan_dispatcher_api.c \
		$(SCAN_DISPATCHER)/domain/scan_dispatcher_usecases.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_ring_buffer.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_ws_server.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_json_writer.c \
		$(SCAN_DISPATCHER)/data/scan_dispatcher_adapter.c \
		$(SCAN_DISPATCHER)/service/scan_dispatcher_api.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lhal -lcjson -lwebsockets -lpthread

# usecases.c is pure logic (seen-set membership) - no libiec61850/cjson/lws
# symbols touched at all, same situation as test_ipc_dispatcher_usecases.
test_scan_orchestration_usecases: scan_orchestration/test_scan_orchestration_usecases.c \
		$(SCAN_ORCHESTRATION)/domain/scan_orchestration_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# api.c pulls in scan_orchestration's own full stack plus scan_dispatcher's
# full stack (its own shared websocket) plus ied_discovery's full stack (each
# scan's own private IedDiscoveryHandle) plus scl_bootstrap's full source set
# (ied_discovery's TCP-probe delegate references it as one translation unit,
# same situation as test_ied_discovery_api's own build rule) - argument-
# validation-only wiring plus a full start/stop lifecycle against a
# deliberately nonexistent interface (never a real subnet scan or MMS
# association), matching every other feature's own api-test convention.
test_scan_orchestration_api: scan_orchestration/test_scan_orchestration_api.c \
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
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lcjson -lwebsockets -lpthread

# port_allocator.c is pure logic (range + free-list bookkeeping) - no
# libiec61850/hal/cjson/lws symbols touched at all, same situation as
# test_ipc_dispatcher_usecases/test_orchestration_staging.
test_device_manager_port_allocator: device_manager/test_device_manager_port_allocator.c \
		$(DEVICE_MANAGER)/data/device_manager_port_allocator.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# registry.c touches hal_thread.h's Semaphore plus orchestration_utils.c's
# safeStringDup - orchestration_utils.h (pulled in transitively) pulls in
# orchestration_types.h's sibling feature _api.h headers, same link line as
# test_orchestration_utils's own rule.
test_device_manager_registry: device_manager/test_device_manager_registry.c \
		$(DEVICE_MANAGER)/data/device_manager_registry.c \
		$(DEVICE_MANAGER)/data/device_manager_port_allocator.c \
		$(ORCHESTRATION)/utils/orchestration_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# api.c pulls in device_manager's own full stack plus orchestration's entire
# stack (all 5 sibling features, fully - same reasoning as
# test_orchestration_api's own build rule) - argument-validation-only wiring,
# never a real Orchestration_run* against a reachable host.
test_device_manager_api: device_manager/test_device_manager_api.c \
		$(DEVICE_MANAGER)/service/device_manager_api.c \
		$(DEVICE_MANAGER)/data/device_manager_registry.c \
		$(DEVICE_MANAGER)/data/device_manager_port_allocator.c \
		$(DEVICE_MANAGER)/domain/device_manager_bootstrap_policy.c \
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
		$(MMS_DATASET_MANAGER)/service/mms_dataset_manager_api.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_discovery.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_provisioning.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_naming.c \
		$(MMS_DATASET_MANAGER)/domain/mms_dataset_manager_usecases.c \
		$(MMS_DATASET_MANAGER)/utils/mms_dataset_manager_utils.c \
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
		$(IPC_DISPATCHER)/data/ipc_dispatcher_dedup_cache.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_conn_state_adapter.c \
		$(IPC_DISPATCHER)/domain/ipc_dispatcher_usecases.c \
		$(IPC_DISPATCHER)/utils/ipc_dispatcher_value_codec.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_json_writer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lcjson -lwebsockets -lpthread

# ring_buffer.c only touches hal_thread.h's Semaphore, no ControlRequest/
# device_manager types at all - needs hal+pthread, no libiec61850/mxml/
# cjson/lws.
test_control_dispatcher_ring_buffer: control_dispatcher/test_control_dispatcher_ring_buffer.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_ring_buffer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lhal -lpthread

# request_queue.c touches hal_thread.h's Semaphore plus ControlRequest (whose
# own header transitively pulls in device_manager/orchestration's sibling
# feature headers) - same link line as test_orchestration_utils's own rule.
test_control_dispatcher_request_queue: control_dispatcher/test_control_dispatcher_request_queue.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_request_queue.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# json_writer.c only touches cJSON - needs cjson, no libiec61850/mxml/lws.
test_control_dispatcher_json_writer: control_dispatcher/test_control_dispatcher_json_writer.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_json_writer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lcjson

# json_parser.c touches cJSON plus orchestration_utils.c's safeStringDup -
# needs cjson + the same iec61850/hal link line as test_orchestration_utils.
test_control_dispatcher_json_parser: control_dispatcher/test_control_dispatcher_json_parser.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_json_parser.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_request_queue.c \
		$(ORCHESTRATION)/utils/orchestration_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lcjson -lpthread

# usecases.c pulls in device_manager's own full stack plus orchestration's
# entire stack (same reasoning as test_device_manager_api's own build rule),
# plus scan_orchestration's own stack (+ scan_dispatcher + ied_discovery, its
# own dependencies) now that this feature also dispatches START_SCAN/
# STOP_SCAN - dispatch/error-mapping tests only, driven against a real
# DeviceManagerHandle/ScanOrchestrationHandle in deliberately-failing,
# no-network cases.
test_control_dispatcher_usecases: control_dispatcher/test_control_dispatcher_usecases.c \
		$(CONTROL_DISPATCHER)/domain/control_dispatcher_usecases.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_json_writer.c \
		$(DEVICE_MANAGER)/service/device_manager_api.c \
		$(DEVICE_MANAGER)/data/device_manager_registry.c \
		$(DEVICE_MANAGER)/data/device_manager_port_allocator.c \
		$(DEVICE_MANAGER)/domain/device_manager_bootstrap_policy.c \
		$(ORCHESTRATION)/service/orchestration_api.c \
		$(ORCHESTRATION)/domain/orchestration_usecases.c \
		$(ORCHESTRATION)/data/orchestration_staging.c \
		$(ORCHESTRATION)/utils/orchestration_utils.c \
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
		$(MMS_DATASET_MANAGER)/service/mms_dataset_manager_api.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_discovery.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_provisioning.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_naming.c \
		$(MMS_DATASET_MANAGER)/domain/mms_dataset_manager_usecases.c \
		$(MMS_DATASET_MANAGER)/utils/mms_dataset_manager_utils.c \
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
		$(IPC_DISPATCHER)/data/ipc_dispatcher_dedup_cache.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_conn_state_adapter.c \
		$(IPC_DISPATCHER)/domain/ipc_dispatcher_usecases.c \
		$(IPC_DISPATCHER)/utils/ipc_dispatcher_value_codec.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_json_writer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lcjson -lwebsockets -lpthread

# api.c pulls in control_dispatcher's own full stack (now including
# ws_server/worker, hence lws+hal) plus device_manager's full stack plus
# orchestration's entire stack (same reasoning as
# test_control_dispatcher_usecases/test_device_manager_api's own build
# rules) - real bind/start/stop on loopback for real (no external IED
# dependency to avoid here, same as test_ipc_dispatcher_api), never a real
# websocket frame sent (that's integration_tests/control_dispatcher/'s job).
test_control_dispatcher_api: control_dispatcher/test_control_dispatcher_api.c \
		$(CONTROL_DISPATCHER)/service/control_dispatcher_api.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_ring_buffer.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_request_queue.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_json_writer.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_json_parser.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_ws_server.c \
		$(CONTROL_DISPATCHER)/data/control_dispatcher_worker.c \
		$(CONTROL_DISPATCHER)/domain/control_dispatcher_usecases.c \
		$(DEVICE_MANAGER)/service/device_manager_api.c \
		$(DEVICE_MANAGER)/data/device_manager_registry.c \
		$(DEVICE_MANAGER)/data/device_manager_port_allocator.c \
		$(DEVICE_MANAGER)/domain/device_manager_bootstrap_policy.c \
		$(ORCHESTRATION)/service/orchestration_api.c \
		$(ORCHESTRATION)/domain/orchestration_usecases.c \
		$(ORCHESTRATION)/data/orchestration_staging.c \
		$(ORCHESTRATION)/utils/orchestration_utils.c \
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
		$(MMS_DATASET_MANAGER)/service/mms_dataset_manager_api.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_discovery.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_provisioning.c \
		$(MMS_DATASET_MANAGER)/data/mms_dataset_manager_naming.c \
		$(MMS_DATASET_MANAGER)/domain/mms_dataset_manager_usecases.c \
		$(MMS_DATASET_MANAGER)/utils/mms_dataset_manager_utils.c \
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
		$(IPC_DISPATCHER)/data/ipc_dispatcher_dedup_cache.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_conn_state_adapter.c \
		$(IPC_DISPATCHER)/domain/ipc_dispatcher_usecases.c \
		$(IPC_DISPATCHER)/utils/ipc_dispatcher_value_codec.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_json_writer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lcjson -lwebsockets -lpthread

run: $(TESTS)
	@status=0; \
	for t in $(TESTS); do \
		echo "=== $$t ==="; \
		./$$t || status=1; \
	done; \
	exit $$status

clean:
	rm -f $(TESTS)
