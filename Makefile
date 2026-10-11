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

.PHONY: all test unit clean
all: $(B)/test_unit

$(B):
	mkdir -p $(B)

$(B)/test_unit: tests/test_unit.c $(COMMON) | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

unit: $(B)/test_unit
	./$(B)/test_unit

test: unit

clean:
	rm -rf $(B)
