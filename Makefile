CFLAGS+=-Wall

all: stfs afl

afl: afl.o stfs.o

stfs: stfs.c
	cc -DSTFS_INLINE_TESTS -Wall -o stfs stfs.c

check: scan-build flawfinder cppcheck

clean:
	rm stfs stfs.o afl

scan-build: clean
	scan-build-3.9 make

flawfinder:
	flawfinder --quiet stfs.c

cppcheck:
	cppcheck --enable=all stfs.c

.PHONY: clean check scan-build flawfinder cppcheck
