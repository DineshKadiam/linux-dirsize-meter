# Use this for warnings:
# CFLAGS ?= -O3 -Wall -Wextra -std=c11 -pedantic
CFLAGS ?= -O3
LDFLAGS ?= -pthread -latomic
PREFIX ?= /usr/local

all: dut

dut: main.c
	$(CC) $(CFLAGS) main.c -o dut $(LDFLAGS)

clean:
	rm -f dut

install: all
	install -D dut $(PREFIX)/bin
	install -D dut.1 $(PREFIX)/share/man/man1/

uninstall:
	rm -f $(PREFIX)/bin/dut
	rm -f $(PREFIX)/share/man/man1/dut.1
