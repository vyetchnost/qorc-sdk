/*==========================================================
 *
 *    File   : scheme_fs.c
 *    Purpose: Named Scheme source files in the Qomu SPI flash.
 *
 *  Layout: the top megabyte of the 2 MB GD25Q16C, 0x100000-0x1FFFFF
 *  (FLASH_UNUSED_3 in the BSP map), split into 16 slots of 64 KB. The
 *  bootloader, its metadata, the gateware and the M4 app all live below
 *  1 MB; every flash access here is checked against FS_BASE..FS_END
 *  first, so nothing below 1 MB can be erased or written.
 *
 *  Slot = one header page (256 B) + up to 65280 bytes of text.
 *  Header: magic "QSF1", length, CRC-32 of the text, NUL-terminated name.
 *  The header page is programmed last, after the text has been written
 *  and read back, so an interrupted save leaves no file behind. Deleting
 *  programs the magic to zero (NOR bits only go 1->0, no erase needed);
 *  a slot's sectors are erased just before they are reused.
 *
 *  Saving a name that already exists writes the new copy to a free slot,
 *  then deletes the old one, so a failed save keeps the old version. A
 *  sequence number in the header settles the moment in between: if power
 *  fails after the new header but before the old one is cleared, the
 *  higher number wins and the stale copy is treated as free space.
 *
 *=========================================================*/

#include <stdint.h>
#include <string.h>
#include "scheme.h"
#include "scheme_fs.h"

#define FS_BASE       0x100000u
#define FS_END        0x200000u
#define FS_SLOT_BYTES 0x10000u
#define FS_SLOTS      ((FS_END - FS_BASE) / FS_SLOT_BYTES)      /* 16 */
#define FS_PAGE       256u
#define FS_SECTOR     4096u
#define FS_DATA_MAX   (FS_SLOT_BYTES - FS_PAGE)                 /* 65280 */
#define FS_MAGIC      0x31465351u                               /* "QSF1" */
#define FS_NAME_MAX   28                                        /* incl. NUL */

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t seq;                           /* newest copy of a name wins */
    char     name[FS_NAME_MAX];
} fs_hdr;                                                       /* 44 bytes */

static const fs_port *port;
static int fs_ok;
static uint8_t page_buf[FS_PAGE] __attribute__((aligned(4)));

/* ---------------------------------------------------------------- guarded flash access */

static int in_range(uint32_t addr, uint32_t len)
{
    return addr >= FS_BASE && len <= FS_END - FS_BASE && addr <= FS_END - len;
}

static void need_fs(void)
{
    if (!fs_ok) scm_error("flash storage not available", NULL);
}

static void flash_read(uint32_t addr, void *buf, uint32_t len)
{
    /* the driver's DMA moves whole words: len must be a multiple of 4 */
    if (!in_range(addr, len) || (len & 3) || ((uintptr_t)buf & 3))
        scm_error("flash read out of range", scm_from_int(addr));
    if (port->read(addr, buf, len) != 0)
        scm_error("flash read failed at", scm_from_int(addr));
}

static void flash_program(uint32_t addr, const void *buf)
{
    if (!in_range(addr, FS_PAGE) || (addr & (FS_PAGE - 1)))
        scm_error("flash write out of range", scm_from_int(addr));
    if (port->program_page(addr, buf) != 0)
        scm_error("flash write failed at", scm_from_int(addr));
}

static void flash_erase(uint32_t addr)
{
    if (!in_range(addr, FS_SECTOR) || (addr & (FS_SECTOR - 1)))
        scm_error("flash erase out of range", scm_from_int(addr));
    if (port->erase_sector(addr) != 0)
        scm_error("flash erase failed at", scm_from_int(addr));
}

/* ---------------------------------------------------------------- helpers */

static uint32_t crc32_add(uint32_t crc, const uint8_t *p, uint32_t n)
{
    while (n--) {
        int k;
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}

static uint32_t slot_base(int slot) { return FS_BASE + (uint32_t)slot * FS_SLOT_BYTES; }

static int read_hdr(int slot, fs_hdr *h)
{
    uint32_t buf[(sizeof(fs_hdr) + 3) / 4];
    flash_read(slot_base(slot), buf, sizeof buf);
    memcpy(h, buf, sizeof *h);
    return h->magic == FS_MAGIC && h->len <= FS_DATA_MAX
        && memchr(h->name, 0, FS_NAME_MAX) != NULL && h->name[0] != 0;
}

static const char *name_arg(scm_obj *args)
{
    scm_obj o = scm_arg(args);
    const char *n = scm_to_cstr(o);
    size_t i, len = strlen(n);
    if (len == 0 || len >= FS_NAME_MAX)
        scm_error("file name must be 1-27 characters:", o);
    for (i = 0; i < len; i++)
        if ((unsigned char)n[i] <= ' ' || (unsigned char)n[i] >= 127)
            scm_error("file name has a space or control character:", o);
    return n;
}

static fs_hdr dir[FS_SLOTS];
static uint8_t live[FS_SLOTS];              /* valid header and newest of its name */
static uint32_t max_seq;

static void scan(void)
{
    int s, t;
    max_seq = 0;
    for (s = 0; s < (int)FS_SLOTS; s++) {
        live[s] = (uint8_t)read_hdr(s, &dir[s]);
        if (live[s] && dir[s].seq > max_seq) max_seq = dir[s].seq;
    }
    for (s = 0; s < (int)FS_SLOTS; s++)
        for (t = 0; t < (int)FS_SLOTS; t++)
            if (s != t && live[s] && live[t] && dir[t].seq > dir[s].seq
                && strcmp(dir[s].name, dir[t].name) == 0)
                live[s] = 0;                /* stale: a newer copy exists */
}

static int find_file(const char *name, fs_hdr *h)
{
    int s;
    scan();
    for (s = 0; s < (int)FS_SLOTS; s++)
        if (live[s] && strcmp(dir[s].name, name) == 0) { *h = dir[s]; return s; }
    return -1;
}

static void delete_slot(int slot)
{
    memset(page_buf, 0xff, FS_PAGE);
    memset(page_buf, 0, 4);                 /* magic -> 0: programming only clears bits */
    flash_program(slot_base(slot), page_buf);
}

static void put_uint(uint32_t v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    scm_puts(b + i);
}

/* ---------------------------------------------------------------- reading */

typedef struct {
    uint32_t addr, left;                    /* next flash byte, bytes still to fetch */
    uint32_t pos, n;                        /* position in / fill of buf             */
    uint8_t  buf[FS_PAGE] __attribute__((aligned(4)));
} fs_reader;

static int reader_getc(void *ctx)
{
    fs_reader *r = (fs_reader *)ctx;
    if (r->pos == r->n) {
        uint32_t n;
        if (r->left == 0) return -1;
        n = r->left < FS_PAGE ? r->left : FS_PAGE;
        flash_read(r->addr, r->buf, (n + 3) & ~3u);
        r->addr += n;
        r->left -= n;
        r->pos = 0;
        r->n = n;
    }
    return r->buf[r->pos++];
}

/* Open a file for reading after checking its CRC (a second pass is cheap
 * at these sizes and catches a half-programmed or decayed slot). */
static void open_file(const char *name, fs_reader *r)
{
    fs_hdr h;
    uint32_t crc = 0xffffffffu;
    int slot = find_file(name, &h), c;
    if (slot < 0) scm_error("no such file:", scm_from_cstr(name));
    r->addr = slot_base(slot) + FS_PAGE;
    r->left = h.len;
    r->pos = r->n = 0;
    while ((c = reader_getc(r)) >= 0) {
        uint8_t b = (uint8_t)c;
        crc = crc32_add(crc, &b, 1);
    }
    if ((crc ^ 0xffffffffu) != h.crc) scm_error("file is corrupt (CRC):", scm_from_cstr(name));
    r->addr = slot_base(slot) + FS_PAGE;
    r->left = h.len;
    r->pos = r->n = 0;
}

/* ---------------------------------------------------------------- writing */

typedef struct {
    int      slot, old, too_big;
    uint32_t len, crc, page, fill, seq;
    char     name[FS_NAME_MAX];
    uint8_t  buf[FS_PAGE] __attribute__((aligned(4)));
} fs_writer;

static void writer_begin(fs_writer *w, const char *name)
{
    int s, free_slot = -1;
    w->old = -1;
    scan();
    for (s = 0; s < (int)FS_SLOTS; s++) {
        if (live[s]) {
            if (strcmp(dir[s].name, name) == 0) w->old = s;
        } else if (free_slot < 0) {
            free_slot = s;
        }
    }
    if (free_slot < 0) {
        if (w->old < 0) scm_error("flash full: 16 files; delete one first", NULL);
        free_slot = w->old;                 /* last slot: overwrite in place */
        w->old = -1;
    }
    w->slot = free_slot;
    w->len = 0;
    w->too_big = 0;
    w->crc = 0xffffffffu;
    w->page = 1;                            /* page 0 is the header, written last */
    w->fill = 0;
    w->seq = max_seq + 1;
    strcpy(w->name, name);
    flash_erase(slot_base(w->slot));        /* sector 0: header + first 15 pages */
}

static void writer_flush(fs_writer *w)
{
    uint32_t addr = slot_base(w->slot) + w->page * FS_PAGE;
    if (w->fill == 0) return;
    w->crc = crc32_add(w->crc, w->buf, w->fill);    /* here, so backspace can undo */
    memset(w->buf + w->fill, 0xff, FS_PAGE - w->fill);
    if ((w->page % (FS_SECTOR / FS_PAGE)) == 0) flash_erase(addr);
    flash_program(addr, w->buf);
    w->page++;
    w->fill = 0;
}

static void writer_putc(fs_writer *w, int c)
{
    uint8_t b = (uint8_t)c;
    if (w->len >= FS_DATA_MAX) { w->too_big = 1; return; }   /* reported at the end */
    w->buf[w->fill++] = b;
    w->len++;
    if (w->fill == FS_PAGE) writer_flush(w);
}

/* Backspace: only within the current line, and only while it is still in
 * the page buffer (a line can't be taken back once its page is in flash). */
static int writer_unputc(fs_writer *w)
{
    if (w->too_big || w->fill == 0 || w->buf[w->fill - 1] == '\n') return 0;
    w->fill--;
    w->len--;
    return 1;
}

static void writer_end(fs_writer *w)
{
    fs_hdr h;
    uint32_t crc = 0xffffffffu, len, want;
    int c;

    if (w->too_big) scm_error("file too big (max 65280 bytes); not saved", NULL);
    writer_flush(w);
    len = w->len;
    want = w->crc ^ 0xffffffffu;

    /* read back before committing the header */
    {
        fs_reader rb;
        rb.addr = slot_base(w->slot) + FS_PAGE;
        rb.left = len;
        rb.pos = rb.n = 0;
        while ((c = reader_getc(&rb)) >= 0) {
            uint8_t b = (uint8_t)c;
            crc = crc32_add(crc, &b, 1);
        }
    }
    if ((crc ^ 0xffffffffu) != want) scm_error("flash verify failed; file not saved", NULL);

    memset(&h, 0, sizeof h);
    h.magic = FS_MAGIC;
    h.len = len;
    h.crc = want;
    h.seq = w->seq;
    strcpy(h.name, w->name);
    memset(page_buf, 0xff, FS_PAGE);
    memcpy(page_buf, &h, sizeof h);
    flash_program(slot_base(w->slot), page_buf);

    if (w->old >= 0) delete_slot(w->old);
}

/* ---------------------------------------------------------------- primitives */

static scm_obj p_files(scm_obj args)
{
    int s, used = 0;
    (void)args;
    need_fs();
    scan();
    for (s = 0; s < (int)FS_SLOTS; s++) {
        const fs_hdr *hp = &dir[s];
        if (!live[s]) continue;
        used++;
        scm_puts("  ");
        scm_puts(hp->name);
        {
            int pad = 28 - (int)strlen(hp->name);
            while (pad-- > 0) scm_puts(" ");
        }
        put_uint(hp->len);
        scm_puts(" bytes\n");
    }
    put_uint((uint32_t)used);
    scm_puts(" of ");
    put_uint(FS_SLOTS);
    scm_puts(" slots used (max 65280 bytes per file)\n");
    return SCM_UNSPEC;
}

static scm_obj p_file_exists(scm_obj args)
{
    fs_hdr h;
    need_fs();
    return scm_from_bool(find_file(name_arg(&args), &h) >= 0);
}

static scm_obj p_delete_file(scm_obj args)
{
    fs_hdr h;
    int slot;
    need_fs();
    slot = find_file(name_arg(&args), &h);
    if (slot < 0) return SCM_FALSE;
    delete_slot(slot);
    return SCM_TRUE;
}

static scm_obj p_cat(scm_obj args)
{
    static fs_reader r;
    char chunk[65];
    int c, n = 0;
    need_fs();
    open_file(name_arg(&args), &r);
    while ((c = reader_getc(&r)) >= 0) {
        chunk[n++] = (char)c;
        if (n == 64) { chunk[n] = 0; scm_puts(chunk); n = 0; scm_check_interrupt(); }
    }
    chunk[n] = 0;
    scm_puts(chunk);
    return SCM_UNSPEC;
}

static scm_obj p_load(scm_obj args)
{
    fs_reader r;                            /* on the stack: load can nest */
    need_fs();
    open_file(name_arg(&args), &r);
    return scm_eval_stream(reader_getc, &r) == 0 ? SCM_UNSPEC : SCM_FALSE;
}

static scm_obj p_save(scm_obj args)
{
    static fs_writer w;
    const char *name, *text;
    need_fs();
    name = name_arg(&args);
    text = scm_to_cstr(scm_arg(&args));
    writer_begin(&w, name);
    while (*text) writer_putc(&w, (unsigned char)*text++);
    writer_end(&w);
    return scm_from_int(w.len);
}

/* (write-file "name"): the following lines, up to one holding only "." or
 * a Ctrl-D, go straight to flash a page at a time. CR and CRLF become LF.
 * Leading blank lines are dropped. Input is echoed; backspace works within
 * the line being typed. Ctrl-C cancels; the old file, if any, is left
 * untouched. */
static void echo(int c)
{
    char t[2];
    t[0] = (char)c;
    t[1] = 0;
    scm_puts(t);                            /* LF -> CRLF on the tty */
}

static scm_obj p_write_file(scm_obj args)
{
    static fs_writer w;
    const char *name;
    int c, bol = 1, dot = 0, last_cr = 0, started = 0;
    need_fs();
    name = name_arg(&args);
    if (!port->con_getc) scm_error("no console to read from", NULL);
    scm_discard_line();
    writer_begin(&w, name);
    scm_puts("Paste the file, then a line with only . (or Ctrl-D). Ctrl-C cancels.\n");
    for (;;) {
        c = port->con_getc();
        if (c < 0) break;
        if (c == 3) { scm_puts("^C\n"); scm_error("write-file cancelled", NULL); }
        if (c == '\n' && last_cr) { last_cr = 0; continue; }
        last_cr = (c == '\r');
        if (c == '\r') c = '\n';
        if (c == 4) {                       /* Ctrl-D: end, keeping a pending "." */
            if (dot) writer_putc(&w, '.');
            if (!bol) scm_puts("\n");
            break;
        }
        if (c == 8 || c == 127) {
            if (dot) { dot = 0; scm_puts("\b \b"); }
            else if (writer_unputc(&w)) scm_puts("\b \b");
            bol = w.fill ? w.buf[w.fill - 1] == '\n' : w.len == 0;
            continue;
        }
        if (c < ' ' && c != '\n' && c != '\t') continue;   /* other control keys */
        echo(c);
        if (dot) {
            dot = 0;
            if (c == '\n') break;           /* the terminator line */
            writer_putc(&w, '.');
            bol = 0;
        }
        if (bol && c == '.') { dot = 1; continue; }
        if (!started && c == '\n') continue;
        started = 1;
        writer_putc(&w, c);
        bol = (c == '\n');
    }
    /* swallow the LF of a CRLF terminator so the REPL doesn't see a blank line */
    if (last_cr && port->con_peek && port->con_peek() == '\n') port->con_getc();
    writer_end(&w);
    scm_puts("Saved ");
    scm_puts(w.name);
    scm_puts(": ");
    put_uint(w.len);
    scm_puts(" bytes\n");
    return SCM_UNSPEC;
}

/* ---------------------------------------------------------------- setup */

int scheme_fs_init(const fs_port *p)
{
    port = p;
    fs_ok = p && p->init && p->init() == 0;
    scm_defprim("files", p_files, 0, 0);
    scm_defprim("file-exists?", p_file_exists, 1, 1);
    scm_defprim("delete-file", p_delete_file, 1, 1);
    scm_defprim("cat", p_cat, 1, 1);
    scm_defprim("load", p_load, 1, 1);
    scm_defprim("save", p_save, 2, 2);
    scm_defprim("write-file", p_write_file, 1, 1);
    return fs_ok;
}

/* Load "boot" if it exists. Runs under scm_eval_cstr's error handler, so a
 * flash error, a Scheme error or Ctrl-C just ends it. */
void scheme_fs_autorun(void)
{
    if (fs_ok)
        scm_eval_cstr("(if (file-exists? \"boot\") (load \"boot\"))");
}
