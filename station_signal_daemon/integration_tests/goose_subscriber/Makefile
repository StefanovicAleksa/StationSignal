# E2E test of the goose_subscriber service API against a real, running IED
# server (integration_tests/ied_simulator/'s "Reporter1" model) publishing a
# live, triggerable GOOSE Control Block. Scoped to this directory only - does
# not imply or replace a root daemon build command. Unity is vendored as a
# prebuilt static lib in third_party/, same as libiec61850/libmxml - linked
# via -lunity.
#
# Links ied_simulator/src/sim_server.c directly (publisher and subscriber run
# in the same test process/binary, over "lo" - libiec61850 supports this) -
# sim_server.c itself has zero src/ includes, so this doesn't compromise the
# simulator's decoupling from production code, only this test binary's
# composition.
#
# REQUIRES CAP_NET_RAW (raw AF_PACKET socket) - unlike every other
# integration test in this repo, `run` must be invoked via sudo:
#   sudo make run

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
IED_MODEL   := $(SRC)/features/ied_model
GOOSE_SUBSCRIBER := $(SRC)/features/goose_subscriber
IED_SIMULATOR := $(ROOT)/integration_tests/ied_simulator

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -I$(IED_SIMULATOR)/src -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lmxml -lunity -lpthread

BIN := e2e_test_goose_subscriber

.PHONY: all run clean

all: run

$(BIN): e2e_test_goose_subscriber.c \
		$(GOOSE_SUBSCRIBER)/service/goose_subscriber_api.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_connection.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_frame_adapter.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/utils/ied_model_ln_category.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c \
		$(IED_SIMULATOR)/src/sim_server.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
