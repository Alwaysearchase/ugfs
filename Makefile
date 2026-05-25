CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -pedantic -Iinclude
TARGET := ugfs
SRCS := src/main.c src/ugfs.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS) include/ugfs.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET).exe filesystem.dat
