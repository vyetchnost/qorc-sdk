/*
 * scheme_fs.h - named Scheme files in the top megabyte of the Qomu SPI flash
 */
#ifndef SCHEME_FS_H
#define SCHEME_FS_H

#include <stdint.h>

typedef struct {
    int (*init)(void);                                  /* 0 = flash present  */
    int (*read)(uint32_t addr, void *buf, uint32_t len);/* len % 4 == 0       */
    int (*program_page)(uint32_t addr, const void *buf);/* 256 B, page aligned*/
    int (*erase_sector)(uint32_t addr);                 /* 4 KB aligned       */
    int (*con_getc)(void);                              /* raw console byte   */
    int (*con_peek)(void);                              /* -1 if none waiting */
} fs_port;

int  scheme_fs_init(const fs_port *port);   /* registers the primitives; 1 if usable */
void scheme_fs_autorun(void);               /* (load "boot") if it exists and allowed */
int  scheme_fs_boot_skipped(void);          /* 1: last two boots never finished       */

#endif
