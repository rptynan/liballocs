THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
SRCROOT := $(realpath $(dir $(THIS_MAKEFILE))/..)

default: all

SYMLINKS := liballocs.so liballocs_preload.a liballocs_preload.so liballocs_dummyweaks.o liballocs_dummyweaks.so

all: $(SYMLINKS) liballocs_systrap.o

$(SYMLINKS): %: $(SRCROOT)/src/%
	ln -sf "$<" "$@"

liballocs_systrap.o: $(SRCROOT)/src/systrap.o
	ln -sf "$<" "$@"

clean:
	rm -f $(SYMLINKS) liballocs_systrap.o
