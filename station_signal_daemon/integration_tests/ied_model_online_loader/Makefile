# E2E test of ied_model_online_loader against a real, running IED server
# (integration_tests/ied_simulator/'s "Reporter1" model, with its MMS file
# services DISABLED - SimServer_create(false)) over loopback. Scoped to this
# directory only - does not imply or replace a root daemon build command.
#
# Proves both halves of the scenario this feature exists for: (1) scl_bootstrap
# genuinely cannot fetch an SCL from this server (regression-proofs the
# precondition), (2) ied_model_online_loader still builds a fully usable
# model directly from the same live server's MMS data model instead.
#
# Links ied_simulator/src/sim_server.c directly (client and server run in the
# same test process/binary over loopback) - sim_server.c itself has zero src/
# includes, so this doesn't compromise the simulator's decoupling from
# production code, only this test binary's composition.
#
# No sudo needed - MMS/TCP only, same as scl_bootstrap's own E2E test.

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
SCL_BOOTSTRAP := $(SRC)/features/scl_bootstrap
IED_MODEL   := $(SRC)/features/ied_model
IED_MODEL_ONLINE_LOADER := $(SRC)/features/ied_model_online_loader
IED_SIMULATOR := $(ROOT)/integration_tests/ied_simulator

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -I$(IED_SIMULATOR)/src -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lmxml -lunity -lpthread

BIN := e2e_test_ied_model_online_loader

.PHONY: all run clean

all: run

$(BIN): e2e_test_ied_model_online_loader.c \
		$(SCL_BOOTSTRAP)/service/scl_bootstrap_api.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_tcp_probe.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_mms_session.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_auth.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_file_download.c \
		$(SCL_BOOTSTRAP)/domain/scl_bootstrap_usecases.c \
		$(SCL_BOOTSTRAP)/utils/scl_bootstrap_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/utils/ied_model_ln_category.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c \
		$(IED_MODEL_ONLINE_LOADER)/service/ied_model_online_loader_api.c \
		$(IED_MODEL_ONLINE_LOADER)/data/ied_model_online_loader_connection.c \
		$(IED_MODEL_ONLINE_LOADER)/data/ied_model_online_loader_auth.c \
		$(IED_MODEL_ONLINE_LOADER)/domain/ied_model_online_loader_usecases.c \
		$(IED_SIMULATOR)/src/sim_server.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
