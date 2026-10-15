SWIFT_SOURCES := main.swift
SWIFT_ENABLE_EXPLICIT_MODULES := NO
SWIFTFLAGS_EXTRAS := -I. -Xfrontend -no-serialize-debugging-options
LD_EXTRAS = -L. -Xlinker -rpath -Xlinker $(BUILDDIR) -lReflectionProbe

all: dylib $(EXE)
.PHONY: dylib

include Makefile.rules

dylib: ReflectionProbe.swift
	"$(MAKE)" -f $(MAKEFILE_RULES) -C $(BUILDDIR) VPATH=$(SRCDIR) \
		CC=$(CC) SWIFTC=$(SWIFTC) ARCH=$(ARCH) DSYMUTIL=$(DSYMUTIL) \
		MAKE_DSYM=YES DYLIB_ONLY=YES DYLIB_NAME=ReflectionProbe \
		SWIFT_ENABLE_EXPLICIT_MODULES=NO \
		DYLIB_SWIFT_SOURCES=ReflectionProbe.swift \
		SWIFTFLAGS_EXTRAS="-enable-library-evolution -I$(BUILDDIR) -Xfrontend -no-serialize-debugging-options"
