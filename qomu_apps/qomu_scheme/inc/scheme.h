/*
 * scheme.h - a small Scheme interpreter for microcontrollers
 *
 * Portable C99 core. The platform supplies character I/O (scm_io), a heap
 * buffer for cells, and optionally SCM_MALLOC/SCM_FREE for string and vector
 * bodies (via scheme_port.h, picked up automatically when present).
 */
#ifndef SCHEME_H
#define SCHEME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scm_cell *scm_obj;
typedef scm_obj (*scm_prim_fn)(scm_obj args);

typedef struct {
    void (*putc)(int c);        /* write one byte                              */
    int  (*getc)(void);         /* blocking read of one byte, -1 on EOF        */
    int  (*avail)(void);        /* nonzero if a byte is waiting (may be NULL)  */
    int  tty;                   /* 1: echo + line editing, LF -> CRLF output   */
    int  (*brk)(void);          /* optional: returns 1 once per out-of-band ^C */
} scm_io;

#define SCM_VARIADIC 255

/* setup */
void    scm_init(void *heap, size_t heap_bytes, const scm_io *io);
void    scm_set_stack_limit(size_t bytes);   /* C stack the evaluator may use */
void    scm_defprim(const char *name, scm_prim_fn fn, int min_args, int max_args);
void    scm_define(const char *name, scm_obj value);

/* running code */
void    scm_repl(void);                      /* returns on EOF (never on tty)   */
int     scm_eval_cstr(const char *src);      /* 0 ok, -1 error (already printed) */

/* helpers for primitives */
scm_obj scm_arg(scm_obj *args);              /* pop next argument               */
int64_t scm_to_int(scm_obj o);               /* errors if not an integer        */
double  scm_to_num(scm_obj o);               /* any number                      */
const char *scm_to_cstr(scm_obj o);          /* string or symbol name           */
scm_obj scm_from_int(int64_t v);
scm_obj scm_from_num(double v);
scm_obj scm_from_cstr(const char *s);
scm_obj scm_from_bool(int b);
scm_obj scm_intern(const char *name);
int     scm_is_true(scm_obj o);
void    scm_error(const char *msg, scm_obj irritant)
#if defined(__GNUC__)
    __attribute__((noreturn))
#endif
    ;
void    scm_check_interrupt(void);           /* raise "interrupted" on pending ^C */
void    scm_puts(const char *s);
void    scm_write(scm_obj o);
void    scm_display(scm_obj o);
size_t  scm_free_cells(void);
size_t  scm_total_cells(void);

extern struct scm_cell scm_c_nil, scm_c_true, scm_c_false, scm_c_unspec;
#define SCM_NIL    (&scm_c_nil)
#define SCM_TRUE   (&scm_c_true)
#define SCM_FALSE  (&scm_c_false)
#define SCM_UNSPEC (&scm_c_unspec)

#ifdef __cplusplus
}
#endif
#endif
