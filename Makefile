# Builds payload/summerlessonvr2.elf (the agent is built first and embedded in it).
all:
	$(MAKE) -C payload

clean:
	$(MAKE) -C agent clean
	$(MAKE) -C payload clean

.PHONY: all clean
