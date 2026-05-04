# Makefile for s1mple

CC      = gcc
CFLAGS  = -Wall -Wextra -Iinclude -pthread
SRCS    = src/main.c src/http.c src/router.c src/process.c
TARGET  = s1mple

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean