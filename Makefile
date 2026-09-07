# synthc - terminal synthesizer
CC      ?= cc
PREFIX  ?= /usr/local

PKGS     = alsa ncursesw
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter \
           $(shell pkg-config --cflags $(PKGS))
LDLIBS   = $(shell pkg-config --libs $(PKGS)) -lpthread -lm

SRC      = src/main.c src/engine.c src/dsp.c src/params.c src/audio.c src/ui.c
OBJ      = $(SRC:.c=.o)
BIN      = synthc

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): src/synth.h

clean:
	rm -f $(OBJ) $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

run: $(BIN)
	./$(BIN)

.PHONY: all clean install run
