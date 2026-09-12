# Makefile — n2k-mux
# Cibles utiles :
#   make            -> construit tout ce qui est disponible
#   make test       -> construit tout puis lance TOUS les tests
#   make debug      -> reconstruit avec les sanitizers (UB + mémoire) et teste
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
# inimerge : fusion d'un INI régénéré dans l'INI existant en préservant les
# commentaires (utilisé par l'interface web à l'enregistrement).
INIMERGE_OBJ := $(BUILD)/inimerge.o
# polar : polaires qtVlm (.pol/.csv) + interpolation bilinéaire (simulateur, web).
POLAR_OBJ    := $(BUILD)/polar.o
# netout : plomberie TCP fan-out, testée et prête pour le futur flux N2K arbité ;
# pas encore liée au daemon (cf. test_netout).
NETOUT_OBJ   := $(BUILD)/netout.o
# ydraw : formateur YDRAW (Yacht Devices RAW text) pour le N2K réseau vers qtVlm ;
# testé, pas encore lié au daemon (cf. test_ydraw).
YDRAW_OBJ    := $(BUILD)/ydraw.o

DAEMON_OBJ   := $(BUILD)/daemon.o

# Tous les objets du pipeline (hors testeurs)
CORE_OBJ := $(JSONL_OBJ) $(REGISTRY_OBJ) $(CONFIG_OBJ) $(ARBITER_OBJ) $(NMEA_OBJ) $(MAPPER_OBJ) $(AISDEDUP_OBJ) $(SOURCES_OBJ) $(STATS_OBJ)

.PHONY: all clean install uninstall test debug
all: n2k-mux n2k-mux-web n2k-sim n2k-filter ydraw-bridge test_jsonl test_registry test_nmea0183 test_config test_arbiter test_mapper test_aisdedup test_sources test_stats test_netout test_ydraw test_inimerge test_polar

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

# --- Module sources (pont daemon→UI web) + son testeur ---
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

# --- Polaires (lecture + interpolation) + son testeur ---
test_polar: $(POLAR_OBJ) $(BUILD)/test_polar.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# --- Fusion INI (préservation des commentaires) + son testeur ---
test_inimerge: $(INIMERGE_OBJ) $(CONFIG_OBJ) $(BUILD)/test_inimerge.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Module (f) : daemon (binaire final) ---
n2k-mux: $(CORE_OBJ) $(BUILD)/cansock.o $(BUILD)/busmap.o $(DAEMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# --- Interface web de gestion (zéro dépendance) ---
n2k-mux-web: $(CONFIG_OBJ) $(INIMERGE_OBJ) $(POLAR_OBJ) $(BUILD)/web.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# --- Outil de test : pont canboat/actisense → YDRAW → TCP (pour qtVlm N2K) ---
ydraw-bridge: $(YDRAW_OBJ) $(NETOUT_OBJ) $(BUILD)/ydraw_bridge.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Outil de test : simulateur de flux N2K (JSON-lines) pour tous les PGN ---
n2k-sim: $(BUILD)/simulator.o $(POLAR_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# --- Filtre N2K→N2K socketcan (frame-passthrough can0 → vcan0 + YDRAW/TCP) ---
n2k-filter: $(BUILD)/canfilter.o $(YDRAW_OBJ) $(NETOUT_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Tests : tous les testeurs + captures de samples/ + bout-en-bout ---------
# Un seul point d'entrée, un seul code de sortie. Voir scripts/run-tests.sh.
test: all
	@scripts/run-tests.sh

# --- Build instrumenté ------------------------------------------------------
# Reconstruit TOUT avec les sanitizers (comportements indéfinis + mémoire) puis
# lance la suite. C'est ce qui attrape les fautes invisibles en -O2 : le cast de
# NaN vers int qui sortait un entier aberrant dans une phrase GGA ne se voyait
# qu'à un autre niveau d'optimisation.
# Les binaires produits REMPLACENT ceux du build normal (mêmes noms) :
# refaire `make clean && make` pour revenir à la version optimisée.
debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE \
	         -fsanitize=undefined,address -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=undefined,address" all
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	ASAN_OPTIONS=detect_leaks=0 scripts/run-tests.sh

# --- Installation système (daemon + service systemd) ---
# make install            installe le daemon, le service et les exemples
install: n2k-mux n2k-mux-web n2k-filter ydraw-bridge n2k-sim
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m755 n2k-mux $(DESTDIR)$(PREFIX)/bin/n2k-mux
	install -m755 n2k-mux-run $(DESTDIR)$(PREFIX)/bin/n2k-mux-run
	install -m755 n2k-mux-can-run $(DESTDIR)$(PREFIX)/bin/n2k-mux-can-run
	install -m755 n2k-mux-sim-run $(DESTDIR)$(PREFIX)/bin/n2k-mux-sim-run
	install -m755 n2k-sim $(DESTDIR)$(PREFIX)/bin/n2k-sim
	install -m755 n2k-mux-web $(DESTDIR)$(PREFIX)/bin/n2k-mux-web
	install -m755 n2k-filter $(DESTDIR)$(PREFIX)/bin/n2k-filter
	install -m755 ydraw-bridge $(DESTDIR)$(PREFIX)/bin/ydraw-bridge
	install -Dm644 n2k-mux.service $(DESTDIR)/etc/systemd/system/n2k-mux.service
	install -Dm644 n2k-mux-can.service $(DESTDIR)/etc/systemd/system/n2k-mux-can.service
	install -Dm644 n2k-mux-sim.service $(DESTDIR)/etc/systemd/system/n2k-mux-sim.service
	install -Dm644 n2k-mux-web.service $(DESTDIR)/etc/systemd/system/n2k-mux-web.service
	install -Dm644 n2k-mux.ini.example $(DESTDIR)/etc/n2k-mux/n2k-mux.ini.example
	install -Dm644 kplex.conf.example $(DESTDIR)/etc/n2k-mux/kplex.conf.example
	install -Dm644 n2k-mux.env.example $(DESTDIR)/etc/default/n2k-mux.example
	@echo "Installé. Pense à : cp /etc/n2k-mux/n2k-mux.ini.example /etc/n2k-mux/n2k-mux.ini"
	@echo "NGX-1/série : systemctl enable --now n2k-mux n2k-mux-web"
	@echo "socketcan   : systemctl enable --now n2k-mux-can n2k-mux-web"
	@echo "sans matériel : systemctl start n2k-mux-sim  (puis onglet Simulateur)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/n2k-mux $(DESTDIR)$(PREFIX)/bin/n2k-mux-run
	rm -f $(DESTDIR)$(PREFIX)/bin/n2k-mux-can-run $(DESTDIR)$(PREFIX)/bin/n2k-filter
	rm -f $(DESTDIR)$(PREFIX)/bin/n2k-mux-sim-run $(DESTDIR)$(PREFIX)/bin/n2k-sim
	rm -f $(DESTDIR)$(PREFIX)/bin/n2k-mux-web $(DESTDIR)$(PREFIX)/bin/ydraw-bridge
	rm -f $(DESTDIR)/etc/systemd/system/n2k-mux.service
	rm -f $(DESTDIR)/etc/systemd/system/n2k-mux-can.service
	rm -f $(DESTDIR)/etc/systemd/system/n2k-mux-sim.service
	rm -f $(DESTDIR)/etc/systemd/system/n2k-mux-web.service
	rm -f $(DESTDIR)/etc/default/n2k-mux.example
	rm -f $(DESTDIR)/etc/n2k-mux/n2k-mux.ini.example

clean:
	rm -rf $(BUILD) n2k-mux n2k-mux-web n2k-sim n2k-filter ydraw-bridge test_jsonl test_registry test_nmea0183 test_config test_arbiter test_mapper test_aisdedup test_sources test_stats test_netout test_ydraw test_inimerge test_polar

# Dépendances d'en-têtes générées par -MMD (recompile si un .h change).
-include $(wildcard $(BUILD)/*.d)
