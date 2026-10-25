CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
CFLAGS  += -D_POSIX_C_SOURCE=200809L -DSHADER_DIR=\"$(CURDIR)/shaders\" -Ibuild
PKGS    := wayland-client wayland-egl wayland-cursor egl opengl
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

# GPU selection: if nvidia-smi exists and works, run on the NVIDIA GPU via
# PRIME render offload. Override with `make run NVIDIA=0` (or NVIDIA=1).
NVIDIA  ?= $(shell nvidia-smi -L >/dev/null 2>&1 && echo 1 || echo 0)
NV_EGL  := $(firstword $(wildcard /usr/share/glvnd/egl_vendor.d/*nvidia*.json \
                                  /etc/glvnd/egl_vendor.d/*nvidia*.json))
ifeq ($(NVIDIA),1)
RUN_ENV := __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia \
           $(if $(NV_EGL),__EGL_VENDOR_LIBRARY_FILENAMES=$(NV_EGL))
endif

PROTOS  := xdg-shell xdg-decoration-unstable-v1
PROTO_H := $(PROTOS:%=build/%-client-protocol.h)
PROTO_C := $(PROTOS:%=build/%-protocol.c)
OBJS    := build/main.o $(PROTO_C:.c=.o)

ton618-kn: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

build:
	mkdir -p build

build/%-client-protocol.h: protocol/%.xml | build
	wayland-scanner client-header $< $@

build/%-protocol.c: protocol/%.xml | build
	wayland-scanner private-code $< $@

build/main.o: src/main.c $(PROTO_H) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%-protocol.o: build/%-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: ton618-kn
	@echo "GPU: $(if $(filter 1,$(NVIDIA)),NVIDIA (PRIME offload),default)"
	$(RUN_ENV) ./ton618-kn $(ARGS)

clean:
	rm -rf build ton618-kn

.PHONY: run clean
.SECONDARY:
