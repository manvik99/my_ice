CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -g -fno-omit-frame-pointer

TARGET = my_ice
SRCS = main.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
