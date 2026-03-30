CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c99 -D_DEFAULT_SOURCE
LIBS    = -lncurses
TARGET  = system_monitor

all: $(TARGET)

$(TARGET): system_monitor.c
	$(CC) $(CFLAGS) -o $(TARGET) system_monitor.c $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
