# POSIX build of the radar_audit host harnesses, mirroring run.ps1's four cl invocations so the
# suite runs on Linux/macOS and in CI (run.ps1 is MSVC-only). Sources are pure C and portable.
CC ?= cc
ROOT := ../..
RAD  := $(ROOT)/components/simulacra_radar
CYD  := $(ROOT)/cyd/main
CFLAGS ?= -O2 -Wall -Wno-unused-parameter

all: ui_dump fleet_dump render_dump expo_dump

ui_dump: ui_dump.c $(RAD)/radar_ui.c
	$(CC) $(CFLAGS) -I$(RAD) $^ -o $@

fleet_dump: fleet_dump.c $(CYD)/fleet_status.c
	$(CC) $(CFLAGS) -I$(CYD) -I$(RAD) $^ -o $@

# /FIportab.h in run.ps1 -> -include: the stub supplies the ESP-IDF bits radar_render.c expects.
render_dump: render_dump.c $(RAD)/radar_render.c $(RAD)/exposure.c
	$(CC) $(CFLAGS) -include host_stubs/portab.h -Ihost_stubs -I$(RAD) $^ -o $@

expo_dump: expo_dump.c $(RAD)/exposure.c
	$(CC) $(CFLAGS) -I$(RAD) $^ -o $@

clean:
	rm -f ui_dump fleet_dump render_dump expo_dump *.exe *.obj

.PHONY: all clean
