CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2 -g -std=c11
INCLUDES = -I./include -I../include
LDFLAGS = -pthread -lm
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lrt
endif

SERVER_SRCS = server/server.c server/network.c server/sync_engine.c common/utils.c
CLIENT_SRCS = client/client.c common/utils.c
METRICS_SRCS = ipc/shm_metrics.c

SERVER_OBJS = $(SERVER_SRCS:.c=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)
METRICS_OBJS = $(METRICS_SRCS:.c=.o)

.PHONY: all clean

all: server_bin client_bin metrics_reader

server_bin: $(SERVER_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o server_bin $(SERVER_OBJS) $(LDFLAGS)

client_bin: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o client_bin $(CLIENT_OBJS) $(LDFLAGS)

metrics_reader: $(METRICS_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o metrics_reader $(METRICS_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) $(METRICS_OBJS) server_bin client_bin metrics_reader
