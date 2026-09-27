# Sopwith GB - build with GBDK-2020 (https://github.com/gbdk-2020/gbdk-2020)
# Usage: make GBDK_HOME=/path/to/gbdk
GBDK_HOME ?= ../gbdk
LCC = $(GBDK_HOME)/bin/lcc
CFLAGS = -Wf--opt-code-speed -Wf--max-allocs-per-node50000 -Wm-yn"SOPWITH GB"

sopwith.gb: src/main.c src/assets.c src/assets.h
	$(LCC) $(CFLAGS) -Wl-m -Wl-j -o $@ src/main.c src/assets.c

# regenerate graphics / world data (needs python3)
assets:
	python3 tools/gen_assets.py

clean:
	rm -f *.gb *.map *.noi *.ihx *.sym *.lst src/*.o src/*.lst src/*.asm src/*.sym
