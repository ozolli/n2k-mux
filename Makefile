# Makefile — n2k-mux
# Cibles utiles :
#   make            -> construit tout ce qui est disponible
#   make test_jsonl -> construit le testeur du parser JSON
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
LDFLAGS ?=

PREFIX  ?= /usr/local
DESTDIR ?=

SRCDIR  := src
BUILD   := build

# Modules livrés au fur et à mesure
JSONL_OBJ    := $(BUILD)/jsonl.o
REGISTRY_OBJ := $(BUILD)/registry.o
NMEA_OBJ     := $(BUILD)/nmea0183.o
CONFIG_OBJ   := $(BUILD)/config.o
ARBITER_OBJ  := $(BUILD)/arbiter.o
MAPPER_OBJ   := $(BUILD)/mapper.o
AISDEDUP_OBJ := $(BUILD)/aisdedup.o
SOURCES_OBJ  := $(BUILD)/sources.o

DAEMON_OBJ   := $(BUILD)/daemon.o

# Tous les objets du pipeline (hors testeurs)
CORE_OBJ := $(JSONL_OBJ) $(REGISTRY_OBJ) $(CONFIG_OBJ) $(ARBITER_OBJ) $(NMEA_OBJ) $(MAPPER_OBJ) $(AISDEDUP_OBJ) $(SOURCES_OBJ)

# GTK pour la GUI (cible séparée : make n2k-mux-gui)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

.PHONY: all clean install uninstall
all: n2k-mux test_jsonl test_registry test_nmea0183 test_config test_arbiter test_mapper test_aisdedup test_sources

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# --- Module (a) : parser jsonl + son testeur ---
test_jsonl: $(JSONL_OBJ) $(BUILD)/test_jsonl.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module (b) : registre src->identité + son testeur ---
test_registry: $(JSONL_OBJ) $(REGISTRY_OBJ) $(BUILD)/test_registry.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module (c) : générateur NMEA 0183 + son testeur ---
test_nmea0183: $(NMEA_OBJ) $(BUILD)/test_nmea0183.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# --- Module (d) : config INI + son testeur ---
test_config: $(CONFIG_OBJ) $(BUILD)/test_config.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module (e) : arbitre (sélection) + son testeur ---
test_arbiter: $(JSONL_OBJ) $(REGISTRY_OBJ) $(CONFIG_OBJ) $(ARBITER_OBJ) $(BUILD)/test_arbiter.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module (e) 2e passe : mapper PGN→0183 + son testeur ---
test_mapper: $(JSONL_OBJ) $(REGISTRY_OBJ) $(CONFIG_OBJ) $(ARBITER_OBJ) $(NMEA_OBJ) $(MAPPER_OBJ) $(BUILD)/test_mapper.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# --- Module AIS : dédup/fusion par MMSI + son testeur ---
test_aisdedup: $(JSONL_OBJ) $(REGISTRY_OBJ) $(CONFIG_OBJ) $(AISDEDUP_OBJ) $(BUILD)/test_aisdedup.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module sources (pont daemon→GUI) + son testeur ---
test_sources: $(JSONL_OBJ) $(REGISTRY_OBJ) $(SOURCES_OBJ) $(BUILD)/test_sources.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module (f) : daemon (binaire final) ---
n2k-mux: $(CORE_OBJ) $(DAEMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# --- Module (g) : GUI GTK3 (cible séparée, nécessite libgtk-3-dev) ---
$(BUILD)/gui.o: $(SRCDIR)/gui.c | $(BUILD)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -c $< -o $@

n2k-mux-gui: $(CONFIG_OBJ) $(SOURCES_OBJ) $(BUILD)/gui.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(GTK_LIBS)

# --- Installation système (daemon + service systemd) ---
# make install            installe le daemon, le service et les exemples
# make install GUI=1      installe aussi la GUI (doit être construite : make n2k-mux-gui)
install: n2k-mux
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m755 n2k-mux $(DESTDIR)$(PREFIX)/bin/n2k-mux
	install -Dm644 n2k-mux.service $(DESTDIR)/etc/systemd/system/n2k-mux.service
	install -Dm644 n2k-mux.ini.example $(DESTDIR)/etc/n2k-mux/n2k-mux.ini.example
	install -Dm644 kplex.conf.example $(DESTDIR)/etc/n2k-mux/kplex.conf.example
	install -Dm644 n2k-mux.env.example $(DESTDIR)/etc/default/n2k-mux.example
ifeq ($(GUI),1)
	install -m755 n2k-mux-gui $(DESTDIR)$(PREFIX)/bin/n2k-mux-gui
endif
	@echo "Installé. Pense à : cp /etc/n2k-mux/n2k-mux.ini.example /etc/n2k-mux/n2k-mux.ini"
	@echo "puis : systemctl daemon-reload && systemctl enable --now n2k-mux"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/n2k-mux $(DESTDIR)$(PREFIX)/bin/n2k-mux-gui
	rm -f $(DESTDIR)/etc/systemd/system/n2k-mux.service
	rm -f $(DESTDIR)/etc/default/n2k-mux.example
	rm -f $(DESTDIR)/etc/n2k-mux/n2k-mux.ini.example

clean:
	rm -rf $(BUILD) n2k-mux n2k-mux-gui test_jsonl test_registry test_nmea0183 test_config test_arbiter test_mapper test_aisdedup test_sources
