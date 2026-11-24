/*
 * scheme.c - a small Scheme interpreter for microcontrollers
 *
 * Design notes
 *  - Objects are pointers to fixed-size cells. Small integers (31-bit) are
 *    immediate (low bit set) and never allocate; larger integers are boxed
 *    int64; floats are SCM_FLT (float by default, to use the M4F FPU).
 *  - Memory: mark/sweep over a caller-supplied cell array. Every freshly
 *    allocated cell is pushed on a "gc stack" so C temporaries are always
 *    rooted; the evaluator trims that stack as frames finish. Marking uses a
 *    bounded explicit stack with overflow rescan, so deep data cannot blow
 *    the C stack during GC.
 *  - Evaluator: direct AST walk with proper tail calls in if/cond/case/and/
 *    or/when/unless/begin/let, let*, letrec, named let and closure bodies.
 *    Non-tail recursion is bounded by scm_set_stack_limit().
 *  - Errors longjmp back to the REPL. Ctrl-C (0x03) on the console
 *    interrupts a running evaluation.
 *  - define-macro (non-hygienic) + quasiquote; expansions are memoized.
 */
#include "scheme.h"

#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <math.h>

#if defined(__has_include)
#  if __has_include("scheme_port.h")
#    include "scheme_port.h"
#  endif
#endif

/* The interpreter's speed matters more than its size: a port can compile
 * just this file with -O2 while the rest of its firmware stays at -Os. */
#if defined(SCM_OPTIMIZE_SPEED) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif

#ifndef SCM_MALLOC
#define SCM_MALLOC(n) malloc(n)
#define SCM_FREE(p)   free(p)
#endif
#ifndef SCM_GCSTACK_SIZE
#define SCM_GCSTACK_SIZE 1024
#endif
#ifndef SCM_MARKSTACK_SIZE
#define SCM_MARKSTACK_SIZE 128
#endif
#ifndef SCM_LINE_MAX
#define SCM_LINE_MAX 200
#endif
#ifndef SCM_TOKEN_MAX
#define SCM_TOKEN_MAX 256
#endif
#ifndef SCM_TYPEAHEAD
#define SCM_TYPEAHEAD 256       /* input read while evaluating, kept for the reader */
#endif
#ifndef SCM_POLL_INTERVAL
#define SCM_POLL_INTERVAL 512
#endif

#ifdef SCM_DOUBLE
typedef double scm_flt;
#define FLT_DIGITS 15
#define F(fn) fn
#else
typedef float scm_flt;
#define FLT_DIGITS 7
#define F(fn) fn##f
#endif

enum {
    T_FREE = 0, T_PAIR, T_INT, T_FLT, T_SYM, T_STR, T_CHAR,
    T_PRIM, T_CLOS, T_MACRO, T_VEC, T_CONST
};

struct scm_cell {
    uint8_t  type;
    uint8_t  mark;
    uint16_t aux;           /* SYM: special-form id; PRIM: arity; CONST: id */
    union {
        struct { scm_obj car, cdr; } p;
        uint32_t w[2];                              /* boxed int64 */
        scm_flt f;
        struct { char *name; scm_obj val; } sym;
        struct { char *s; uint32_t len; } str;
        int32_t ch;
        struct { scm_prim_fn fn; const char *name; } prim;
        struct { scm_obj code; scm_obj env; } clos; /* code = (params . body) */
        struct { scm_obj *v; uint32_t len; } vec;
    } u;
};

enum { C_NIL = 1, C_TRUE, C_FALSE, C_UNSPEC, C_EOF, C_UNBOUND };
struct scm_cell scm_c_nil     = { T_CONST, 0, C_NIL,     {{0, 0}} };
struct scm_cell scm_c_true    = { T_CONST, 0, C_TRUE,    {{0, 0}} };
struct scm_cell scm_c_false   = { T_CONST, 0, C_FALSE,   {{0, 0}} };
struct scm_cell scm_c_unspec  = { T_CONST, 0, C_UNSPEC,  {{0, 0}} };
static struct scm_cell c_eof     = { T_CONST, 0, C_EOF,     {{0, 0}} };
static struct scm_cell c_unbound = { T_CONST, 0, C_UNBOUND, {{0, 0}} };

#define NIL     SCM_NIL
#define TRUE_   SCM_TRUE
#define FALSE_  SCM_FALSE
#define UNSPEC  SCM_UNSPEC
#define EOF_OBJ (&c_eof)
#define UNBOUND (&c_unbound)

#define FIX_MIN (-(1L << 30))
#define FIX_MAX ((1L << 30) - 1)
#define FIXNUMP(o)  (((uintptr_t)(o)) & 1)
#define FIX_MAKE(n) ((scm_obj)((((uintptr_t)(intptr_t)(n)) << 1) | 1))
#define FIX_VAL(o)  ((int32_t)(((intptr_t)(o)) >> 1))

#define CAR(o) ((o)->u.p.car)
#define CDR(o) ((o)->u.p.cdr)

static inline int type_of(scm_obj o) { return FIXNUMP(o) ? T_INT : o->type; }
static inline int is_pair(scm_obj o) { return !FIXNUMP(o) && o->type == T_PAIR; }
static inline int is_sym(scm_obj o)  { return !FIXNUMP(o) && o->type == T_SYM; }
static inline int is_int(scm_obj o)  { return FIXNUMP(o) || o->type == T_INT; }
static inline int is_num(scm_obj o)  { return FIXNUMP(o) || o->type == T_INT || o->type == T_FLT; }

/* ------------------------------------------------------------------ state */

static const scm_io *io;
static struct scm_cell *heap;
static size_t ncells, nfree;
static scm_obj freelist;
static scm_obj symlist;

static scm_obj gcstack[SCM_GCSTACK_SIZE];
static int gctop;

static jmp_buf *err_jmp;
static char  *stack_base;
static size_t stack_max;
static uintptr_t stack_limit;      /* stack_base - stack_max, 0 = no check */
static unsigned poll_count;
static unsigned gensym_counter;

/* console input */
static char line[SCM_LINE_MAX], prev_line[SCM_LINE_MAX];
static int line_len, line_pos;
static unsigned char tahead[SCM_TYPEAHEAD];
static int ta_head, ta_tail;
static int at_toplevel;
static int last_cr;

/* reader */
static const char *rd_str;
static int rd_unget[4];
static int rd_nunget;

/* output limiting (for error messages) */
static int out_limit, out_count;

/* special form ids */
enum {
    SF_NONE, SF_QUOTE, SF_QUASI, SF_UNQUOTE, SF_UNQUOTE_SPLICING, SF_IF,
    SF_DEFINE, SF_DEFMACRO, SF_SET, SF_LAMBDA, SF_BEGIN, SF_LET, SF_LETSTAR,
    SF_LETREC, SF_COND, SF_CASE, SF_AND, SF_OR, SF_WHEN, SF_UNLESS
};
static scm_obj s_quote, s_quasi, s_unquote, s_unquote_splicing, s_begin,
               s_else, s_arrow, s_lambda, s_define;

static scm_obj eval(scm_obj x, scm_obj env);
static void print(scm_obj o, int write);

/* ------------------------------------------------------------------ output */

static void outc(int c)
{
    if (out_limit) {
        if (out_count >= out_limit) return;
        out_count++;
    }
    if (io->tty && c == '\n') io->putc('\r');
    io->putc(c);
}

void scm_puts(const char *s) { while (*s) outc((unsigned char)*s++); }

/* ------------------------------------------------------------------ errors */

static void reset_input(void)
{
    line_len = line_pos = 0;
    rd_nunget = 0;
    rd_str = NULL;
}

void scm_error(const char *msg, scm_obj irritant)
{
    out_limit = 0;
    scm_puts("\nError: ");
    scm_puts(msg);
    if (irritant) {
        outc(' ');
        out_limit = 160; out_count = 0;
        print(irritant, 1);
        if (out_count >= out_limit) { out_limit = 0; scm_puts(" ..."); }
        out_limit = 0;
    }
    outc('\n');
    reset_input();
    if (err_jmp) longjmp(*err_jmp, 1);
    for (;;) { }
}

static void update_stack_limit(void)
{
    uintptr_t base = (uintptr_t)stack_base;
    stack_limit = (base && stack_max && stack_max < base) ? base - stack_max : 0;
}

static inline void check_stack(void)
{
    /* one compare per eval: deeper than stack_max below the recorded base */
    if ((uintptr_t)__builtin_frame_address(0) < stack_limit)
        scm_error("recursion too deep (C stack limit)", NULL);
}

void scm_set_stack_limit(size_t bytes) { stack_max = bytes; update_stack_limit(); }

/*
 * Look for Ctrl-C while evaluating. Other input is kept for the reader. When
 * the typeahead buffer is full we stop reading, so further bytes wait in the
 * device's own FIFO (and the host holds back) instead of being dropped.
 */
static void poll_break(void)
{
    if (io->brk && io->brk()) {         /* ^C seen by the platform's input pump */
        ta_head = ta_tail = 0;
        scm_error("interrupted", NULL);
    }
    if (!io->avail) return;
    while (((ta_tail + 1) % (int)sizeof tahead) != ta_head && io->avail()) {
        int c = io->getc();
        if (c == 3) {
            ta_head = ta_tail = 0;
            scm_error("interrupted", NULL);
        }
        tahead[ta_tail] = (unsigned char)c;
        ta_tail = (ta_tail + 1) % (int)sizeof tahead;
    }
}

void scm_check_interrupt(void) { poll_break(); }

/* ------------------------------------------------------------------ memory */

static inline void push(scm_obj o)
{
    if (gctop >= SCM_GCSTACK_SIZE) {
        gctop = 0;
        scm_error("recursion too deep (gc root stack)", NULL);
    }
    gcstack[gctop++] = o;
}

static inline int is_heap(scm_obj o)
{
    return !FIXNUMP(o) && (struct scm_cell *)o >= heap
        && (struct scm_cell *)o < heap + ncells;
}

static scm_obj markstack[SCM_MARKSTACK_SIZE];
static int msp, mark_overflow;

static void mark_push(scm_obj o)
{
    if (!o || !is_heap(o) || o->mark) return;
    o->mark = 1;
    switch (o->type) {
    case T_PAIR: case T_SYM: case T_CLOS: case T_MACRO: case T_VEC:
        if (msp < SCM_MARKSTACK_SIZE) markstack[msp++] = o;
        else mark_overflow = 1;
        break;
    default:
        break;
    }
}

static void mark_children(scm_obj o)
{
    uint32_t i;
    switch (o->type) {
    case T_PAIR:  mark_push(CAR(o)); mark_push(CDR(o)); break;
    case T_SYM:   mark_push(o->u.sym.val); break;
    case T_CLOS:
    case T_MACRO: mark_push(o->u.clos.code); mark_push(o->u.clos.env); break;
    case T_VEC:   for (i = 0; i < o->u.vec.len; i++) mark_push(o->u.vec.v[i]); break;
    default: break;
    }
}

static void mark_drain(void)
{
    while (msp > 0) mark_children(markstack[--msp]);
}

static void gc(void)
{
    size_t i;
    int k;
    msp = 0; mark_overflow = 0;
    for (k = 0; k < gctop; k++) { mark_push(gcstack[k]); mark_drain(); }
    mark_push(symlist); mark_drain();
    while (mark_overflow) {
        mark_overflow = 0;
        for (i = 0; i < ncells; i++)
            if (heap[i].mark && heap[i].type != T_FREE) { mark_children(&heap[i]); mark_drain(); }
    }
    freelist = NULL; nfree = 0;
    for (i = ncells; i-- > 0; ) {
        scm_obj c = &heap[i];
        if (c->mark) { c->mark = 0; continue; }
        switch (c->type) {
        case T_STR: SCM_FREE(c->u.str.s); break;
        case T_VEC: SCM_FREE(c->u.vec.v); break;
        case T_SYM: SCM_FREE(c->u.sym.name); break;  /* only uninterned (gensym) die */
        default: break;
        }
#ifdef SCM_GC_STRESS
        memset(c, 0xA5, sizeof *c);
#endif
        c->type = T_FREE;
        CDR(c) = freelist;
        freelist = c;
        nfree++;
    }
}

static scm_obj alloc(int type)
{
    scm_obj c;
#ifdef SCM_GC_STRESS
    gc();
#endif
    if (!freelist) {
        gc();
        if (!freelist) scm_error("out of memory (cell heap full)", NULL);
    }
    c = freelist;
    freelist = CDR(c);
    nfree--;
    memset(c, 0, sizeof *c);
    c->type = (uint8_t)type;
    push(c);
    return c;
}

static void *xmalloc(size_t n)
{
    void *p = SCM_MALLOC(n ? n : 1);
    if (!p) {
        gc();       /* may free dead strings */
        p = SCM_MALLOC(n ? n : 1);
        if (!p) scm_error("out of memory (malloc)", NULL);
    }
    return p;
}

size_t scm_free_cells(void)  { return nfree; }
size_t scm_total_cells(void) { return ncells; }

/* ------------------------------------------------------------------ constructors */

static scm_obj cons(scm_obj a, scm_obj d)
{
    scm_obj c = alloc(T_PAIR);
    CAR(c) = a; CDR(c) = d;
    return c;
}

scm_obj scm_from_int(int64_t v)
{
    scm_obj c;
    if (v >= FIX_MIN && v <= FIX_MAX) return FIX_MAKE(v);
    c = alloc(T_INT);
    memcpy(c->u.w, &v, sizeof v);
    return c;
}

static int64_t int_val(scm_obj o)
{
    int64_t v;
    if (FIXNUMP(o)) return FIX_VAL(o);
    memcpy(&v, o->u.w, sizeof v);
    return v;
}

static scm_obj make_flt(scm_flt f)
{
    scm_obj c = alloc(T_FLT);
    c->u.f = f;
    return c;
}

scm_obj scm_from_num(double v) { return make_flt((scm_flt)v); }
scm_obj scm_from_bool(int b) { return b ? TRUE_ : FALSE_; }
int scm_is_true(scm_obj o) { return o != FALSE_; }

static scm_obj make_char(int c)
{
    scm_obj o = alloc(T_CHAR);
    o->u.ch = c;
    return o;
}

static scm_obj make_string(const char *s, size_t len)
{
    scm_obj o = alloc(T_STR);
    char *b = xmalloc(len + 1);
    if (s) memcpy(b, s, len); else memset(b, ' ', len);
    b[len] = 0;
    o->u.str.s = b;
    o->u.str.len = (uint32_t)len;
    return o;
}

scm_obj scm_from_cstr(const char *s) { return make_string(s, strlen(s)); }

static scm_obj make_closure(scm_obj params, scm_obj body, scm_obj env, int type)
{
    scm_obj code = cons(params, body);
    scm_obj c = alloc(type);
    c->u.clos.code = code;
    c->u.clos.env = env;
    return c;
}

static scm_obj make_symbol(const char *name, size_t len)
{
    scm_obj s = alloc(T_SYM);
    char *b = xmalloc(len + 1);
    memcpy(b, name, len);
    b[len] = 0;
    s->u.sym.name = b;
    s->u.sym.val = UNBOUND;
    return s;
}

static scm_obj intern_n(const char *name, size_t len)
{
    scm_obj l, s;
    for (l = symlist; l != NIL; l = CDR(l)) {
        s = CAR(l);
        if (strlen(s->u.sym.name) == len && memcmp(s->u.sym.name, name, len) == 0)
            return s;
    }
    s = make_symbol(name, len);
    symlist = cons(s, symlist);
    return s;
}

scm_obj scm_intern(const char *name) { return intern_n(name, strlen(name)); }

/* list builder: keeps only the head on the gc stack */
typedef struct { scm_obj head, tail; int g; } lbuf;

static void lb_init(lbuf *b) { b->head = b->tail = NIL; b->g = gctop; }

static void lb_add(lbuf *b, scm_obj v)
{
    scm_obj c = cons(v, NIL);
    if (b->head == NIL) {
        b->head = c;
        gctop = b->g;
        push(c);
        b->g = gctop;
    } else {
        CDR(b->tail) = c;
        gctop = b->g;
    }
    b->tail = c;
}

static void lb_tail(lbuf *b, scm_obj rest)
{
    if (b->head == NIL) { b->head = rest; gctop = b->g; push(rest); b->g = gctop; }
    else CDR(b->tail) = rest;
}

/* ------------------------------------------------------------------ argument helpers */

static scm_obj car_s(scm_obj o)
{
    if (!is_pair(o)) scm_error("bad syntax", o);
    return CAR(o);
}

static scm_obj cdr_s(scm_obj o)
{
    if (!is_pair(o)) scm_error("bad syntax", o);
    return CDR(o);
}

scm_obj scm_arg(scm_obj *args)
{
    scm_obj a;
    if (!is_pair(*args)) scm_error("missing argument", NULL);
    a = CAR(*args);
    *args = CDR(*args);
    return a;
}

static scm_obj check(scm_obj o, int type, const char *what)
{
    if (type_of(o) != type) scm_error(what, o);
    return o;
}

int64_t scm_to_int(scm_obj o)
{
    if (!is_int(o)) scm_error("not an integer:", o);
    return int_val(o);
}

double scm_to_num(scm_obj o)
{
    if (is_int(o)) return (double)int_val(o);
    if (type_of(o) == T_FLT) return o->u.f;
    scm_error("not a number:", o);
}

static scm_flt to_flt(scm_obj o)
{
    if (is_int(o)) return (scm_flt)int_val(o);
    if (type_of(o) == T_FLT) return o->u.f;
    scm_error("not a number:", o);
}

const char *scm_to_cstr(scm_obj o)
{
    if (type_of(o) == T_STR) return o->u.str.s;
    if (type_of(o) == T_SYM) return o->u.sym.name;
    scm_error("not a string:", o);
}

static scm_obj str_arg(scm_obj *args) { return check(scm_arg(args), T_STR, "not a string:"); }

static long index_arg(scm_obj *args, long limit)
{
    scm_obj o = scm_arg(args);
    int64_t i = scm_to_int(o);
    if (i < 0 || i > limit) scm_error("index out of range:", o);
    return (long)i;
}

/* ------------------------------------------------------------------ number formatting */

static int fmt_int(char *buf, int64_t v, int radix)
{
    char tmp[72];
    int n = 0, len = 0;
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { int d = (int)(m % (unsigned)radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); m /= (unsigned)radix; } while (m);
    if (v < 0) buf[len++] = '-';
    while (n) buf[len++] = tmp[--n];
    buf[len] = 0;
    return len;
}

static int fmt_flt(char *buf, double f)
{
    char d[20];
    char *p = buf;
    int e = 0, i, nd;
    uint64_t digits, lim = 1;
    double m;

    if (f != f) { strcpy(buf, "+nan.0"); return 6; }
    if (f > 1e300 * 1e10) { strcpy(buf, "+inf.0"); return 6; }
    if (f < -1e300 * 1e10) { strcpy(buf, "-inf.0"); return 6; }
    if (f < 0) { *p++ = '-'; f = -f; }
    if (f == 0) { strcpy(p, "0.0"); return (int)(p - buf) + 3; }

    m = f;
    while (m >= 10.0) { m /= 10.0; e++; }
    while (m < 1.0)   { m *= 10.0; e--; }
    for (i = 1; i < FLT_DIGITS; i++) lim *= 10;
    digits = (uint64_t)(m * (double)lim + 0.5);
    if (digits >= lim * 10) { digits /= 10; e++; }
    for (i = FLT_DIGITS - 1; i >= 0; i--) { d[i] = (char)('0' + digits % 10); digits /= 10; }
    nd = FLT_DIGITS;
    while (nd > 1 && d[nd - 1] == '0') nd--;

    if (e >= -5 && e < FLT_DIGITS) {
        if (e >= 0) {
            for (i = 0; i <= e; i++) *p++ = i < nd ? d[i] : '0';
            *p++ = '.';
            if (nd > e + 1) for (i = e + 1; i < nd; i++) *p++ = d[i];
            else *p++ = '0';
        } else {
            *p++ = '0'; *p++ = '.';
            for (i = 0; i < -e - 1; i++) *p++ = '0';
            for (i = 0; i < nd; i++) *p++ = d[i];
        }
    } else {
        *p++ = d[0]; *p++ = '.';
        if (nd > 1) for (i = 1; i < nd; i++) *p++ = d[i];
        else *p++ = '0';
        *p++ = 'e';
        p += fmt_int(p, e, 10);
    }
    *p = 0;
    return (int)(p - buf);
}

/* ------------------------------------------------------------------ printer */

static const char *char_names[][2] = {
    {"space", " "}, {"newline", "\n"}, {"tab", "\t"}, {"return", "\r"},
    {"nul", "\0"}, {"escape", "\033"}, {"delete", "\177"}, {"alarm", "\a"},
    {"backspace", "\b"}, {NULL, NULL}
};

static void print_char(int c, int write)
{
    int i;
    if (!write) { outc(c); return; }
    scm_puts("#\\");
    for (i = 0; char_names[i][0]; i++)
        if ((unsigned char)char_names[i][1][0] == c) { scm_puts(char_names[i][0]); return; }
    if (c < 32 || c >= 127) {
        char b[8];
        outc('x');
        fmt_int(b, c, 16);
        scm_puts(b);
        return;
    }
    outc(c);
}

static void print_string(scm_obj o, int write)
{
    uint32_t i;
    if (!write) {
        for (i = 0; i < o->u.str.len; i++) outc((unsigned char)o->u.str.s[i]);
        return;
    }
    outc('"');
    for (i = 0; i < o->u.str.len; i++) {
        int c = (unsigned char)o->u.str.s[i];
        switch (c) {
        case '"':  scm_puts("\\\""); break;
        case '\\': scm_puts("\\\\"); break;
        case '\n': scm_puts("\\n"); break;
        case '\t': scm_puts("\\t"); break;
        case '\r': scm_puts("\\r"); break;
        default:   outc(c);
        }
    }
    outc('"');
}

static void print(scm_obj o, int write)
{
    char buf[48];
    uint32_t i;
    check_stack();
    if (out_limit && out_count >= out_limit) return;
    switch (type_of(o)) {
    case T_INT: fmt_int(buf, int_val(o), 10); scm_puts(buf); break;
    case T_FLT: fmt_flt(buf, o->u.f); scm_puts(buf); break;
    case T_SYM: scm_puts(o->u.sym.name); break;
    case T_STR: print_string(o, write); break;
    case T_CHAR: print_char(o->u.ch, write); break;
    case T_PRIM: scm_puts("#<primitive "); scm_puts(o->u.prim.name); outc('>'); break;
    case T_CLOS: scm_puts("#<procedure>"); break;
    case T_MACRO: scm_puts("#<macro>"); break;
    case T_VEC:
        scm_puts("#(");
        for (i = 0; i < o->u.vec.len; i++) { if (i) outc(' '); print(o->u.vec.v[i], write); }
        outc(')');
        break;
    case T_PAIR:
        if (CAR(o) == s_quote && is_pair(CDR(o)) && CDR(CDR(o)) == NIL) {
            outc('\''); print(CAR(CDR(o)), write); break;
        }
        outc('(');
        for (;;) {
            print(CAR(o), write);
            o = CDR(o);
            if (out_limit && out_count >= out_limit) return;
            if (is_pair(o)) { outc(' '); continue; }
            if (o != NIL) { scm_puts(" . "); print(o, write); }
            break;
        }
        outc(')');
        break;
    case T_CONST:
        switch (o->aux) {
        case C_NIL: scm_puts("()"); break;
        case C_TRUE: scm_puts("#t"); break;
        case C_FALSE: scm_puts("#f"); break;
        case C_UNSPEC: scm_puts("#<unspecified>"); break;
        case C_EOF: scm_puts("#<eof>"); break;
        default: scm_puts("#<unbound>"); break;
        }
        break;
    default: scm_puts("#<?>"); break;
    }
}

void scm_write(scm_obj o)   { print(o, 1); }
void scm_display(scm_obj o) { print(o, 0); }

/* ------------------------------------------------------------------ console line editor */

static int raw_getc(void)
{
    if (ta_head != ta_tail) {
        int c = tahead[ta_head];
        ta_head = (ta_head + 1) % (int)sizeof tahead;
        return c;
    }
    return io->getc();
}

static void erase_line(void)
{
    while (line_len > 0) { scm_puts("\b \b"); line_len--; }
}

static int con_getc(void)
{
    int c;
    if (line_pos < line_len) return (unsigned char)line[line_pos++];
    if (!io->tty) return raw_getc();

    line_len = line_pos = 0;
    scm_puts(at_toplevel ? "> " : "  ");
    for (;;) {
        c = raw_getc();
        if (c < 0) return -1;
        if (c == '\n' && last_cr) { last_cr = 0; continue; }
        last_cr = (c == '\r');
        if (c == '\r' || c == '\n') {
            outc('\n');
            if (line_len > 0) { memcpy(prev_line, line, (size_t)line_len); prev_line[line_len] = 0; }
            line[line_len++] = '\n';
            break;
        }
        if (c == 8 || c == 127) {
            if (line_len > 0) { line_len--; scm_puts("\b \b"); }
            continue;
        }
        if (c == 3) {                       /* ^C: abandon input */
            scm_puts("^C");
            line_len = 0;
            scm_error("interrupted", NULL);
        }
        if (c == 21) { erase_line(); continue; }   /* ^U */
        if (c == 27) {                      /* escape sequences */
            int c2 = raw_getc(), c3;
            if (c2 != '[' && c2 != 'O') continue;
            do { c3 = raw_getc(); } while (c3 >= 0 && !(c3 >= 0x40 && c3 <= 0x7e));
            if (c3 == 'A') {                /* up arrow: recall previous line */
                int n = (int)strlen(prev_line);
                erase_line();
                memcpy(line, prev_line, (size_t)n);
                line_len = n;
                for (n = 0; n < line_len; n++) outc((unsigned char)line[n]);
            }
            continue;
        }
        if (c < 32 && c != '\t') continue;
        if (line_len < SCM_LINE_MAX - 1) { line[line_len++] = (char)c; outc(c); }
        else outc(7);
    }
    return (unsigned char)line[line_pos++];
}

/* ------------------------------------------------------------------ reader */

static int rd_getc(void)
{
    if (rd_nunget > 0) return rd_unget[--rd_nunget];
    if (rd_str) return *rd_str ? (unsigned char)*rd_str++ : -1;
    return con_getc();
}

static void rd_ungetc(int c) { if (rd_nunget < 4) rd_unget[rd_nunget++] = c; }

static int is_delim(int c)
{
    return c < 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
        || c == '(' || c == ')' || c == '[' || c == ']' || c == '"' || c == ';' || c == '\'';
}

static int skip_ws(void)
{
    int c;
    for (;;) {
        c = rd_getc();
        if (c == ';') { while (c >= 0 && c != '\n') c = rd_getc(); continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') continue;
        if (c == '#') {
            int c2 = rd_getc();
            if (c2 == '|') {                 /* block comment */
                int prev = 0;
                for (;;) {
                    c = rd_getc();
                    if (c < 0) return c;
                    if (prev == '|' && c == '#') break;
                    prev = c;
                }
                continue;
            }
            rd_ungetc(c2);
            /* c2 is held in rd_unget; '#' is returned to the caller */
        }
        return c;
    }
}

static int digit_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* parse a number; returns NULL if tok is not numeric */
static scm_obj parse_number(const char *tok, int radix)
{
    const char *p = tok;
    int neg = 0, any = 0, overflow = 0;
    int64_t v = 0;
    double dv = 0;

    while (p[0] == '#') {
        switch (p[1]) {
        case 'x': case 'X': radix = 16; break;
        case 'b': case 'B': radix = 2; break;
        case 'o': case 'O': radix = 8; break;
        case 'd': case 'D': radix = 10; break;
        case 'e': case 'E': case 'i': case 'I': break;
        default: return NULL;
        }
        p += 2;
    }
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if (!*p) return NULL;
    for (; *p; p++) {
        int d = digit_val((unsigned char)*p);
        if (d >= radix) break;
        any = 1;
        if (!overflow && (__builtin_mul_overflow(v, (int64_t)radix, &v) || __builtin_add_overflow(v, (int64_t)d, &v)))
            overflow = 1;
        dv = dv * radix + d;
    }
    if (!*p && any) {
        if (overflow) return make_flt((scm_flt)(neg ? -dv : dv));
        return scm_from_int(neg ? -v : v);
    }
    if (radix != 10) return NULL;
    {   /* decimal float */
        char *end;
        double f;
        p = tok;
        while (p[0] == '#') p += 2;
        if (!strcmp(p, "+inf.0")) return make_flt((scm_flt)HUGE_VAL);
        if (!strcmp(p, "-inf.0")) return make_flt((scm_flt)-HUGE_VAL);
        if (!strcmp(p, "+nan.0") || !strcmp(p, "-nan.0")) return make_flt((scm_flt)NAN);
        if (!((p[0] >= '0' && p[0] <= '9') || p[0] == '.' || p[0] == '+' || p[0] == '-')) return NULL;
        f = strtod(p, &end);
        if (end == p || *end) return NULL;
        return make_flt((scm_flt)f);
    }
}

static scm_obj read_datum(void);

static scm_obj read_list(int close)
{
    lbuf b;
    int c;
    lb_init(&b);
    for (;;) {
        c = skip_ws();
        if (c < 0) scm_error("unexpected end of input", NULL);
        if (c == ')' || c == ']') {
            if (c != close) scm_error("mismatched parenthesis", NULL);
            return b.head;
        }
        if (c == '.') {
            int c2 = rd_getc();
            if (is_delim(c2)) {
                rd_ungetc(c2);
                if (b.head == NIL) scm_error("bad dotted list", NULL);
                lb_tail(&b, read_datum());
                c = skip_ws();
                if (c != close) scm_error("bad dotted list", NULL);
                return b.head;
            }
            rd_ungetc(c2);
            /* fall into token reading with the '.' already consumed */
            {
                char tok[SCM_TOKEN_MAX];
                int n = 0;
                tok[n++] = '.';
                for (;;) {
                    c = rd_getc();
                    if (is_delim(c)) { rd_ungetc(c); break; }
                    if (n < SCM_TOKEN_MAX - 1) tok[n++] = (char)c;
                }
                tok[n] = 0;
                {
                    scm_obj num = parse_number(tok, 10);
                    lb_add(&b, num ? num : intern_n(tok, (size_t)n));
                }
            }
            continue;
        }
        rd_ungetc(c);
        lb_add(&b, read_datum());
    }
}

static scm_obj read_string_lit(void)
{
    char buf[SCM_TOKEN_MAX];
    int n = 0, c;
    for (;;) {
        c = rd_getc();
        if (c < 0) scm_error("unterminated string", NULL);
        if (c == '"') break;
        if (c == '\\') {
            c = rd_getc();
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'a': c = 7; break;
            case '0': c = 0; break;
            case 'x': {
                int v = 0, d;
                while ((c = rd_getc()) >= 0 && c != ';' && (d = digit_val(c)) < 16) v = v * 16 + d;
                c = v;
                break;
            }
            case '\n':      /* line continuation */
                while ((c = rd_getc()) == ' ' || c == '\t') { }
                rd_ungetc(c);
                continue;
            default: break;
            }
        }
        if (n >= SCM_TOKEN_MAX - 1) scm_error("string literal too long", NULL);
        buf[n++] = (char)c;
    }
    return make_string(buf, (size_t)n);
}

static scm_obj read_datum(void)
{
    char tok[SCM_TOKEN_MAX];
    int n = 0;
    int c = skip_ws();
    scm_obj o;

    if (c < 0) return EOF_OBJ;
    at_toplevel = 0;
    switch (c) {
    case '(': return read_list(')');
    case '[': return read_list(']');
    case ')': case ']': scm_error("unexpected ')'", NULL);
    case '\'': o = read_datum(); return cons(s_quote, cons(o, NIL));
    case '`':  o = read_datum(); return cons(s_quasi, cons(o, NIL));
    case ',': {
        int c2 = rd_getc();
        scm_obj s = s_unquote;
        if (c2 == '@') s = s_unquote_splicing; else rd_ungetc(c2);
        o = read_datum();
        return cons(s, cons(o, NIL));
    }
    case '"': return read_string_lit();
    case '#': {
        int c2 = rd_getc();
        if (c2 == '(') {
            scm_obj l = read_list(')'), v, p;
            uint32_t len = 0, i;
            for (p = l; p != NIL; p = CDR(p)) len++;
            v = alloc(T_VEC);
            v->u.vec.v = xmalloc(len * sizeof(scm_obj));
            for (i = 0, p = l; i < len; i++, p = CDR(p)) v->u.vec.v[i] = CAR(p);
            v->u.vec.len = len;
            return v;
        }
        if (c2 == '\\') {
            int i;
            c = rd_getc();
            tok[n++] = (char)c;
            for (;;) {
                c = rd_getc();
                if (is_delim(c)) { rd_ungetc(c); break; }
                if (n < SCM_TOKEN_MAX - 1) tok[n++] = (char)c;
            }
            tok[n] = 0;
            if (n == 1) return make_char((unsigned char)tok[0]);
            for (i = 0; char_names[i][0]; i++)
                if (!strcmp(tok, char_names[i][0])) return make_char((unsigned char)char_names[i][1][0]);
            if (tok[0] == 'x') {
                int v = 0, d;
                for (i = 1; i < n && (d = digit_val((unsigned char)tok[i])) < 16; i++) v = v * 16 + d;
                if (i == n) return make_char(v);
            }
            scm_error("unknown character name", scm_from_cstr(tok));
        }
        if (c2 == ';') { read_datum(); return read_datum(); }   /* datum comment */
        tok[n++] = '#';
        c = c2;
        break;
    }
    default: break;
    }

    /* atom */
    for (;;) {
        if (n < SCM_TOKEN_MAX - 1) tok[n++] = (char)c;
        c = rd_getc();
        if (is_delim(c)) { rd_ungetc(c); break; }
    }
    tok[n] = 0;
    if (!strcmp(tok, "#t") || !strcmp(tok, "#true")) return TRUE_;
    if (!strcmp(tok, "#f") || !strcmp(tok, "#false")) return FALSE_;
    o = parse_number(tok, 10);
    if (o) return o;
    if (tok[0] == '#') scm_error("bad # syntax", scm_from_cstr(tok));
    return intern_n(tok, (size_t)n);
}

/* ------------------------------------------------------------------ environments */

static scm_obj lookup_binding(scm_obj sym, scm_obj env)
{
    scm_obj b;
    for (; env != NIL; env = CDR(env))
        for (b = CAR(env); b != NIL; b = CDR(b))
            if (CAR(CAR(b)) == sym) return CAR(b);
    return NULL;
}

static scm_obj lookup(scm_obj sym, scm_obj env)
{
    scm_obj b = lookup_binding(sym, env);
    if (b) return CDR(b);
    if (sym->u.sym.val == UNBOUND) scm_error("unbound variable:", sym);
    return sym->u.sym.val;
}

static void define_var(scm_obj sym, scm_obj val, scm_obj env)
{
    scm_obj b;
    if (env == NIL) { sym->u.sym.val = val; return; }
    for (b = CAR(env); b != NIL; b = CDR(b))
        if (CAR(CAR(b)) == sym) { CDR(CAR(b)) = val; return; }
    CAR(env) = cons(cons(sym, val), CAR(env));
}

/*
 * Make the frame for a call. With fresh != 0 the argument list was just built
 * by the evaluator and nothing else refers to it, so its cells become the
 * frame's binding list in place: (v1 v2 ...) -> ((p1 . v1) (p2 . v2) ...),
 * one new cell per parameter instead of two. Lists that may be shared (from
 * apply, or macro arguments, which are source code) are never modified.
 */
static scm_obj bind_args(scm_obj params, scm_obj args, scm_obj env, scm_obj fn, int fresh)
{
    scm_obj frame = cons(NIL, env);
    int g = gctop;
    if (fresh) {
        scm_obj a = args, last = NIL;
        while (is_pair(params)) {
            if (!is_pair(a)) scm_error("too few arguments to", fn);
            CAR(a) = cons(CAR(params), CAR(a));     /* reachable through args */
            gctop = g;
            last = a;
            params = CDR(params);
            a = CDR(a);
        }
        if (params != NIL) {                         /* rest parameter gets the remaining list */
            scm_obj tail = cons(cons(params, a), NIL);
            gctop = g;
            if (last == NIL) CAR(frame) = tail;
            else { CDR(last) = tail; CAR(frame) = args; }
        } else {
            if (a != NIL) scm_error("too many arguments to", fn);
            CAR(frame) = last == NIL ? NIL : args;
        }
        return frame;
    }
    while (is_pair(params)) {
        if (!is_pair(args)) scm_error("too few arguments to", fn);
        CAR(frame) = cons(cons(CAR(params), CAR(args)), CAR(frame));
        gctop = g;
        params = CDR(params);
        args = CDR(args);
    }
    if (params != NIL) {
        CAR(frame) = cons(cons(params, args), CAR(frame));
        gctop = g;
    } else if (args != NIL) {
        scm_error("too many arguments to", fn);
    }
    return frame;
}

/* ------------------------------------------------------------------ evaluator */

static scm_obj call_prim(scm_obj fn, scm_obj args)
{
    int min = fn->aux & 0xff, max = fn->aux >> 8, n = 0;
    scm_obj a;
    for (a = args; is_pair(a); a = CDR(a)) n++;
    if (n < min || (max != SCM_VARIADIC && n > max))
        scm_error("wrong number of arguments to", fn);
    return fn->u.prim.fn(args);
}

static scm_obj eval_body(scm_obj body, scm_obj env)
{
    scm_obj v = UNSPEC;
    int g = gctop;
    for (; is_pair(body); body = CDR(body)) {
        gctop = g;
        v = eval(CAR(body), env);
    }
    return v;
}

static scm_obj apply(scm_obj fn, scm_obj args)
{
    int g = gctop;
    scm_obj v;
    if (FIXNUMP(fn)) scm_error("not a procedure:", fn);
    if (fn->type == T_PRIM) v = call_prim(fn, args);
    else if (fn->type == T_CLOS) {
        scm_obj env = bind_args(CAR(fn->u.clos.code), args, fn->u.clos.env, fn, 0);
        v = eval_body(CDR(fn->u.clos.code), env);
    } else scm_error("not a procedure:", fn);
    gctop = g;
    push(v);
    return v;
}

/*
 * Evaluate x, handling leaves (numbers, variables, self-evaluating data) in
 * line instead of paying for a full eval() call. Like eval(), the result is
 * left on the gc stack (except fixnums, which aren't heap objects): a later
 * argument could set! the variable it came from.
 */
static inline scm_obj eval_leaf(scm_obj x, scm_obj env)
{
    scm_obj v;
    if (FIXNUMP(x)) return x;
    if (x->type == T_PAIR) return eval(x, env);
    v = x->type == T_SYM ? lookup(x, env) : x;
    push(v);
    return v;
}

static scm_obj evlist(scm_obj l, scm_obj env)
{
    lbuf b;
    lb_init(&b);
    for (; is_pair(l); l = CDR(l)) lb_add(&b, eval_leaf(CAR(l), env));
    if (l != NIL) scm_error("bad argument list", l);
    return b.head;
}

static scm_obj list2(scm_obj a, scm_obj b) { return cons(a, cons(b, NIL)); }

/* quasiquote with proper nesting depth */
static scm_obj qq(scm_obj x, scm_obj env, int depth)
{
    lbuf b;
    scm_obj p;
    if (!is_pair(x)) return x;
    if (CAR(x) == s_unquote) {
        if (depth == 1) return eval(car_s(CDR(x)), env);
        return list2(s_unquote, qq(car_s(CDR(x)), env, depth - 1));
    }
    if (CAR(x) == s_unquote_splicing && depth > 1)
        return list2(s_unquote_splicing, qq(car_s(CDR(x)), env, depth - 1));
    if (CAR(x) == s_quasi)
        return list2(s_quasi, qq(car_s(CDR(x)), env, depth + 1));
    lb_init(&b);
    for (p = x; is_pair(p); p = CDR(p)) {
        scm_obj item = CAR(p);
        if (item == s_unquote) {            /* `(a . ,b) */
            lb_tail(&b, depth == 1 ? eval(car_s(CDR(p)), env) : qq(p, env, depth));
            return b.head;
        }
        if (depth == 1 && is_pair(item) && CAR(item) == s_unquote_splicing) {
            scm_obj l = eval(car_s(CDR(item)), env);
            b.g = gctop;                    /* keep l rooted while copying it */
            for (; is_pair(l); l = CDR(l)) lb_add(&b, CAR(l));
            continue;
        }
        lb_add(&b, qq(item, env, depth));
    }
    if (p != NIL) lb_tail(&b, p);
    return b.head;
}

static int eqv(scm_obj a, scm_obj b)
{
    if (a == b) return 1;
    if (FIXNUMP(a) || FIXNUMP(b)) return 0;
    if (a->type != b->type) return 0;
    switch (a->type) {
    case T_INT: return int_val(a) == int_val(b);
    case T_FLT: return a->u.f == b->u.f;
    case T_CHAR: return a->u.ch == b->u.ch;
    default: return 0;
    }
}

static int equal(scm_obj a, scm_obj b)
{
    uint32_t i;
    check_stack();
    for (;;) {
        if (eqv(a, b)) return 1;
        if (FIXNUMP(a) || FIXNUMP(b) || a->type != b->type) return 0;
        switch (a->type) {
        case T_STR:
            return a->u.str.len == b->u.str.len && !memcmp(a->u.str.s, b->u.str.s, a->u.str.len);
        case T_VEC:
            if (a->u.vec.len != b->u.vec.len) return 0;
            for (i = 0; i < a->u.vec.len; i++) if (!equal(a->u.vec.v[i], b->u.vec.v[i])) return 0;
            return 1;
        case T_PAIR:
            if (!equal(CAR(a), CAR(b))) return 0;
            a = CDR(a); b = CDR(b);
            continue;
        default: return 0;
        }
    }
}

/*
 * Fast path for the commonest primitives called with one or two arguments:
 * the evaluator passes the evaluated arguments in C variables instead of
 * consing an argument list. Returns NULL when the case isn't covered (wrong
 * types, overflow into floats, ...); the caller then builds the list and
 * calls the primitive normally, so results and errors are unchanged.
 */
static scm_obj p_nullp(scm_obj a);
static scm_obj p_pairp(scm_obj a);
static scm_obj p_not(scm_obj a);
static scm_obj p_zerop(scm_obj a);
static scm_obj p_lt(scm_obj a);
static scm_obj p_gt(scm_obj a);
static scm_obj p_le(scm_obj a);
static scm_obj p_ge(scm_obj a);
static scm_obj p_numeq(scm_obj a);
static scm_obj p_add(scm_obj a);
static scm_obj p_sub(scm_obj a);
static scm_obj p_mul(scm_obj a);
static scm_obj p_eq(scm_obj a);
static scm_obj p_cons(scm_obj a);
static scm_obj p_car(scm_obj a);
static scm_obj p_cdr(scm_obj a);

static scm_obj prim_fast(scm_prim_fn f, int n, scm_obj a, scm_obj b)
{
    if (n == 1) {
        if (f == p_car)   return is_pair(a) ? CAR(a) : NULL;
        if (f == p_cdr)   return is_pair(a) ? CDR(a) : NULL;
        if (f == p_nullp) return a == NIL ? TRUE_ : FALSE_;
        if (f == p_pairp) return is_pair(a) ? TRUE_ : FALSE_;
        if (f == p_not)   return a == FALSE_ ? TRUE_ : FALSE_;
        if (f == p_zerop) return FIXNUMP(a) ? (a == FIX_MAKE(0) ? TRUE_ : FALSE_) : NULL;
        return NULL;
    }
    if (f == p_cons) return cons(a, b);
    if (f == p_eq) {                    /* same object, or equal integers */
        if (a == b) return TRUE_;
        if (!is_int(a) || !is_int(b)) return FALSE_;
        return FIXNUMP(a) && FIXNUMP(b) ? FALSE_ : NULL;   /* boxed: compare values */
    }
    if (FIXNUMP(a) && FIXNUMP(b)) {
        int32_t x = FIX_VAL(a), y = FIX_VAL(b);
        if (f == p_add)   return scm_from_int((int64_t)x + y);
        if (f == p_sub)   return scm_from_int((int64_t)x - y);
        if (f == p_lt)    return x <  y ? TRUE_ : FALSE_;
        if (f == p_numeq) return x == y ? TRUE_ : FALSE_;
        if (f == p_gt)    return x >  y ? TRUE_ : FALSE_;
        if (f == p_le)    return x <= y ? TRUE_ : FALSE_;
        if (f == p_ge)    return x >= y ? TRUE_ : FALSE_;
        if (f == p_mul)   return scm_from_int((int64_t)x * y);
    }
    return NULL;
}

#define RET(v) do { scm_obj r_ = (v); gctop = gc; push(r_); return r_; } while (0)


static scm_obj eval(scm_obj x, scm_obj env)
{
    int gc = gctop, g1;
    scm_obj op, fn, args, body, rest;

    check_stack();
tail:
    gctop = gc;
    push(x);
    push(env);
    if (++poll_count >= SCM_POLL_INTERVAL) { poll_count = 0; poll_break(); }

    if (FIXNUMP(x)) RET(x);
    if (x->type == T_SYM) RET(lookup(x, env));
    if (x->type != T_PAIR) RET(x);

    op = CAR(x);
    rest = CDR(x);
    g1 = gctop;

    if (is_sym(op) && op->aux) {
        switch (op->aux) {
        case SF_QUOTE:
            RET(car_s(rest));

        case SF_QUASI:
            RET(qq(car_s(rest), env, 1));

        case SF_UNQUOTE:
        case SF_UNQUOTE_SPLICING:
            scm_error("unquote outside quasiquote", x);

        case SF_IF: {
            scm_obj c = eval_leaf(car_s(rest), env);
            rest = cdr_s(rest);
            if (c != FALSE_) { x = car_s(rest); goto tail; }
            rest = CDR(rest);
            if (!is_pair(rest)) RET(UNSPEC);
            x = CAR(rest);
            goto tail;
        }

        case SF_DEFINE: {
            scm_obj target = car_s(rest), val;
            if (is_pair(target)) {                  /* (define (name . params) body...) */
                val = make_closure(CDR(target), CDR(rest), env, T_CLOS);
                target = CAR(target);
            } else {
                rest = CDR(rest);
                val = is_pair(rest) ? eval(CAR(rest), env) : UNSPEC;
            }
            define_var(check(target, T_SYM, "bad define:"), val, env);
            RET(target);
        }

        case SF_DEFMACRO: {
            scm_obj target = car_s(rest), m;
            if (is_pair(target)) {
                m = make_closure(CDR(target), cdr_s(rest), env, T_MACRO);
                target = CAR(target);
            } else {
                scm_obj f = eval(car_s(cdr_s(rest)), env);
                if (type_of(f) != T_CLOS) scm_error("define-macro needs a procedure:", f);
                m = make_closure(CAR(f->u.clos.code), CDR(f->u.clos.code), f->u.clos.env, T_MACRO);
            }
            define_var(check(target, T_SYM, "bad define-macro:"), m, env);
            RET(target);
        }

        case SF_SET: {
            scm_obj sym = check(car_s(rest), T_SYM, "set!: not a symbol:"), b, val;
            val = eval(car_s(cdr_s(rest)), env);
            b = lookup_binding(sym, env);
            if (b) CDR(b) = val;
            else if (sym->u.sym.val != UNBOUND) sym->u.sym.val = val;
            else scm_error("set!: unbound variable:", sym);
            RET(UNSPEC);
        }

        case SF_LAMBDA:
            RET(make_closure(car_s(rest), cdr_s(rest), env, T_CLOS));

        case SF_BEGIN:
            body = rest;
            goto seq;

        case SF_LET: {
            scm_obj first = car_s(rest), frame, b;
            if (is_sym(first)) {                    /* named let */
                lbuf pl, al;
                scm_obj name = first, env1, clos;
                rest = cdr_s(rest);
                lb_init(&pl);
                for (b = car_s(rest); is_pair(b); b = CDR(b)) lb_add(&pl, car_s(CAR(b)));
                lb_init(&al);
                for (b = car_s(rest); is_pair(b); b = CDR(b))
                    lb_add(&al, eval(car_s(cdr_s(CAR(b))), env));
                env1 = cons(NIL, env);
                clos = make_closure(pl.head, CDR(rest), env1, T_CLOS);
                CAR(env1) = cons(cons(name, clos), NIL);
                fn = clos;
                args = al.head;
                goto do_apply;
            }
            frame = cons(NIL, env);
            {
                int g = gctop;
                for (b = first; is_pair(b); b = CDR(b)) {
                    scm_obj bind = CAR(b), v;
                    if (is_pair(bind)) { v = is_pair(CDR(bind)) ? eval(CAR(CDR(bind)), env) : UNSPEC; bind = CAR(bind); }
                    else v = UNSPEC;
                    CAR(frame) = cons(cons(bind, v), CAR(frame));
                    gctop = g;
                }
            }
            env = frame;
            body = CDR(rest);
            goto seq;
        }

        case SF_LETSTAR:
        case SF_LETREC: {
            scm_obj frame = cons(NIL, env), b;
            int g = gctop;
            if (op->aux == SF_LETREC)
                for (b = car_s(rest); is_pair(b); b = CDR(b)) {
                    CAR(frame) = cons(cons(car_s(CAR(b)), UNSPEC), CAR(frame));
                    gctop = g;
                }
            for (b = car_s(rest); is_pair(b); b = CDR(b)) {
                scm_obj bind = CAR(b), v;
                if (is_pair(bind)) { v = is_pair(CDR(bind)) ? eval(CAR(CDR(bind)), frame) : UNSPEC; bind = CAR(bind); }
                else v = UNSPEC;
                define_var(bind, v, frame);
                gctop = g;
            }
            env = frame;
            body = CDR(rest);
            goto seq;
        }

        case SF_COND:
            for (; is_pair(rest); rest = CDR(rest)) {
                scm_obj clause = car_s(rest), v;
                gctop = g1;
                if (car_s(clause) == s_else) { body = CDR(clause); goto seq; }
                v = eval(CAR(clause), env);
                if (v != FALSE_) {
                    scm_obj b = CDR(clause);
                    if (!is_pair(b)) RET(v);
                    if (CAR(b) == s_arrow) {
                        fn = eval(car_s(CDR(b)), env);
                        args = cons(v, NIL);
                        goto do_apply;
                    }
                    body = b;
                    goto seq;
                }
            }
            RET(UNSPEC);

        case SF_CASE: {
            scm_obj key = eval(car_s(rest), env);
            for (rest = CDR(rest); is_pair(rest); rest = CDR(rest)) {
                scm_obj clause = car_s(rest), d;
                if (car_s(clause) == s_else) { body = CDR(clause); goto seq; }
                for (d = CAR(clause); is_pair(d); d = CDR(d))
                    if (eqv(CAR(d), key)) { body = CDR(clause); goto seq; }
            }
            RET(UNSPEC);
        }

        case SF_AND:
            if (!is_pair(rest)) RET(TRUE_);
            for (; is_pair(CDR(rest)); rest = CDR(rest)) {
                gctop = g1;
                if (eval(CAR(rest), env) == FALSE_) RET(FALSE_);
            }
            x = CAR(rest);
            goto tail;

        case SF_OR:
            if (!is_pair(rest)) RET(FALSE_);
            for (; is_pair(CDR(rest)); rest = CDR(rest)) {
                scm_obj v;
                gctop = g1;
                v = eval(CAR(rest), env);
                if (v != FALSE_) RET(v);
            }
            x = CAR(rest);
            goto tail;

        case SF_WHEN:
        case SF_UNLESS: {
            scm_obj v = eval(car_s(rest), env);
            if ((v != FALSE_) != (op->aux == SF_WHEN)) RET(UNSPEC);
            body = CDR(rest);
            goto seq;
        }
        default:
            break;
        }
    }

    fn = eval_leaf(op, env);
    if (!FIXNUMP(fn) && fn->type == T_MACRO) {
        scm_obj menv = bind_args(CAR(fn->u.clos.code), rest, fn->u.clos.env, fn, 0);
        scm_obj exp = eval_body(CDR(fn->u.clos.code), menv);
        if (is_pair(exp)) { CAR(x) = CAR(exp); CDR(x) = CDR(exp); }
        else { CAR(x) = s_begin; CDR(x) = cons(exp, NIL); }
        goto tail;
    }
    if (!FIXNUMP(fn) && fn->type == T_PRIM && is_pair(rest)
        && (CDR(rest) == NIL || (is_pair(CDR(rest)) && CDR(CDR(rest)) == NIL))) {
        /* one or two arguments: try the fast path before consing a list */
        int n = CDR(rest) == NIL ? 1 : 2;
        scm_obj a0 = eval_leaf(CAR(rest), env), a1 = NIL, v;   /* stays rooted */
        if (n == 2) a1 = eval_leaf(CAR(CDR(rest)), env);
        v = prim_fast(fn->u.prim.fn, n, a0, a1);
        if (v) RET(v);
        args = n == 1 ? cons(a0, NIL) : cons(a0, cons(a1, NIL));
        RET(call_prim(fn, args));
    }
    args = evlist(rest, env);

do_apply:
    if (FIXNUMP(fn)) scm_error("not a procedure:", fn);
    if (fn->type == T_PRIM) RET(call_prim(fn, args));
    if (fn->type != T_CLOS) scm_error("not a procedure:", fn);
    env = bind_args(CAR(fn->u.clos.code), args, fn->u.clos.env, fn, 1);   /* args are fresh here */
    body = CDR(fn->u.clos.code);

seq:
    if (!is_pair(body)) RET(UNSPEC);
    {
        int g2 = gctop;
        while (is_pair(CDR(body))) {
            eval(CAR(body), env);
            gctop = g2;
            body = CDR(body);
        }
    }
    x = CAR(body);
    goto tail;
}

/* ------------------------------------------------------------------ primitives: numbers */

static scm_obj arith(int op, scm_obj args)
{
    scm_obj a = scm_arg(&args);
    int64_t acc = 0;
    scm_flt facc = 0;
    int isf;

    if (!is_num(a)) scm_error("not a number:", a);
    isf = !is_int(a);
    if (isf) facc = a->u.f; else acc = int_val(a);
    if (args == NIL && op == '-') {
        if (isf) return make_flt(-facc);
        if (acc == INT64_MIN) return make_flt(-(scm_flt)acc);
        return scm_from_int(-acc);
    }
    while (args != NIL) {
        scm_obj b = scm_arg(&args);
        if (!is_num(b)) scm_error("not a number:", b);
        if (!isf && is_int(b)) {
            int64_t bv = int_val(b), r;
            int ovf = op == '+' ? __builtin_add_overflow(acc, bv, &r)
                    : op == '-' ? __builtin_sub_overflow(acc, bv, &r)
                    : __builtin_mul_overflow(acc, bv, &r);
            if (!ovf) { acc = r; continue; }
            isf = 1; facc = (scm_flt)acc;
        }
        if (!isf) { isf = 1; facc = (scm_flt)acc; }
        {
            scm_flt bv = to_flt(b);
            facc = op == '+' ? facc + bv : op == '-' ? facc - bv : facc * bv;
        }
    }
    return isf ? make_flt(facc) : scm_from_int(acc);
}

static scm_obj p_add(scm_obj a) { return a == NIL ? FIX_MAKE(0) : arith('+', a); }
static scm_obj p_sub(scm_obj a) { return arith('-', a); }
static scm_obj p_mul(scm_obj a) { return a == NIL ? FIX_MAKE(1) : arith('*', a); }

static scm_obj p_div(scm_obj args)
{
    scm_obj a = scm_arg(&args), b;
    if (args == NIL) { b = a; a = FIX_MAKE(1); }
    else b = scm_arg(&args);
    for (;;) {
        if (is_int(a) && is_int(b)) {
            int64_t x = int_val(a), y = int_val(b);
            if (y == 0) scm_error("division by zero", NULL);
            if (x % y == 0 && !(x == INT64_MIN && y == -1)) a = scm_from_int(x / y);
            else a = make_flt((scm_flt)x / (scm_flt)y);
        } else {
            a = make_flt(to_flt(a) / to_flt(b));
        }
        if (args == NIL) return a;
        b = scm_arg(&args);
    }
}

static scm_obj intdiv(scm_obj args, int kind)
{
    scm_obj ao = scm_arg(&args), bo = scm_arg(&args);
    int64_t a, b, r;
    if (!is_int(ao) || !is_int(bo)) {           /* allow integral floats */
        scm_flt x = to_flt(ao), y = to_flt(bo), q;
        if (y == 0) scm_error("division by zero", NULL);
        q = F(trunc)(x / y);
        if (kind == 'q') return make_flt(q);
        {
            scm_flt rem = x - q * y;
            if (kind == 'm' && rem != 0 && ((rem < 0) != (y < 0))) rem += y;
            return make_flt(rem);
        }
    }
    a = int_val(ao); b = int_val(bo);
    if (b == 0) scm_error("division by zero", NULL);
    if (b == -1) return kind == 'q' ? scm_from_int(a == INT64_MIN ? a : -a) : FIX_MAKE(0);
    if (kind == 'q') return scm_from_int(a / b);
    r = a % b;
    if (kind == 'm' && r != 0 && ((r < 0) != (b < 0))) r += b;
    return scm_from_int(r);
}

static scm_obj p_quotient(scm_obj a)  { return intdiv(a, 'q'); }
static scm_obj p_remainder(scm_obj a) { return intdiv(a, 'r'); }
static scm_obj p_modulo(scm_obj a)    { return intdiv(a, 'm'); }

static int numcmp(scm_obj a, scm_obj b)
{
    if (is_int(a) && is_int(b)) {
        int64_t x = int_val(a), y = int_val(b);
        return x < y ? -1 : x > y;
    } else {
        scm_flt x = to_flt(a), y = to_flt(b);
        return x < y ? -1 : x > y;
    }
}

static scm_obj compare(scm_obj args, int op)
{
    scm_obj a = scm_arg(&args);
    if (!is_num(a)) scm_error("not a number:", a);
    while (args != NIL) {
        scm_obj b = scm_arg(&args);
        int c;
        if (!is_num(b)) scm_error("not a number:", b);
        c = numcmp(a, b);
        if (!(op == '=' ? c == 0 : op == '<' ? c < 0 : op == '>' ? c > 0 : op == 'l' ? c <= 0 : c >= 0))
            return FALSE_;
        a = b;
    }
    return TRUE_;
}

static scm_obj p_numeq(scm_obj a) { return compare(a, '='); }
static scm_obj p_lt(scm_obj a)    { return compare(a, '<'); }
static scm_obj p_gt(scm_obj a)    { return compare(a, '>'); }
static scm_obj p_le(scm_obj a)    { return compare(a, 'l'); }
static scm_obj p_ge(scm_obj a)    { return compare(a, 'g'); }

static scm_obj minmax(scm_obj args, int wantmax)
{
    scm_obj best = scm_arg(&args);
    int inexact = !is_int(best);
    if (!is_num(best)) scm_error("not a number:", best);
    while (args != NIL) {
        scm_obj b = scm_arg(&args);
        int c;
        if (!is_num(b)) scm_error("not a number:", b);
        if (!is_int(b)) inexact = 1;
        c = numcmp(b, best);
        if (wantmax ? c > 0 : c < 0) best = b;
    }
    return inexact && is_int(best) ? make_flt(to_flt(best)) : best;
}
static scm_obj p_max(scm_obj a) { return minmax(a, 1); }
static scm_obj p_min(scm_obj a) { return minmax(a, 0); }

static scm_obj p_abs(scm_obj args)
{
    scm_obj a = scm_arg(&args);
    if (is_int(a)) { int64_t v = int_val(a); return v < 0 ? (v == INT64_MIN ? make_flt(-(scm_flt)v) : scm_from_int(-v)) : a; }
    return make_flt(F(fabs)(to_flt(a)));
}

static scm_obj rounding(scm_obj args, int kind)
{
    scm_obj a = scm_arg(&args);
    scm_flt f;
    if (is_int(a)) return a;
    f = to_flt(a);
    switch (kind) {
    case 'f': f = F(floor)(f); break;
    case 'c': f = F(ceil)(f); break;
    case 't': f = F(trunc)(f); break;
    default:  f = F(rint)(f); break;    /* round half to even */
    }
    return make_flt(f);
}
static scm_obj p_floor(scm_obj a)    { return rounding(a, 'f'); }
static scm_obj p_ceiling(scm_obj a)  { return rounding(a, 'c'); }
static scm_obj p_truncate(scm_obj a) { return rounding(a, 't'); }
static scm_obj p_round(scm_obj a)    { return rounding(a, 'r'); }

static scm_obj p_exact(scm_obj args)
{
    scm_obj a = scm_arg(&args);
    scm_flt f;
    if (is_int(a)) return a;
    f = to_flt(a);
    if (f != f || f > 9.2e18f || f < -9.2e18f) scm_error("exact: no exact representation:", a);
    return scm_from_int((int64_t)F(rint)(f));
}

static scm_obj p_inexact(scm_obj args) { return make_flt(to_flt(scm_arg(&args))); }

static scm_obj p_sqrt(scm_obj args)
{
    scm_obj a = scm_arg(&args);
    if (is_int(a) && int_val(a) >= 0) {
        int64_t v = int_val(a), r = (int64_t)sqrt((double)v);
        while (r * r > v) r--;
        while ((r + 1) * (r + 1) <= v) r++;
        if (r * r == v) return scm_from_int(r);
    }
    return make_flt(F(sqrt)(to_flt(a)));
}

static scm_obj p_expt(scm_obj args)
{
    scm_obj a = scm_arg(&args), b = scm_arg(&args);
    if (is_int(a) && is_int(b) && int_val(b) >= 0) {
        int64_t base = int_val(a), e = int_val(b), r = 1;
        int ovf = 0;
        while (e > 0 && !ovf) {
            if (e & 1) ovf |= __builtin_mul_overflow(r, base, &r);
            e >>= 1;
            if (e) ovf |= __builtin_mul_overflow(base, base, &base);
        }
        if (!ovf) return scm_from_int(r);
    }
    return make_flt(F(pow)(to_flt(a), to_flt(b)));
}

#define MATH1(name, fn) static scm_obj name(scm_obj args) { return make_flt(F(fn)(to_flt(scm_arg(&args)))); }
MATH1(p_exp, exp)
MATH1(p_log, log)
MATH1(p_sin, sin)
MATH1(p_cos, cos)
MATH1(p_tan, tan)
MATH1(p_asin, asin)
MATH1(p_acos, acos)

static scm_obj p_atan(scm_obj args)
{
    scm_flt y = to_flt(scm_arg(&args));
    if (args != NIL) return make_flt(F(atan2)(y, to_flt(scm_arg(&args))));
    return make_flt(F(atan)(y));
}

static scm_obj bitop(scm_obj args, int op)
{
    int64_t acc = op == '&' ? -1 : 0;
    while (args != NIL) {
        int64_t v = scm_to_int(scm_arg(&args));
        acc = op == '&' ? (acc & v) : op == '|' ? (acc | v) : (acc ^ v);
    }
    return scm_from_int(acc);
}
static scm_obj p_logand(scm_obj a) { return bitop(a, '&'); }
static scm_obj p_logior(scm_obj a) { return bitop(a, '|'); }
static scm_obj p_logxor(scm_obj a) { return bitop(a, '^'); }
static scm_obj p_lognot(scm_obj a) { return scm_from_int(~scm_to_int(scm_arg(&a))); }

static scm_obj p_ash(scm_obj args)
{
    int64_t v = scm_to_int(scm_arg(&args)), s = scm_to_int(scm_arg(&args));
    if (s >= 0) {
        int64_t r;
        if (v == 0) return FIX_MAKE(0);
        if (s > 62 || (r = (int64_t)((uint64_t)v << s)) >> s != v) scm_error("ash: overflow", NULL);
        return scm_from_int(r);
    }
    return scm_from_int(s < -63 ? (v < 0 ? -1 : 0) : v >> -s);
}

static scm_obj p_number_to_string(scm_obj args)
{
    char buf[72];
    scm_obj n = scm_arg(&args);
    int radix = args == NIL ? 10 : (int)scm_to_int(scm_arg(&args));
    if (radix < 2 || radix > 36) scm_error("bad radix", NULL);
    if (is_int(n)) fmt_int(buf, int_val(n), radix);
    else fmt_flt(buf, to_flt(n));
    return scm_from_cstr(buf);
}

static scm_obj p_string_to_number(scm_obj args)
{
    scm_obj s = str_arg(&args), r;
    int radix = args == NIL ? 10 : (int)scm_to_int(scm_arg(&args));
    if (radix < 2 || radix > 36) scm_error("bad radix", NULL);
    r = s->u.str.len ? parse_number(s->u.str.s, radix) : NULL;
    return r ? r : FALSE_;
}

/* ------------------------------------------------------------------ primitives: predicates */

#define PRED(name, expr) static scm_obj name(scm_obj args) { scm_obj o = scm_arg(&args); return scm_from_bool(expr); }
PRED(p_nullp, o == NIL)
PRED(p_pairp, is_pair(o))
PRED(p_symbolp, is_sym(o))
PRED(p_stringp, type_of(o) == T_STR)
PRED(p_charp, type_of(o) == T_CHAR)
PRED(p_numberp, is_num(o))
PRED(p_integerp, is_int(o) || (type_of(o) == T_FLT && o->u.f == F(floor)(o->u.f)))
PRED(p_exactp, is_int(o))
PRED(p_inexactp, type_of(o) == T_FLT)
PRED(p_procedurep, type_of(o) == T_PRIM || type_of(o) == T_CLOS)
PRED(p_booleanp, o == TRUE_ || o == FALSE_)
PRED(p_vectorp, type_of(o) == T_VEC)
PRED(p_not, o == FALSE_)
PRED(p_eofp, o == EOF_OBJ)
PRED(p_zerop, to_flt(o) == 0)
PRED(p_positivep, numcmp(o, FIX_MAKE(0)) > 0)
PRED(p_negativep, numcmp(o, FIX_MAKE(0)) < 0)
PRED(p_oddp, (scm_to_int(o) & 1) != 0)
PRED(p_evenp, (scm_to_int(o) & 1) == 0)

static scm_obj p_listp(scm_obj args)
{
    scm_obj slow = scm_arg(&args), fast = slow;
    for (;;) {
        if (fast == NIL) return TRUE_;
        if (!is_pair(fast)) return FALSE_;
        fast = CDR(fast);
        if (fast == NIL) return TRUE_;
        if (!is_pair(fast)) return FALSE_;
        fast = CDR(fast);
        slow = CDR(slow);
        if (fast == slow) return FALSE_;
    }
}

static scm_obj p_eq(scm_obj a)    { scm_obj x = scm_arg(&a), y = scm_arg(&a); return scm_from_bool(x == y || (is_int(x) && is_int(y) && int_val(x) == int_val(y))); }
static scm_obj p_eqv(scm_obj a)   { scm_obj x = scm_arg(&a), y = scm_arg(&a); return scm_from_bool(eqv(x, y)); }
static scm_obj p_equal(scm_obj a) { scm_obj x = scm_arg(&a), y = scm_arg(&a); return scm_from_bool(equal(x, y)); }

/* ------------------------------------------------------------------ primitives: lists */

static scm_obj pair_arg(scm_obj *args) { return check(scm_arg(args), T_PAIR, "not a pair:"); }

static scm_obj p_cons(scm_obj a) { scm_obj x = scm_arg(&a), y = scm_arg(&a); return cons(x, y); }
static scm_obj p_car(scm_obj a)  { return CAR(pair_arg(&a)); }
static scm_obj p_cdr(scm_obj a)  { return CDR(pair_arg(&a)); }
static scm_obj p_setcar(scm_obj a) { scm_obj p = pair_arg(&a); CAR(p) = scm_arg(&a); return UNSPEC; }
static scm_obj p_setcdr(scm_obj a) { scm_obj p = pair_arg(&a); CDR(p) = scm_arg(&a); return UNSPEC; }

static scm_obj cxr(scm_obj args, const char *path)
{
    scm_obj o = scm_arg(&args);
    const char *p = path + strlen(path);
    while (p-- > path) {
        if (!is_pair(o)) scm_error("not a pair:", o);
        o = *p == 'a' ? CAR(o) : CDR(o);
    }
    return o;
}
static scm_obj p_caar(scm_obj a)   { return cxr(a, "aa"); }
static scm_obj p_cadr(scm_obj a)   { return cxr(a, "ad"); }
static scm_obj p_cdar(scm_obj a)   { return cxr(a, "da"); }
static scm_obj p_cddr(scm_obj a)   { return cxr(a, "dd"); }
static scm_obj p_caddr(scm_obj a)  { return cxr(a, "add"); }
static scm_obj p_cdddr(scm_obj a)  { return cxr(a, "ddd"); }
static scm_obj p_cadddr(scm_obj a) { return cxr(a, "addd"); }

static scm_obj p_list(scm_obj a) { return a; }

static long list_length(scm_obj l)
{
    long n = 0;
    scm_obj o = l;
    for (; is_pair(o); o = CDR(o)) n++;
    if (o != NIL) scm_error("not a proper list:", l);
    return n;
}

static scm_obj p_length(scm_obj a) { return scm_from_int(list_length(scm_arg(&a))); }

static scm_obj p_reverse(scm_obj args)
{
    scm_obj l = scm_arg(&args), r = NIL;
    int g = gctop;
    for (; is_pair(l); l = CDR(l)) { r = cons(CAR(l), r); gctop = g; push(r); }
    if (l != NIL) scm_error("reverse: not a proper list", NULL);
    return r;
}

static scm_obj p_append(scm_obj args)
{
    lbuf b;
    lb_init(&b);
    if (args == NIL) return NIL;
    while (CDR(args) != NIL) {
        scm_obj l = CAR(args);
        for (; is_pair(l); l = CDR(l)) lb_add(&b, CAR(l));
        if (l != NIL) scm_error("append: not a proper list", CAR(args));
        args = CDR(args);
    }
    lb_tail(&b, CAR(args));
    return b.head;
}

static scm_obj p_list_tail(scm_obj args)
{
    scm_obj l = scm_arg(&args);
    int64_t k = scm_to_int(scm_arg(&args));
    while (k-- > 0) { if (!is_pair(l)) scm_error("list-tail: index too large", NULL); l = CDR(l); }
    return l;
}

static scm_obj p_list_ref(scm_obj args)
{
    scm_obj l = scm_arg(&args);
    int64_t k = scm_to_int(scm_arg(&args));
    while (k-- > 0 && is_pair(l)) l = CDR(l);
    if (!is_pair(l)) scm_error("list-ref: index too large", NULL);
    return CAR(l);
}

static scm_obj p_list_copy(scm_obj args)
{
    lbuf b;
    scm_obj l = scm_arg(&args);
    lb_init(&b);
    for (; is_pair(l); l = CDR(l)) lb_add(&b, CAR(l));
    if (l != NIL) lb_tail(&b, l);
    return b.head;
}

static scm_obj p_last_pair(scm_obj args)
{
    scm_obj l = pair_arg(&args);
    while (is_pair(CDR(l))) l = CDR(l);
    return l;
}

static scm_obj mem_generic(scm_obj args, int kind)
{
    scm_obj x = scm_arg(&args), l = scm_arg(&args);
    for (; is_pair(l); l = CDR(l))
        if (kind == 'q' ? CAR(l) == x : kind == 'v' ? eqv(CAR(l), x) : equal(CAR(l), x)) return l;
    return FALSE_;
}
static scm_obj p_memq(scm_obj a)   { return mem_generic(a, 'q'); }
static scm_obj p_memv(scm_obj a)   { return mem_generic(a, 'v'); }
static scm_obj p_member(scm_obj a) { return mem_generic(a, 'e'); }

static scm_obj ass_generic(scm_obj args, int kind)
{
    scm_obj x = scm_arg(&args), l = scm_arg(&args);
    for (; is_pair(l); l = CDR(l)) {
        scm_obj e = CAR(l);
        if (!is_pair(e)) continue;
        if (kind == 'q' ? CAR(e) == x : kind == 'v' ? eqv(CAR(e), x) : equal(CAR(e), x)) return e;
    }
    return FALSE_;
}
static scm_obj p_assq(scm_obj a)  { return ass_generic(a, 'q'); }
static scm_obj p_assv(scm_obj a)  { return ass_generic(a, 'v'); }
static scm_obj p_assoc(scm_obj a) { return ass_generic(a, 'e'); }

static scm_obj p_apply(scm_obj args)
{
    scm_obj fn = scm_arg(&args);
    lbuf b;
    lb_init(&b);
    while (is_pair(args) && CDR(args) != NIL) { lb_add(&b, CAR(args)); args = CDR(args); }
    if (is_pair(args)) { list_length(CAR(args)); lb_tail(&b, CAR(args)); }
    return apply(fn, b.head);
}

#define MAX_MAP_LISTS 8
static scm_obj map_generic(scm_obj args, int collect)
{
    scm_obj fn = scm_arg(&args), cur[MAX_MAP_LISTS];
    int n = 0, i;
    lbuf b;
    for (; is_pair(args); args = CDR(args)) {
        if (n == MAX_MAP_LISTS) scm_error("map: too many lists", NULL);
        cur[n++] = CAR(args);
    }
    lb_init(&b);
    for (;;) {
        scm_obj a = NIL, v;
        int g = gctop;
        for (i = 0; i < n; i++) if (!is_pair(cur[i])) return collect ? b.head : UNSPEC;
        for (i = n - 1; i >= 0; i--) a = cons(CAR(cur[i]), a);
        v = apply(fn, a);
        if (collect) lb_add(&b, v);
        else gctop = g;
        for (i = 0; i < n; i++) cur[i] = CDR(cur[i]);
    }
}
static scm_obj p_map(scm_obj a)      { return map_generic(a, 1); }
static scm_obj p_for_each(scm_obj a) { return map_generic(a, 0); }

/* ------------------------------------------------------------------ primitives: symbols, chars, strings */

static scm_obj p_symbol_to_string(scm_obj a) { return scm_from_cstr(check(scm_arg(&a), T_SYM, "not a symbol:")->u.sym.name); }
static scm_obj p_string_to_symbol(scm_obj a) { scm_obj s = str_arg(&a); return intern_n(s->u.str.s, s->u.str.len); }

static scm_obj p_gensym(scm_obj args)
{
    char buf[40];
    const char *prefix = args != NIL ? scm_to_cstr(scm_arg(&args)) : "g";
    size_t n = strlen(prefix);
    if (n > 20) n = 20;
    memcpy(buf, prefix, n);
    fmt_int(buf + n, ++gensym_counter, 10);
    return make_symbol(buf, strlen(buf));       /* uninterned */
}

static scm_obj char_arg(scm_obj *a) { return check(scm_arg(a), T_CHAR, "not a character:"); }

static scm_obj p_char_to_integer(scm_obj a) { return FIX_MAKE(char_arg(&a)->u.ch); }
static scm_obj p_integer_to_char(scm_obj a) { return make_char((int)scm_to_int(scm_arg(&a))); }
static scm_obj p_char_upcase(scm_obj a)   { int c = char_arg(&a)->u.ch; return make_char(c >= 'a' && c <= 'z' ? c - 32 : c); }
static scm_obj p_char_downcase(scm_obj a) { int c = char_arg(&a)->u.ch; return make_char(c >= 'A' && c <= 'Z' ? c + 32 : c); }
static scm_obj p_char_alphap(scm_obj a)   { int c = char_arg(&a)->u.ch; return scm_from_bool((c | 32) >= 'a' && (c | 32) <= 'z'); }
static scm_obj p_char_numericp(scm_obj a) { int c = char_arg(&a)->u.ch; return scm_from_bool(c >= '0' && c <= '9'); }
static scm_obj p_char_whitep(scm_obj a)   { int c = char_arg(&a)->u.ch; return scm_from_bool(c == ' ' || (c >= 9 && c <= 13)); }

static scm_obj charcmp(scm_obj args, int op)
{
    int x = char_arg(&args)->u.ch;
    while (args != NIL) {
        int y = char_arg(&args)->u.ch;
        if (!(op == '=' ? x == y : op == '<' ? x < y : x > y)) return FALSE_;
        x = y;
    }
    return TRUE_;
}
static scm_obj p_char_eq(scm_obj a) { return charcmp(a, '='); }
static scm_obj p_char_lt(scm_obj a) { return charcmp(a, '<'); }
static scm_obj p_char_gt(scm_obj a) { return charcmp(a, '>'); }

static scm_obj p_string_length(scm_obj a) { return FIX_MAKE(str_arg(&a)->u.str.len); }

static scm_obj p_string_ref(scm_obj a)
{
    scm_obj s = str_arg(&a);
    long i = index_arg(&a, (long)s->u.str.len - 1);
    return make_char((unsigned char)s->u.str.s[i]);
}

static scm_obj p_string_set(scm_obj a)
{
    scm_obj s = str_arg(&a);
    long i = index_arg(&a, (long)s->u.str.len - 1);
    s->u.str.s[i] = (char)char_arg(&a)->u.ch;
    return UNSPEC;
}

static scm_obj p_make_string(scm_obj a)
{
    long n = index_arg(&a, 1L << 20);
    scm_obj s = make_string(NULL, (size_t)n);
    if (a != NIL) memset(s->u.str.s, char_arg(&a)->u.ch, (size_t)n);
    return s;
}

static scm_obj p_substring(scm_obj a)
{
    scm_obj s = str_arg(&a);
    long start = index_arg(&a, s->u.str.len), end = a != NIL ? index_arg(&a, s->u.str.len) : (long)s->u.str.len;
    if (end < start) scm_error("substring: end before start", NULL);
    return make_string(s->u.str.s + start, (size_t)(end - start));
}

static scm_obj p_string_append(scm_obj args)
{
    size_t len = 0;
    scm_obj a, r;
    char *p;
    for (a = args; is_pair(a); a = CDR(a)) len += check(CAR(a), T_STR, "not a string:")->u.str.len;
    r = make_string(NULL, len);
    p = r->u.str.s;
    for (a = args; is_pair(a); a = CDR(a)) { memcpy(p, CAR(a)->u.str.s, CAR(a)->u.str.len); p += CAR(a)->u.str.len; }
    return r;
}

static scm_obj p_string_copy(scm_obj a) { scm_obj s = str_arg(&a); return make_string(s->u.str.s, s->u.str.len); }

static int strcmp_obj(scm_obj a, scm_obj b)
{
    uint32_t n = a->u.str.len < b->u.str.len ? a->u.str.len : b->u.str.len;
    int c = memcmp(a->u.str.s, b->u.str.s, n);
    if (c) return c;
    return a->u.str.len < b->u.str.len ? -1 : a->u.str.len > b->u.str.len;
}

static scm_obj strcmp_chain(scm_obj args, int op)
{
    scm_obj x = str_arg(&args);
    while (args != NIL) {
        scm_obj y = str_arg(&args);
        int c = strcmp_obj(x, y);
        if (!(op == '=' ? c == 0 : op == '<' ? c < 0 : c > 0)) return FALSE_;
        x = y;
    }
    return TRUE_;
}
static scm_obj p_string_eq(scm_obj a) { return strcmp_chain(a, '='); }
static scm_obj p_string_lt(scm_obj a) { return strcmp_chain(a, '<'); }
static scm_obj p_string_gt(scm_obj a) { return strcmp_chain(a, '>'); }

static scm_obj p_string_to_list(scm_obj args)
{
    scm_obj s = str_arg(&args), r = NIL;
    uint32_t i = s->u.str.len;
    int g = gctop;
    while (i-- > 0) {
        scm_obj c = make_char((unsigned char)s->u.str.s[i]);
        r = cons(c, r);
        gctop = g;
        push(r);
    }
    return r;
}

static scm_obj p_list_to_string(scm_obj args)
{
    scm_obj l = scm_arg(&args), s, p;
    long n = list_length(l), i;
    for (p = l; p != NIL; p = CDR(p)) check(CAR(p), T_CHAR, "not a character:");
    s = make_string(NULL, (size_t)n);
    for (i = 0, p = l; i < n; i++, p = CDR(p)) s->u.str.s[i] = (char)CAR(p)->u.ch;
    return s;
}

static scm_obj p_string(scm_obj args) { return p_list_to_string(cons(args, NIL)); }

/* ------------------------------------------------------------------ primitives: vectors */

static scm_obj vec_arg(scm_obj *a) { return check(scm_arg(a), T_VEC, "not a vector:"); }

static scm_obj make_vector(uint32_t n, scm_obj fill)
{
    scm_obj v = alloc(T_VEC);
    uint32_t i;
    v->u.vec.v = xmalloc(n * sizeof(scm_obj));
    for (i = 0; i < n; i++) v->u.vec.v[i] = fill;
    v->u.vec.len = n;
    return v;
}

static scm_obj p_make_vector(scm_obj a)
{
    long n = index_arg(&a, 1L << 16);
    return make_vector((uint32_t)n, a != NIL ? scm_arg(&a) : FALSE_);
}

static scm_obj p_vector(scm_obj args)
{
    long n = list_length(args), i;
    scm_obj v = make_vector((uint32_t)n, FALSE_);
    for (i = 0; i < n; i++, args = CDR(args)) v->u.vec.v[i] = CAR(args);
    return v;
}

static scm_obj p_vector_length(scm_obj a) { return FIX_MAKE(vec_arg(&a)->u.vec.len); }
static scm_obj p_vector_ref(scm_obj a)  { scm_obj v = vec_arg(&a); return v->u.vec.v[index_arg(&a, (long)v->u.vec.len - 1)]; }
static scm_obj p_vector_set(scm_obj a)  { scm_obj v = vec_arg(&a); long i = index_arg(&a, (long)v->u.vec.len - 1); v->u.vec.v[i] = scm_arg(&a); return UNSPEC; }
static scm_obj p_vector_fill(scm_obj a) { scm_obj v = vec_arg(&a), x = scm_arg(&a); uint32_t i; for (i = 0; i < v->u.vec.len; i++) v->u.vec.v[i] = x; return UNSPEC; }
static scm_obj p_list_to_vector(scm_obj a) { return p_vector(scm_arg(&a)); }

static scm_obj p_vector_to_list(scm_obj args)
{
    scm_obj v = vec_arg(&args), r = NIL;
    uint32_t i = v->u.vec.len;
    int g = gctop;
    while (i-- > 0) { r = cons(v->u.vec.v[i], r); gctop = g; push(r); }
    return r;
}

/* ------------------------------------------------------------------ primitives: I/O and system */

static scm_obj p_display(scm_obj a) { print(scm_arg(&a), 0); return UNSPEC; }
static scm_obj p_write(scm_obj a)   { print(scm_arg(&a), 1); return UNSPEC; }
static scm_obj p_newline(scm_obj a) { (void)a; outc('\n'); return UNSPEC; }
static scm_obj p_write_char(scm_obj a) { outc(char_arg(&a)->u.ch); return UNSPEC; }

static scm_obj p_read(scm_obj a)
{
    const char *saved = rd_str;
    scm_obj o;
    (void)a;
    rd_str = NULL;
    o = read_datum();
    rd_str = saved;
    return o;
}

static int peeked_char = -2;

static scm_obj p_read_char(scm_obj a)
{
    int c;
    (void)a;
    if (peeked_char != -2) { c = peeked_char; peeked_char = -2; }
    else if (line_pos < line_len) c = (unsigned char)line[line_pos++];
    else c = raw_getc();
    return c < 0 ? EOF_OBJ : make_char(c);
}

static scm_obj p_peek_char(scm_obj a)
{
    (void)a;
    if (peeked_char == -2) {
        if (line_pos < line_len) peeked_char = (unsigned char)line[line_pos++];
        else peeked_char = raw_getc();
    }
    return peeked_char < 0 ? EOF_OBJ : make_char(peeked_char);
}

static scm_obj p_char_ready(scm_obj a)
{
    (void)a;
    return scm_from_bool(peeked_char != -2 || line_pos < line_len || ta_head != ta_tail
                         || (io->avail && io->avail()));
}

static scm_obj p_eval(scm_obj a) { return eval(scm_arg(&a), NIL); }

static scm_obj p_error(scm_obj args)
{
    scm_obj msg = scm_arg(&args);
    out_limit = 0;
    scm_puts("\nError: ");
    print(msg, 0);
    for (; is_pair(args); args = CDR(args)) { outc(' '); print(CAR(args), 1); }
    outc('\n');
    reset_input();
    if (err_jmp) longjmp(*err_jmp, 1);
    for (;;) { }
}

static scm_obj p_gc(scm_obj a) { (void)a; gc(); return scm_from_int((int64_t)nfree); }

static scm_obj p_room(scm_obj a)
{
    (void)a;
    return cons(scm_from_int((int64_t)nfree), cons(scm_from_int((int64_t)ncells), NIL));
}

static scm_obj p_void(scm_obj a) { (void)a; return UNSPEC; }

static scm_obj p_macroexpand1(scm_obj args)
{
    scm_obj x = scm_arg(&args), m;
    if (!is_pair(x) || !is_sym(CAR(x))) return x;
    m = CAR(x)->u.sym.val;
    if (FIXNUMP(m) || m->type != T_MACRO) return x;
    return eval_body(CDR(m->u.clos.code), bind_args(CAR(m->u.clos.code), CDR(x), m->u.clos.env, m, 0));
}

/* ------------------------------------------------------------------ prelude */

static const char prelude[] =
"(define (list? x) (if (null? x) #t (if (pair? x) (list? (cdr x)) #f)))\n"
"(define (filter p l) (cond ((null? l) '()) ((p (car l)) (cons (car l) (filter p (cdr l)))) (else (filter p (cdr l)))))\n"
"(define (fold f acc l) (if (null? l) acc (fold f (f (car l) acc) (cdr l))))\n"
"(define (reduce f init l) (if (null? l) init (fold f (car l) (cdr l))))\n"
"(define (iota n . start) (let loop ((i (- n 1)) (acc '())) (if (< i 0) acc (loop (- i 1) (cons (+ i (if (pair? start) (car start) 0)) acc)))))\n"
"(define-macro (do bindings test-and-result . body)\n"
"  (let ((loop (gensym \"do\")))\n"
"    `(let ,loop ,(map (lambda (b) (list (car b) (cadr b))) bindings)\n"
"       (if ,(car test-and-result)\n"
"           (begin (if #f #f) ,@(cdr test-and-result))\n"
"           (begin ,@body (,loop ,@(map (lambda (b) (if (null? (cddr b)) (car b) (caddr b))) bindings)))))))\n"
"(define-macro (while test . body)\n"
"  (let ((loop (gensym \"while\")))\n"
"    `(let ,loop () (when ,test ,@body (,loop)))))\n"
"(define-macro (dotimes spec . body)\n"
"  (let ((n (gensym \"n\")))\n"
"    `(let ((,n ,(cadr spec))) (do ((,(car spec) 0 (+ ,(car spec) 1))) ((>= ,(car spec) ,n)) ,@body))))\n"
"(define-macro (delay expr)\n"
"  `(let ((done #f) (val #f)) (lambda () (if done val (begin (set! val ,expr) (set! done #t) val)))))\n"
"(define (force p) (p))\n";

/* ------------------------------------------------------------------ init */

void scm_defprim(const char *name, scm_prim_fn fn, int min_args, int max_args)
{
    int g = gctop;
    scm_obj sym = scm_intern(name), p = alloc(T_PRIM);
    p->u.prim.fn = fn;
    p->u.prim.name = sym->u.sym.name;
    p->aux = (uint16_t)((min_args & 0xff) | ((max_args & 0xff) << 8));
    sym->u.sym.val = p;
    gctop = g;
}

void scm_define(const char *name, scm_obj value)
{
    int g = gctop;
    push(value);
    scm_intern(name)->u.sym.val = value;
    gctop = g;
}

static scm_obj special(const char *name, int id)
{
    scm_obj s = scm_intern(name);
    s->aux = (uint16_t)id;
    return s;
}

static const struct { const char *name; scm_prim_fn fn; uint8_t min, max; } prims[] = {
    {"+", p_add, 0, SCM_VARIADIC}, {"-", p_sub, 1, SCM_VARIADIC},
    {"*", p_mul, 0, SCM_VARIADIC}, {"/", p_div, 1, SCM_VARIADIC},
    {"quotient", p_quotient, 2, 2}, {"remainder", p_remainder, 2, 2}, {"modulo", p_modulo, 2, 2},
    {"=", p_numeq, 1, SCM_VARIADIC}, {"<", p_lt, 1, SCM_VARIADIC}, {">", p_gt, 1, SCM_VARIADIC},
    {"<=", p_le, 1, SCM_VARIADIC}, {">=", p_ge, 1, SCM_VARIADIC},
    {"max", p_max, 1, SCM_VARIADIC}, {"min", p_min, 1, SCM_VARIADIC}, {"abs", p_abs, 1, 1},
    {"floor", p_floor, 1, 1}, {"ceiling", p_ceiling, 1, 1}, {"truncate", p_truncate, 1, 1},
    {"round", p_round, 1, 1}, {"exact", p_exact, 1, 1}, {"inexact->exact", p_exact, 1, 1},
    {"inexact", p_inexact, 1, 1}, {"exact->inexact", p_inexact, 1, 1},
    {"sqrt", p_sqrt, 1, 1}, {"expt", p_expt, 2, 2}, {"exp", p_exp, 1, 1}, {"log", p_log, 1, 1},
    {"sin", p_sin, 1, 1}, {"cos", p_cos, 1, 1}, {"tan", p_tan, 1, 1},
    {"asin", p_asin, 1, 1}, {"acos", p_acos, 1, 1}, {"atan", p_atan, 1, 2},
    {"logand", p_logand, 0, SCM_VARIADIC}, {"logior", p_logior, 0, SCM_VARIADIC},
    {"logxor", p_logxor, 0, SCM_VARIADIC}, {"lognot", p_lognot, 1, 1}, {"ash", p_ash, 2, 2},
    {"number->string", p_number_to_string, 1, 2}, {"string->number", p_string_to_number, 1, 2},
    {"null?", p_nullp, 1, 1}, {"pair?", p_pairp, 1, 1}, {"symbol?", p_symbolp, 1, 1},
    {"string?", p_stringp, 1, 1}, {"char?", p_charp, 1, 1}, {"number?", p_numberp, 1, 1},
    {"integer?", p_integerp, 1, 1}, {"exact?", p_exactp, 1, 1}, {"inexact?", p_inexactp, 1, 1},
    {"procedure?", p_procedurep, 1, 1}, {"boolean?", p_booleanp, 1, 1}, {"vector?", p_vectorp, 1, 1},
    {"not", p_not, 1, 1}, {"eof-object?", p_eofp, 1, 1}, {"zero?", p_zerop, 1, 1},
    {"positive?", p_positivep, 1, 1}, {"negative?", p_negativep, 1, 1},
    {"odd?", p_oddp, 1, 1}, {"even?", p_evenp, 1, 1}, {"list?", p_listp, 1, 1},
    {"eq?", p_eq, 2, 2}, {"eqv?", p_eqv, 2, 2}, {"equal?", p_equal, 2, 2},
    {"cons", p_cons, 2, 2}, {"car", p_car, 1, 1}, {"cdr", p_cdr, 1, 1},
    {"set-car!", p_setcar, 2, 2}, {"set-cdr!", p_setcdr, 2, 2},
    {"caar", p_caar, 1, 1}, {"cadr", p_cadr, 1, 1}, {"cdar", p_cdar, 1, 1}, {"cddr", p_cddr, 1, 1},
    {"caddr", p_caddr, 1, 1}, {"cdddr", p_cdddr, 1, 1}, {"cadddr", p_cadddr, 1, 1},
    {"list", p_list, 0, SCM_VARIADIC}, {"length", p_length, 1, 1}, {"reverse", p_reverse, 1, 1},
    {"append", p_append, 0, SCM_VARIADIC}, {"list-tail", p_list_tail, 2, 2}, {"list-ref", p_list_ref, 2, 2},
    {"list-copy", p_list_copy, 1, 1}, {"last-pair", p_last_pair, 1, 1},
    {"memq", p_memq, 2, 2}, {"memv", p_memv, 2, 2}, {"member", p_member, 2, 2},
    {"assq", p_assq, 2, 2}, {"assv", p_assv, 2, 2}, {"assoc", p_assoc, 2, 2},
    {"apply", p_apply, 1, SCM_VARIADIC}, {"map", p_map, 2, SCM_VARIADIC}, {"for-each", p_for_each, 2, SCM_VARIADIC},
    {"symbol->string", p_symbol_to_string, 1, 1}, {"string->symbol", p_string_to_symbol, 1, 1},
    {"gensym", p_gensym, 0, 1},
    {"char->integer", p_char_to_integer, 1, 1}, {"integer->char", p_integer_to_char, 1, 1},
    {"char-upcase", p_char_upcase, 1, 1}, {"char-downcase", p_char_downcase, 1, 1},
    {"char-alphabetic?", p_char_alphap, 1, 1}, {"char-numeric?", p_char_numericp, 1, 1},
    {"char-whitespace?", p_char_whitep, 1, 1},
    {"char=?", p_char_eq, 1, SCM_VARIADIC}, {"char<?", p_char_lt, 1, SCM_VARIADIC}, {"char>?", p_char_gt, 1, SCM_VARIADIC},
    {"string-length", p_string_length, 1, 1}, {"string-ref", p_string_ref, 2, 2},
    {"string-set!", p_string_set, 3, 3}, {"make-string", p_make_string, 1, 2},
    {"substring", p_substring, 2, 3}, {"string-append", p_string_append, 0, SCM_VARIADIC},
    {"string-copy", p_string_copy, 1, 1},
    {"string=?", p_string_eq, 1, SCM_VARIADIC}, {"string<?", p_string_lt, 1, SCM_VARIADIC},
    {"string>?", p_string_gt, 1, SCM_VARIADIC},
    {"string->list", p_string_to_list, 1, 1}, {"list->string", p_list_to_string, 1, 1},
    {"string", p_string, 0, SCM_VARIADIC},
    {"make-vector", p_make_vector, 1, 2}, {"vector", p_vector, 0, SCM_VARIADIC},
    {"vector-length", p_vector_length, 1, 1}, {"vector-ref", p_vector_ref, 2, 2},
    {"vector-set!", p_vector_set, 3, 3}, {"vector-fill!", p_vector_fill, 2, 2},
    {"vector->list", p_vector_to_list, 1, 1}, {"list->vector", p_list_to_vector, 1, 1},
    {"display", p_display, 1, 1}, {"write", p_write, 1, 1}, {"newline", p_newline, 0, 0},
    {"write-char", p_write_char, 1, 1}, {"read", p_read, 0, 0}, {"read-char", p_read_char, 0, 0},
    {"peek-char", p_peek_char, 0, 0}, {"char-ready?", p_char_ready, 0, 0},
    {"eval", p_eval, 1, 2}, {"error", p_error, 1, SCM_VARIADIC},
    {"gc", p_gc, 0, 0}, {"room", p_room, 0, 0}, {"void", p_void, 0, SCM_VARIADIC},
    {"macroexpand-1", p_macroexpand1, 1, 1},
};

void scm_init(void *heap_mem, size_t heap_bytes, const scm_io *the_io)
{
    size_t i;
    uintptr_t base = ((uintptr_t)heap_mem + 7) & ~(uintptr_t)7;
    heap_bytes -= base - (uintptr_t)heap_mem;
    io = the_io;
    heap = (struct scm_cell *)base;
    ncells = heap_bytes / sizeof(struct scm_cell);
    freelist = NULL; nfree = 0;
    for (i = ncells; i-- > 0; ) {
        heap[i].type = T_FREE;
        heap[i].mark = 0;
        CDR(&heap[i]) = freelist;
        freelist = &heap[i];
        nfree++;
    }
    gctop = 0;
    symlist = NIL;
    reset_input();

    s_quote = special("quote", SF_QUOTE);
    s_quasi = special("quasiquote", SF_QUASI);
    s_unquote = special("unquote", SF_UNQUOTE);
    s_unquote_splicing = special("unquote-splicing", SF_UNQUOTE_SPLICING);
    special("if", SF_IF);
    s_define = special("define", SF_DEFINE);
    special("define-macro", SF_DEFMACRO);
    special("set!", SF_SET);
    s_lambda = special("lambda", SF_LAMBDA);
    s_begin = special("begin", SF_BEGIN);
    special("let", SF_LET);
    special("let*", SF_LETSTAR);
    special("letrec", SF_LETREC);
    special("letrec*", SF_LETREC);
    special("cond", SF_COND);
    special("case", SF_CASE);
    special("and", SF_AND);
    special("or", SF_OR);
    special("when", SF_WHEN);
    special("unless", SF_UNLESS);
    s_else = scm_intern("else");
    s_arrow = scm_intern("=>");
    gctop = 0;

    for (i = 0; i < sizeof prims / sizeof prims[0]; i++)
        scm_defprim(prims[i].name, prims[i].fn, prims[i].min, prims[i].max);

    scm_eval_cstr(prelude);
}

/* ------------------------------------------------------------------ REPL */

int scm_eval_cstr(const char *src)
{
    jmp_buf jb, *saved_jmp = err_jmp;
    const char *saved_str = rd_str;
    int saved_gc = gctop;
    volatile int result = 0;

    {   /* keep the shallowest entry point seen as the stack base */
        char *here = (char *)__builtin_frame_address(0);
        if (!stack_base || here > stack_base) { stack_base = here; update_stack_limit(); }
    }
    err_jmp = &jb;
    if (setjmp(jb)) {
        result = -1;
    } else {
        rd_str = src;
        rd_nunget = 0;
        for (;;) {
            scm_obj x;
            gctop = saved_gc;
            x = read_datum();
            if (x == EOF_OBJ) break;
            eval(x, NIL);
        }
    }
    gctop = saved_gc;
    rd_str = saved_str;
    rd_nunget = 0;
    err_jmp = saved_jmp;
    return result;
}

void scm_repl(void)
{
    jmp_buf jb;
    stack_base = (char *)__builtin_frame_address(0);
    update_stack_limit();
    err_jmp = &jb;
    setjmp(jb);
    for (;;) {
        scm_obj x, v;
        gctop = 0;
        out_limit = 0;
        peeked_char = -2;
        at_toplevel = 1;
        x = read_datum();
        if (x == EOF_OBJ) break;
        v = eval(x, NIL);
        if (v != UNSPEC) {
            print(v, 1);
            outc('\n');
        }
    }
    err_jmp = NULL;
}
