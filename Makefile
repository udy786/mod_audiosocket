# Out-of-tree build of mod_audiosocket against an installed FreeSWITCH.
# Needs the FreeSWITCH development files (headers and freeswitch.pc).
#
#   make                          build mod_audiosocket.so
#   sudo make install             copy it into the FreeSWITCH module directory, and put a default
#                                 audiosocket.conf.xml in FreeSWITCH's autoload_configs (an existing
#                                 file is never overwritten; FORCE_CONFIG=1 replaces it)
#   make FS_MODDIR=/path install  override the module directory
#   make FS_CONFDIR=/path install override FreeSWITCH's config directory (the one that holds autoload_configs)
#
# If pkg-config cannot find freeswitch.pc, set PKG_CONFIG_PATH, for example:
#   PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig make

MOD       := mod_audiosocket
CORE_SRC  := src/audiosocket_proto.c src/audiosocket_buf.c src/audiosocket_net.c src/audiosocket_conf.c src/audiosocket_json.c src/audiosocket_util.c
CORE_HDR  := src/audiosocket_proto.h src/audiosocket_buf.h src/audiosocket_net.h src/audiosocket_conf.h src/audiosocket_json.h src/audiosocket_util.h
MOD_SRC   := src/$(MOD).c $(CORE_SRC)

FS_CFLAGS := $(shell pkg-config --cflags freeswitch 2>/dev/null)
FS_MODDIR ?= $(shell pkg-config --variable=modulesdir freeswitch 2>/dev/null)
FS_SYSCONF := $(shell pkg-config --variable=sysconfdir freeswitch 2>/dev/null)
FS_CONFDIR ?= $(if $(FS_SYSCONF),$(FS_SYSCONF)/freeswitch,)
CONF_NAME := audiosocket.conf.xml

CFLAGS    ?= -O2
# Hardening: stack protector, checked copy functions, and format-string warnings. -U first because some
# FreeSWITCH headers and distributions define _FORTIFY_SOURCE already.
HARDEN    := -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security
MOD_FLAGS := -std=gnu99 -fPIC -Wall -Wextra -Wno-unused-parameter $(HARDEN) $(FS_CFLAGS)

# DESTDIR (empty by default) puts the files under another root, for building packages.
DESTDIR   ?=

all: $(MOD).so

$(MOD).so: $(MOD_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) $(MOD_FLAGS) -shared -o $@ $(MOD_SRC) $(LDFLAGS)

install: $(MOD).so
	@test -n "$(FS_MODDIR)" || (echo "FS_MODDIR is empty: set it explicitly" && exit 1)
	install -m 0755 "$(MOD).so" "$(DESTDIR)$(FS_MODDIR)/$(MOD).so"
	@$(MAKE) --no-print-directory install-config

# Put the default settings file in place unless the user already has one. FORCE_CONFIG=1 (anything but
# empty or 0) replaces it, after saving the old one next to it as audiosocket.conf.xml.bak.
CONF_DIR   = $(DESTDIR)$(FS_CONFDIR)/autoload_configs
install-config:
	@if [ -z "$(FS_CONFDIR)" ] || [ ! -d "$(CONF_DIR)" ]; then \
		echo "NOTE: FreeSWITCH's config directory was not found (FS_CONFDIR=$(FS_CONFDIR))."; \
		echo "      Copy examples/$(CONF_NAME) into its autoload_configs folder, or run: make FS_CONFDIR=/path install-config"; \
	elif [ -e "$(CONF_DIR)/$(CONF_NAME)" ] && { [ -z "$(FORCE_CONFIG)" ] || [ "$(FORCE_CONFIG)" = "0" ]; }; then \
		echo "Keeping your existing $(CONF_DIR)/$(CONF_NAME) (FORCE_CONFIG=1 replaces it)"; \
	else \
		if [ -e "$(CONF_DIR)/$(CONF_NAME)" ]; then \
			cp -p "$(CONF_DIR)/$(CONF_NAME)" "$(CONF_DIR)/$(CONF_NAME).bak" && \
			echo "Saved the old file as $(CONF_DIR)/$(CONF_NAME).bak"; \
		fi; \
		install -m 0644 "examples/$(CONF_NAME)" "$(CONF_DIR)/$(CONF_NAME)" && \
		echo "Installed the default settings file $(CONF_DIR)/$(CONF_NAME)"; \
	fi
	@if [ -f "$(CONF_DIR)/modules.conf.xml" ] && ! grep -v '^[[:space:]]*<!--' "$(CONF_DIR)/modules.conf.xml" | grep -q 'load module="$(MOD)"'; then \
		echo "NOTE: to load the module at startup, add <load module=\"$(MOD)\"/> to $(CONF_DIR)/modules.conf.xml"; \
	fi

clean:
	rm -rf $(MOD).so build

.PHONY: all install install-config clean
