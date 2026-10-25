PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

BUILD := build
PROTOCOL_XMLS := \
	protocol/wlr-layer-shell-unstable-v1.xml \
	$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml \
	$(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml \
	$(WAYLAND_PROTOCOLS)/staging/fractional-scale/fractional-scale-v1.xml

PROTOCOL_NAMES := $(basename $(notdir $(PROTOCOL_XMLS)))
PROTOCOL_HEADERS := $(PROTOCOL_NAMES:%=$(BUILD)/%-client-protocol.h)
PROTOCOL_SOURCES := $(PROTOCOL_NAMES:%=$(BUILD)/%-protocol.c)

SOURCES := $(wildcard src/*.c) $(PROTOCOL_SOURCES)
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SOURCES)))

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -I$(BUILD) \
	$(shell $(PKG_CONFIG) --cflags wayland-client cairo)
LDLIBS += $(shell $(PKG_CONFIG) --libs wayland-client cairo) -lm

vpath %.xml protocol $(dir $(PROTOCOL_XMLS))
vpath %.c src $(BUILD)

$(BUILD)/howdy-island: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c $(PROTOCOL_HEADERS) $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%-client-protocol.h: %.xml | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@

$(BUILD)/%-protocol.c: %.xml | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: clean
.SECONDARY:
