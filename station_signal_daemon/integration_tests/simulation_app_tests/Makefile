# Standalone multi-instance fake IED app, for manual frontend/API scan+connect
# testing (see run_simulated_ieds.sh at the repo root) - not part of any
# automated test suite. Adapted from ../ied_simulator/Makefile; that original
# stays untouched since other integration_tests/<feature>/ E2E Makefiles link
# its sim_server.c directly. Fully decoupled from src/ - only depends on
# third_party/ (no mxml needed either: the model is built directly via the
# dynamic model API, not parsed from SCL).

ROOT        := ../..
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib

CC      := gcc
CFLAGS  := -g -Wall -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lpthread

BIN := ied_sim_app

.PHONY: all clean

all: $(BIN)

$(BIN): src/main.c src/sim_server.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(BIN)
