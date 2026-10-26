# tools/gpu: host-side generators for husky_gpu.c's in-kernel GPU jobs.
#
#   make MESA_SRC=/path/to/mesa [OUT=../../artifacts/xnu-12377/bsd/dev/husky_gpu_compute.h]
#
# Mesa (MIT) is used only as a library at build time: the genxml packers and cs_builder.h, plus
# util/ralloc.c and util/u_dynarray.c. Mesa commit 1f9035d is the one tested. Python 3 runs
# gen_pack.py. mul_add_kernel.h is generated from mul_add.vhasm by vhasm.py (needs mako and
# Mesa's valhall/asm.py); it is checked in.
MESA_SRC ?= $(error set MESA_SRC to a Mesa checkout)
BUILD    ?= build
OUT      ?= ../../artifacts/xnu-12377/bsd/dev/husky_gpu_compute.h
PYTHON   ?= python3
CC       ?= cc
CXX      ?= c++

include mesa_defs.mk

GENXML := $(MESA_SRC)/src/panfrost/genxml
GEN    := $(BUILD)/gen
INC    := -I$(GEN) -I$(MESA_SRC)/include -I$(MESA_SRC)/src -I$(MESA_SRC)/src/panfrost -I.
CFLAGS_MESA := -O1 -g -w $(MESA_DEFS) $(INC)

all: $(OUT)

$(GEN)/genxml/v11_pack.h $(GEN)/genxml/common_pack.h: $(GENXML)/gen_pack.py $(GENXML)/v11.xml
	@mkdir -p $(GEN)/genxml
	$(PYTHON) $(GENXML)/gen_pack.py $(GENXML)/common.xml > $(GEN)/genxml/common_pack.h
	$(PYTHON) $(GENXML)/gen_pack.py $(GENXML)/v11.xml > $(GEN)/genxml/v11_pack.h

$(BUILD)/ralloc.o: $(MESA_SRC)/src/util/ralloc.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 $(CFLAGS_MESA) -c $< -o $@

$(BUILD)/u_dynarray.o: $(MESA_SRC)/src/util/u_dynarray.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 $(CFLAGS_MESA) -c $< -o $@

$(BUILD)/shim.o: mesa_util_shim.c
	@mkdir -p $(BUILD)
	$(CC) -O1 -c $< -o $@

$(BUILD)/gen-husky-compute: gen-husky-compute.cpp mali_compute_build.h mul_add_kernel.h \
    $(GEN)/genxml/v11_pack.h $(BUILD)/ralloc.o $(BUILD)/u_dynarray.o $(BUILD)/shim.o
	$(CXX) -std=gnu++17 $(CFLAGS_MESA) -DPAN_ARCH=11 $< $(BUILD)/ralloc.o $(BUILD)/u_dynarray.o \
	    $(BUILD)/shim.o -o $@ -lpthread

$(OUT): $(BUILD)/gen-husky-compute
	$(BUILD)/gen-husky-compute $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
