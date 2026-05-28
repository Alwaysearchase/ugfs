CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -pedantic -Iinclude
TARGET := ugfs
SRCS := src/main.c \
	src/runtime.c \
	src/disk.c \
	src/inode.c \
	src/alloc.c \
	src/directory.c \
	src/users.c \
	src/format.c \
	src/commands.c \
	src/file_ops.c \
	src/shell.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS) include/ugfs.h src/ugfs_internal.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET).exe filesystem.dat
