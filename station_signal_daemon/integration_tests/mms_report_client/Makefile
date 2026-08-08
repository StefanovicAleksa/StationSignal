# E2E test of the mms_report_client service API against a real, running IED
# server (integration_tests/ied_simulator/'s "Reporter1" model) with a live,
# triggerable buffered Report Control Block. Scoped to this directory only -
# does not imply or replace a root daemon build command. Unity is vendored as
# a prebuilt static lib in third_party/, same as libiec61850/libmxml - linked
# via -lunity.
#
# Links ied_simulator/src/sim_server.c directly (client and server run in the
# same test process/binary - libiec61850 supports this, it's just two
# independent IedConnection/IedServer objects over loopback) - sim_server.c
# itself has zero src/ includes, so this doesn't compromise the simulator's
# decoupling from production code, only this test binary's composition.

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
IED_MODEL   := $(SRC)/features/ied_model
MMS_REPORT_CLIENT := $(SRC)/features/mms_report_client
MMS_DATASET_MANAGER := $(SRC)/features/mms_dataset_manager
IED_SIMULATOR := $(ROOT)/integration_tests/ied_simulator

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -I$(IED_SIMULATOR)/src -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lmxml -lunity -lpthread

BIN := e2e_test_mms_report_client

.PHONY: all run clean

all: run

$(BIN): e2e_test_mms_report_client.c \
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
		$(IED_MODEL)/utils/ied_model_utils.c \
		$(IED_SIMULATOR)/src/sim_server.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
