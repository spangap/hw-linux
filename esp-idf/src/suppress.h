/**
 * suppress.h — the port's tickless idle, named for the kernel.
 *
 * Put in front of every source of the freertos component (-include, from
 * esp-idf/CMakeLists.txt), because FreeRTOS.h only takes a
 * portSUPPRESS_TICKS_AND_SLEEP defined before it and the linux port defines
 * none. The function is src/tick.cpp's.
 */
#pragma once

#ifndef __ASSEMBLER__
#ifdef __cplusplus
extern "C"
#endif
void vPortSuppressTicksAndSleep(unsigned long xExpectedIdleTime);

#define portSUPPRESS_TICKS_AND_SLEEP(xExpectedIdleTime) vPortSuppressTicksAndSleep(xExpectedIdleTime)
#endif
