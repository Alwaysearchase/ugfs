#include "ugfs_internal.h"

int dir_read_entry_from_inode(const FsInodeDisk *dir_inode,
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

int dir_write_entry_to_inode(FsInodeDisk *dir_inode,
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

int dir_lookup(uint16_t dir_ino, const char *name, uint16_t *out_ino) {
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

int dir_find_child_name(uint16_t parent_ino, uint16_t child_ino,
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

int dir_add_entry(uint16_t dir_ino, const char *name, uint16_t child_ino) {
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

int dir_remove_entry(uint16_t dir_ino, const char *name, uint16_t *old_ino) {
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

bool dir_is_empty(uint16_t dir_ino) {
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

int path_next(const char **cursor, char *name) {
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

bool path_has_next(const char *cursor) {
    char name[FS_NAME_MAX + 1];
    const char *p = cursor;
    return path_next(&p, name) == 1;
}

int resolve_path(const char *path, uint16_t *out_ino) {
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

int resolve_parent(const char *path, uint16_t *parent_ino, char *leaf) {
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

int build_path(uint16_t ino, char *out, size_t out_size) {
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

void pack_dir_entry(unsigned char *block, uint32_t slot,
                           uint16_t ino, const char *name) {
    FsDirEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.ino = ino;
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    memcpy(block + slot * FS_DIR_ENTRY_SIZE, &entry, sizeof(entry));
}

int write_raw_dir_block(uint32_t block_no, uint16_t self, uint16_t parent) {
    unsigned char block[FS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    pack_dir_entry(block, 0, self, ".");
    pack_dir_entry(block, 1, parent, "..");
    return disk_write_block(block_no, block);
}
