# SummerLessonVR2 agent: freestanding, position independent, loaded into the game by the payload.
LLVM ?= /opt/homebrew/opt/llvm/bin
CC := $(LLVM)/clang
# Homebrew ships lld in its own formula
LLD ?= $(firstword $(wildcard $(LLVM)/ld.lld /opt/homebrew/opt/lld/bin/ld.lld) ld.lld)
CFLAGS := -target x86_64-unknown-freebsd12 -ffreestanding -fno-builtin -nostdlib -fPIC \
	-fvisibility=hidden -O2 -Wall -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -mno-red-zone -std=gnu11 -fno-omit-frame-pointer
SRCS := core.c hooks.c vr.c vr2.c gen1.c ps5compat.c mirror.c acb.c hand.c pads.c trace.c fov.c rtime.c multiview.c insetpp.c inset.c menu.c
# the payload's shader translator, for libSceVrHand's shaders (hand.c)
OBJS := $(SRCS:%.c=build/%.o) build/rdna2gcn.o
HDRS := agent.h psvr1.h vr.h vr2.h nids_gen.h hooks.inc imports.inc mirror_gcn.h inset_gcn.h ../common/firmware.h ../common/test.h
ifeq ($(RELEASE),1) # make RELEASE=1 (from the payload's): no periodic detail in the log (LOG_DETAIL)
CFLAGS += -DRELEASE
endif
# objects of the other mode (release or not) are not reused: make tracks no flags
MODE := $(if $(filter 1,$(RELEASE)),release,debug)
$(shell mkdir -p build; [ "$$(cat build/.mode 2>/dev/null)" = "$(MODE)" ] || { rm -f build/*.o build/agent.elf; echo $(MODE) > build/.mode; })

all: build/agent.elf

nids_gen.h hooks.inc: imports.def hooks.def ../tools/gen_nids.py
	python3 ../tools/gen_nids.py .

mirror_gcn.h: mirror.s ../tools/gcn_asm.py
	python3 ../tools/gcn_asm.py mirror.s $@ k_mirror_gcn

inset_gcn.h: ../tools/gpu/inset.c ../tools/gcn_cc.py
	GCN_ENTRY=inset_main python3 ../tools/gcn_cc.py ../tools/gpu/inset.c $@ k_inset_gcn

build/%.o: %.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/rdna2gcn.o: ../payload/rdna2gcn.c ../payload/rdna2gcn.h ../payload/rdna2gcn_tables.h compat/string.h
	@mkdir -p build
	$(CC) $(CFLAGS) -isystem compat -c -o $@ $<

build/agent.elf: $(OBJS)
	$(LLD) -pie --no-dynamic-linker -z max-page-size=0x4000 -z norelro --hash-style=sysv \
		-e astro_hdr -o $@ $(OBJS)

clean:
	rm -rf build nids_gen.h hooks.inc
