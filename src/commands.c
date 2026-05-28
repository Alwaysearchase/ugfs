#include "ugfs_internal.h"

int cmd_mkdir(const char *path) {
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

int cmd_chdir(const char *path) {
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

int cmd_dir(const char *path) {
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

int cmd_create(const char *path, uint16_t perm) {
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

int cmd_rm(const char *path) {
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

int cmd_rmdir(const char *path) {
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

int cmd_chmod(const char *path, uint16_t perm) {
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

int cmd_stat(const char *path) {
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

int cmd_fsinfo(void) {
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

int cmd_cp(const char *src, const char *dst) {
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

int cmd_mv(const char *src, const char *dst) {
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
