CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
TARGET = stillalive
SRCS = stillalive.c stillalive_data.h

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) stillalive.c -o $(TARGET)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
