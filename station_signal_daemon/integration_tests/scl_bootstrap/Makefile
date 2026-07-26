# E2E test of the scl_bootstrap service API against a real, running IED
# server (integration_tests/ied_simulator/'s "Reporter1" model, with its MMS
# file services pointed at a real fixture directory) over loopback. Scoped to
# this directory only - does not imply or replace a root daemon build
# command. Unity is vendored as a prebuilt static lib in third_party/, same
# as libiec61850/libmxml - linked via -lunity.
#
# Links ied_simulator/src/sim_server.c directly (client and server run in the
# same test process/binary - libiec61850 supports this, it's just two
# independent IedConnection/IedServer objects over loopback) - sim_server.c
# itself has zero src/ includes, so this doesn't compromise the simulator's
# decoupling from production code, only this test binary's composition.
#
# Unlike goose_subscriber's E2E test, this one needs no raw socket / CAP_NET_RAW
# access - scl_bootstrap only ever uses plain TCP (hal_socket.h) and MMS
# (IedConnection), so this runs as a normal user, no sudo required.

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
SCL_BOOTSTRAP := $(SRC)/features/scl_bootstrap
IED_SIMULATOR := $(ROOT)/integration_tests/ied_simulator

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -I$(IED_SIMULATOR)/src -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lmxml -lunity -lpthread

BIN := e2e_test_scl_bootstrap

.PHONY: all run clean

all: run

$(BIN): e2e_test_scl_bootstrap.c \
		$(SCL_BOOTSTRAP)/service/scl_bootstrap_api.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_tcp_probe.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_mms_session.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_auth.c \
		$(SCL_BOOTSTRAP)/data/scl_bootstrap_file_download.c \
		$(SCL_BOOTSTRAP)/domain/scl_bootstrap_usecases.c \
		$(SCL_BOOTSTRAP)/utils/scl_bootstrap_utils.c \
		$(IED_SIMULATOR)/src/sim_server.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
