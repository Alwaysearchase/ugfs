#include "ugfs_internal.h"

int parse_uint16(const char *s, uint16_t *out, int base) {
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

int parse_uint32(const char *s, uint32_t *out, int base) {
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

int parse_args(char *line, char *argv[], int max_args) {
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

void join_args(char *argv[], int argc, int start, char *out, size_t out_size) {
    int i;
    out[0] = '\0';
    for (i = start; i < argc; ++i) {
        if (i > start) {
            strncat(out, " ", out_size - strlen(out) - 1);
        }
        strncat(out, argv[i], out_size - strlen(out) - 1);
    }
}

void print_help(void) {
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

int dispatch(int argc, char *argv[], const char *default_disk_path) {
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
