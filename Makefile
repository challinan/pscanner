CFLAGS += -Wall -ggdb

# This will install into the user's ~/bin if it exists
# or into $INSTALLDIR if not, obviously requiring sudo
INSTALLDIR=/usr/bin

ifeq ($(shell ls -d ${HOME}/bin 2>/dev/null),${HOME}/bin)
    INSTALLDIR=${HOME}/bin
endif

pscanner: pscanner.c

clean:
	rm -f pscanner pscanner.o

install:
	install pscanner ${INSTALLDIR}
