CC ?= cc
ROOT := ../..
SRC := probe_dump.c $(ROOT)/main/probe_frame.c $(ROOT)/main/probe_agents.c $(ROOT)/main/ssid_pool.c $(ROOT)/main/uniq_id.c $(ROOT)/main/phantom.c $(ROOT)/main/wifi_density.c $(ROOT)/main/surveil_oui.c
INC := -Ihost_stubs -I$(ROOT)/main -I$(ROOT)/components/simulacra_radar
CFLAGS ?= -O2 -Wall -Wno-unused-parameter
all: probe_dump
probe_dump: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o probe_dump
clean:
	rm -f probe_dump probe_dump.exe
