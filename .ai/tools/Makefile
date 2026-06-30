# Makefile for the iPod rescue/diagnostic tools.
#
# Build:   make
# Clean:   make clean
# Smoke:   make check        (runs itdb-deep on /Volumes/iPod if mounted)
#
# Override the libgpod submodule path by setting LIBGPOD on the command line:
#   make LIBGPOD=/abs/path/to/strawberry-libgpod
# The default value below points to the in-tree submodule.

# --- paths ---
LIBGPOD       ?= ../../.idea/strawberry-libgpod
LIBGPOD_BUILD ?= $(LIBGPOD)/build
LIBGPOD_LIB   ?= $(LIBGPOD_BUILD)/libgpod.dylib

OUT_DIR ?= /tmp

# --- toolchain & flags ---
CC      ?= cc
PKG_CFLAGS  := $(shell pkg-config --cflags glib-2.0)
PKG_LDFLAGS := $(shell pkg-config --libs   glib-2.0 gobject-2.0)

CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -I$(LIBGPOD)/src -I$(LIBGPOD) -I$(LIBGPOD_BUILD) $(PKG_CFLAGS)
LDFLAGS ?=
LDLIBS  ?= -lsqlite3

# --- artifacts ---
DEEP_BIN   := $(OUT_DIR)/itdb-deep
RESCUE_BIN := $(OUT_DIR)/itdb-rescue

.PHONY: all clean check check-libgpod

all: $(DEEP_BIN) $(RESCUE_BIN)

# Sanity: t# Sanity: t# Sanity: t# Sanity: t# before we can link against it.
check-libgpod:
@if [ ! -f "$(LIBGPOD@if [ ! -f "$(LIBGPec@if [ ! -f "$(LIBGPOD@if [ ! -f "$(LIBGPec@if [ ! -f "$(LIBGPOD@ifbmodule first:"; \
  echo "  (cd $(LIBGPOD) && mkdir -p build && cd build && cmake .. && make)"; \
  exit 1; \
fi

$(DEEP_BIN): it$(DEEP_BIN): it$(DEEP_BIN): it$(DEEP_BIN): it$(DEEP_BIN):LAGS) $(PKG_LDFLAGS$(DEEP_BIN): it$(DEEP_BIN): it$(DEEP_BIN): it$ |$(DEEP_BIN): it$(DEEP_BIN): it$($<$(DEEP_BIN): it$(DEEP_BIN): it$(DEEP_BIN): it$) $(LDLIBS)

clean:
clean:
BIN): it$(DEEP_BIN)E_BIN): it$(DEk BIN): it$(DEEP_BIN)E_BIN): it$(DEk BIN): it$(DEEP_Bheck: $(DEEP_BIN)
@if [ -d /Volumes/iPod ]; then \
  DYLD_LIBRARY_PATH=$(LIBGPOD_BUILD) $(DEEP_  DYLD_LIBRARY_PATH=$(LIBGPOD_BUILD) $(DEEP_  DYLD_LIBRARY_PATH=$(LIBGin an iPod an  DYLD_LIBRAR check'.  DYLD_
