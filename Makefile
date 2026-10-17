CC      ?= cc
OPT     ?= -O2
CFLAGS  ?= -Wall -Wextra -Werror -g $(OPT) -std=gnu11 -pthread
LDLIBS  = -lm -pthread
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LDLIBS += -lrt
endif
B = build

COMMON = common/protocol.c common/stats.c common/ntp.c common/utils.c
SERVER_LIB = server/server_core.c server/metrics.c server/poller.c server/drift.c server/exposition.c server/config.c

.PHONY: all test unit integration asan ubsan tsan clean
CLIENT_SRCS = client/client_core.c

all: server_bin client_bin metrics_reader loadgen healthcheck $(B)/test_unit $(B)/test_integration

client_bin: client/client.c $(CLIENT_SRCS) $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

loadgen: tools/loadgen.c $(CLIENT_SRCS) $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

metrics_reader: ipc/shm_metrics.c $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

server_bin: server/server.c $(SERVER_LIB) $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

healthcheck: tools/healthcheck.c | $(B)
	$(CC) $(CFLAGS) -o $@ $^

$(B):
	mkdir -p $(B)

UNIT_EXTRA = server/config.c server/drift.c
$(B)/test_unit: tests/test_unit.c $(UNIT_EXTRA) $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(B)/test_integration: tests/test_integration.c $(SERVER_LIB) $(CLIENT_SRCS) $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

integration: $(B)/test_integration
	./$(B)/test_integration auto
	./$(B)/test_integration poll

unit: $(B)/test_unit
	./$(B)/test_unit

test: unit integration

# Sanitizer builds of both test binaries (the integration test runs the server in a
# thread of the same process, so TSAN sees both sides).
SAN_SRCS_U = tests/test_unit.c $(UNIT_EXTRA) $(COMMON)
SAN_SRCS_I = tests/test_integration.c $(SERVER_LIB) $(CLIENT_SRCS) $(COMMON)
asan ubsan tsan: | $(B)
	$(CC) $(CFLAGS) $(SAN_$@) -o $(B)/unit_$@ $(SAN_SRCS_U) $(LDLIBS)
	$(CC) $(CFLAGS) $(SAN_$@) -o $(B)/integration_$@ $(SAN_SRCS_I) $(LDLIBS)
	./$(B)/unit_$@
	./$(B)/integration_$@ auto
	./$(B)/integration_$@ poll

SAN_asan = -fsanitize=address -fno-omit-frame-pointer
SAN_ubsan = -fsanitize=undefined -fno-sanitize-recover=undefined
SAN_tsan = -fsanitize=thread

clean:
	rm -rf $(B) server_bin client_bin metrics_reader loadgen healthcheck
