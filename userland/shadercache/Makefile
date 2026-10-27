A2V ?= /home/nhansp/aosp/agy-work/p1-air2valhall/out/work
LLVM_LIB ?= /usr/lib/llvm-18/lib
CONTRACT := ../contract
OFFLINE := ../plugin/MaliMetal/offline

MM_LIBS := util_c11 blake3 util_simd util compiler isaspec nir pan_valhall_disasm pan_bifrost_disasm \
	pan_midgard_disasm pan_bifrost pan_midgard pan_compiler pan_shared pan_model pan_format_v11 \
	pan_arch_v11 pan_lib

mali-shadercache: mali_shadercache.cpp metallib.cpp $(OFFLINE)/mvpack.c
	$(CC) -O2 -std=gnu11 -I$(CONTRACT) -I$(OFFLINE) -c $(OFFLINE)/mvpack.c -o mvpack.o
	$(CXX) -O2 -std=c++17 -Wall -I$(CONTRACT) -I$(OFFLINE) mali_shadercache.cpp metallib.cpp mvpack.o -o $@ \
		$(A2V)/libair2valhall.a -Wl,--start-group $(addprefix $(A2V)/lib/libmm_,$(addsuffix .a,$(MM_LIBS))) \
		-Wl,--end-group -lm -lpthread -L$(LLVM_LIB) -lLLVM-18

clean:
	rm -f mali-shadercache mvpack.o
