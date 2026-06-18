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
STATS_OBJ    := $(BUILD)/stats.o
# netout : plomberie TCP fan-out, testée et prête pour le futur flux N2K arbité ;
# pas encore liée au daemon (cf. test_netout).
NETOUT_OBJ   := $(BUILD)/netout.o
# ydraw : formateur YDRAW (Yacht Devices RAW text) pour le N2K réseau vers qtVlm ;
# testé, pas encore lié au daemon (cf. test_ydraw).
YDRAW_OBJ    := $(BUILD)/ydraw.o

DAEMON_OBJ   := $(BUILD)/daemon.o

# Tous les objets du pipeline (hors testeurs)
CORE_OBJ := $(JSONL_OBJ) $(REGISTRY_OBJ) $(CONFIG_OBJ) $(ARBITER_OBJ) $(NMEA_OBJ) $(MAPPER_OBJ) $(AISDEDUP_OBJ) $(SOURCES_OBJ) $(STATS_OBJ)

# GTK pour la GUI (cible séparée : make n2k-mux-gui)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

.PHONY: all clean install uninstall
all: n2k-mux n2k-sim ydraw-bridge test_jsonl test_registry test_nmea0183 test_config test_arbiter test_mapper test_aisdedup test_sources test_stats test_netout test_ydraw

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

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

# --- Module stats (débit / charge de bus estimée) + son testeur ---
test_stats: $(STATS_OBJ) $(BUILD)/test_stats.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module netout (serveur TCP de diffusion) + son testeur ---
test_netout: $(NETOUT_OBJ) $(BUILD)/test_netout.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module ydraw (formateur YDRAW pour le N2K réseau) + son testeur ---
test_ydraw: $(YDRAW_OBJ) $(BUILD)/test_ydraw.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module (f) : daemon (binaire final) ---
n2k-mux: $(CORE_OBJ) $(DAEMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# --- Outil de test : pont canboat/actisense → YDRAW → TCP (pour qtVlm N2K) ---
ydraw-bridge: $(YDRAW_OBJ) $(NETOUT_OBJ) $(BUILD)/ydraw_bridge.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Outil de test : simulateur de flux N2K (JSON-lines) pour tous les PGN ---
n2k-sim: $(BUILD)/simulator.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# --- Module (g) : GUI GTK3 (cible séparée, nécessite libgtk-3-dev) ---
$(BUILD)/gui.o: $(SRCDIR)/gui.c | $(BUILD)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -MMD -MP -c $< -o $@

n2k-mux-gui: $(CONFIG_OBJ) $(SOURCES_OBJ) $(BUILD)/gui.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(GTK_LIBS)

# --- Installation système (daemon + service systemd) ---
# make install            installe le daemon, le service et les exemples
# make install GUI=1      installe aussi la GUI (doit être construite : make n2k-mux-gui)
install: n2k-mux
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m755 n2k-mux $(DESTDIR)$(PREFIX)/bin/n2k-mux
	install -m755 n2k-mux-run $(DESTDIR)$(PREFIX)/bin/n2k-mux-run
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
	rm -f $(DESTDIR)$(PREFIX)/bin/n2k-mux $(DESTDIR)$(PREFIX)/bin/n2k-mux-run $(DESTDIR)$(PREFIX)/bin/n2k-mux-gui
	rm -f $(DESTDIR)/etc/systemd/system/n2k-mux.service
	rm -f $(DESTDIR)/etc/default/n2k-mux.example
	rm -f $(DESTDIR)/etc/n2k-mux/n2k-mux.ini.example

clean:
	rm -rf $(BUILD) n2k-mux n2k-mux-gui n2k-sim ydraw-bridge test_jsonl test_registry test_nmea0183 test_config test_arbiter test_mapper test_aisdedup test_sources test_stats test_netout test_ydraw

# Dépendances d'en-têtes générées par -MMD (recompile si un .h change).
-include $(wildcard $(BUILD)/*.d)
