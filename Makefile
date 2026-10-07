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

all: $(TARGET_BIN)

$(TARGET_BIN): $(SRCS)
	$(CC) $(CFLAGS) stillalive.c $(LIBS) -o $(TARGET_BIN)

clean:
	-@$(RM) $(TARGET_BIN) 2>nul || rm -f $(TARGET_BIN) 2>/dev/null || true

run: $(TARGET_BIN)
	./$(TARGET_BIN)

.PHONY: all clean run
