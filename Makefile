# r2fuse - expose radare2 filesystems (r_fs) as FUSE volumes

PREFIX?=/usr/local
BINDIR?=$(PREFIX)/bin
PKG_CONFIG?=pkg-config
FUSE_PC?=fuse3
CC?=cc

# on macOS prefer the Fusetta (FSKit) build of libfuse3 when installed with
# `brew install hsorbo/tap/libfuse`, falling back to whatever provides fuse3.pc
FUSETTA_PC?=$(firstword $(wildcard /opt/homebrew/opt/libfuse/lib/pkgconfig /usr/local/opt/libfuse/lib/pkgconfig))
ifneq ($(FUSETTA_PC),)
export PKG_CONFIG_PATH:=$(FUSETTA_PC):$(PKG_CONFIG_PATH)
endif
CFLAGS?=-O2 -g
CFLAGS+=-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -std=gnu11
CFLAGS+=$(shell $(PKG_CONFIG) --cflags r_fs r_io $(FUSE_PC))
ifeq ($(STATIC),1)
# self-contained binary: link the static archives of r2 and libfuse when they
# exist (fully static with -static on linux/musl)
libdir_of=$(firstword $(shell $(PKG_CONFIG) --variable=libdir $(1)))
static_libs=$(foreach l,$(patsubst -l%,%,$(filter -l%,$(shell $(PKG_CONFIG) --static --libs $(1)))),\
	$(if $(wildcard $(call libdir_of,$(1))/lib$(l).a),$(call libdir_of,$(1))/lib$(l).a,-l$(l)))
LDFLAGS+=$(call static_libs,r_fs r_io) $(call static_libs,$(FUSE_PC))
LDFLAGS+=$(filter-out -l%,$(shell $(PKG_CONFIG) --static --libs r_fs r_io $(FUSE_PC)))
ifneq ($(shell uname),Darwin)
LDFLAGS+=-static
endif
else
LDFLAGS+=$(shell $(PKG_CONFIG) --libs r_fs r_io $(FUSE_PC))
endif
ifeq ($(shell uname),Darwin)
# keep the libfuse found at build time, even if another one is in the default path
LDFLAGS+=-Wl,-rpath,$(shell $(PKG_CONFIG) --variable=libdir $(FUSE_PC))
endif

SRCS=$(wildcard src/*.c)
OBJS=$(SRCS:.c=.o)
BIN=r2fuse

# directory with filesystem images, defaults to the radare2-testbins checkout
TESTBINS?=../radare2/test/bins/fs

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

src/%.o: src/%.c src/rfs.h
	$(CC) $(CFLAGS) -c -o $@ $<

# self-contained binary, builds radare2 and libfuse from source (see dist/static.sh)
static:
	./dist/static.sh

check test: $(BIN)
	./test/run.sh $(TESTBINS)

install: $(BIN)
	mkdir -p $(DESTDIR)$(BINDIR)
	cp -f $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(BIN) $(BIN).exe $(OBJS)

.PHONY: all static check test install uninstall clean
