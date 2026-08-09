# E2E test of the ied_model service API against a real SCL fixture
# (fixtures/breaker1.cid). Scoped to this directory only - does not imply or
# replace a root daemon build command. Unity is vendored as a prebuilt static
# lib in third_party/, same as libiec61850/libmxml - linked via -lunity.

ROOT        := ../..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
IED_MODEL   := $(SRC)/features/ied_model

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -liec61850 -lhal -lmxml -lunity -lpthread

BIN := e2e_test_ied_model

.PHONY: all run clean

all: run

$(BIN): e2e_test_ied_model.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/utils/ied_model_ln_category.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
