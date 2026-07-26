# E2E test of the ied_discovery service API against a real, running IED
# server (integration_tests/ied_simulator/'s "Reporter1" model) over
# loopback. Scoped to this directory only - does not imply or replace a root
# daemon build command. Unity is vendored as a prebuilt static lib in
# third_party/, same as libiec61850/libmxml - linked via -lunity.
#
# Links ied_simulator/src/sim_server.c directly (client and server run in the
# same test process/binary - libiec61850 supports this, it's just two
# independent IedConnection/IedServer objects over loopback) - sim_server.c
# itself has zero src/ includes, so this doesn't compromise the simulator's
# decoupling from production code, only this test binary's composition.
#
# No sudo needed - ied_discovery only ever uses plain TCP (hal_socket.h, via
# scl_bootstrap's delegate) and MMS (IedConnection), same as
# integration_tests/scl_bootstrap/.

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
IED_DISCOVERY := $(SRC)/features/ied_discovery
SCL_BOOTSTRAP := $(SRC)/features/scl_bootstrap
IED_SIMULATOR := $(ROOT)/integration_tests/ied_simulator

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -I$(IED_SIMULATOR)/src -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lunity -lpthread

BIN := e2e_test_ied_discovery

.PHONY: all run clean

all: run

$(BIN): e2e_test_ied_discovery.c \
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
		$(IED_SIMULATOR)/src/sim_server.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
