#include "ugfs_internal.h"

int write_free_group(uint32_t block_no) {
    unsigned char block[FS_BLOCK_SIZE];
    FsFreeGroup group;
    memset(&group, 0, sizeof(group));
    group.magic = FS_FREE_GROUP_MAGIC;
    group.count = g_super.free_count;
    memcpy(group.blocks, g_super.free_stack, sizeof(uint32_t) * g_super.free_count);
    memset(block, 0, sizeof(block));
    memcpy(block, &group, sizeof(group));
    return disk_write_block(block_no, block);
}

int push_free_block(uint32_t block_no, bool count_total) {
    if (block_no < FS_DATA_START_BLOCK || block_no >= FS_TOTAL_BLOCKS) {
        return -1;
    }
    if (g_super.free_count == FS_NICFREE) {
        if (write_free_group(block_no) != 0) {
            return -1;
        }
        g_super.free_count = 0;
    }
    g_super.free_stack[g_super.free_count++] = block_no;
    if (count_total) {
        ++g_super.total_free_blocks;
    }
    g_super.dirty = 1;
    return 0;
}

int alloc_block(uint32_t *out_block) {
    uint32_t block_no;
    if (!out_block || g_super.total_free_blocks == 0 || g_super.free_count == 0) {
        return -1;
    }
    block_no = g_super.free_stack[--g_super.free_count];
    --g_super.total_free_blocks;

    if (g_super.free_count == 0 && g_super.total_free_blocks > 0) {
        unsigned char raw[FS_BLOCK_SIZE];
        FsFreeGroup group;
        memset(&group, 0, sizeof(group));
        if (disk_read_block(block_no, raw) == 0) {
            memcpy(&group, raw, sizeof(group));
            if (group.magic == FS_FREE_GROUP_MAGIC && group.count <= FS_NICFREE) {
                g_super.free_count = group.count;
                memcpy(g_super.free_stack, group.blocks, sizeof(uint32_t) * group.count);
            }
        }
    }

    zero_block(block_no);
    g_super.dirty = 1;
    *out_block = block_no;
    return 0;
}

int free_block(uint32_t block_no) {
    if (push_free_block(block_no, true) != 0) {
        return -1;
    }
    return 0;
}

int refill_inode_stack(void) {
    uint16_t ino;
    FsInodeDisk inode;
    g_super.inode_free_count = 0;
    for (ino = 4; ino < FS_MAX_INODES && g_super.inode_free_count < FS_NICINOD; ++ino) {
        if (read_inode(ino, &inode) == 0 && inode.mode == 0) {
            g_super.inode_stack[g_super.inode_free_count++] = ino;
        }
    }
    return g_super.inode_free_count > 0 ? 0 : -1;
}

int alloc_inode(uint16_t *out_ino) {
    if (!out_ino || g_super.total_free_inodes == 0) {
        return -1;
    }
    if (g_super.inode_free_count == 0 && refill_inode_stack() != 0) {
        return -1;
    }
    *out_ino = (uint16_t)g_super.inode_stack[--g_super.inode_free_count];
    --g_super.total_free_inodes;
    g_super.dirty = 1;
    return 0;
}

int free_inode(uint16_t ino) {
    FsInodeDisk empty;
    if (ino <= FS_PASSWD_INO || ino >= FS_MAX_INODES) {
        return -1;
    }
    memset(&empty, 0, sizeof(empty));
    if (write_inode(ino, &empty) != 0) {
        return -1;
    }
    if (g_super.inode_free_count < FS_NICINOD) {
        g_super.inode_stack[g_super.inode_free_count++] = ino;
    }
    ++g_super.total_free_inodes;
    g_super.dirty = 1;
    return 0;
}

int inode_get_block(FsInodeDisk *inode, uint32_t logical_block,
                           bool allocate, uint32_t *out_block) {
    if (!inode || !out_block || logical_block >= FS_DIRECT_BLOCKS) {
        return -1;
    }
    if (inode->addr[logical_block] == 0) {
        uint32_t new_block;
        if (!allocate) {
            *out_block = 0;
            return 0;
        }
        if (alloc_block(&new_block) != 0) {
            return -1;
        }
        inode->addr[logical_block] = new_block;
    }
    *out_block = inode->addr[logical_block];
    return 0;
}

int inode_truncate(FsInodeDisk *inode) {
    uint32_t i;
    if (!inode) {
        return -1;
    }
    for (i = 0; i < FS_DIRECT_BLOCKS; ++i) {
        if (inode->addr[i] != 0) {
            free_block(inode->addr[i]);
            inode->addr[i] = 0;
        }
    }
    inode->size = 0;
    inode->mtime = now_u32();
    return 0;
}
