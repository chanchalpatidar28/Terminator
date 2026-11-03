CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -pthread
LDLIBS  := -lX11 -pthread
SRC     := src/main.c src/shared.c src/keymap.c src/engine.c src/script.c src/gui.c
OBJ     := $(SRC:.c=.o)

all: terminator

terminator: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/common.h src/shell.h
	$(CC) $(CFLAGS) -c $< -o $@

test: terminator
	sh tests/run_tests.sh

clean:
	rm -f $(OBJ) terminator

.PHONY: all clean test
