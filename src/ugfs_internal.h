#ifndef UGFS_INTERNAL_H
#define UGFS_INTERNAL_H

#include "ugfs.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern FILE *g_disk;
extern FsSuper g_super;
extern FsOpenFile g_sys_open[FS_SYS_OPEN_MAX];
extern FsSession g_session;
extern char g_disk_path[FS_PATH_MAX];

uint32_t now_u32(void);
void reset_runtime(void);
bool is_mounted(void);
int ensure_layout(void);
int require_mounted(void);
int require_login(void);
void mode_to_string(uint16_t mode, char out[11]);

int disk_seek_abs(long offset);
int disk_read_block(uint32_t block_no, void *buf);
int disk_write_block(uint32_t block_no, const void *buf);
int flush_super(void);
int load_super(void);
void zero_block(uint32_t block_no);

long inode_offset(uint16_t ino);
int read_inode(uint16_t ino, FsInodeDisk *inode);
int write_inode(uint16_t ino, const FsInodeDisk *inode);
bool inode_is_file(const FsInodeDisk *inode);
bool inode_is_dir(const FsInodeDisk *inode);
bool has_perm(const FsInodeDisk *inode, uint32_t op);

int write_free_group(uint32_t block_no);
int push_free_block(uint32_t block_no, bool count_total);
int alloc_block(uint32_t *out_block);
int free_block(uint32_t block_no);
int refill_inode_stack(void);
int alloc_inode(uint16_t *out_ino);
int free_inode(uint16_t ino);
int inode_get_block(FsInodeDisk *inode, uint32_t logical_block,
                    bool allocate, uint32_t *out_block);
int inode_truncate(FsInodeDisk *inode);

int dir_read_entry_from_inode(const FsInodeDisk *dir_inode,
                              uint32_t index,
                              FsDirEntry *entry);
int dir_write_entry_to_inode(FsInodeDisk *dir_inode,
                             uint32_t index,
                             const FsDirEntry *entry);
int dir_lookup(uint16_t dir_ino, const char *name, uint16_t *out_ino);
int dir_find_child_name(uint16_t parent_ino, uint16_t child_ino,
                        char *name, size_t name_size);
int dir_add_entry(uint16_t dir_ino, const char *name, uint16_t child_ino);
int dir_remove_entry(uint16_t dir_ino, const char *name, uint16_t *old_ino);
bool dir_is_empty(uint16_t dir_ino);
int path_next(const char **cursor, char *name);
bool path_has_next(const char *cursor);
int resolve_path(const char *path, uint16_t *out_ino);
int resolve_parent(const char *path, uint16_t *parent_ino, char *leaf);
int build_path(uint16_t ino, char *out, size_t out_size);
void pack_dir_entry(unsigned char *block, uint32_t slot,
                    uint16_t ino, const char *name);
int write_raw_dir_block(uint32_t block_no, uint16_t self, uint16_t parent);

int read_users(FsPwdEntry users[FS_MAX_USERS]);
int write_users(const FsPwdEntry users[FS_MAX_USERS]);
int find_user(const FsPwdEntry users[FS_MAX_USERS], uint16_t uid);
int make_initial_inodes(void);

int cmd_login(uint16_t uid, const char *password);
int close_fd_internal(int ufd);
int cmd_logout(void);
int cmd_useradd(uint16_t uid, uint16_t gid, const char *password);
int cmd_passwd(uint16_t uid, const char *password);

int cmd_mkdir(const char *path);
int cmd_chdir(const char *path);
int cmd_dir(const char *path);
int cmd_create(const char *path, uint16_t perm);
int cmd_rm(const char *path);
int cmd_rmdir(const char *path);
int cmd_chmod(const char *path, uint16_t perm);
int cmd_stat(const char *path);
int cmd_fsinfo(void);
int cmd_cp(const char *src, const char *dst);
int cmd_mv(const char *src, const char *dst);

int file_is_open(uint16_t ino);
int parse_open_flags(const char *mode, uint32_t *flags);
int cmd_open(const char *path, const char *mode);
int cmd_close(int fd);
int file_write_at(uint16_t ino, uint32_t offset, const unsigned char *buf,
                  uint32_t size, uint32_t *new_offset);
int file_read_at(uint16_t ino, uint32_t offset, unsigned char *buf,
                 uint32_t size, uint32_t *read_size);
int cmd_write(int fd, const char *text);
int cmd_read(int fd, uint32_t size);
int cmd_cat(const char *path);
int cmd_append(const char *path, const char *text);

int parse_uint16(const char *s, uint16_t *out, int base);
int parse_uint32(const char *s, uint32_t *out, int base);
int parse_args(char *line, char *argv[], int max_args);
void join_args(char *argv[], int argc, int start, char *out, size_t out_size);
void print_help(void);
int dispatch(int argc, char *argv[], const char *default_disk_path);

#endif
