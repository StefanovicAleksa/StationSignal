# E2E test of the ipc_dispatcher service API: real bind, real libwebsockets
# service thread, a hand-rolled minimal websocket test client (deliberately
# NOT libwebsockets client mode - see e2e_test_ipc_dispatcher.c's header
# comment), driving hand-built MmsReportRecord/GooseSubscriberRecord
# fixtures through IpcDispatcher_onMmsReport/_onGooseRecord and asserting
# real JSON delivery. No ied_simulator needed - unlike mms_report_client/
# goose_subscriber's own E2E tests, there's no external IED to talk to here,
# only this feature's own transport. Scoped to this directory only - does
# not imply or replace a root daemon build command. Unity is vendored as a
# prebuilt static lib in third_party/, same as libiec61850/libmxml/cJSON/
# libwebsockets - linked via -lunity.

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
IED_MODEL   := $(SRC)/features/ied_model
MMS_REPORT_CLIENT := $(SRC)/features/mms_report_client
GOOSE_SUBSCRIBER := $(SRC)/features/goose_subscriber
IPC_DISPATCHER := $(SRC)/features/ipc_dispatcher

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lmxml -lcjson -lwebsockets -lunity -lpthread

BIN := e2e_test_ipc_dispatcher

.PHONY: all run clean

all: run

$(BIN): e2e_test_ipc_dispatcher.c \
		$(IPC_DISPATCHER)/service/ipc_dispatcher_api.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_ring_buffer.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_ws_server.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_mms_adapter.c \
		$(IPC_DISPATCHER)/data/ipc_dispatcher_goose_adapter.c \
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
		$(GOOSE_SUBSCRIBER)/service/goose_subscriber_api.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_connection.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_frame_adapter.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
