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

# vigil_sim IS DELIBERATELY ABSENT FROM `all`, and this note exists so that absence reads as a
# decision rather than as the "added to run.ps1 but not the Makefile" mistake this project has hit
# before. vigil_sim.c opens a Win32 window (windows.h, user32, gdi32), so it cannot build here and
# adding it would break CI on Linux. It is a demo/presentation tool, not part of verification:
# nothing in tests/ exercises it, and the logic it displays is already covered by render_dump,
# ui_dump and fleet_dump. Build it on Windows with:  pwsh run.ps1 -Sim
#
# If it ever needs to run on Linux, the split is already right: sim_fleet.c and the render core are
# portable C, and only the Win32 front-end in vigil_sim.c would need replacing.

clean:
	rm -f ui_dump fleet_dump render_dump expo_dump *.exe *.obj *.ppm

.PHONY: all clean
