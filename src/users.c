#include "ugfs_internal.h"

int read_users(FsPwdEntry users[FS_MAX_USERS]) {
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

int write_users(const FsPwdEntry users[FS_MAX_USERS]) {
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

int find_user(const FsPwdEntry users[FS_MAX_USERS], uint16_t uid) {
    uint32_t i;
    for (i = 0; i < FS_MAX_USERS; ++i) {
        if (users[i].used && users[i].uid == uid) {
            return (int)i;
        }
    }
    return -1;
}

int cmd_login(uint16_t uid, const char *password) {
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

int close_fd_internal(int ufd) {
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

int cmd_logout(void) {
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

int cmd_useradd(uint16_t uid, uint16_t gid, const char *password) {
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

int cmd_passwd(uint16_t uid, const char *password) {
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
