#include "ugfs_internal.h"

long inode_offset(uint16_t ino) {
    return ((long)FS_INODE_START_BLOCK * (long)FS_BLOCK_SIZE) +
           ((long)ino * (long)FS_INODE_SIZE);
}

int read_inode(uint16_t ino, FsInodeDisk *inode) {
    if (!g_disk || ino >= FS_MAX_INODES || !inode) {
        return -1;
    }
    if (disk_seek_abs(inode_offset(ino)) != 0) {
        return -1;
    }
    return fread(inode, 1, sizeof(*inode), g_disk) == sizeof(*inode) ? 0 : -1;
}

int write_inode(uint16_t ino, const FsInodeDisk *inode) {
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

bool inode_is_file(const FsInodeDisk *inode) {
    return inode && ((inode->mode & FS_TYPE_MASK) == FS_TYPE_FILE);
}

bool inode_is_dir(const FsInodeDisk *inode) {
    return inode && ((inode->mode & FS_TYPE_MASK) == FS_TYPE_DIR);
}

bool has_perm(const FsInodeDisk *inode, uint32_t op) {
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
