#include "ugfs.h"

int main(int argc, char **argv) {
    const char *disk_path = UGFS_DISK_FILE;
    if (argc >= 2) {
        disk_path = argv[1];
    }
    return ugfs_run_shell(disk_path);
}
