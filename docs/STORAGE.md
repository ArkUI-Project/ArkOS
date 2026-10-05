# ArkOS 0.3 原生磁盘、ArkFS 与外部卷（历史说明）

> 本文保留0.3历史实现，不能代表0.7现状。当前x86具有Ring3、真正用户态SMP、AHCI安装和二进制存储；ARM64仍为独立开发目标。当前源码结构与接口见[架构](ARCHITECTURE.md)和[API](API.md)。

ArkOS 使用自写的 ATA PIO 驱动直接读写磁盘扇区。文件数据保存在独立的
`arkos-data.img` 中；ISO 只负责启动。整个运行环境不使用 Linux 内核、
宿主文件夹映射或浏览器存储。

## 启动与持久化

只在第一次使用时创建数据盘：

```sh
python3 scripts/create-disk.py arkos-data.img
qemu-system-x86_64 -machine pc -m 256M \
  -drive if=ide,index=0,format=raw,file=arkos-data.img \
  -drive if=ide,index=2,media=cdrom,readonly=on,file=build/arkos-0.3.0.iso \
  -boot d -vga std -serial stdio -nic none
```

创建脚本默认生成 32 MiB 稀疏文件。它使用排他创建，遇到已有文件、符号
链接或设备路径都会拒绝覆盖。`--size-mib` 可指定 4 至 131072 MiB，扩大镜像
不会增加当前版本的逻辑文件容量。

在终端执行：

```sh
mkdir /home/ark/Documents/test
write /home/ark/Documents/test/hello.txt hello persistent disk
sync
cat /home/ark/Documents/test/hello.txt
reboot
```

重启后使用相同的 `arkos-data.img`，文件仍可读取。中文名称与 UTF-8 内容
同样保留。`sync` 成功表示已提交快照，并完成 ATA FLUSH CACHE；虚拟机后端
必须正常兑现 flush 请求，才能保证突然断电后的持久性。不要使用 QEMU 的
`-snapshot` 或禁用宿主 flush 的缓存选项来验证持久化。

未连接受支持的数据盘，或卷格式、校验失败时，ArkOS 会明确显示 RAM 模式。
RAM 模式仍可使用文件应用，但更改无法跨重启保存。内核不会格式化空盘、
未知文件系统或两个快照均损坏的卷。

## 支持范围与容量

- Primary IDE master，传统 I/O 端口 `0x1F0–0x1F7` / `0x3F6`。
- IDENTIFY、LBA28 READ SECTORS、WRITE SECTORS、FLUSH CACHE；512 字节逻辑扇区。
- PIO 轮询，最多 255 扇区一批；ATA 错误、设备故障及超时会中止同步。
- 64 个文件或目录条目，目录与根目录也占条目。
- 每个文本文件最多 16383 字节；完整绝对路径最多 127 个 UTF-8 字节。
- 支持目录、文件读写、文件复制、文件/目录子树重命名、空目录删除。
- 不允许删除根目录、删除非空目录、把目录移入自身，或在不存在的父目录创建。
- 不实现二进制文件、符号链接、权限、分区表、AHCI、NVMe、USB 存储或热插拔。

`storage_capacity_bytes()` 返回逻辑理论上限 **1048512 字节**（64 × 16383）；
`storage_used_bytes()` 返回所有普通文件正文之和。目录占用条目，所以实际可用
正文容量还受空闲条目数影响。32 MiB 是物理镜像容量，启动串口日志单独报告；
其中约 2.04 MiB 用于两份快照及保留区，其余空间预留，不应称为可用文件容量。

`vfs_write` 等操作先修改内存；`storage_sync()` 提交所有已修改文件。桌面定时
同步、记事本保存以及关机流程应调用它。同步失败时修改仍在 RAM 中，
`storage_status()` 显示错误状态，`storage_error()` 提供原因，下一次同步可重试。
原生 VFS API 的相对路径默认为 `/home/ark`；终端自行将工作目录转成绝对路径。

## 磁盘格式 v1

全部数值为小端。CRC 使用 IEEE CRC-32（多项式 `0xEDB88320`，初值和终值异或
`0xFFFFFFFF`，与 Python `zlib.crc32` 一致）。

| 区域 | 扇区 | 内容 |
| --- | ---: | --- |
| 超级块 | 0 | 只读格式描述和 CRC |
| 保留 | 1–7 | 当前不用 |
| 快照 A | 8–2087 | 1 扇区提交头 + 最多 2079 扇区正文 |
| 快照 B | 2088–4167 | 同上 |

超级块（512 字节）：

| 字节偏移 | 字段 |
| --- | --- |
| 0 | 8 字节 `ARKFS1\0\0` |
| 8 | u32 版本 1 |
| 12 | u32 扇区大小 512 |
| 16 | u32 创建时总扇区数 |
| 20 | u32 每份快照扇区数 2080 |
| 24 / 28 | u32 快照 A / B 首扇区 8 / 2088 |
| 32 / 36 / 40 | u32 条目数 64 / 路径缓冲 128 / 文件缓冲 16384 |
| 508 | u32 前 508 字节的 CRC |

快照头（512 字节）：

| 字节偏移 | 字段 |
| --- | --- |
| 0 | 8 字节 `ARKBANK1` |
| 8 | u64 generation，首次为 1 |
| 16 | u32 正文总字节数，不含扇区补零 |
| 20 | u32 正文 CRC |
| 24 | u32 条目数 |
| 28 | u32 格式版本 1 |
| 32 | u32 提交标记 `0x41524B31` |
| 508 | u32 前 508 字节的 CRC |

正文仅序列化实际使用的条目，依次为：u32 类型（1 文件，2 目录）、u32 正文
长度、128 字节以 NUL 结尾的绝对路径、正文实际字节。目录长度必须为 0，
文本中不允许内嵌 NUL。每条记录不再额外对齐，最终正文向扇区边界补零。
加载器验证长度、类型、CRC、UTF-8、路径规范、父目录和重复名称。

## 提交顺序与恢复

1. 将完整的当前正文写入**非活动**快照区。
2. 执行 FLUSH CACHE，确认正文先落盘。
3. 写入新的 generation、正文 CRC、提交标记和头部 CRC。
4. 再次执行 FLUSH CACHE，成功后才更新内存中的活动快照。

启动时先选择 generation 最大且提交头有效的快照，校验正文与目录结构；若
失败，尝试另一份。提交过程中断电不会改写先前活动快照。校验错误的头或正文
不会被加载。两份都坏时进入 RAM 模式且保留原盘，不自动修复或格式化。
CRC 用于意外损坏检测，不是密码学验证；不承诺抵御恶意篡改、两个快照均被
硬件破坏、控制器虚报 flush 成功或宿主文件系统损坏。

## 规范参考

驱动为独立实现。ATA 命令和寄存器语义参考 T13 ATA/ATAPI-5 工作草案，
由 Seagate 托管：<https://www.seagate.com/support/disc/manuals/ata/d1153r17.pdf>。
ArkFS 磁盘格式、序列化器和恢复逻辑为本项目原创代码，不采用 Linux 文件系统代码。

## 自动验证

运行 `./tests/test-storage.sh`。测试在 AddressSanitizer / UndefinedBehaviorSanitizer
下使用真实的 ArkFS 序列化与恢复代码，只替换扇区传输设备。覆盖 UTF-8 目录、
规范化路径、目录子树移动、大小与条目上限、完整快照读回、正文半写入、提交头
半写入、flush 失败、最新快照损坏，以及未知卷不写盘。这个测试不替代实际
QEMU ATA 启动与跨进程重启验证。

## 0.3：FAT32 与 NTFS 外部数据盘

ArkFS 保留在 primary IDE master（`index=0`）；新增的自写 `block.c` 驱动访问
primary IDE slave（`index=1`）。两个驱动在每次操作前选择目标设备，因此桌面
自动保存 ArkFS 和写入外部 FAT32 可使用同一 IDE 总线。

```sh
qemu-system-x86_64 -machine pc -m 256M -vga std \
  -drive if=ide,index=0,format=raw,file=arkos-data.img \
  -drive if=ide,index=1,format=raw,file=compat-data.img \
  -drive if=ide,index=2,media=cdrom,readonly=on,file=build/arkos-0.3.0.iso \
  -boot d -serial stdio -nic none
```

外部盘可直接包含一个 FAT32/NTFS 卷，也可使用 MBR 的主分区：`0B` / `0C`
为 FAT32，`07` 为 NTFS。每种文件系统最多挂载一个卷；重叠、越界的 MBR 分区
会被拒绝。不支持 GPT、扩展分区、动态磁盘或 BitLocker。内核不会格式化磁盘。
挂载点为 `/mnt/fat32` 与 `/mnt/ntfs`；`mounts`、文件侧栏和设置可查看状态。

| 文件系统 | 当前行为 |
| --- | --- |
| ArkFS | 自研双快照读写，原有 64 条目与 16383 字节/文件限制不变 |
| FAT32 | 列目录、读取、创建/覆盖文件、新建目录、删除文件/空目录；有界普通文件移动 |
| NTFS | 只读列目录及受支持文件读取；没有任何 NTFS 写入实现 |

所有 GUI 和 Shell 文本读取、复制、编辑都受 **16383 字节**应用缓冲区限制；
较大文件可列出真实大小，打开时明确报错。内嵌 NUL 的二进制文件不能按文本
打开。底层读取接口支持偏移读取；这不等于应用可以处理任意大小的文件。
完整路径最多 127 个 UTF-8 字节，包含挂载点前缀。

外部卷使用独立的 64 项缓存，包括 `/mnt` 和挂载根目录。它不会写入 ArkFS
快照，也不会挤占 ArkFS 的 64 条目。缓存以一次开机中访问过的不同路径计数，
不驱逐或将旧句柄分配给其他路径，保证打开记事本时浏览别的目录不会改写另一
文件。达到上限会明确报错，需要重启清空缓存；当前尚无通用缓存淘汰策略。

### FAT32 实现边界

- 512 字节逻辑扇区，每簇 1–128 个扇区且必须为 2 的幂；严格验证 FAT32 簇数、
  卷大小、FAT 大小、根目录、活动 FAT 及保留表项。
- 支持一份或两份 FAT，更新镜像 FAT，或遵守已配置的活动 FAT。
- 支持 VFAT 长文件名和 UTF-16 代理对，转换为 UTF-8；中文文件名可读写。
  ASCII 名称匹配忽略大小写；没有实现完整 Unicode 大小写折叠或旧 OEM 代码页。
- 文件覆写先分配新簇链、写入数据并 flush，然后切换目录项，再释放旧簇链。
  使用有效的长名与唯一短名别名，更新 FSInfo 的已知空闲计数和搜索提示。
- 目录项扫描最多 4096 扇区；路径最多 32 层；读取遍历最多 65536 簇；删除或
  覆写旧链最多 4096 簇；分配搜索限制为前 1048576 个数据簇。遇到边界停止并
  返回错误，不进行无限扫描。
- 普通文件移动采用复制、flush、再删除，最大 16383 字节；不支持 FAT32
  目录子树移动。跨文件系统移动也不是原子操作。NTFS 作为移动源会预先拒绝，
  不会复制一半后再尝试删除只读源。
- 遵守 FAT 的只读文件属性；卷标记为未正常卸载或发生硬盘错误时只读挂载。
  实际写入或 flush 出错后，该卷在本次运行中切换为只读。

FAT32 没有日志，当前实现也没有增加事务日志。突然断电可能留下分配但未引用
的簇或不完整的新目录项，不能宣称具有 ArkFS 双快照同等级的恢复能力。发生
异常断电后，应使用宿主 FAT 检查工具检查测试数据盘。不要把唯一数据副本用于
实验内核测试。

### NTFS 实现边界

`ntfs.c` 为原创只读解析器。支持有效 NTFS 3.x 卷上的 MFT 记录、目录索引、
Unicode 文件名、驻留数据和受支持的非驻留簇映射。压缩、加密、稀疏、属性列表
扩展以及不满足解析器边界的记录会被明确拒绝；不进行恢复、修复或写入。
实际支持范围以 `include/ntfs.h` 的接口约束和驱动错误返回为准。

### 统一 VFS 调用约定

新的调用方应包含 `extfs.h`，使用 `vfs_entry(index)` 访问记录并用
`vfs_entry_limit()` 遍历。`vfs_find()` 返回的外部索引从 64 开始，不能再用于
直接下标访问原始 `vfs_files[64]`。列目录前调用 `vfs_list(path)`，读取正文前
调用 `vfs_read(index)` 并检查返回值，统一落盘用 `vfs_sync()`。

`vfs_create/write/remove/mkdir/copy/rename` 根据路径路由；`ExtVolumeInfo`
分别给出外部卷挂载状态、只读状态、分区容量和挂载点。容量来自真实卷/分区，
不等于内存文本应用可以同时处理的数据量。挂载入口从不接收格式化请求。

### 内核二进制对象与驱动存储

`ARK_BLOB` 管理的 COW 对象区分两个命名空间。`@pkg.`、`@registry.`、`@swap.`
和 `@drv.` 前缀属于内核命名空间：非内核 LIST 会跳过它们，用户态按这些
名称读写一律拒绝（`blob.c` 的 `blob_request`）。对象以 uid 1000 记录在系统
卷，blob 记录本身不含用户数据，校验和由写入方计算。

可加载 `.arco` 驱动是最大的内核对象，单个文件上限 1 MiB（`ARK_BLOB_MAX`
为 8 MiB），比 VFS 文本文件的 16 KiB 上限大两个数量级：驱动映像不会放进
ArkFS 文件系统，而是作为 `@drv.<name>` blob 保存，并由受保护清单
`@drv.manifest` 记录每个文件的整份 SHA-256。VFS 文件上限保持 16 KiB 不变，
格式与装载见 [DRIVERS](DRIVERS.md)。

### 独立兼容性验证

`tests/test-fat32.sh` 使用 **dosfstools 的 mkfs.fat** 和 **mtools** 创建输入，
随后运行原生 FAT32 代码的 ASan/UBSan 测试，再由宿主 `fsck.fat -n` 检查，
并用 `mtype` 读取原生驱动写入的英文和中文文件。覆盖长名、跨扇区目录、目录
扩展、创建/删除、最大文本文件、覆写、偏移读、重命名、重挂载、畸形 BPB 拒绝、循环目录有界退出和未知卷不写入。
这是与独立工具的交叉验证，不是用本项目自己的格式化器自测。

`tests/external_vm_test.py` 为真实 QEMU ATA 集成测试入口：使用独立数据盘副本，
运行 BIOS 写入和另一 UEFI 进程读取，之后由宿主检查 FAT32，并比较 NTFS 分区
前后 SHA-256。每次测试保存冻结 ISO 的散列和结果；结果是否通过应以该次
`result.json` 为准，不能仅凭存在测试脚本认定已通过。

FAT 磁盘格式参考 Microsoft FAT Specification；仅参考公开格式说明，没有
复制 Windows 或 Linux 文件系统实现：
<https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf>。
