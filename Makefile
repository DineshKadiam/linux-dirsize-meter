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
	install -vDm 0755 -t $(DESTDIR)$(PREFIX)/bin/ dut
	install -vDm 0644 -t $(DESTDIR)$(PREFIX)/share/man/man1/ dut.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dut
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/dut.1
