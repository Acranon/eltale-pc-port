#include "common.h"
#include "port/pak.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *path = argv[1];
    remove(path);
    assert(Port_PakLoad(path) == 0);
    OSPfs p;
    assert(osPfsInitPak(NULL, &p, 0) == 0);
    s32 free_bytes, file, found;
    assert(osPfsFreeBlocks(&p, &free_bytes) == 0 && free_bytes == 123 * 256);
    u8 name[16] = "SxbQx", ext[4] = { 0x1a, 0, 0, 0 };
    assert(osPfsAllocateFile(&p, 0x3943, 0x4e45544a, name, ext, 512, &file) == 0);
    assert(osPfsFindFile(&p, 0x3943, 0x4e45544a, name, ext, &found) == 0 && found == file);
    OSPfsState state;
    assert(osPfsFileState(&p, file, &state) == 0 && state.file_size == 512);
    u8 write_buffer[512], read_buffer[512];
    for (int i = 0; i < 512; i++) write_buffer[i] = (u8)(i ^ 0xa5);
    assert(osPfsReadWriteFile(&p, file, PFS_WRITE, 0, 512, write_buffer) == 0);
    assert(Port_PakLoad(path) == 0);
    assert(osPfsInitPak(NULL, &p, 0) == 0);
    memset(read_buffer, 0, sizeof read_buffer);
    assert(osPfsReadWriteFile(&p, file, PFS_READ, 0, 512, read_buffer) == 0);
    assert(memcmp(write_buffer, read_buffer, 512) == 0);
    FILE *raw = fopen(path, "rb");
    assert(raw);
    u8 image[32768];
    assert(fread(image, 1, sizeof image, raw) == sizeof image);
    assert(fgetc(raw) == EOF);
    fclose(raw);
    assert(image[0x100 + 5 * 2] == 0 && image[0x100 + 5 * 2 + 1] == 6);
    assert(image[0x100 + 6 * 2] == 0 && image[0x100 + 6 * 2 + 1] == 1);
    assert(memcmp(image + 0x100, image + 0x200, 256) == 0);
    assert(memcmp(image + 0x500, write_buffer, 512) == 0);
    assert(osPfsDeleteFile(&p, 0x3943, 0x4e45544a, name, ext) == 0);
    assert(osPfsFreeBlocks(&p, &free_bytes) == 0 && free_bytes == 123 * 256);
    assert(osPfsFileState(&p, file, &state) == PFS_ERR_INVALID);
    raw = fopen(path, "r+b");
    assert(raw);
    assert(fseek(raw, 0x20, SEEK_SET) == 0);
    assert(fputc(0xff, raw) != EOF);
    assert(fseek(raw, 0x101, SEEK_SET) == 0);
    assert(fputc(0xff, raw) != EOF);
    assert(fclose(raw) == 0);
    assert(Port_PakLoad(path) == 0);
    assert(osPfsInitPak(NULL, &p, 0) == PFS_ERR_ID_FATAL);
    assert(osPfsRepairId(&p) == 0);
    assert(osPfsChecker(&p) == 0);
    assert(osPfsInitPak(NULL, &p, 0) == 0);
    remove(path);
    puts("pak-test OK");
    return 0;
}
