CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE -pthread
TARGET = cdma_udp_sim

SRCS = main.c channel.c station.c cdma.c framing.c calc_crc.c udp.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
