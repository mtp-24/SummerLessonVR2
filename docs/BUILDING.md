# Building

Needs the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk), LLVM (clang and
ld.lld; Homebrew's on macOS) and Python 3:

    export PS5_PAYLOAD_SDK=~/ps5sdk/opt/ps5-payload-sdk
    export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config   # macOS with Homebrew's LLVM
    make            # a debug build (the tools below)
    make RELEASE=1  # the one to hand out

`LLVM_CONFIG` is read by the SDK's own compiler wrapper (`prospero-llvm-config`), which finds
its LLVM through it; without it the payload's build stops at the SDK's clang. Run
`make clean` between a debug and a release build.

The output is `payload/summerlessonvr2.elf`; the in-game part (`agent/`) is built first
and embedded in it. The agent's Makefile takes clang from `LLVM` (default
`/opt/homebrew/opt/llvm/bin`) and ld.lld from `LLD`; the payload's from the SDK's
toolchain. A release build keeps only the player's settings and the settings menu
([SETTINGS.md](SETTINGS.md)), a short log, and none of the debug tools: the debug menu
(which takes the settings menu's place in a debug build), the test mode and test-runner
commands on the control port, and the measuring options are left out, and it always
runs its built-in agent.

The GPU shaders' headers (`agent/*_gcn.h`) are checked in; rebuilding them needs LLVM's
AMDGPU tools (`llvm-mc`, from `LLVM_MC` or Homebrew's LLVM; `tools/gcn_asm.py`,
`tools/gcn_cc.py`). The TAA patch header (`agent/taa_patch.h`) is made by
`tools/gen_taa_patch.py` the same way.

## The source

- `agent/` runs inside the game: the PS VR emulation (`hooks.c`, `vr.c`), the PS VR2
  output and tracking (`vr2.c`), the GPU compatibility layer (`gen1.c`, `acb.c`), the
  PS5-library fixes (`ps5compat.c`), the TV picture (`mirror.c`), hand tracking
  (`hand.c`), DFR ("inset mode" in the code and the technical docs: `multiview.c`,
  `inset.c`, `insetpp.c`), eye tracking
  (`fov.c`), the settings menu and the debug menu (`menu.c`), the test mode's controller
  (`pads.c`) and the per-thread timing (`rtime.c`).
- `payload/` is the loader: it builds the libraries and the shader on first run
  (`prepare.c`, `rdna2gcn.c`), puts the agent into the game (`main.c`, `pt.c`) and
  applies DFR's patches to the game's code.
- `common/` is shared by both: the firmware profiles (`firmware.h`), the test mode and
  the headset cameras' state.
- `tools/` holds the test runner ([TESTING.md](TESTING.md)), the address finder for other
  builds of the game (`ebootmap/`, [OTHER-GAMES.md](OTHER-GAMES.md)), the shader tools,
  the host tests and the measurement scripts (`fov/`), and the hand-tracker gate's replay
  (`hand/`).

## Host tests

The host tests in `tools/fov/` build with the host's `cc` and need no console; each
takes `-I../../agent` and some the game's eboots (kept under `dumps/`, never committed):

    cd tools/fov
    cc -O2 -I../../agent -o /tmp/mvtest multiview_test.c && /tmp/mvtest <hikari-1.00 eboot.bin> /tmp/mv.bin [<chisato> <allison> <hikari-1.02 eboot.bin>]
    cc -O2 -I../../agent -o /tmp/pptest insetpp_test.c -lm && /tmp/pptest
    cc -O2 -I../../agent -o /tmp/menutest menu_test.c -lz && /tmp/menutest /tmp/menu.png
    cc -O2 -DRELEASE -I../../agent -o /tmp/menutest-r menu_test.c -lz && /tmp/menutest-r /tmp/menu-r.png
    cc -O2 -o /tmp/insettest inset_test.c && /tmp/insettest
    cc -O2 -I../../agent -o /tmp/iwtest insetwin_test.c && /tmp/iwtest
    cc -O2 -I../../agent -o /tmp/rt rtime_test.c && /tmp/rt

`multiview_test` boots the agent as each build against its eboot and checks every patch
entry on the game's own bytes ([OTHER-GAMES.md](OTHER-GAMES.md), Inset mode).
