#include "ugfs_internal.h"

int make_initial_inodes(void) {
    FsInodeDisk inode;
    unsigned char block[FS_BLOCK_SIZE];
    FsPwdEntry users[FS_MAX_USERS];
    uint32_t t = now_u32();

    memset(&inode, 0, sizeof(inode));
    write_inode(0, &inode);

    memset(&inode, 0, sizeof(inode));
    /* The original course sample uses DEFAULTMODE 0777; keeping root writable
       makes the non-root demo users able to create their own test directories. */
    inode.mode = FS_TYPE_DIR | 0777u;
    inode.nlink = 1;
    inode.uid = 0;
    inode.gid = 0;
    inode.size = 3u * FS_DIR_ENTRY_SIZE;
    inode.addr[0] = FS_ROOT_DIR_BLOCK;
    inode.atime = inode.mtime = inode.ctime = t;
    if (write_inode(FS_ROOT_INO, &inode) != 0) {
        return -1;
    }

    memset(block, 0, sizeof(block));
    pack_dir_entry(block, 0, FS_ROOT_INO, ".");
    pack_dir_entry(block, 1, FS_ROOT_INO, "..");
    pack_dir_entry(block, 2, FS_ETC_INO, "etc");
    if (disk_write_block(FS_ROOT_DIR_BLOCK, block) != 0) {
        return -1;
    }

    memset(&inode, 0, sizeof(inode));
    inode.mode = FS_TYPE_DIR | 0755u;
    inode.nlink = 1;
    inode.uid = 0;
    inode.gid = 0;
    inode.size = 3u * FS_DIR_ENTRY_SIZE;
    inode.addr[0] = FS_ETC_DIR_BLOCK;
    inode.atime = inode.mtime = inode.ctime = t;
    if (write_inode(FS_ETC_INO, &inode) != 0) {
        return -1;
    }

    memset(block, 0, sizeof(block));
    pack_dir_entry(block, 0, FS_ETC_INO, ".");
    pack_dir_entry(block, 1, FS_ROOT_INO, "..");
    pack_dir_entry(block, 2, FS_PASSWD_INO, "passwd");
    if (disk_write_block(FS_ETC_DIR_BLOCK, block) != 0) {
        return -1;
    }

    memset(&inode, 0, sizeof(inode));
    inode.mode = FS_TYPE_FILE | 0644u;
    inode.nlink = 1;
    inode.uid = 0;
    inode.gid = 0;
    inode.size = sizeof(FsPwdEntry) * FS_MAX_USERS;
    inode.addr[0] = FS_PASSWD_BLOCK;
    inode.atime = inode.mtime = inode.ctime = t;
    if (write_inode(FS_PASSWD_INO, &inode) != 0) {
        return -1;
    }

    memset(users, 0, sizeof(users));
    users[0].used = 1;
    users[0].uid = 0;
    users[0].gid = 0;
    strcpy(users[0].password, "root");
    users[1].used = 1;
    users[1].uid = 2116;
    users[1].gid = 3;
    strcpy(users[1].password, "dddd");
    users[2].used = 1;
    users[2].uid = 2117;
    users[2].gid = 3;
    strcpy(users[2].password, "bbbb");
    users[3].used = 1;
    users[3].uid = 2118;
    users[3].gid = 4;
    strcpy(users[3].password, "abcd");
    users[4].used = 1;
    users[4].uid = 2119;
    users[4].gid = 4;
    strcpy(users[4].password, "cccc");
    memset(block, 0, sizeof(block));
    memcpy(block, users, sizeof(users));
    return disk_write_block(FS_PASSWD_BLOCK, block);
}

int ugfs_format(const char *disk_path) {
    unsigned char zero[FS_BLOCK_SIZE];
    uint32_t i;
    int b;

    if (ensure_layout() != 0) {
        return -1;
    }
    ugfs_unmount();

    if (!disk_path || disk_path[0] == '\0') {
        disk_path = UGFS_DISK_FILE;
    }
    strncpy(g_disk_path, disk_path, sizeof(g_disk_path) - 1);
    g_disk_path[sizeof(g_disk_path) - 1] = '\0';

    g_disk = fopen(disk_path, "wb+");
    if (!g_disk) {
        printf("Cannot create disk file %s: %s\n", disk_path, strerror(errno));
        return -1;
    }

    memset(zero, 0, sizeof(zero));
    for (i = 0; i < FS_TOTAL_BLOCKS; ++i) {
        if (fwrite(zero, 1, sizeof(zero), g_disk) != sizeof(zero)) {
            printf("Failed to initialize virtual disk.\n");
            ugfs_unmount();
            return -1;
        }
    }
    fflush(g_disk);

    memset(&g_super, 0, sizeof(g_super));
    strncpy(g_super.magic, FS_MAGIC, sizeof(g_super.magic) - 1);
    g_super.version = FS_VERSION;
    g_super.block_size = FS_BLOCK_SIZE;
    g_super.total_blocks = FS_TOTAL_BLOCKS;
    g_super.inode_start_block = FS_INODE_START_BLOCK;
    g_super.inode_blocks = FS_INODE_BLOCKS;
    g_super.data_start_block = FS_DATA_START_BLOCK;
    g_super.data_blocks = FS_DATA_BLOCKS;
    g_super.max_inodes = FS_MAX_INODES;
    g_super.root_ino = FS_ROOT_INO;
    g_super.passwd_ino = FS_PASSWD_INO;
    g_super.next_inode_scan = 4;

    for (b = (int)FS_TOTAL_BLOCKS - 1; b >= (int)FS_DATA_START_BLOCK + 3; --b) {
        if (push_free_block((uint32_t)b, true) != 0) {
            ugfs_unmount();
            return -1;
        }
    }

    g_super.total_free_inodes = FS_MAX_INODES - 4u;
    g_super.inode_free_count = 0;
    for (i = 53; i >= 4 && g_super.inode_free_count < FS_NICINOD; --i) {
        g_super.inode_stack[g_super.inode_free_count++] = i;
        if (i == 4) {
            break;
        }
    }

    if (make_initial_inodes() != 0 || flush_super() != 0) {
        printf("Failed to write initial file system metadata.\n");
        ugfs_unmount();
        return -1;
    }

    reset_runtime();
    printf("Formatted %s: %u blocks, %u data blocks, %u inodes.\n",
           disk_path, (unsigned)FS_TOTAL_BLOCKS, (unsigned)FS_DATA_BLOCKS,
           (unsigned)FS_MAX_INODES);
    return 0;
}

int ugfs_mount(const char *disk_path) {
    if (ensure_layout() != 0) {
        return -1;
    }
    ugfs_unmount();
    if (!disk_path || disk_path[0] == '\0') {
        disk_path = UGFS_DISK_FILE;
    }
    strncpy(g_disk_path, disk_path, sizeof(g_disk_path) - 1);
    g_disk_path[sizeof(g_disk_path) - 1] = '\0';

    g_disk = fopen(disk_path, "rb+");
    if (!g_disk) {
        return -1;
    }
    if (load_super() != 0) {
        fclose(g_disk);
        g_disk = NULL;
        return -1;
    }
    reset_runtime();
    return 0;
}

void ugfs_unmount(void) {
    if (g_disk) {
        flush_super();
        fclose(g_disk);
        g_disk = NULL;
    }
    memset(&g_super, 0, sizeof(g_super));
    reset_runtime();
}
