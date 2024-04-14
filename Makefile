.PHONY: all run clean

UNAME != uname
LDFLAGS-Linux := -lbsd
LD=gcc

LIBDB_PATH ?= /usr/lib
CFLAGS-Alpine := -DALPINE
EXE := it

CFLAGS += ${CFLAGS-${DISTRO}} -I/usr/local/include -I/usr/include
LDFLAGS += ${LDFLAGS-${UNAME}} -L/usr/local/lib -L/usr/lib -ldb

all: ${EXE} ${EXE}-echo

${EXE}: ${EXE}.c
	${LD} -o $@ $^ ${LDFLAGS}
	# ${LINK.c} -g -o $@ ${EXE}.c ${LIBFILES-${UNAME}}

${EXE}-echo: ${EXE}-echo.c
	${LD} -o $@ $^ ${LDFLAGS}
	# ${LINK.c} -g -o $@ ${EXE}-echo.c ${LIBFILES-${UNAME}}

run: ${EXE}
	cat data.txt | ./${EXE}

clean:
	rm ${EXE}
