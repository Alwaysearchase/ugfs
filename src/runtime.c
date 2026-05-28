#include "ugfs_internal.h"

FILE *g_disk = NULL;
FsSuper g_super;
FsOpenFile g_sys_open[FS_SYS_OPEN_MAX];
FsSession g_session;
char g_disk_path[FS_PATH_MAX] = UGFS_DISK_FILE;

uint32_t now_u32(void) {
    return (uint32_t)time(NULL);
}

void reset_runtime(void) {
    uint32_t i;
    memset(g_sys_open, 0, sizeof(g_sys_open));
    memset(&g_session, 0, sizeof(g_session));
    g_session.cwd_ino = FS_ROOT_INO;
    strcpy(g_session.cwd_path, "/");
    for (i = 0; i < FS_USER_OPEN_MAX; ++i) {
        g_session.user_open[i] = -1;
    }
}

bool is_mounted(void) {
    return g_disk != NULL;
}

int ensure_layout(void) {
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

int require_mounted(void) {
    if (!is_mounted()) {
        printf("No file system is mounted. Run: format\n");
        return -1;
    }
    return 0;
}

int require_login(void) {
    if (require_mounted() != 0) {
        return -1;
    }
    if (!g_session.logged_in) {
        printf("Please login first.\n");
        return -1;
    }
    return 0;
}

void mode_to_string(uint16_t mode, char out[11]) {
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
