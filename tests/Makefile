# Scoped to tests/ only - does not imply or replace a root daemon build command
# (see CLAUDE.md: no root build system exists yet, and this doesn't invent one).
#
# Strictly unit tests: no file I/O beyond one self-contained temp-file case in
# test_ied_model_api (proving IedModel_loadFromFile's own wiring, not the
# loader's real-world parsing breadth - that's covered by the E2E test in
# integration_tests/ instead).
#
# Unity is vendored as a prebuilt static lib in third_party/, same as
# libiec61850/libmxml - linked via -lunity, not compiled from source here.

ROOT        := ..
SRC         := $(ROOT)/src
THIRD_PARTY_INCLUDE := $(ROOT)/third_party/include
THIRD_PARTY_LIB      := $(ROOT)/third_party/lib
IED_MODEL   := $(SRC)/features/ied_model
MMS_REPORT_CLIENT := $(SRC)/features/mms_report_client
GOOSE_SUBSCRIBER := $(SRC)/features/goose_subscriber
SCL_BOOTSTRAP := $(SRC)/features/scl_bootstrap
ORCHESTRATION := $(SRC)/orchestration

CC      := gcc
CFLAGS  := -g -Wall -I$(SRC) -idirafter $(THIRD_PARTY_INCLUDE)
LDFLAGS := -L$(THIRD_PARTY_LIB) -lunity

TESTS := test_ied_model_utils test_ied_model_usecases test_ied_model_api \
         test_mms_report_client_utils test_mms_report_client_usecases test_mms_report_client_api \
         test_goose_subscriber_utils test_goose_subscriber_usecases test_goose_subscriber_api \
         test_scl_bootstrap_usecases test_scl_bootstrap_utils \
         test_orchestration_staging test_orchestration_usecases test_orchestration_utils test_orchestration_api

.PHONY: all clean run

all: run

# utils.c makes no calls into compiled libiec61850 functions - only mxml + header
# constants - so this binary only needs mxml linked (plus unity, from LDFLAGS).
test_ied_model_utils: ied_model/test_ied_model_utils.c $(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lmxml

# usecases.c builds/reads a real IedModel via the dynamic model API but never
# touches mxml.
test_ied_model_usecases: ied_model/test_ied_model_usecases.c $(IED_MODEL)/domain/ied_model_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# api.c pulls in the real loader (for the one wiring test) plus usecases and
# utils - needs the full stack.
test_ied_model_api: ied_model/test_ied_model_api.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

# utils.c only touches MmsValue construction/cloning - needs iec61850+hal, no mxml.
test_mms_report_client_utils: mms_report_client/test_mms_report_client_utils.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# usecases.c builds MmsReportRecord from real MmsValue instances (public
# constructors) but never opens a socket or touches IedConnection/ClientReport.
test_mms_report_client_usecases: mms_report_client/test_mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# api.c pulls in the full stack for argument-validation-only wiring tests
# (never calls _start() with real report targets, no real connection attempted).
test_mms_report_client_api: mms_report_client/test_mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/service/mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_connection.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_report_adapter.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

# utils.c only touches MmsValue construction/cloning - needs iec61850+hal, no mxml.
test_goose_subscriber_utils: goose_subscriber/test_goose_subscriber_utils.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# usecases.c builds GooseSubscriberRecord from real MmsValue instances (public
# constructors) but never opens a socket or touches GooseSubscriber/GooseReceiver.
test_goose_subscriber_usecases: goose_subscriber/test_goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# api.c pulls in the full stack for argument-validation-only wiring tests
# (never calls _start() with real GOOSE targets, no real raw socket opened).
test_goose_subscriber_api: goose_subscriber/test_goose_subscriber_api.c \
		$(GOOSE_SUBSCRIBER)/service/goose_subscriber_api.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_connection.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_frame_adapter.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c \
		$(IED_MODEL)/service/ied_model_api.c \
		$(IED_MODEL)/data/ied_model_scl_loader.c \
		$(IED_MODEL)/domain/ied_model_usecases.c \
		$(IED_MODEL)/utils/ied_model_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

# usecases.c is pure logic (extension matching/priority/host-list validation)
# - needs no libiec61850 symbols, just LinkedList (declared in a vendored
# header but implemented entirely in libiec61850.a, hence still -liec61850).
test_scl_bootstrap_usecases: scl_bootstrap/test_scl_bootstrap_usecases.c \
		$(SCL_BOOTSTRAP)/domain/scl_bootstrap_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# utils.c is plain string helpers - no libiec61850 symbols touched at all,
# but scl_bootstrap_types.h (transitively included) pulls in iec61850_client.h
# for IedClientError, so the same link line is needed to resolve LinkedList.
test_scl_bootstrap_utils: scl_bootstrap/test_scl_bootstrap_utils.c \
		$(SCL_BOOTSTRAP)/utils/scl_bootstrap_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# staging.c is pure libc I/O (mkstemp/write/unlink) - deliberately decoupled
# from every LinkedList/IedModelHandle type, so this is the only orchestration
# test that needs neither -liec61850 nor -lhal.
test_orchestration_staging: orchestration/test_orchestration_staging.c \
		$(ORCHESTRATION)/data/orchestration_staging.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# usecases.c is pure logic over SclBootstrapResult/LinkedList - no libiec61850
# symbols touched beyond LinkedList itself, same situation as
# test_scl_bootstrap_usecases (see that rule's comment).
test_orchestration_usecases: orchestration/test_orchestration_usecases.c \
		$(ORCHESTRATION)/domain/orchestration_usecases.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# utils.c touches no libiec61850 symbols directly, but orchestration_types.h
# (pulled in transitively via orchestration_api.h in the test file) pulls in
# all four sibling features' _api.h headers - same link line as every other
# feature's utils test.
test_orchestration_utils: orchestration/test_orchestration_utils.c \
		$(ORCHESTRATION)/utils/orchestration_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lpthread

# api.c pulls in the full stack (all 4 sibling features, fully) for
# argument-validation-only wiring tests - never calls Orchestration_run
# against a real reachable host, no real connection/socket opened (mirrors
# test_goose_subscriber_api/test_mms_report_client_api's own convention).
test_orchestration_api: orchestration/test_orchestration_api.c \
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
		$(MMS_REPORT_CLIENT)/service/mms_report_client_api.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_connection.c \
		$(MMS_REPORT_CLIENT)/data/mms_report_client_report_adapter.c \
		$(MMS_REPORT_CLIENT)/domain/mms_report_client_usecases.c \
		$(MMS_REPORT_CLIENT)/utils/mms_report_client_utils.c \
		$(GOOSE_SUBSCRIBER)/service/goose_subscriber_api.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_connection.c \
		$(GOOSE_SUBSCRIBER)/data/goose_subscriber_frame_adapter.c \
		$(GOOSE_SUBSCRIBER)/domain/goose_subscriber_usecases.c \
		$(GOOSE_SUBSCRIBER)/utils/goose_subscriber_utils.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -liec61850 -lhal -lmxml -lpthread

run: $(TESTS)
	@status=0; \
	for t in $(TESTS); do \
		echo "=== $$t ==="; \
		./$$t || status=1; \
	done; \
	exit $$status

clean:
	rm -f $(TESTS)
