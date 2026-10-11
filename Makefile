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
SERVER_LIB = server/server_core.c server/metrics.c

.PHONY: all test unit clean
CLIENT_SRCS = client/client_core.c

all: server_bin client_bin metrics_reader $(B)/test_unit

client_bin: client/client.c $(CLIENT_SRCS) $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

metrics_reader: ipc/shm_metrics.c $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

server_bin: server/server.c $(SERVER_LIB) $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(B):
	mkdir -p $(B)

$(B)/test_unit: tests/test_unit.c $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

unit: $(B)/test_unit
	./$(B)/test_unit

test: unit

clean:
	rm -rf $(B)
