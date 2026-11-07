# SummerLessonVR2 payload (needs the PS5 payload SDK: https://github.com/ps5-payload-dev/sdk)
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif
CFLAGS := -O2 -Wall
ifeq ($(NATIVE),1) # a test payload with the native 90 and 120 modes (payload/main.c NATIVE_MODES)
CFLAGS += -DNATIVE_MODES=1
endif
ifeq ($(RELEASE),1) # make RELEASE=1: little logging, no debug tools, no test-runner commands
CFLAGS += -DRELEASE
endif
SRCS := main.c pt.c prepare.c selfpager.c fself.c rdna2gcn.c
HDRS := pt.h prepare.h selfpager.h fself.h rdna2gcn.h rdna2gcn_tables.h ../common/firmware.h ../common/test.h

summerlessonvr2.elf: $(SRCS) $(HDRS) ../agent/build/agent.elf
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lkernel_sys -lSceSystemService -lSceUserService

../agent/build/agent.elf: FORCE
	$(MAKE) -C ../agent RELEASE=$(RELEASE)

FORCE:

clean:
	rm -f summerlessonvr2.elf
