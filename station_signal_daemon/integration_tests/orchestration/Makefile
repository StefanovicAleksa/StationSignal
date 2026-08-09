# E2E test of the full orchestration sequence (ipc_dispatcher -> scl_bootstrap
# -> ied_model -> mms_report_client -> goose_subscriber) against a real,
# running IED server (integration_tests/ied_simulator/'s "Reporter1" model,
# with its MMS file services pointed at a real fixture directory AND
# publishing a live, triggerable GOOSE Control Block) over loopback. Scoped
# to this directory only - does not imply or replace a root daemon build
# command. Unity is vendored as a prebuilt static lib in third_party/, same
# as libiec61850/libmxml - linked via -lunity.
#
# Union of goose_subscriber's (raw socket) and scl_bootstrap's (MMS file
# services) link lines, plus every orchestration + ipc_dispatcher .c file.
# ipc_dispatcher is now wired internally by orchestration_api.c itself (see
# that file), so this test observes report/GOOSE data records via a real
# websocket connection to orchestration's own ipc_dispatcher port, not via
# Orchestration_setReportCallback/_setGooseRecordCallback (removed) - see
# e2e_test_orchestration.c's own header comment.
#
# Links ied_simulator/src/sim_server.c directly (server and both clients run
# in the same test process/binary, over loopback/"lo" - libiec61850 supports
# this) - sim_server.c itself has zero src/ includes, so this doesn't
# compromise the simulator's decoupling from production code, only this test
# binary's composition.
#
# REQUIRES CAP_NET_RAW (raw AF_PACKET socket) - inherited transitively from
# the GOOSE subscriber step, same requirement as goose_subscriber's own E2E
# test - `run` must be invoked via sudo:
#   sudo make run

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
ORCHESTRATION := $(SRC)/orchestration
SCL_BOOTSTRAP := $(SRC)/features/scl_bootstrap
IED_MODEL   := $(SRC)/features/ied_model
IED_MODEL_ONLINE_LOADER := $(SRC)/features/ied_model_online_loader
MMS_REPORT_CLIENT := $(SRC)/features/mms_report_client
MMS_DATASET_MANAGER := $(SRC)/features/mms_dataset_manager
GOOSE_SUBSCRIBER := $(SRC)/features/goose_subscriber
IPC_DISPATCHER := $(SRC)/features/ipc_dispatcher
IED_SIMULATOR := $(ROOT)/integration_tests/ied_simulator

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -I$(IED_SIMULATOR)/src -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lmxml -lcjson -lwebsockets -lunity -lpthread

BIN := e2e_test_orchestration

.PHONY: all run clean

all: run

$(BIN): e2e_test_orchestration.c \
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
		$(IPC_DISPATCHER)/data/ipc_dispatcher_json_writer.c \
		$(IED_SIMULATOR)/src/sim_server.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
