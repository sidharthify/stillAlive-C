CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
TARGET = stillalive
SRCS = stillalive.c stillalive_data.h

ifeq ($(OS),Windows_NT)
    TARGET_BIN = $(TARGET).exe
    LIBS ?= -lwinmm
    RM = del /f /q 2>nul || rm -f
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        CFLAGS += -D_DARWIN_C_SOURCE
    endif
    TARGET_BIN = $(TARGET)
    LIBS ?=
    RM = rm -f
endif

EMCC ?= emcc
WEB_DIR = build/web
EMFLAGS ?= -O2 -Wall -Wextra -sASYNCIFY -sEXIT_RUNTIME=1 -sENVIRONMENT=web

all: $(TARGET_BIN)

$(TARGET_BIN): $(SRCS)
	$(CC) $(CFLAGS) stillalive.c $(LIBS) -o $(TARGET_BIN)

clean:
	-@$(RM) $(TARGET_BIN) 2>nul || rm -f $(TARGET_BIN) 2>/dev/null || true
	-@rm -rf build 2>/dev/null || true

run: $(TARGET_BIN)
	./$(TARGET_BIN)

# browser build: emscripten + xterm.js, output is a static site in build/web
web: $(WEB_DIR)/stillalive.js

$(WEB_DIR)/stillalive.js: $(SRCS) web/index.html res/song/stillalive.mp3
	mkdir -p $(WEB_DIR)/res/song
	$(EMCC) $(EMFLAGS) stillalive.c -o $(WEB_DIR)/stillalive.js
	cp web/index.html $(WEB_DIR)/index.html
	cp res/song/stillalive.mp3 $(WEB_DIR)/res/song/stillalive.mp3

.PHONY: all clean run web
