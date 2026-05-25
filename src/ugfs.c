#include "ugfs.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *g_disk = NULL;
static FsSuper g_super;
static FsOpenFile g_sys_open[FS_SYS_OPEN_MAX];
static FsSession g_session;
static char g_disk_path[FS_PATH_MAX] = UGFS_DISK_FILE;

static uint32_t now_u32(void) {
    return (uint32_t)time(NULL);
}

static void reset_runtime(void) {
    uint32_t i;
    memset(g_sys_open, 0, sizeof(g_sys_open));
    memset(&g_session, 0, sizeof(g_session));
    g_session.cwd_ino = FS_ROOT_INO;
    strcpy(g_session.cwd_path, "/");
    for (i = 0; i < FS_USER_OPEN_MAX; ++i) {
        g_session.user_open[i] = -1;
    }
}

static bool is_mounted(void) {
    return g_disk != NULL;
}

static int ensure_layout(void) {
    if (sizeof(FsInodeDisk) != FS_INODE_SIZE) {
        fprintf(stderr, "internal error: FsInodeDisk size is %u, expected %u\n",
                (unsigned)sizeof(FsInodeDisk), (unsigned)FS_INODE_SIZE);
        return -1;
    }
    if (sizeof(FsDirEntry) != FS_DIR_ENTRY_SIZE) {
        fprintf(stderr, "internal error: FsDirEntry size is %u, expected %u\n",
                (unsigned)sizeof(FsDirEntry), (unsigned)FS_DIR_ENTRY_SIZE);
        return -1;
    }
    if (sizeof(FsPwdEntry) * FS_MAX_USERS > FS_BLOCK_SIZE) {
        fprintf(stderr, "internal error: password table is too large\n");
        return -1;
    }
    if (sizeof(FsSuper) > FS_BLOCK_SIZE) {
        fprintf(stderr, "internal error: super block is too large\n");
        return -1;
    }
    return 0;
}

static int disk_seek_abs(long offset) {
    if (!g_disk) {
        return -1;
    }
    return fseek(g_disk, offset, SEEK_SET);
}

static int disk_read_block(uint32_t block_no, void *buf) {
    if (!g_disk || block_no >= FS_TOTAL_BLOCKS) {
        return -1;
    }
    if (disk_seek_abs((long)block_no * (long)FS_BLOCK_SIZE) != 0) {
        return -1;
    }
    return fread(buf, 1, FS_BLOCK_SIZE, g_disk) == FS_BLOCK_SIZE ? 0 : -1;
}

static int disk_write_block(uint32_t block_no, const void *buf) {
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

static int flush_super(void) {
    unsigned char block[FS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    memcpy(block, &g_super, sizeof(g_super));
    if (disk_write_block(FS_SUPER_BLOCK, block) != 0) {
        return -1;
    }
    fflush(g_disk);
    return 0;
}

static int load_super(void) {
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

static long inode_offset(uint16_t ino) {
    return ((long)FS_INODE_START_BLOCK * (long)FS_BLOCK_SIZE) +
           ((long)ino * (long)FS_INODE_SIZE);
}

static int read_inode(uint16_t ino, FsInodeDisk *inode) {
    if (!g_disk || ino >= FS_MAX_INODES || !inode) {
        return -1;
    }
    if (disk_seek_abs(inode_offset(ino)) != 0) {
        return -1;
    }
    return fread(inode, 1, sizeof(*inode), g_disk) == sizeof(*inode) ? 0 : -1;
}

static int write_inode(uint16_t ino, const FsInodeDisk *inode) {
    if (!g_disk || ino >= FS_MAX_INODES || !inode) {
        return -1;
    }
    if (disk_seek_abs(inode_offset(ino)) != 0) {
        return -1;
    }
    if (fwrite(inode, 1, sizeof(*inode), g_disk) != sizeof(*inode)) {
        return -1;
    }
    return 0;
}

static bool inode_is_file(const FsInodeDisk *inode) {
    return inode && ((inode->mode & FS_TYPE_MASK) == FS_TYPE_FILE);
}

static bool inode_is_dir(const FsInodeDisk *inode) {
    return inode && ((inode->mode & FS_TYPE_MASK) == FS_TYPE_DIR);
}

static bool has_perm(const FsInodeDisk *inode, uint32_t op) {
    uint32_t perm;
    uint32_t bits;
    if (!inode) {
        return false;
    }
    if (!g_session.logged_in) {
        return false;
    }
    if (g_session.uid == 0) {
        return true;
    }
    perm = inode->mode & FS_PERM_MASK;
    if (g_session.uid == inode->uid) {
        bits = (perm >> 6) & 07u;
    } else if (g_session.gid == inode->gid) {
        bits = (perm >> 3) & 07u;
    } else {
        bits = perm & 07u;
    }
    return (bits & op) == op;
}

static void zero_block(uint32_t block_no) {
    unsigned char block[FS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    disk_write_block(block_no, block);
}

static int write_free_group(uint32_t block_no) {
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

static int push_free_block(uint32_t block_no, bool count_total) {
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

static int alloc_block(uint32_t *out_block) {
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

static int free_block(uint32_t block_no) {
    if (push_free_block(block_no, true) != 0) {
        return -1;
    }
    return 0;
}

static int refill_inode_stack(void) {
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

static int alloc_inode(uint16_t *out_ino) {
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

static int free_inode(uint16_t ino) {
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

static int inode_get_block(FsInodeDisk *inode, uint32_t logical_block,
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

static int inode_truncate(FsInodeDisk *inode) {
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

static int dir_read_entry_from_inode(const FsInodeDisk *dir_inode,
                                     uint32_t index,
                                     FsDirEntry *entry) {
    uint32_t logical;
    uint32_t slot;
    unsigned char block[FS_BLOCK_SIZE];
    if (!dir_inode || !entry || index >= FS_MAX_DIR_ENTRIES) {
        return -1;
    }
    logical = index / FS_DIR_ENTRIES_PER_BLOCK;
    slot = index % FS_DIR_ENTRIES_PER_BLOCK;
    if (dir_inode->addr[logical] == 0) {
        memset(entry, 0, sizeof(*entry));
        return 0;
    }
    if (disk_read_block(dir_inode->addr[logical], block) != 0) {
        return -1;
    }
    memcpy(entry, block + slot * FS_DIR_ENTRY_SIZE, sizeof(*entry));
    return 0;
}

static int dir_write_entry_to_inode(FsInodeDisk *dir_inode,
                                    uint32_t index,
                                    const FsDirEntry *entry) {
    uint32_t logical;
    uint32_t slot;
    uint32_t block_no;
    unsigned char block[FS_BLOCK_SIZE];
    if (!dir_inode || !entry || index >= FS_MAX_DIR_ENTRIES) {
        return -1;
    }
    logical = index / FS_DIR_ENTRIES_PER_BLOCK;
    slot = index % FS_DIR_ENTRIES_PER_BLOCK;
    if (inode_get_block(dir_inode, logical, true, &block_no) != 0) {
        return -1;
    }
    if (disk_read_block(block_no, block) != 0) {
        return -1;
    }
    memcpy(block + slot * FS_DIR_ENTRY_SIZE, entry, sizeof(*entry));
    if (disk_write_block(block_no, block) != 0) {
        return -1;
    }
    if ((index + 1u) * FS_DIR_ENTRY_SIZE > dir_inode->size) {
        dir_inode->size = (index + 1u) * FS_DIR_ENTRY_SIZE;
    }
    dir_inode->mtime = now_u32();
    return 0;
}

static int dir_lookup(uint16_t dir_ino, const char *name, uint16_t *out_ino) {
    FsInodeDisk dir_inode;
    FsDirEntry entry;
    uint32_t i;
    if (!name || !out_ino || read_inode(dir_ino, &dir_inode) != 0 ||
        !inode_is_dir(&dir_inode)) {
        return -1;
    }
    for (i = 0; i < FS_MAX_DIR_ENTRIES; ++i) {
        if (dir_read_entry_from_inode(&dir_inode, i, &entry) != 0) {
            return -1;
        }
        if (entry.ino != 0 && strcmp(entry.name, name) == 0) {
            *out_ino = entry.ino;
            return 0;
        }
    }
    return -1;
}

static int dir_find_child_name(uint16_t parent_ino, uint16_t child_ino,
                               char *name, size_t name_size) {
    FsInodeDisk dir_inode;
    FsDirEntry entry;
    uint32_t i;
    if (!name || name_size == 0 || read_inode(parent_ino, &dir_inode) != 0 ||
        !inode_is_dir(&dir_inode)) {
        return -1;
    }
    for (i = 0; i < FS_MAX_DIR_ENTRIES; ++i) {
        if (dir_read_entry_from_inode(&dir_inode, i, &entry) != 0) {
            return -1;
        }
        if (entry.ino == child_ino && strcmp(entry.name, ".") != 0 &&
            strcmp(entry.name, "..") != 0) {
            strncpy(name, entry.name, name_size - 1);
            name[name_size - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

static int dir_add_entry(uint16_t dir_ino, const char *name, uint16_t child_ino) {
    FsInodeDisk dir_inode;
    FsDirEntry entry;
    uint32_t i;
    uint16_t tmp;
    if (!name || strlen(name) == 0 || strlen(name) > FS_NAME_MAX) {
        return -1;
    }
    if (dir_lookup(dir_ino, name, &tmp) == 0) {
        return -1;
    }
    if (read_inode(dir_ino, &dir_inode) != 0 || !inode_is_dir(&dir_inode)) {
        return -1;
    }
    for (i = 0; i < FS_MAX_DIR_ENTRIES; ++i) {
        if (dir_read_entry_from_inode(&dir_inode, i, &entry) != 0) {
            return -1;
        }
        if (entry.ino == 0) {
            memset(&entry, 0, sizeof(entry));
            entry.ino = child_ino;
            strncpy(entry.name, name, FS_NAME_MAX);
            entry.name[FS_NAME_MAX] = '\0';
            if (dir_write_entry_to_inode(&dir_inode, i, &entry) != 0) {
                return -1;
            }
            return write_inode(dir_ino, &dir_inode);
        }
    }
    return -1;
}

static int dir_remove_entry(uint16_t dir_ino, const char *name, uint16_t *old_ino) {
    FsInodeDisk dir_inode;
    FsDirEntry entry;
    uint32_t i;
    if (!name || read_inode(dir_ino, &dir_inode) != 0 || !inode_is_dir(&dir_inode)) {
        return -1;
    }
    for (i = 0; i < FS_MAX_DIR_ENTRIES; ++i) {
        if (dir_read_entry_from_inode(&dir_inode, i, &entry) != 0) {
            return -1;
        }
        if (entry.ino != 0 && strcmp(entry.name, name) == 0) {
            if (old_ino) {
                *old_ino = entry.ino;
            }
            memset(&entry, 0, sizeof(entry));
            if (dir_write_entry_to_inode(&dir_inode, i, &entry) != 0) {
                return -1;
            }
            return write_inode(dir_ino, &dir_inode);
        }
    }
    return -1;
}

static bool dir_is_empty(uint16_t dir_ino) {
    FsInodeDisk dir_inode;
    FsDirEntry entry;
    uint32_t i;
    if (read_inode(dir_ino, &dir_inode) != 0 || !inode_is_dir(&dir_inode)) {
        return false;
    }
    for (i = 0; i < FS_MAX_DIR_ENTRIES; ++i) {
        if (dir_read_entry_from_inode(&dir_inode, i, &entry) != 0) {
            return false;
        }
        if (entry.ino != 0 && strcmp(entry.name, ".") != 0 &&
            strcmp(entry.name, "..") != 0) {
            return false;
        }
    }
    return true;
}

static int path_next(const char **cursor, char *name) {
    const char *p;
    size_t len = 0;
    if (!cursor || !*cursor || !name) {
        return -1;
    }
    p = *cursor;
    while (*p == '/' || *p == '\\') {
        ++p;
    }
    if (*p == '\0') {
        *cursor = p;
        return 0;
    }
    while (p[len] != '\0' && p[len] != '/' && p[len] != '\\') {
        ++len;
    }
    if (len == 0 || len > FS_NAME_MAX) {
        return -1;
    }
    memcpy(name, p, len);
    name[len] = '\0';
    *cursor = p + len;
    return 1;
}

static bool path_has_next(const char *cursor) {
    char name[FS_NAME_MAX + 1];
    const char *p = cursor;
    return path_next(&p, name) == 1;
}

static int resolve_path(const char *path, uint16_t *out_ino) {
    uint16_t cur;
    uint16_t next;
    char name[FS_NAME_MAX + 1];
    const char *p;
    int r;
    if (!path || !out_ino) {
        return -1;
    }
    cur = (path[0] == '/' || path[0] == '\\') ? FS_ROOT_INO : g_session.cwd_ino;
    p = path;
    while ((r = path_next(&p, name)) == 1) {
        if (strcmp(name, ".") == 0) {
            continue;
        }
        if (strcmp(name, "..") == 0) {
            if (dir_lookup(cur, "..", &next) != 0) {
                return -1;
            }
            cur = next;
            continue;
        }
        if (dir_lookup(cur, name, &next) != 0) {
            return -1;
        }
        cur = next;
    }
    if (r < 0) {
        return -1;
    }
    *out_ino = cur;
    return 0;
}

static int resolve_parent(const char *path, uint16_t *parent_ino, char *leaf) {
    uint16_t cur;
    uint16_t next;
    char name[FS_NAME_MAX + 1];
    const char *p;
    int r;
    if (!path || !parent_ino || !leaf) {
        return -1;
    }
    cur = (path[0] == '/' || path[0] == '\\') ? FS_ROOT_INO : g_session.cwd_ino;
    p = path;
    r = path_next(&p, name);
    if (r != 1) {
        return -1;
    }
    for (;;) {
        if (!path_has_next(p)) {
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                return -1;
            }
            *parent_ino = cur;
            strcpy(leaf, name);
            return 0;
        }
        if (strcmp(name, ".") == 0) {
            /* no-op */
        } else if (strcmp(name, "..") == 0) {
            if (dir_lookup(cur, "..", &next) != 0) {
                return -1;
            }
            cur = next;
        } else {
            if (dir_lookup(cur, name, &next) != 0) {
                return -1;
            }
            cur = next;
        }
        r = path_next(&p, name);
        if (r != 1) {
            return -1;
        }
    }
}

static int build_path(uint16_t ino, char *out, size_t out_size) {
    char parts[64][FS_NAME_MAX + 1];
    uint16_t cur = ino;
    uint16_t parent = ino;
    int depth = 0;
    int i;
    if (!out || out_size == 0) {
        return -1;
    }
    if (ino == FS_ROOT_INO) {
        strncpy(out, "/", out_size - 1);
        out[out_size - 1] = '\0';
        return 0;
    }
    while (cur != FS_ROOT_INO && depth < 64) {
        if (dir_lookup(cur, "..", &parent) != 0) {
            return -1;
        }
        if (dir_find_child_name(parent, cur, parts[depth], sizeof(parts[depth])) != 0) {
            return -1;
        }
        cur = parent;
        ++depth;
    }
    out[0] = '\0';
    strncat(out, "/", out_size - strlen(out) - 1);
    for (i = depth - 1; i >= 0; --i) {
        strncat(out, parts[i], out_size - strlen(out) - 1);
        if (i > 0) {
            strncat(out, "/", out_size - strlen(out) - 1);
        }
    }
    return 0;
}

static int read_users(FsPwdEntry users[FS_MAX_USERS]) {
    FsInodeDisk inode;
    unsigned char block[FS_BLOCK_SIZE];
    if (read_inode(FS_PASSWD_INO, &inode) != 0 || inode.addr[0] == 0) {
        return -1;
    }
    if (disk_read_block(inode.addr[0], block) != 0) {
        return -1;
    }
    memcpy(users, block, sizeof(FsPwdEntry) * FS_MAX_USERS);
    return 0;
}

static int write_users(const FsPwdEntry users[FS_MAX_USERS]) {
    FsInodeDisk inode;
    unsigned char block[FS_BLOCK_SIZE];
    if (read_inode(FS_PASSWD_INO, &inode) != 0 || inode.addr[0] == 0) {
        return -1;
    }
    memset(block, 0, sizeof(block));
    memcpy(block, users, sizeof(FsPwdEntry) * FS_MAX_USERS);
    if (disk_write_block(inode.addr[0], block) != 0) {
        return -1;
    }
    inode.mtime = now_u32();
    return write_inode(FS_PASSWD_INO, &inode);
}

static int find_user(const FsPwdEntry users[FS_MAX_USERS], uint16_t uid) {
    uint32_t i;
    for (i = 0; i < FS_MAX_USERS; ++i) {
        if (users[i].used && users[i].uid == uid) {
            return (int)i;
        }
    }
    return -1;
}

static int require_mounted(void) {
    if (!is_mounted()) {
        printf("No file system is mounted. Run: format\n");
        return -1;
    }
    return 0;
}

static int require_login(void) {
    if (require_mounted() != 0) {
        return -1;
    }
    if (!g_session.logged_in) {
        printf("Please login first.\n");
        return -1;
    }
    return 0;
}

static void pack_dir_entry(unsigned char *block, uint32_t slot,
                           uint16_t ino, const char *name) {
    FsDirEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.ino = ino;
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    memcpy(block + slot * FS_DIR_ENTRY_SIZE, &entry, sizeof(entry));
}

static int write_raw_dir_block(uint32_t block_no, uint16_t self, uint16_t parent) {
    unsigned char block[FS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    pack_dir_entry(block, 0, self, ".");
    pack_dir_entry(block, 1, parent, "..");
    return disk_write_block(block_no, block);
}

static int make_initial_inodes(void) {
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

static void mode_to_string(uint16_t mode, char out[11]) {
    static const char chars[] = {'r', 'w', 'x'};
    uint16_t perm = mode & FS_PERM_MASK;
    int i;
    out[0] = ((mode & FS_TYPE_MASK) == FS_TYPE_DIR) ? 'd' : '-';
    for (i = 0; i < 9; ++i) {
        uint16_t bit = (uint16_t)(1u << (8 - i));
        out[i + 1] = (perm & bit) ? chars[i % 3] : '-';
    }
    out[10] = '\0';
}

static int cmd_login(uint16_t uid, const char *password) {
    FsPwdEntry users[FS_MAX_USERS];
    int idx;
    if (require_mounted() != 0) {
        return -1;
    }
    if (g_session.logged_in) {
        printf("Already logged in as %u. Run logout first.\n", g_session.uid);
        return -1;
    }
    if (read_users(users) != 0) {
        printf("Cannot read password table.\n");
        return -1;
    }
    idx = find_user(users, uid);
    if (idx < 0 || strcmp(users[idx].password, password) != 0) {
        printf("Incorrect uid or password.\n");
        return -1;
    }
    g_session.logged_in = true;
    g_session.uid = users[idx].uid;
    g_session.gid = users[idx].gid;
    g_session.cwd_ino = FS_ROOT_INO;
    strcpy(g_session.cwd_path, "/");
    printf("Login ok: uid=%u gid=%u\n", g_session.uid, g_session.gid);
    return 0;
}

static int close_fd_internal(int ufd) {
    int sysfd;
    if (ufd < 0 || ufd >= (int)FS_USER_OPEN_MAX ||
        g_session.user_open[ufd] < 0) {
        return -1;
    }
    sysfd = g_session.user_open[ufd];
    g_session.user_open[ufd] = -1;
    if (sysfd >= 0 && sysfd < (int)FS_SYS_OPEN_MAX && g_sys_open[sysfd].used) {
        if (g_sys_open[sysfd].ref_count > 0) {
            --g_sys_open[sysfd].ref_count;
        }
        if (g_sys_open[sysfd].ref_count == 0) {
            memset(&g_sys_open[sysfd], 0, sizeof(g_sys_open[sysfd]));
        }
    }
    return 0;
}

static int cmd_logout(void) {
    uint32_t i;
    if (require_login() != 0) {
        return -1;
    }
    for (i = 0; i < FS_USER_OPEN_MAX; ++i) {
        if (g_session.user_open[i] >= 0) {
            close_fd_internal((int)i);
        }
    }
    printf("Logout uid=%u\n", g_session.uid);
    reset_runtime();
    return 0;
}

static int cmd_useradd(uint16_t uid, uint16_t gid, const char *password) {
    FsPwdEntry users[FS_MAX_USERS];
    uint32_t i;
    if (require_login() != 0) {
        return -1;
    }
    if (g_session.uid != 0) {
        printf("Only root can add users.\n");
        return -1;
    }
    if (!password || strlen(password) >= sizeof(users[0].password)) {
        printf("Password is too long.\n");
        return -1;
    }
    if (read_users(users) != 0) {
        return -1;
    }
    if (find_user(users, uid) >= 0) {
        printf("User %u already exists.\n", uid);
        return -1;
    }
    for (i = 0; i < FS_MAX_USERS; ++i) {
        if (!users[i].used) {
            users[i].used = 1;
            users[i].uid = uid;
            users[i].gid = gid;
            strcpy(users[i].password, password);
            if (write_users(users) != 0) {
                return -1;
            }
            printf("User added: uid=%u gid=%u\n", uid, gid);
            return 0;
        }
    }
    printf("Password table is full.\n");
    return -1;
}

static int cmd_passwd(uint16_t uid, const char *password) {
    FsPwdEntry users[FS_MAX_USERS];
    int idx;
    if (require_login() != 0) {
        return -1;
    }
    if (g_session.uid != 0 && g_session.uid != uid) {
        printf("Permission denied.\n");
        return -1;
    }
    if (!password || strlen(password) >= sizeof(users[0].password)) {
        printf("Password is too long.\n");
        return -1;
    }
    if (read_users(users) != 0) {
        return -1;
    }
    idx = find_user(users, uid);
    if (idx < 0) {
        printf("No such user.\n");
        return -1;
    }
    strcpy(users[idx].password, password);
    if (write_users(users) != 0) {
        return -1;
    }
    printf("Password changed for uid=%u\n", uid);
    return 0;
}

static int cmd_mkdir(const char *path) {
    uint16_t parent_ino;
    uint16_t child_ino;
    uint16_t existing;
    uint32_t block_no;
    char name[FS_NAME_MAX + 1];
    FsInodeDisk parent;
    FsInodeDisk child;
    uint32_t t = now_u32();
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_parent(path, &parent_ino, name) != 0) {
        printf("Invalid path.\n");
        return -1;
    }
    if (read_inode(parent_ino, &parent) != 0 || !inode_is_dir(&parent) ||
        !has_perm(&parent, FS_MODE_WRITE | FS_MODE_EXEC)) {
        printf("Permission denied or parent is not a directory.\n");
        return -1;
    }
    if (dir_lookup(parent_ino, name, &existing) == 0) {
        printf("Path already exists.\n");
        return -1;
    }
    if (alloc_inode(&child_ino) != 0) {
        printf("No free inode for directory.\n");
        return -1;
    }
    if (alloc_block(&block_no) != 0) {
        free_inode(child_ino);
        printf("No free data block for directory.\n");
        return -1;
    }
    if (write_raw_dir_block(block_no, child_ino, parent_ino) != 0) {
        free_block(block_no);
        free_inode(child_ino);
        return -1;
    }
    memset(&child, 0, sizeof(child));
    child.mode = FS_TYPE_DIR | 0755u;
    child.nlink = 1;
    child.uid = g_session.uid;
    child.gid = g_session.gid;
    child.size = 2u * FS_DIR_ENTRY_SIZE;
    child.addr[0] = block_no;
    child.atime = child.mtime = child.ctime = t;
    if (write_inode(child_ino, &child) != 0 ||
        dir_add_entry(parent_ino, name, child_ino) != 0) {
        free_block(block_no);
        free_inode(child_ino);
        return -1;
    }
    flush_super();
    printf("Directory created: %s\n", path);
    return 0;
}

static int cmd_chdir(const char *path) {
    uint16_t ino;
    FsInodeDisk inode;
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_path(path, &ino) != 0 || read_inode(ino, &inode) != 0 ||
        !inode_is_dir(&inode)) {
        printf("No such directory.\n");
        return -1;
    }
    if (!has_perm(&inode, FS_MODE_EXEC)) {
        printf("Permission denied.\n");
        return -1;
    }
    g_session.cwd_ino = ino;
    if (build_path(ino, g_session.cwd_path, sizeof(g_session.cwd_path)) != 0) {
        strcpy(g_session.cwd_path, "?");
    }
    return 0;
}

static int cmd_dir(const char *path) {
    uint16_t ino;
    FsInodeDisk dir_inode;
    FsInodeDisk child_inode;
    FsDirEntry entry;
    char mode[11];
    uint32_t i;
    if (require_login() != 0) {
        return -1;
    }
    if (!path) {
        ino = g_session.cwd_ino;
    } else if (resolve_path(path, &ino) != 0) {
        printf("No such directory.\n");
        return -1;
    }
    if (read_inode(ino, &dir_inode) != 0 || !inode_is_dir(&dir_inode)) {
        printf("Not a directory.\n");
        return -1;
    }
    if (!has_perm(&dir_inode, FS_MODE_READ)) {
        printf("Permission denied.\n");
        return -1;
    }
    printf("mode        uid   gid   inode    size name\n");
    for (i = 0; i < FS_MAX_DIR_ENTRIES; ++i) {
        if (dir_read_entry_from_inode(&dir_inode, i, &entry) != 0) {
            return -1;
        }
        if (entry.ino == 0) {
            continue;
        }
        if (read_inode(entry.ino, &child_inode) != 0) {
            continue;
        }
        mode_to_string(child_inode.mode, mode);
        printf("%s %5u %5u %7u %7u %s\n",
               mode,
               (unsigned)child_inode.uid,
               (unsigned)child_inode.gid,
               (unsigned)entry.ino,
               (unsigned)child_inode.size,
               entry.name);
    }
    return 0;
}

static int cmd_create(const char *path, uint16_t perm) {
    uint16_t parent_ino;
    uint16_t ino;
    uint16_t existing;
    char name[FS_NAME_MAX + 1];
    FsInodeDisk parent;
    FsInodeDisk inode;
    uint32_t t = now_u32();
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_parent(path, &parent_ino, name) != 0) {
        printf("Invalid path.\n");
        return -1;
    }
    if (read_inode(parent_ino, &parent) != 0 || !inode_is_dir(&parent) ||
        !has_perm(&parent, FS_MODE_WRITE | FS_MODE_EXEC)) {
        printf("Permission denied or parent is not a directory.\n");
        return -1;
    }
    if (dir_lookup(parent_ino, name, &existing) == 0) {
        printf("File already exists.\n");
        return -1;
    }
    if (alloc_inode(&ino) != 0) {
        printf("No free inode.\n");
        return -1;
    }
    memset(&inode, 0, sizeof(inode));
    inode.mode = FS_TYPE_FILE | (perm & FS_PERM_MASK);
    inode.nlink = 1;
    inode.uid = g_session.uid;
    inode.gid = g_session.gid;
    inode.atime = inode.mtime = inode.ctime = t;
    if (write_inode(ino, &inode) != 0 || dir_add_entry(parent_ino, name, ino) != 0) {
        free_inode(ino);
        return -1;
    }
    flush_super();
    printf("File created: %s\n", path);
    return 0;
}

static int file_is_open(uint16_t ino) {
    uint32_t i;
    for (i = 0; i < FS_SYS_OPEN_MAX; ++i) {
        if (g_sys_open[i].used && g_sys_open[i].ino == ino) {
            return 1;
        }
    }
    return 0;
}

static int parse_open_flags(const char *mode, uint32_t *flags) {
    if (!mode || !flags) {
        return -1;
    }
    if (strcmp(mode, "r") == 0) {
        *flags = FS_OF_READ;
    } else if (strcmp(mode, "w") == 0) {
        *flags = FS_OF_WRITE;
    } else if (strcmp(mode, "rw") == 0 || strcmp(mode, "wr") == 0) {
        *flags = FS_OF_READ | FS_OF_WRITE;
    } else if (strcmp(mode, "a") == 0) {
        *flags = FS_OF_WRITE | FS_OF_APPEND;
    } else {
        return -1;
    }
    return 0;
}

static int cmd_open(const char *path, const char *mode) {
    uint16_t ino;
    FsInodeDisk inode;
    uint32_t flags;
    uint32_t i;
    uint32_t j;
    if (require_login() != 0) {
        return -1;
    }
    if (parse_open_flags(mode, &flags) != 0) {
        printf("Open mode must be r, w, rw, or a.\n");
        return -1;
    }
    if (resolve_path(path, &ino) != 0 || read_inode(ino, &inode) != 0 ||
        !inode_is_file(&inode)) {
        printf("No such file.\n");
        return -1;
    }
    if ((flags & FS_OF_READ) && !has_perm(&inode, FS_MODE_READ)) {
        printf("No read permission.\n");
        return -1;
    }
    if ((flags & FS_OF_WRITE) && !has_perm(&inode, FS_MODE_WRITE)) {
        printf("No write permission.\n");
        return -1;
    }
    for (i = 0; i < FS_SYS_OPEN_MAX; ++i) {
        if (!g_sys_open[i].used) {
            break;
        }
    }
    if (i == FS_SYS_OPEN_MAX) {
        printf("System open file table is full.\n");
        return -1;
    }
    for (j = 0; j < FS_USER_OPEN_MAX; ++j) {
        if (g_session.user_open[j] < 0) {
            break;
        }
    }
    if (j == FS_USER_OPEN_MAX) {
        printf("User open file table is full.\n");
        return -1;
    }
    g_sys_open[i].used = true;
    g_sys_open[i].ino = ino;
    g_sys_open[i].flags = flags;
    g_sys_open[i].offset = (flags & FS_OF_APPEND) ? inode.size : 0;
    g_sys_open[i].ref_count = 1;
    g_session.user_open[j] = (int)i;
    printf("fd=%u\n", (unsigned)j);
    return (int)j;
}

static int cmd_close(int fd) {
    if (require_login() != 0) {
        return -1;
    }
    if (close_fd_internal(fd) != 0) {
        printf("Bad fd.\n");
        return -1;
    }
    printf("Closed fd=%d\n", fd);
    return 0;
}

static int file_write_at(uint16_t ino, uint32_t offset, const unsigned char *buf,
                         uint32_t size, uint32_t *new_offset) {
    FsInodeDisk inode;
    uint32_t pos = offset;
    uint32_t left = size;
    if (read_inode(ino, &inode) != 0 || !inode_is_file(&inode)) {
        return -1;
    }
    if (offset > FS_MAX_FILE_SIZE || size > FS_MAX_FILE_SIZE ||
        offset + size > FS_MAX_FILE_SIZE) {
        printf("File is too large. Max file size is %u bytes.\n",
               (unsigned)FS_MAX_FILE_SIZE);
        return -1;
    }
    while (left > 0) {
        uint32_t logical = pos / FS_BLOCK_SIZE;
        uint32_t off = pos % FS_BLOCK_SIZE;
        uint32_t chunk = FS_BLOCK_SIZE - off;
        uint32_t block_no;
        unsigned char block[FS_BLOCK_SIZE];
        if (chunk > left) {
            chunk = left;
        }
        if (inode_get_block(&inode, logical, true, &block_no) != 0) {
            return -1;
        }
        if (disk_read_block(block_no, block) != 0) {
            return -1;
        }
        memcpy(block + off, buf + (pos - offset), chunk);
        if (disk_write_block(block_no, block) != 0) {
            return -1;
        }
        pos += chunk;
        left -= chunk;
    }
    if (pos > inode.size) {
        inode.size = pos;
    }
    inode.mtime = now_u32();
    if (write_inode(ino, &inode) != 0) {
        return -1;
    }
    if (new_offset) {
        *new_offset = pos;
    }
    flush_super();
    return 0;
}

static int file_read_at(uint16_t ino, uint32_t offset, unsigned char *buf,
                        uint32_t size, uint32_t *read_size) {
    FsInodeDisk inode;
    uint32_t pos = offset;
    uint32_t left;
    uint32_t done = 0;
    if (read_inode(ino, &inode) != 0 || !inode_is_file(&inode)) {
        return -1;
    }
    if (offset >= inode.size) {
        if (read_size) {
            *read_size = 0;
        }
        return 0;
    }
    left = size;
    if (offset + left > inode.size) {
        left = inode.size - offset;
    }
    while (left > 0) {
        uint32_t logical = pos / FS_BLOCK_SIZE;
        uint32_t off = pos % FS_BLOCK_SIZE;
        uint32_t chunk = FS_BLOCK_SIZE - off;
        uint32_t block_no = inode.addr[logical];
        unsigned char block[FS_BLOCK_SIZE];
        if (chunk > left) {
            chunk = left;
        }
        if (block_no == 0) {
            memset(buf + done, 0, chunk);
        } else {
            if (disk_read_block(block_no, block) != 0) {
                return -1;
            }
            memcpy(buf + done, block + off, chunk);
        }
        pos += chunk;
        done += chunk;
        left -= chunk;
    }
    inode.atime = now_u32();
    write_inode(ino, &inode);
    if (read_size) {
        *read_size = done;
    }
    return 0;
}

static int cmd_write(int fd, const char *text) {
    int sysfd;
    uint32_t new_off;
    if (require_login() != 0) {
        return -1;
    }
    if (!text) {
        text = "";
    }
    if (fd < 0 || fd >= (int)FS_USER_OPEN_MAX ||
        (sysfd = g_session.user_open[fd]) < 0 ||
        !g_sys_open[sysfd].used ||
        !(g_sys_open[sysfd].flags & FS_OF_WRITE)) {
        printf("Bad fd or file is not open for write.\n");
        return -1;
    }
    if (file_write_at(g_sys_open[sysfd].ino, g_sys_open[sysfd].offset,
                      (const unsigned char *)text, (uint32_t)strlen(text),
                      &new_off) != 0) {
        return -1;
    }
    g_sys_open[sysfd].offset = new_off;
    printf("Wrote %u bytes.\n", (unsigned)strlen(text));
    return 0;
}

static int cmd_read(int fd, uint32_t size) {
    int sysfd;
    unsigned char *buf;
    uint32_t got;
    if (require_login() != 0) {
        return -1;
    }
    if (fd < 0 || fd >= (int)FS_USER_OPEN_MAX ||
        (sysfd = g_session.user_open[fd]) < 0 ||
        !g_sys_open[sysfd].used ||
        !(g_sys_open[sysfd].flags & FS_OF_READ)) {
        printf("Bad fd or file is not open for read.\n");
        return -1;
    }
    buf = (unsigned char *)malloc(size + 1u);
    if (!buf) {
        return -1;
    }
    if (file_read_at(g_sys_open[sysfd].ino, g_sys_open[sysfd].offset,
                     buf, size, &got) != 0) {
        free(buf);
        return -1;
    }
    buf[got] = '\0';
    fwrite(buf, 1, got, stdout);
    printf("\n");
    g_sys_open[sysfd].offset += got;
    free(buf);
    return 0;
}

static int cmd_cat(const char *path) {
    uint16_t ino;
    FsInodeDisk inode;
    unsigned char *buf;
    uint32_t got;
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_path(path, &ino) != 0 || read_inode(ino, &inode) != 0 ||
        !inode_is_file(&inode)) {
        printf("No such file.\n");
        return -1;
    }
    if (!has_perm(&inode, FS_MODE_READ)) {
        printf("Permission denied.\n");
        return -1;
    }
    buf = (unsigned char *)malloc(inode.size + 1u);
    if (!buf) {
        return -1;
    }
    if (file_read_at(ino, 0, buf, inode.size, &got) != 0) {
        free(buf);
        return -1;
    }
    buf[got] = '\0';
    fwrite(buf, 1, got, stdout);
    printf("\n");
    free(buf);
    return 0;
}

static int cmd_append(const char *path, const char *text) {
    uint16_t ino;
    FsInodeDisk inode;
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_path(path, &ino) != 0 || read_inode(ino, &inode) != 0 ||
        !inode_is_file(&inode)) {
        printf("No such file.\n");
        return -1;
    }
    if (!has_perm(&inode, FS_MODE_WRITE)) {
        printf("Permission denied.\n");
        return -1;
    }
    if (file_write_at(ino, inode.size, (const unsigned char *)text,
                      (uint32_t)strlen(text), NULL) != 0) {
        return -1;
    }
    printf("Appended %u bytes.\n", (unsigned)strlen(text));
    return 0;
}

static int cmd_rm(const char *path) {
    uint16_t parent_ino;
    uint16_t ino;
    char name[FS_NAME_MAX + 1];
    FsInodeDisk parent;
    FsInodeDisk inode;
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_parent(path, &parent_ino, name) != 0 ||
        dir_lookup(parent_ino, name, &ino) != 0 ||
        read_inode(ino, &inode) != 0) {
        printf("No such file.\n");
        return -1;
    }
    if (!inode_is_file(&inode)) {
        printf("Use rmdir to remove directories.\n");
        return -1;
    }
    if (file_is_open(ino)) {
        printf("File is open.\n");
        return -1;
    }
    if (read_inode(parent_ino, &parent) != 0 ||
        !has_perm(&parent, FS_MODE_WRITE | FS_MODE_EXEC)) {
        printf("Permission denied.\n");
        return -1;
    }
    inode_truncate(&inode);
    dir_remove_entry(parent_ino, name, NULL);
    free_inode(ino);
    flush_super();
    printf("Removed file: %s\n", path);
    return 0;
}

static int cmd_rmdir(const char *path) {
    uint16_t parent_ino;
    uint16_t ino;
    char name[FS_NAME_MAX + 1];
    FsInodeDisk parent;
    FsInodeDisk inode;
    uint32_t i;
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_parent(path, &parent_ino, name) != 0 ||
        dir_lookup(parent_ino, name, &ino) != 0 ||
        read_inode(ino, &inode) != 0) {
        printf("No such directory.\n");
        return -1;
    }
    if (ino == FS_ROOT_INO || !inode_is_dir(&inode)) {
        printf("Not a removable directory.\n");
        return -1;
    }
    if (!dir_is_empty(ino)) {
        printf("Directory is not empty.\n");
        return -1;
    }
    if (read_inode(parent_ino, &parent) != 0 ||
        !has_perm(&parent, FS_MODE_WRITE | FS_MODE_EXEC)) {
        printf("Permission denied.\n");
        return -1;
    }
    for (i = 0; i < FS_DIRECT_BLOCKS; ++i) {
        if (inode.addr[i] != 0) {
            free_block(inode.addr[i]);
        }
    }
    dir_remove_entry(parent_ino, name, NULL);
    free_inode(ino);
    flush_super();
    printf("Removed directory: %s\n", path);
    return 0;
}

static int cmd_chmod(const char *path, uint16_t perm) {
    uint16_t ino;
    FsInodeDisk inode;
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_path(path, &ino) != 0 || read_inode(ino, &inode) != 0) {
        printf("No such file or directory.\n");
        return -1;
    }
    if (g_session.uid != 0 && g_session.uid != inode.uid) {
        printf("Only owner or root can chmod.\n");
        return -1;
    }
    inode.mode = (inode.mode & FS_TYPE_MASK) | (perm & FS_PERM_MASK);
    inode.ctime = now_u32();
    if (write_inode(ino, &inode) != 0) {
        return -1;
    }
    printf("Mode changed.\n");
    return 0;
}

static int cmd_stat(const char *path) {
    uint16_t ino;
    FsInodeDisk inode;
    char mode[11];
    uint32_t i;
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_path(path, &ino) != 0 || read_inode(ino, &inode) != 0) {
        printf("No such file or directory.\n");
        return -1;
    }
    mode_to_string(inode.mode, mode);
    printf("inode: %u\n", ino);
    printf("mode : %s (%03o)\n", mode, inode.mode & FS_PERM_MASK);
    printf("uid  : %u\n", inode.uid);
    printf("gid  : %u\n", inode.gid);
    printf("size : %u\n", inode.size);
    printf("blocks:");
    for (i = 0; i < FS_DIRECT_BLOCKS; ++i) {
        if (inode.addr[i] != 0) {
            printf(" %u", inode.addr[i]);
        }
    }
    printf("\n");
    return 0;
}

static int cmd_fsinfo(void) {
    if (require_mounted() != 0) {
        return -1;
    }
    printf("disk file         : %s\n", g_disk_path);
    printf("block size        : %u\n", (unsigned)FS_BLOCK_SIZE);
    printf("total blocks      : %u\n", (unsigned)g_super.total_blocks);
    printf("data blocks       : %u\n", (unsigned)g_super.data_blocks);
    printf("free data blocks  : %u\n", (unsigned)g_super.total_free_blocks);
    printf("free stack depth  : %u / %u\n",
           (unsigned)g_super.free_count, (unsigned)FS_NICFREE);
    printf("inode count       : %u\n", (unsigned)g_super.max_inodes);
    printf("free inodes       : %u\n", (unsigned)g_super.total_free_inodes);
    printf("max file size     : %u bytes\n", (unsigned)FS_MAX_FILE_SIZE);
    return 0;
}

static int cmd_cp(const char *src, const char *dst) {
    uint16_t src_ino;
    FsInodeDisk src_inode;
    unsigned char *buf;
    uint32_t got;
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_path(src, &src_ino) != 0 || read_inode(src_ino, &src_inode) != 0 ||
        !inode_is_file(&src_inode)) {
        printf("No such source file.\n");
        return -1;
    }
    if (!has_perm(&src_inode, FS_MODE_READ)) {
        printf("No read permission.\n");
        return -1;
    }
    if (cmd_create(dst, src_inode.mode & FS_PERM_MASK) != 0) {
        return -1;
    }
    buf = (unsigned char *)malloc(src_inode.size + 1u);
    if (!buf) {
        return -1;
    }
    if (file_read_at(src_ino, 0, buf, src_inode.size, &got) != 0) {
        free(buf);
        return -1;
    }
    {
        uint16_t dst_ino;
        FsInodeDisk dst_inode;
        if (resolve_path(dst, &dst_ino) != 0 || read_inode(dst_ino, &dst_inode) != 0) {
            free(buf);
            return -1;
        }
        if (file_write_at(dst_ino, 0, buf, got, NULL) != 0) {
            free(buf);
            return -1;
        }
    }
    free(buf);
    printf("Copied %s -> %s\n", src, dst);
    return 0;
}

static int cmd_mv(const char *src, const char *dst) {
    uint16_t old_parent;
    uint16_t new_parent;
    uint16_t ino;
    uint16_t tmp;
    char old_name[FS_NAME_MAX + 1];
    char new_name[FS_NAME_MAX + 1];
    FsInodeDisk old_parent_inode;
    FsInodeDisk new_parent_inode;
    FsInodeDisk inode;
    if (require_login() != 0) {
        return -1;
    }
    if (resolve_parent(src, &old_parent, old_name) != 0 ||
        dir_lookup(old_parent, old_name, &ino) != 0 ||
        resolve_parent(dst, &new_parent, new_name) != 0 ||
        read_inode(old_parent, &old_parent_inode) != 0 ||
        read_inode(new_parent, &new_parent_inode) != 0 ||
        read_inode(ino, &inode) != 0) {
        printf("Invalid source or destination.\n");
        return -1;
    }
    if (dir_lookup(new_parent, new_name, &tmp) == 0) {
        printf("Destination already exists.\n");
        return -1;
    }
    if (inode_is_dir(&inode) && old_parent != new_parent) {
        printf("Moving directories across parents is not supported.\n");
        return -1;
    }
    if (!has_perm(&old_parent_inode, FS_MODE_WRITE | FS_MODE_EXEC) ||
        !has_perm(&new_parent_inode, FS_MODE_WRITE | FS_MODE_EXEC)) {
        printf("Permission denied.\n");
        return -1;
    }
    if (dir_add_entry(new_parent, new_name, ino) != 0) {
        return -1;
    }
    if (dir_remove_entry(old_parent, old_name, NULL) != 0) {
        dir_remove_entry(new_parent, new_name, NULL);
        return -1;
    }
    printf("Renamed/moved %s -> %s\n", src, dst);
    return 0;
}

static int parse_uint16(const char *s, uint16_t *out, int base) {
    char *end = NULL;
    unsigned long value;
    if (!s || !out) {
        return -1;
    }
    errno = 0;
    value = strtoul(s, &end, base);
    if (errno != 0 || !end || *end != '\0' || value > 65535ul) {
        return -1;
    }
    *out = (uint16_t)value;
    return 0;
}

static int parse_uint32(const char *s, uint32_t *out, int base) {
    char *end = NULL;
    unsigned long value;
    if (!s || !out) {
        return -1;
    }
    errno = 0;
    value = strtoul(s, &end, base);
    if (errno != 0 || !end || *end != '\0' || value > 0xfffffffful) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int parse_args(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < max_args) {
        while (isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        if (*p == '"') {
            ++p;
            argv[argc++] = p;
            while (*p != '\0' && *p != '"') {
                ++p;
            }
            if (*p == '"') {
                *p++ = '\0';
            }
        } else {
            argv[argc++] = p;
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                ++p;
            }
            if (*p != '\0') {
                *p++ = '\0';
            }
        }
    }
    return argc;
}

static void join_args(char *argv[], int argc, int start, char *out, size_t out_size) {
    int i;
    out[0] = '\0';
    for (i = start; i < argc; ++i) {
        if (i > start) {
            strncat(out, " ", out_size - strlen(out) - 1);
        }
        strncat(out, argv[i], out_size - strlen(out) - 1);
    }
}

static void print_help(void) {
    printf("Commands:\n");
    printf("  format [disk]                create and mount a new virtual disk\n");
    printf("  mount [disk]                 mount an existing virtual disk\n");
    printf("  login <uid> <password>       login, default users include 2118/abcd\n");
    printf("  logout                       logout current user\n");
    printf("  dir [path]                   list directory\n");
    printf("  mkdir <path>                 create directory\n");
    printf("  rmdir <path>                 remove empty directory\n");
    printf("  cd|chdir <path>              change directory\n");
    printf("  pwd                          print current directory\n");
    printf("  create <path> [mode]         create file, mode is octal, default 644\n");
    printf("  open <path> <r|w|rw|a>       open file and print fd\n");
    printf("  write <fd> <text>            write text to opened file\n");
    printf("  read <fd> <size>             read bytes from opened file\n");
    printf("  close <fd>                   close fd\n");
    printf("  cat <path>                   print whole file\n");
    printf("  append <path> <text>         append text to file\n");
    printf("  rm|delete <path>             remove file\n");
    printf("  cp <src> <dst>               copy file\n");
    printf("  mv|rename <src> <dst>        rename or move file\n");
    printf("  chmod <path> <mode>          change permission mode\n");
    printf("  stat <path>                  show inode information\n");
    printf("  fsinfo                       show super block and free stack status\n");
    printf("  useradd <uid> <gid> <pwd>    add user, root only\n");
    printf("  passwd <uid> <newpwd>        change password\n");
    printf("  help                         show this help\n");
    printf("  halt|exit                    flush and exit\n");
}

static int dispatch(int argc, char *argv[], const char *default_disk_path) {
    uint16_t u16a;
    uint16_t u16b;
    uint32_t u32;
    int fd;
    char text[1024];
    if (argc == 0) {
        return 0;
    }
    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
        print_help();
    } else if (strcmp(argv[0], "format") == 0) {
        return ugfs_format(argc >= 2 ? argv[1] : default_disk_path);
    } else if (strcmp(argv[0], "mount") == 0 || strcmp(argv[0], "install") == 0) {
        if (ugfs_mount(argc >= 2 ? argv[1] : default_disk_path) != 0) {
            printf("Mount failed. Run format first if the disk does not exist.\n");
            return -1;
        }
        printf("Mounted %s\n", argc >= 2 ? argv[1] : default_disk_path);
    } else if (strcmp(argv[0], "login") == 0) {
        if (argc != 3 || parse_uint16(argv[1], &u16a, 10) != 0) {
            printf("Usage: login <uid> <password>\n");
            return -1;
        }
        return cmd_login(u16a, argv[2]);
    } else if (strcmp(argv[0], "logout") == 0) {
        return cmd_logout();
    } else if (strcmp(argv[0], "dir") == 0 || strcmp(argv[0], "ls") == 0) {
        return cmd_dir(argc >= 2 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "mkdir") == 0) {
        if (argc != 2) {
            printf("Usage: mkdir <path>\n");
            return -1;
        }
        return cmd_mkdir(argv[1]);
    } else if (strcmp(argv[0], "rmdir") == 0) {
        if (argc != 2) {
            printf("Usage: rmdir <path>\n");
            return -1;
        }
        return cmd_rmdir(argv[1]);
    } else if (strcmp(argv[0], "cd") == 0 || strcmp(argv[0], "chdir") == 0) {
        if (argc != 2) {
            printf("Usage: cd <path>\n");
            return -1;
        }
        return cmd_chdir(argv[1]);
    } else if (strcmp(argv[0], "pwd") == 0) {
        if (require_login() != 0) {
            return -1;
        }
        printf("%s\n", g_session.cwd_path);
    } else if (strcmp(argv[0], "create") == 0 || strcmp(argv[0], "creat") == 0) {
        u16a = 0644u;
        if (argc < 2 || argc > 3) {
            printf("Usage: create <path> [mode]\n");
            return -1;
        }
        if (argc == 3 && (parse_uint16(argv[2], &u16a, 8) != 0 || u16a > 0777u)) {
            printf("Mode must be octal, for example 644 or 777.\n");
            return -1;
        }
        return cmd_create(argv[1], u16a);
    } else if (strcmp(argv[0], "open") == 0) {
        if (argc != 3) {
            printf("Usage: open <path> <r|w|rw|a>\n");
            return -1;
        }
        return cmd_open(argv[1], argv[2]);
    } else if (strcmp(argv[0], "close") == 0) {
        if (argc != 2) {
            printf("Usage: close <fd>\n");
            return -1;
        }
        fd = atoi(argv[1]);
        return cmd_close(fd);
    } else if (strcmp(argv[0], "write") == 0) {
        if (argc < 3) {
            printf("Usage: write <fd> <text>\n");
            return -1;
        }
        fd = atoi(argv[1]);
        join_args(argv, argc, 2, text, sizeof(text));
        return cmd_write(fd, text);
    } else if (strcmp(argv[0], "read") == 0) {
        if (argc != 3 || parse_uint32(argv[2], &u32, 10) != 0) {
            printf("Usage: read <fd> <size>\n");
            return -1;
        }
        fd = atoi(argv[1]);
        return cmd_read(fd, u32);
    } else if (strcmp(argv[0], "cat") == 0) {
        if (argc != 2) {
            printf("Usage: cat <path>\n");
            return -1;
        }
        return cmd_cat(argv[1]);
    } else if (strcmp(argv[0], "append") == 0) {
        if (argc < 3) {
            printf("Usage: append <path> <text>\n");
            return -1;
        }
        join_args(argv, argc, 2, text, sizeof(text));
        return cmd_append(argv[1], text);
    } else if (strcmp(argv[0], "rm") == 0 || strcmp(argv[0], "delete") == 0) {
        if (argc != 2) {
            printf("Usage: rm <path>\n");
            return -1;
        }
        return cmd_rm(argv[1]);
    } else if (strcmp(argv[0], "chmod") == 0) {
        if (argc != 3 || parse_uint16(argv[2], &u16a, 8) != 0 || u16a > 0777u) {
            printf("Usage: chmod <path> <octal-mode>\n");
            return -1;
        }
        return cmd_chmod(argv[1], u16a);
    } else if (strcmp(argv[0], "stat") == 0) {
        if (argc != 2) {
            printf("Usage: stat <path>\n");
            return -1;
        }
        return cmd_stat(argv[1]);
    } else if (strcmp(argv[0], "fsinfo") == 0) {
        return cmd_fsinfo();
    } else if (strcmp(argv[0], "cp") == 0) {
        if (argc != 3) {
            printf("Usage: cp <src> <dst>\n");
            return -1;
        }
        return cmd_cp(argv[1], argv[2]);
    } else if (strcmp(argv[0], "mv") == 0 || strcmp(argv[0], "rename") == 0) {
        if (argc != 3) {
            printf("Usage: mv <src> <dst>\n");
            return -1;
        }
        return cmd_mv(argv[1], argv[2]);
    } else if (strcmp(argv[0], "useradd") == 0) {
        if (argc != 4 || parse_uint16(argv[1], &u16a, 10) != 0 ||
            parse_uint16(argv[2], &u16b, 10) != 0) {
            printf("Usage: useradd <uid> <gid> <password>\n");
            return -1;
        }
        return cmd_useradd(u16a, u16b, argv[3]);
    } else if (strcmp(argv[0], "passwd") == 0) {
        if (argc != 3 || parse_uint16(argv[1], &u16a, 10) != 0) {
            printf("Usage: passwd <uid> <new-password>\n");
            return -1;
        }
        return cmd_passwd(u16a, argv[2]);
    } else if (strcmp(argv[0], "halt") == 0 || strcmp(argv[0], "exit") == 0 ||
               strcmp(argv[0], "quit") == 0) {
        ugfs_unmount();
        return 1;
    } else {
        printf("Unknown command: %s. Run help.\n", argv[0]);
        return -1;
    }
    return 0;
}

int ugfs_run_shell(const char *default_disk_path) {
    char line[2048];
    char *argv[64];
    int argc;
    int r;
    if (!default_disk_path || default_disk_path[0] == '\0') {
        default_disk_path = UGFS_DISK_FILE;
    }
    if (ugfs_mount(default_disk_path) == 0) {
        printf("Mounted %s\n", default_disk_path);
    } else {
        printf("No existing virtual disk mounted. Run 'format' to create one.\n");
    }
    print_help();
    for (;;) {
        if (is_mounted() && g_session.logged_in) {
            printf("%u:%s> ", g_session.uid, g_session.cwd_path);
        } else {
            printf("MiniFS> ");
        }
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        argc = parse_args(line, argv, 64);
        r = dispatch(argc, argv, default_disk_path);
        if (r == 1) {
            break;
        }
    }
    ugfs_unmount();
    return 0;
}
