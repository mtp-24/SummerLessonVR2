/* The test mode's scripts (tools/testrun.sh `replay`): how many steps of pad.txt and head.txt the
 * agent's tables hold (agent/pads.c, agent/vr.c) and the injector fills (payload/main.c), one
 * figure for both. A head step is recorded at most every 50 ms while the head moves (~4.5 a second
 * in play), a controller step on each change (~6 a second): 65536 and 32768 hold an hour or more.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#define TEST_PAD_STEPS 32768
#define TEST_HEAD_STEPS 65536
