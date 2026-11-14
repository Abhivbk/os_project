CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -Iinclude -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS = -pthread -lrt
SRC     = $(wildcard src/*.c)
BIN     = campuscompute

all: $(BIN)
$(BIN): $(SRC) include/common.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

test: $(BIN)
	bash tests/run_tests.sh

clean:
	rm -f $(BIN) logs/*.log
.PHONY: all test clean
