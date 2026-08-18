CC ?= cc
ROOT := ../..
# Link-only stub for phantom_sync_ble's ble_devices_count/ble_device_sync -- see the file's own
# comment for why the real ble_devices.c is deliberately not pulled in here. run.ps1 has always
# listed it; the Makefile did not, so the POSIX build failed to link.
SRC := probe_dump.c host_stubs/ble_devices_stub.c $(ROOT)/main/probe_frame.c $(ROOT)/main/probe_agents.c $(ROOT)/main/ssid_pool.c $(ROOT)/main/uniq_id.c $(ROOT)/main/phantom.c $(ROOT)/main/wifi_density.c $(ROOT)/main/surveil_oui.c $(ROOT)/main/surveil_ble_name.c
INC := -Ihost_stubs -I$(ROOT)/main -I$(ROOT)/components/simulacra_radar
CFLAGS ?= -O2 -Wall -Wno-unused-parameter
all: probe_dump
probe_dump: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o probe_dump
clean:
	rm -f probe_dump probe_dump.exe
