.PHONY: all run clean

UNAME != uname
LDFLAGS-Linux := -lbsd
LD=gcc

CFLAGS-Alpine := -DALPINE

CFLAGS += ${CFLAGS-${DISTRO}} -I/usr/local/include -I/usr/include
LDFLAGS += ${LDFLAGS-${UNAME}} -L/usr/local/lib -L/usr/lib -ldb

all: itd it

it: it.c
	${LD} -o $@ $^

itd: itd.c
	${LD} -o $@ $^ ${LDFLAGS}

clean:
	rm it itd
