# E2E test of control_dispatcher's real bind + real bidirectional websocket
# traffic: a hand-rolled RFC6455 client (first in this repo to also SEND
# frames, not just receive - see e2e_test_control_dispatcher.c's own header
# comment) drives START_REPORTING/STOP_REPORTING against a real
# DeviceManagerHandle wired to a real "Reporter1" IED simulator instance.
# Scoped to this directory only - does not imply or replace a root daemon
# build command. Unity is vendored as a prebuilt static lib in third_party/,
# same as libiec61850/libmxml - linked via -lunity.
#
# Union of control_dispatcher's own full stack, device_manager's full stack,
# and orchestration's entire stack (same as integration_tests/device_manager/'s
# own Makefile).
#
# Links ied_simulator/src/sim_server.c directly (server and client both run
# in the same test process, over loopback/"lo") - sim_server.c itself has
# zero src/ includes, so this doesn't compromise the simulator's decoupling
# from production code, only this test binary's composition.
#
# REQUIRES CAP_NET_RAW for the START_REPORTING case (inherited transitively
# from the GOOSE subscriber step every DeviceManager_startReporting call
# reaches via orchestration) - `run` must be invoked via sudo:
#   sudo make run
# The malformed-JSON/unknown-action cases need no such privilege.

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
CONTROL_DISPATCHER := $(SRC)/features/control_dispatcher
DEVICE_MANAGER := $(SRC)/device_manager
ORCHESTRATION := $(SRC)/orchestration
SCL_BOOTSTRAP := $(SRC)/features/scl_bootstrap
IED_MODEL   := $(SRC)/features/ied_model
IED_MODEL_ONLINE_LOADER := $(SRC)/features/ied_model_online_loader
MMS_REPORT_CLIENT := $(SRC)/features/mms_report_client
GOOSE_SUBSCRIBER := $(SRC)/features/goose_subscriber
IPC_DISPATCHER := $(SRC)/features/ipc_dispatcher
IED_SIMULATOR := $(ROOT)/integration_tests/ied_simulator

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -I$(IED_SIMULATOR)/src -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lmxml -lcjson -lwebsockets -lunity -lpthread

BIN := e2e_test_control_dispatcher

.PHONY: all run clean

all: run

$(BIN): e2e_test_control_dispatcher.c \
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
		$(IPC_DISPATCHER)/data/ipc_dispatcher_json_writer.c \
		$(IED_SIMULATOR)/src/sim_server.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
