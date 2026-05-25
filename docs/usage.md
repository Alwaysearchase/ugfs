# 使用说明

## 是否需要 VSCode

不需要。这个项目是标准 C 命令行程序，VSCode 只是可选编辑器。

推荐方式：

- 写代码和查看文件：VSCode、Visual Studio、CLion、Dev-C++ 都可以。
- 编译：MinGW GCC、Visual Studio MSVC、CMake 均可。
- 演示：直接运行生成的 `ugfs.exe` 或 `ugfs`。

## 编译方式

### 方式一：MinGW GCC

在项目根目录运行：

```bash
gcc -std=c99 -Wall -Wextra -pedantic -Iinclude src/main.c src/ugfs.c -o ugfs
```

Windows 下运行：

```bash
.\ugfs.exe
```

Linux/macOS 下运行：

```bash
./ugfs
```

### 方式二：CMake

```bash
cmake -S . -B build
cmake --build build
```

Windows 下可执行文件通常在：

```text
build\Debug\ugfs.exe
```

或：

```text
build\ugfs.exe
```

## 初始用户

格式化后默认有这些用户：

| uid | gid | password |
| --- | --- | --- |
| 0 | 0 | root |
| 2116 | 3 | dddd |
| 2117 | 3 | bbbb |
| 2118 | 4 | abcd |
| 2119 | 4 | cccc |

## 推荐演示流程

```text
format
login 2118 abcd
dir
mkdir docs
cd docs
pwd
create a.txt 777
open a.txt w
write 0 hello_operating_system
close 0
open a.txt r
read 0 100
close 0
append a.txt _course_design
cat a.txt
stat a.txt
cp a.txt b.txt
dir
rm b.txt
fsinfo
logout
exit
```

重新运行程序后测试持久化：

```text
login 2118 abcd
cd docs
cat a.txt
dir
exit
```

如果 `a.txt` 仍然存在且内容正确，说明虚拟磁盘持久化保存成功。

## 主要命令

| 命令 | 作用 |
| --- | --- |
| `format [disk]` | 创建并格式化虚拟磁盘 |
| `mount [disk]` | 挂载已有虚拟磁盘 |
| `login <uid> <password>` | 用户登录 |
| `logout` | 用户注销 |
| `dir [path]` | 列目录 |
| `mkdir <path>` | 创建目录 |
| `rmdir <path>` | 删除空目录 |
| `cd <path>` / `chdir <path>` | 改变当前目录 |
| `pwd` | 显示当前目录 |
| `create <path> [mode]` | 创建文件 |
| `open <path> <r|w|rw|a>` | 打开文件 |
| `write <fd> <text>` | 写文件 |
| `read <fd> <size>` | 读文件 |
| `close <fd>` | 关闭文件 |
| `cat <path>` | 显示整个文件 |
| `append <path> <text>` | 追加写 |
| `rm <path>` / `delete <path>` | 删除普通文件 |
| `cp <src> <dst>` | 文件复制 |
| `mv <src> <dst>` / `rename <src> <dst>` | 重命名或移动文件 |
| `chmod <path> <mode>` | 修改权限 |
| `stat <path>` | 查看 inode 信息 |
| `fsinfo` | 查看超级块和空闲块栈状态 |
| `useradd <uid> <gid> <pwd>` | 添加用户，仅 root |
| `passwd <uid> <newpwd>` | 修改密码 |
| `exit` / `halt` | 保存并退出 |

## 注意事项

- 当前实现采用 10 个直接数据块地址，单文件最大长度为 `10 * 512 = 5120` 字节。
- 文件名最大长度为 29 字符。
- 空闲数据块使用 UNIX 成组链接法；inode 使用超级块中的空闲 inode 栈加扫描补充。
- 格式化后的根目录 `/` 默认权限为 `0777`，方便非 root 用户完成课程设计演示；`/etc` 仍为普通系统目录权限。
- `write` 命令写入带空格文本时建议加引号，例如：

```text
write 0 "hello operating system"
```
