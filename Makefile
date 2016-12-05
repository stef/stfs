CFLAGS+=-Wall -O2

all: stfs afl

afl: afl.o stfs.o

stfs: stfs.o test.o

check: scan-build flawfinder cppcheck

clean:
	rm -f stfs afl *.o

scan-build: clean
	scan-build-3.9 make

flawfinder:
	flawfinder --quiet stfs.c

cppcheck:
	cppcheck --enable=all stfs.c

.PHONY: clean check scan-build flawfinder cppcheck
