# ArkOS 原生终端

ArkOS 的终端是运行在受信任桌面 Ring3 进程中的原生命令解释器，文件、任务和设备请求经过自研内核系统调用。系统支持独立ELF进程与原生线程，但终端不是Bash或Linux/POSIX环境，不能运行Linux软件、包管理器、Python、Node.js或SSH。

终端输出使用UTF-8，保留中文路径、文件内容和命令参数。原生拼音目前接入笔记编辑器，尚未接入终端输入框。

## 基础使用

工作目录默认 `/home/ark`，提示符会显示当前目录：

```text
ark:/home/ark$ pwd
/home/ark
```

绝对路径从 `/` 开始，相对路径以当前目录为基准；支持重复斜杠、`.` 和 `..` 的规范化。路径区分大小写，完整路径最多 127 个 UTF-8 字节，父目录必须已经存在。

```sh
mkdir Documents/demo
cd Documents/demo
echo '你好，ArkOS！' > greeting.txt
cat greeting.txt
cp greeting.txt backup.txt
mv backup.txt archived.txt
ls -l
sync
```

只有在存储状态显示磁盘已挂载时，`sync` 成功才表示数据已提交到磁盘。无磁盘／格式不受支持时使用 RAM 模式，重启后修改会消失。终端操作修改内存中的文件状态；桌面周期同步，`sync` 可主动确认提交。正常关机、重启前也会同步；检测到同步失败会取消关机／重启并显示错误。

## 命令

| 命令 | 功能 |
| --- | --- |
| `help` | 列出命令、语法和能力限制 |
| `about` | 内核和存储状态 |
| `uname [-a\|-r]` | 系统标识；`-r` 只显示版本 |
| `clear` | 清空终端滚动记录 |
| `history` | 最近最多 32 条交互命令 |
| `pwd` | 显示当前目录 |
| `cd [目录]` | 更换目录；不指定参数时回到 `/home/ark` |
| `ls [-l] [路径]` | 列出直接子项，或显示指定文件；`-l` 显示类型和字节数 |
| `mkdir <目录>` | 创建单层目录，不递归创建父目录 |
| `rmdir <目录>` | 删除空目录；保护当前目录及其祖先 |
| `touch <文件>` | 新建空文件；已有文件保持内容 |
| `cat [文件…]` | 连续输出一个或多个文件；不指定文件时读取上一级管线文本 |
| `write <文件> [文本…]` | 替换内容；参数以空格连接，不附加换行 |
| `append <文件> [文本…]` | 追加内容；参数以空格连接，不附加换行 |
| `cp <源> <目标>` | 复制文件，目标是目录时使用源文件名；不递归复制目录 |
| `mv <源> <目标>` | 移动或重命名文件／目录；目标条目必须未使用，不覆盖已有文件 |
| `rm <文件>` | 删除文件；不删除目录 |
| `echo [文本…]` | 输出参数并附加一个换行 |
| `head [-n 数量] [文件]` | 输出前 N 行，默认 10 行 |
| `tail [-n 数量] [文件]` | 输出后 N 行，默认 10 行 |
| `grep [-n] [-v] <文本> [文件]` | 按字面子串筛选行；`-n` 带原行号，`-v` 反选，不支持正则表达式 |
| `wc [-l\|-w\|-c] [文件]` | 统计换行数／空白分隔词数／UTF-8 字节数；默认按这三个顺序输出 |
| `sh <文件>` | 按行运行原生 Shell 脚本 |
| `mounts` | 显示个人存储和外部卷状态；外部卷在启动时自动发现 |
| `df` | 显示存储状态、磁盘容量、文件内容字节数和条目使用量 |
| `sync` | 显式提交文件系统更改到挂载磁盘 |
| `mem` | 显示引导器报告的内存，不是实时可用内存 |
| `uptime` | 显示开机经过时间 |
| `date` | 显示 RTC 时钟；未实现年月日和时区转换 |
| `open files\|notes\|settings\|about` | 打开对应桌面应用 |
| `dev [子命令]` | 设备清单、可加载 `.arco` 驱动与受界限块读写；子命令、权限与限制见 [DRIVERS.md](DRIVERS.md) |
| `reboot` / `shutdown` | 同步已挂载磁盘，然后请求重启／关机 |

`cat`、`head`、`tail`、`grep`、`wc` 在未指定文件时读取前一个管线阶段的文本。没有管线输入时视为空文本，不会阻塞等待键盘。`wc -l` 统计换行符；最后一行若没有换行符，不计入换行数。

## 引号、转义、重定向和管线

单引号与双引号可以包含空格、`>` 和 `|`。单引号内的反斜杠是普通字符；其他位置的反斜杠保护紧随其后的字节。不支持 `$VAR`、命令替换、通配符展开，也不将 `\n` 转换为换行。一个词可以由相邻的引号片段组成。

```sh
echo "a | b > c" > "带 空格.txt"
write escaped\ name.txt 'literal text'
echo first > lines.txt
echo second >> lines.txt
cat lines.txt | grep second | wc -l
```

`>` 替换目标内容，`>>` 追加内容；目标不存在时会创建。重定向必须写在整条命令的末尾，且只有一个目标。不支持 `<`、`2>`、`&&`、`;` 等语法。以独立词开头的 `#` 引入到本行末尾的注释；要输出这样的 `#`，请加引号或反斜杠。

管线最多 8 个阶段，每个阶段最多输出 32768 字节。这是桌面用户进程中的有界文本传递，不是 Unix 进程间管道。每条命令最多 1023 个 UTF-8 字节，每阶段最多 48 个参数（包含命令名）；桌面输入框可能采用更短的输入上限。`cd`、`clear`、`open`、`sh`、`reboot`、`shutdown` 不能放入管线或使用重定向。

错误直接显示在终端，不会写入重定向文件。解析失败、管线输出溢出或目标文件大小超限时，不会覆写目标文件。脚本和多阶段管线不是事务：较早命令已经完成的副作用不会因为后续失败而回滚。

## 脚本

`sh` 运行 ArkOS 原生命令文件，每行一条命令；支持上述引号、管线与重定向。最多处理 64 个物理行（包括空行和注释），脚本最多嵌套 4 层。某行出错时立即停止并报告行号，已经执行的修改保留。

```sh
# demo.sh 内容
mkdir Documents/session
echo 'Hello ArkOS' > Documents/session/result.txt
cat Documents/session/result.txt | wc -w
sync
```

脚本在同一 Shell 状态中执行，`cd` 会影响后续交互命令。它不是 POSIX shell 脚本引擎；没有变量、循环、条件分支、后台任务和退出状态变量。

## 容量与程序接口

文件系统总共支持 64 个条目，目录和预置文件也计入。每个文本文件最多 16383 字节，字符串结尾占用额外一个字节；文本接口不支持 NUL 字节、二进制流、Unix mode位和符号链接。当前会话UID和目录权限由内核核验；二进制截图/录屏使用独立BLOB服务。`df` 的磁盘总容量不是可任意使用的文件容量，实际容量还受条目数和每文件上限约束。

终端显示环形缓冲区为 256 行，每行最多 255 个有效 UTF-8 字节；长行会在完整码点边界换行。历史记录最多 32 条。

- `shell_execute(const char *)`：执行并记录交互命令。
- `shell_print(const char *)`：输出一个完整文本记录，同时写串口。
- `shell_cwd()`：当前工作目录。
- `shell_prompt()`：根据当前目录生成提示符；指针指向静态缓冲区。
- `shell_history_get(int age)`：见 `include/shell_extra.h`，`0` 为最近一条，超出范围返回空指针；适合桌面历史导航。
- `shell_action`：`1=文件`、`2=笔记`、`3=设置`、`4=关于`、`6=浏览器`、`10=任务`、`11=截图录屏`、`12=安装`、`13=待办`、`14=计时器`。

## 验证

宿主测试使用真正的 `user/shell.c` 和 `kernel/vfs.c`，只替换硬件／磁盘接口。覆盖 UTF-8 边界、引号和转义、目录规范化及移动、管线与重定向、脚本停止和递归上限、容量超限时目标保留、同步失败时禁止关机，并进行确定性错误语法压力测试。

```sh
gcc -std=c11 -Wall -Wextra -Werror -g -DARK_STORAGE_HOST_TEST \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  tests/host_shell_test.c user/shell.c kernel/vfs.c \
  -o build/host_shell_test
ASAN_OPTIONS=detect_leaks=0 ./build/host_shell_test
```

关闭 LeakSanitizer 是为了兼容无法读取宿主进程信息的受限执行环境；AddressSanitizer 和 UndefinedBehaviorSanitizer 仍然启用。磁盘持久化和 QEMU 输入测试由独立集成测试验证。

## 外部 FAT32／NTFS 卷（0.3）

终端通过统一 VFS 访问 `/mnt/fat32` 和 `/mnt/ntfs`。`ls`、`cd`、`cat`、文本管线和 `cp` 使用相同命令；NTFS 始终只读。外部写入仅由 FAT32 后端提供，受该后端的命名／操作限制。

```sh
mounts
ls -l /mnt/ntfs
cat /mnt/ntfs/README.txt
cp /mnt/ntfs/README.txt /home/ark/import.txt
cp /home/ark/import.txt /mnt/fat32/IMPORT.TXT
sync
```

当前外部缓存最多 64 个条目，路径仍为 127 字节；文件内容视图／复制最多 16383 字节，不接受包含 NUL 的二进制数据。较大文件可以列出大小，打开时明确报错。`sync` 和关机同步覆盖所有已挂载文件系统；若个人目录仅在 RAM 中，仍会单独提醒它不会持久保存。详见 [NTFS.md](NTFS.md)。


## 0.7 新命令

| 命令 | 行为与边界 |
|---|---|
| `ps` / `lscpu` | 真实任务快照/在线CPU数，最多32任务；共享线程的映射预算不宜重复相加 |
| `kill TID` | 内核核验后终止所属整个进程组；不能终止SYSTEM或当前进程组 |
| `id` / `whoami` | 当前会话UID与用户名 |
| `net` | E1000链路、IPv4、RX/TX包计数，不是Wi-Fi设置命令 |
| `blobs` | 列出当前用户二进制媒体名称和大小；没有命令行播放/导出功能 |
| `run clock` / `run paint` / `run markdown` | 受控目录启动真正的独立Ring3 ELF；仍服从每用户授权 |
| `stat FILE` | 文件路径与字节大小；不是完整POSIX stat结构 |
| `find [DIR]` | 列出当前VFS缓存中此前可见的路径，不会递归扫描外部磁盘 |
| `sort [FILE]` | 最多1024行、32768字节，按UTF-8字节序排序；输出统一追加换行 |
| `uniq [FILE]` | 合并相邻重复行；结合`sort`可去重；不是语言学排序 |
| `open browser\|tasks\|capture\|installer\|todo\|timer` | 打开对应内置工具 |

`sort`/`uniq`未给文件时读取管线文本，例如`cat names.txt | sort | uniq`。所有工具仍受文本文件16383字节的写入上限约束。测试中的网络服务器仅提供普通HTTP响应，不参与浏览器渲染、输入法、截图或磁盘操作。

## 设备与驱动命令（0.13）

`dev` 查询设备模型与可加载 `.arco` 驱动，并在受界限范围内做一次真实块传输：

| 子命令 | 行为与边界 |
|---|---|
| `dev` / `dev list` | 列出内核设备节点：类、状态与 detail；只需活动会话 |
| `dev drivers` | 列出已安装驱动、版本、状态与 MSI-X 绑定/ISR 计数 |
| `dev query NAME` | 名称、状态、字节数与整份文件 SHA-256 |
| `dev install PATH.arco\|blob:NAME` | 需要管理员：按整份文件哈希校验后装载 |
| `dev remove NAME` | 需要管理员：DEINIT、节点置 ABSENT、释放槽位与向量 |
| `dev blk UNIT read\|write LBA COUNT` | 对块单元做一次受校验传输；UNIT 0–1，COUNT ≤ 128（64 KiB），LBA 为 512 字节扇区 |

`dev blk` 是系统诊断入口：Shell 作为 SYSTEM 调用者满足 DEVICE 能力检查，内核在搬运任何扇区前校验节点、区间并排除 ArkFS 系统卷；`write` 仅对 SYSTEM 开放，不会写入正在运行的系统盘。普通应用需要自己获得 DEVICE 能力才能用 `ARK_DEV_READ`。驱动格式、信任模型与限制见 [DRIVERS.md](DRIVERS.md)。
