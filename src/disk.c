#include "ugfs_internal.h"

int disk_seek_abs(long offset) {
    if (!g_disk) {
        return -1;
    }
    return fseek(g_disk, offset, SEEK_SET);
}

int disk_read_block(uint32_t block_no, void *buf) {
    if (!g_disk || block_no >= FS_TOTAL_BLOCKS) {
        return -1;
    }
    if (disk_seek_abs((long)block_no * (long)FS_BLOCK_SIZE) != 0) {
        return -1;
    }
    return fread(buf, 1, FS_BLOCK_SIZE, g_disk) == FS_BLOCK_SIZE ? 0 : -1;
}

int disk_write_block(uint32_t block_no, const void *buf) {
    if (!g_disk || block_no >= FS_TOTAL_BLOCKS) {
        return -1;
    }
    if (disk_seek_abs((long)block_no * (long)FS_BLOCK_SIZE) != 0) {
        return -1;
    }
    if (fwrite(buf, 1, FS_BLOCK_SIZE, g_disk) != FS_BLOCK_SIZE) {
        return -1;
    }
    return 0;
}

int flush_super(void) {
    unsigned char block[FS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    memcpy(block, &g_super, sizeof(g_super));
    if (disk_write_block(FS_SUPER_BLOCK, block) != 0) {
        return -1;
    }
    fflush(g_disk);
    return 0;
}

int load_super(void) {
    unsigned char block[FS_BLOCK_SIZE];
    if (disk_read_block(FS_SUPER_BLOCK, block) != 0) {
        return -1;
    }
    memcpy(&g_super, block, sizeof(g_super));
    if (strncmp(g_super.magic, FS_MAGIC, strlen(FS_MAGIC)) != 0 ||
        g_super.version != FS_VERSION ||
        g_super.block_size != FS_BLOCK_SIZE ||
        g_super.total_blocks != FS_TOTAL_BLOCKS) {
        return -1;
    }
    return 0;
}

void zero_block(uint32_t block_no) {
    unsigned char block[FS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    disk_write_block(block_no, block);
}
