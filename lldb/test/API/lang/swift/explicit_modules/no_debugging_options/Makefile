SWIFT_SOURCES := main.swift
SWIFT_ENABLE_EXPLICIT_MODULES := NO
DISABLE_SWIFT_INTERFACE := YES
USE_PRIVATE_MODULE_CACHE := YES
NO_DEBUG_OPTIONS := -Xfrontend -no-serialize-debugging-options
SWIFTFLAGS_EXTRAS = $(NO_DEBUG_OPTIONS) -I$(BUILDDIR)
LD_EXTRAS = -L$(BUILDDIR) -lMapped \
	-Xlinker -needed_library -Xlinker $(BUILDDIR)/hidden/libUnrelated.dylib

all: Mapped Unrelated Collision $(EXE)

include Makefile.rules

ifeq "$(PARTIAL_MODULE_MAP)" "YES"
$(SWIFT_OBJECTS): SWIFTFLAGS += -D PARTIAL_MODULE_MAP \
	-Xfrontend -explicit-swift-module-map-file \
	-Xfrontend "$(BUILDDIR)/partial-module-map.json" \
	-Xcc -fmodule-map-file="$(SRCDIR)/CShim/module.modulemap"
$(SWIFT_OBJECTS): | partial-module-map.json

# The SDK remains implicit; CShim has only a modulemap, without a built PCM.
partial-module-map.json: Mapped
	printf '%s\n' '[{"moduleName":"Mapped","modulePath":"$(BUILDDIR)/Mapped.swiftmodule","isFramework":false},{"moduleName":"CShim","clangModuleMapPath":"$(SRCDIR)/CShim/module.modulemap","isFramework":false,"isBridgingHeaderDependency":false}]' > "$@"
endif

.PHONY: Mapped Unrelated Collision
Mapped:
	"$(MAKE)" MAKE_DSYM=$(MAKE_DSYM) CC=$(CC) SWIFTC=$(SWIFTC) \
		ARCH=$(ARCH) DSYMUTIL=$(DSYMUTIL) \
		VPATH=$(SRCDIR) -I $(SRCDIR) \
		-f $(THIS_FILE_DIR)/Makefile.rules \
		DYLIB_SWIFT_SOURCES=Mapped.swift DYLIB_NAME=Mapped DYLIB_ONLY=YES \
		SWIFT_ENABLE_EXPLICIT_MODULES=NO DISABLE_SWIFT_INTERFACE=YES \
		USE_PRIVATE_MODULE_CACHE=YES \
		SWIFTFLAGS_EXTRAS="$(NO_DEBUG_OPTIONS)" SWIFT_SOURCES= LD_EXTRAS= all

Collision:
	mkdir -p "$(BUILDDIR)/filesystem"
	$(SWIFTC) $(SWIFTFLAGS) -swift-version 5 -parse-as-library -enable-library-evolution \
		-module-name Unrelated -emit-module \
		-emit-module-interface-path $(BUILDDIR)/filesystem/Unrelated.swiftinterface \
		-emit-module-path $(BUILDDIR)/filesystem/Unrelated.swiftmodule \
		$(SRCDIR)/Collision.swift

Unrelated:
	mkdir -p "$(BUILDDIR)/hidden"
	"$(MAKE)" -C "$(BUILDDIR)/hidden" \
		MAKE_DSYM=$(MAKE_DSYM) CC=$(CC) SWIFTC=$(SWIFTC) \
		ARCH=$(ARCH) DSYMUTIL=$(DSYMUTIL) \
		VPATH=$(SRCDIR) -I $(SRCDIR) \
		-f $(THIS_FILE_DIR)/Makefile.rules \
		DYLIB_SWIFT_SOURCES=Unrelated.swift DYLIB_NAME=Unrelated DYLIB_ONLY=YES \
		DYLIB_INSTALL_NAME=@executable_path/hidden/libUnrelated.dylib \
		SWIFT_ENABLE_EXPLICIT_MODULES=NO DISABLE_SWIFT_INTERFACE=YES \
		USE_PRIVATE_MODULE_CACHE=YES \
		SWIFTFLAGS_EXTRAS="$(NO_DEBUG_OPTIONS)" SWIFT_SOURCES= LD_EXTRAS= all
