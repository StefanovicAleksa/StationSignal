# Standalone fake IED ("Reporter1") for manual testing and for other
# integration_tests/<feature>/ E2E tests to link against (e.g.
# integration_tests/mms_report_client/). Fully decoupled from src/ - only
# depends on third_party/ (no mxml needed either: the model is built
# directly via the dynamic model API, not parsed from SCL).

ROOT        := ../..
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib

CC      := gcc
CFLAGS  := -g -Wall -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lpthread

BIN := ied_simulator

.PHONY: all run clean

all: $(BIN)

$(BIN): src/main.c src/sim_server.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
