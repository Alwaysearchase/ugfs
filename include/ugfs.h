#ifndef UGFS_H
#define UGFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define UGFS_DISK_FILE "filesystem.dat"

#define FS_BLOCK_SIZE 512u
#define FS_TOTAL_BLOCKS 546u
#define FS_BOOT_BLOCK 0u
#define FS_SUPER_BLOCK 1u
#define FS_INODE_START_BLOCK 2u
#define FS_INODE_BLOCKS 32u
#define FS_DATA_START_BLOCK (FS_INODE_START_BLOCK + FS_INODE_BLOCKS)
#define FS_DATA_BLOCKS (FS_TOTAL_BLOCKS - FS_DATA_START_BLOCK)

#define FS_ROOT_INO 1u
#define FS_ETC_INO 2u
#define FS_PASSWD_INO 3u

#define FS_ROOT_DIR_BLOCK FS_DATA_START_BLOCK
#define FS_ETC_DIR_BLOCK (FS_DATA_START_BLOCK + 1u)
#define FS_PASSWD_BLOCK (FS_DATA_START_BLOCK + 2u)

#define FS_NICFREE 50u
#define FS_NICINOD 50u
#define FS_DIRECT_BLOCKS 10u
#define FS_INODE_SIZE 64u
#define FS_INODES_PER_BLOCK (FS_BLOCK_SIZE / FS_INODE_SIZE)
#define FS_MAX_INODES (FS_INODE_BLOCKS * FS_INODES_PER_BLOCK)
#define FS_NAME_MAX 29u
#define FS_DIR_ENTRY_SIZE 32u
#define FS_DIR_ENTRIES_PER_BLOCK (FS_BLOCK_SIZE / FS_DIR_ENTRY_SIZE)
#define FS_MAX_DIR_ENTRIES (FS_DIRECT_BLOCKS * FS_DIR_ENTRIES_PER_BLOCK)
#define FS_MAX_FILE_SIZE (FS_DIRECT_BLOCKS * FS_BLOCK_SIZE)
#define FS_MAX_USERS 16u
#define FS_SYS_OPEN_MAX 40u
#define FS_USER_OPEN_MAX 20u
#define FS_PATH_MAX 256u

#define FS_MAGIC "UGFS26"
#define FS_VERSION 1u
#define FS_FREE_GROUP_MAGIC 0x55474647u

#define FS_TYPE_MASK 07000u
#define FS_TYPE_FILE 01000u
#define FS_TYPE_DIR 02000u
#define FS_PERM_MASK 0777u

#define FS_MODE_READ 4u
#define FS_MODE_WRITE 2u
#define FS_MODE_EXEC 1u

#define FS_OF_READ 0x01u
#define FS_OF_WRITE 0x02u
#define FS_OF_APPEND 0x04u

typedef struct FsInodeDisk {
    uint16_t mode;
    uint16_t nlink;
    uint16_t uid;
    uint16_t gid;
    uint32_t size;
    uint32_t addr[FS_DIRECT_BLOCKS];
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
} FsInodeDisk;

typedef struct FsDirEntry {
    uint16_t ino;
    char name[30];
} FsDirEntry;

typedef struct FsPwdEntry {
    uint16_t uid;
    uint16_t gid;
    uint8_t used;
    char password[23];
    uint8_t reserved[4];
} FsPwdEntry;

typedef struct FsFreeGroup {
    uint32_t magic;
    uint32_t count;
    uint32_t blocks[FS_NICFREE];
} FsFreeGroup;

typedef struct FsSuper {
    char magic[8];
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_start_block;
    uint32_t inode_blocks;
    uint32_t data_start_block;
    uint32_t data_blocks;
    uint32_t max_inodes;
    uint32_t root_ino;
    uint32_t passwd_ino;
    uint32_t total_free_blocks;
    uint32_t free_count;
    uint32_t free_stack[FS_NICFREE];
    uint32_t total_free_inodes;
    uint32_t inode_free_count;
    uint32_t inode_stack[FS_NICINOD];
    uint32_t next_inode_scan;
    uint32_t dirty;
} FsSuper;

typedef struct FsOpenFile {
    bool used;
    uint16_t ino;
    uint32_t flags;
    uint32_t offset;
    uint32_t ref_count;
} FsOpenFile;

typedef struct FsSession {
    bool logged_in;
    uint16_t uid;
    uint16_t gid;
    uint16_t cwd_ino;
    char cwd_path[FS_PATH_MAX];
    int user_open[FS_USER_OPEN_MAX];
} FsSession;

int ugfs_format(const char *disk_path);
int ugfs_mount(const char *disk_path);
void ugfs_unmount(void);
int ugfs_run_shell(const char *default_disk_path);

#endif
