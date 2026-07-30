# Makefile for s1mple

CC      = gcc
CFLAGS  = -Wall -Wextra -Iinclude -pthread
SRCS    = src/main.c src/http.c src/router.c src/process.c src/registry.c src/proxy.c src/logger.c \
<<<<<<< Updated upstream
		  src/security.c src/config.c src/static.c
=======
<<<<<<< Updated upstream
		  src/security.c src/config.c
=======
		  src/security.c src/config.c src/static.c src/event.c
>>>>>>> Stashed changes
>>>>>>> Stashed changes
TARGET  = s1mple

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean