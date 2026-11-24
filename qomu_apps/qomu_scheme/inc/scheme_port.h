/*
 * scheme_port.h - platform hooks for scheme.c on the Qomu (EOS S3, FreeRTOS)
 *
 * scheme.c includes this automatically when it is on the include path.
 */
#ifndef SCHEME_PORT_H
#define SCHEME_PORT_H

#include "FreeRTOS.h"

/* String and vector bodies come from the FreeRTOS heap (heap_4). */
#define SCM_MALLOC(n) pvPortMalloc(n)
#define SCM_FREE(p)   vPortFree(p)

/* GC root stack: 4 bytes per entry. ~7 entries per level of non-tail recursion. */
#define SCM_GCSTACK_SIZE 1024

/* Longest console input line. */
#define SCM_LINE_MAX 200

/* Compile scheme.c with -O2 (the SDK builds everything with -Os). Costs code
 * space; remove to measure the difference. */
#define SCM_OPTIMIZE_SPEED 1

#endif
