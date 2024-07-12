# Use this for warnings:
# CFLAGS = -O3 -flto -Wall -Wextra -std=c11 -pedantic
CFLAGS = -O3 -flto
LDFLAGS = -pthread -latomic
PREFIX = /usr/local

all: dut

dut: main.c
	$(CC) $(CFLAGS) main.c -o dut $(LDFLAGS)

clean:
	rm -f dut

install: all
	install -D dut $(PREFIX)/bin

uninstall:
	rm -f $(PREFIX)/bin/dut
