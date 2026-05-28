#include "ugfs_internal.h"

int file_is_open(uint16_t ino) {
    uint32_t i;
    for (i = 0; i < FS_SYS_OPEN_MAX; ++i) {
        if (g_sys_open[i].used && g_sys_open[i].ino == ino) {
            return 1;
        }
    }
    return 0;
}

int parse_open_flags(const char *mode, uint32_t *flags) {
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

int cmd_open(const char *path, const char *mode) {
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

int cmd_close(int fd) {
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

int file_write_at(uint16_t ino, uint32_t offset, const unsigned char *buf,
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

int file_read_at(uint16_t ino, uint32_t offset, unsigned char *buf,
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

int cmd_write(int fd, const char *text) {
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

int cmd_read(int fd, uint32_t size) {
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

int cmd_cat(const char *path) {
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

int cmd_append(const char *path, const char *text) {
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
