#include "common.h"
#include "port/pak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAK_SIZE = 32768, PAGE_SIZE = 256, FIRST_PAGE = 5, LAST_PAGE = 127,
       DIR_BASE = 0x300, DIR_COUNT = 16 };
static u8 pak[PAK_SIZE];
static const char *pak_path;
static int pak_ready;

static u16 be16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }
static u32 be32(const u8 *p) { return ((u32)be16(p) << 16) | be16(p + 2); }
static void put16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = v; }
static void put32(u8 *p, u32 v) { put16(p, v >> 16); put16(p + 2, v); }
static u16 inode(int page) { return be16(pak + 0x100 + page * 2); }
static void inode_set(int page, u16 next) {
    put16(pak + 0x100 + page * 2, next);
    put16(pak + 0x200 + page * 2, next);
}
static u8 *entry(int file) { return pak + DIR_BASE + file * 32; }
static int occupied(int file) { return be32(entry(file)) != 0; }

static void inode_checksum(void) {
    unsigned sum = 0;
    for (int i = FIRST_PAGE * 2; i < PAGE_SIZE; i++) sum += pak[0x100 + i];
    pak[0x101] = pak[0x201] = (u8)sum;
}

static void format_pak(void) {
    memset(pak, 0, sizeof pak);
    u8 *id = pak + 0x20;
    put32(id, 0x454C5441); /* stable serial; not a ROM asset */
    put32(id + 4, 0x4C455031);
    put16(id + 0x18, 0x0501);
    id[0x1a] = 1;
    unsigned sum = 0;
    for (int i = 0; i < 14; i++) sum += be16(id + i * 2);
    put16(id + 0x1c, (u16)sum);
    put16(id + 0x1e, (u16)(0xfff2u - sum));
    memcpy(pak + 0x60, id, 32);
    memcpy(pak + 0x80, id, 32);
    memcpy(pak + 0xc0, id, 32);
    for (int i = FIRST_PAGE; i <= LAST_PAGE; i++) inode_set(i, PFS_PAGE_NOT_USED);
    inode_checksum();
}

static int id_valid(const u8 *id) {
    unsigned sum = 0, inverted = 0;
    for (int i = 0; i < 14; i++) {
        u16 word = be16(id + i * 2);
        sum += word;
        inverted += (u16)~word;
    }
    return be16(id + 0x1c) == (u16)sum &&
           be16(id + 0x1e) == (u16)inverted &&
           (be16(id + 0x18) & 1) && id[0x1a] == 1;
}

static int inode_valid(int base) {
    unsigned sum = 0;
    for (int i = FIRST_PAGE * 2; i < PAGE_SIZE; i++) sum += pak[base + i];
    return pak[base + 1] == (u8)sum;
}

static int flush_pak(void) {
    if (!pak_path) return 0;
    FILE *f = fopen(pak_path, "wb");
    if (!f) { perror(pak_path); return -1; }
    int ok = fwrite(pak, 1, sizeof pak, f) == sizeof pak;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { fprintf(stderr, "failed to write Controller Pak %s\n", pak_path); return -1; }
    return 0;
}

int Port_PakLoad(const char *path) {
    pak_path = path;
    pak_ready = 0;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) {
        int ok = fread(pak, 1, sizeof pak, f) == sizeof pak && fgetc(f) == EOF;
        fclose(f);
        if (!ok) { fprintf(stderr, "Controller Pak must be exactly 32768 bytes: %s\n", path); return -1; }
    } else {
        format_pak();
        if (flush_pak()) return -1;
    }
    pak_ready = 1;
    return 0;
}

static s32 check(OSPfs *p) {
    return !pak_ready || !p || p->channel != 0 || !(p->status & PFS_INITIALIZED)
        ? PFS_ERR_NOPACK : 0;
}

s32 osPfsInitPak(OSMesgQueue *q, OSPfs *p, int channel) {
    if (!pak_ready || channel != 0) return PFS_ERR_NOPACK;
    memset(p, 0, sizeof *p);
    p->queue = q;
    p->channel = channel;
    p->status = PFS_INITIALIZED;
    p->banks = 1;
    p->dir_size = DIR_COUNT;
    p->inode_start_page = FIRST_PAGE;
    p->inode_table = 8;
    p->minode_table = 16;
    p->dir_table = 24;
    memcpy(p->id, pak + 0x20, 32);
    memcpy(p->label, pak + 0xe0, 32);
    p->version = p->id[0x1b];
    if (!id_valid(p->id)) {
        p->status |= PFS_ID_BROKEN;
        return PFS_ERR_ID_FATAL;
    }
    return 0;
}
s32 osPfsInit(OSMesgQueue *q, OSPfs *p, int channel) { return osPfsInitPak(q, p, channel); }
s32 func_8003D0F0(OSMesgQueue *q, OSPfs *p, int channel) { return osPfsInitPak(q, p, channel); }
s32 func_8003D554(OSMesgQueue *q, OSPfs *p, int channel) {
    (void)q; (void)channel;
    if (p) p->status |= PFS_MOTOR_INITIALIZED;
    return 0;
}
s32 func_8003D6B0(OSMesgQueue *q, OSPfs *p, int channel) {
    (void)q; (void)p; (void)channel; return PFS_ERR_DEVICE;
}
s32 osMotorStart(OSPfs *p) { (void)p; return 0; }
s32 osMotorStop(OSPfs *p) { (void)p; return 0; }
s32 osPfsIsPlug(OSMesgQueue *q, u8 *pattern) { (void)q; *pattern = pak_ready ? 1 : 0; return 0; }
s32 osPfsChecker(OSPfs *p) {
    s32 err = check(p);
    if (err) return err;
    if (!inode_valid(0x100)) {
        if (!inode_valid(0x200)) return PFS_ERR_INCONSISTENT;
        memcpy(pak + 0x100, pak + 0x200, PAGE_SIZE);
        if (flush_pak()) return PFS_ERR_CONTRFAIL;
    }
    return 0;
}
s32 osPfsRepairId(OSPfs *p) {
    s32 err = check(p);
    if (err) return err;
    if (!id_valid(pak + 0x20)) {
        const int backups[] = { 0x60, 0x80, 0xc0 };
        const u8 *good = NULL;
        for (unsigned i = 0; i < sizeof backups / sizeof backups[0]; i++) {
            if (id_valid(pak + backups[i])) { good = pak + backups[i]; break; }
        }
        if (!good) return PFS_ERR_ID_FATAL;
        memcpy(pak + 0x20, good, 32);
        if (flush_pak()) return PFS_ERR_CONTRFAIL;
    }
    memcpy(p->id, pak + 0x20, 32);
    p->status &= ~PFS_ID_BROKEN;
    return 0;
}

static s32 osPfsFindFile_impl(OSPfs *p, u16 company, u32 game, u8 *name, u8 *ext, s32 *file) {
    s32 err = check(p);
    if (err) return err;
    for (int i = 0; i < DIR_COUNT; i++) {
        u8 *e = entry(i);
        int match = game ? be32(e) == game && be16(e + 4) == company &&
            (!name || !memcmp(e + 16, name, 16)) && (!ext || !memcmp(e + 12, ext, 4))
            : !occupied(i);
        if (match) { *file = i; return 0; }
    }
    return PFS_ERR_INVALID;
}

static int chain_length(int first) {
    int seen[128] = {0}, count = 0;
    for (int page = first; page != PFS_EOF; page = inode(page)) {
        if (page < FIRST_PAGE || page > LAST_PAGE || seen[page]) return -1;
        seen[page] = 1;
        count++;
    }
    return count;
}

static s32 osPfsFileState_impl(OSPfs *p, s32 file, OSPfsState *st) {
    s32 err = check(p);
    if (err) return err;
    if (file < 0 || file >= DIR_COUNT || !occupied(file)) return PFS_ERR_INVALID;
    u8 *e = entry(file);
    int pages = chain_length(e[7]);
    if (e[6] || pages <= 0) return PFS_ERR_INCONSISTENT;
    st->file_size = pages * PAGE_SIZE;
    st->game_code = be32(e);
    st->company_code = be16(e + 4);
    memcpy(st->ext_name, e + 12, 4);
    memcpy(st->game_name, e + 16, 16);
    return 0;
}

s32 osPfsFreeBlocks(OSPfs *p, s32 *bytes) {
    s32 err = check(p);
    if (err) return err;
    int free_pages = 0;
    for (int i = FIRST_PAGE; i <= LAST_PAGE; i++) free_pages += inode(i) == PFS_PAGE_NOT_USED;
    *bytes = free_pages * PAGE_SIZE;
    return 0;
}

static s32 osPfsAllocateFile_impl(OSPfs *p, u16 company, u32 game, u8 *name, u8 *ext,
                      int size, s32 *file) {
    s32 err = check(p), existing;
    if (err) return err;
    if (!game || !company || size <= 0 || !name || !ext) return PFS_ERR_INVALID;
    if (osPfsFindFile_impl(p, company, game, name, ext, &existing) == 0) return PFS_ERR_EXIST;
    int pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    int free_bytes;
    osPfsFreeBlocks(p, &free_bytes);
    if (pages * PAGE_SIZE > free_bytes) return PFS_DATA_FULL;
    if (osPfsFindFile_impl(p, 0, 0, NULL, NULL, file)) return PFS_DIR_FULL;
    int first = 0, prev = 0;
    for (int i = FIRST_PAGE; i <= LAST_PAGE && pages; i++) {
        if (inode(i) != PFS_PAGE_NOT_USED) continue;
        if (!first) first = i;
        if (prev) inode_set(prev, i);
        inode_set(i, PFS_EOF);
        prev = i;
        pages--;
    }
    inode_checksum();
    u8 *e = entry(*file);
    memset(e, 0, 32);
    put32(e, game);
    put16(e + 4, company);
    e[7] = first;
    memcpy(e + 12, ext, 4);
    memcpy(e + 16, name, 16);
    return flush_pak() ? PFS_ERR_CONTRFAIL : 0;
}

static s32 osPfsDeleteFile_impl(OSPfs *p, u16 company, u32 game, u8 *name, u8 *ext) {
    s32 file, err = osPfsFindFile_impl(p, company, game, name, ext, &file);
    if (err) return err;
    int page = entry(file)[7], seen[128] = {0};
    while (page != PFS_EOF) {
        if (page < FIRST_PAGE || page > LAST_PAGE || seen[page]) return PFS_ERR_INCONSISTENT;
        seen[page] = 1;
        int next = inode(page);
        inode_set(page, PFS_PAGE_NOT_USED);
        page = next;
    }
    inode_checksum();
    memset(entry(file), 0, 32);
    return flush_pak() ? PFS_ERR_CONTRFAIL : 0;
}

static s32 osPfsReadWriteFile_impl(OSPfs *p, s32 file, u8 flag, int offset, int size, u8 *buffer) {
    OSPfsState st;
    s32 err = osPfsFileState_impl(p, file, &st);
    if (err) return err;
    if (!buffer || (offset | size) & 31 || offset < 0 || size <= 0 ||
        (uint64_t)(unsigned)offset + (unsigned)size > st.file_size || flag > PFS_WRITE)
        return PFS_ERR_INVALID;
    int page = entry(file)[7];
    for (int i = 0; i < offset / PAGE_SIZE; i++) page = inode(page);
    while (size) {
        int in_page = offset % PAGE_SIZE;
        int chunk = PAGE_SIZE - in_page;
        if (chunk > size) chunk = size;
        u8 *data = pak + page * PAGE_SIZE + in_page;
        if (flag == PFS_WRITE) memcpy(data, buffer, chunk);
        else memcpy(buffer, data, chunk);
        buffer += chunk;
        offset += chunk;
        size -= chunk;
        if (size) page = inode(page);
    }
    if (flag == PFS_WRITE) {
        entry(file)[8] |= 2;
        if (flush_pak()) return PFS_ERR_CONTRFAIL;
    }
    return 0;
}

s32 osPfsNumFiles(OSPfs *p, s32 *max, s32 *used) {
    s32 err = check(p);
    if (err) return err;
    *max = DIR_COUNT; *used = 0;
    for (int i = 0; i < DIR_COUNT; i++) *used += occupied(i);
    return 0;
}
s32 func_8003EDC0(OSPfs *p, s32 *max, s32 *used) {
    return osPfsNumFiles(p, max, used);
}

/* ELTALE_TRACE_PAK=1: each file call and its result. */
static int trace_pak(void) { static int t = -1; if (t < 0) t = getenv("ELTALE_TRACE_PAK") != NULL; return t; }
s32 osPfsFindFile(OSPfs *p, u16 company, u32 game, u8 *name, u8 *ext, s32 *file) {
    s32 r = osPfsFindFile_impl(p, company, game, name, ext, file);
    if (trace_pak()) fprintf(stderr, "PAK find game=%08x -> %d file=%d\n", game, r, r ? -1 : *file);
    return r;
}
s32 osPfsFileState(OSPfs *p, s32 file, OSPfsState *st) {
    s32 r = osPfsFileState_impl(p, file, st);
    if (trace_pak()) fprintf(stderr, "PAK state file=%d -> %d\n", file, r);
    return r;
}
s32 osPfsAllocateFile(OSPfs *p, u16 company, u32 game, u8 *name, u8 *ext, int size, s32 *file) {
    s32 r = osPfsAllocateFile_impl(p, company, game, name, ext, size, file);
    if (trace_pak()) fprintf(stderr, "PAK allocate size=%d ext=%02x%02x%02x%02x -> %d file=%d\n", size, ext[0], ext[1], ext[2],
                             ext[3], r, r ? -1 : *file);
    return r;
}
s32 osPfsDeleteFile(OSPfs *p, u16 company, u32 game, u8 *name, u8 *ext) {
    s32 r = osPfsDeleteFile_impl(p, company, game, name, ext);
    if (trace_pak()) fprintf(stderr, "PAK delete -> %d\n", r);
    return r;
}
s32 osPfsReadWriteFile(OSPfs *p, s32 file, u8 flag, int offset, int size, u8 *buffer) {
    s32 r = osPfsReadWriteFile_impl(p, file, flag, offset, size, buffer);
    if (trace_pak()) fprintf(stderr, "PAK %s file=%d offset=%d size=%d -> %d\n", flag ? "write" : "read", file, offset, size, r);
    return r;
}
