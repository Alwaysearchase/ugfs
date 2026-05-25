# UNIX 成组链接法文件系统课程设计

这是一个标准 C 实现的 UNIX 风格模拟文件系统课程设计项目。程序使用普通宿主机文件
`filesystem.dat` 作为虚拟磁盘，磁盘空闲块管理采用 UNIX 成组链接法。

已实现内容：

- 虚拟磁盘文件、超级块、inode 区、数据区
- UNIX 成组链接法空闲数据块分配与回收
- inode 分配与回收
- 多用户登录、注销、用户表
- 多级目录、`.`、`..`、绝对路径和相对路径
- `format`、`login`、`logout`、`dir`、`mkdir`、`chdir/cd`、`create`
- `open`、`read`、`write`、`close`、`rm/delete`、`rmdir`
- 新增功能：`pwd`、`cat`、`append`、`cp`、`mv/rename`、`chmod`、`stat`、`fsinfo`、`useradd`、`passwd`

VSCode 不是必须的。你可以用 VSCode 打开这个目录，也可以直接用命令行编译运行。

详细编译、演示和报告说明见：

- `docs/usage.md`
- `docs/report_notes.md`
- `tests/demo_commands.txt`
