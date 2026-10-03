# ArkOS 原生 NTFS 只读驱动

`kernel/ntfs.c` 是 ArkOS 自研内核中的原创 NTFS 3.0／3.1 读取实现，未使用 Linux 内核或移植 Linux 文件系统驱动。该模块只接受块读取回调，没有写入回调；任何 NTFS 修改请求都由统一 VFS 层拒绝。

启动时，外部存储层识别受支持磁盘和分区，将 NTFS 卷暴露在 `/mnt/ntfs`。自动发现的范围、控制器支持和分区规则以存储层文档为准。当前最多挂载一个 NTFS 卷。

```sh
mounts
ls -l /mnt/ntfs
cat /mnt/ntfs/README.txt
cp /mnt/ntfs/README.txt /home/ark/ntfs-copy.txt
cp /mnt/ntfs/README.txt /mnt/fat32/NTFSCOPY.TXT
sync
```

源卷 NTFS 始终只读。复制到个人目录使用原有 ArkFS／RAM；复制到外部 FAT32 使用 FAT32 写入能力。FAT32 对创建、命名、空间和其他写入操作的限制仍然适用。

## 实际支持范围

- 从引导扇区读取卷几何信息，支持 512 字节逻辑扇区、512～65536 字节簇。
- 支持 512～4096 字节 MFT 记录，512～8192 字节目录索引块。
- 读取 `$MFT` 非驻留数据映射，包括多段、正负 LCN 相对偏移；每个流最多 128 段。
- 检查 `FILE` 和 `INDX` 更新序列／扇区尾部修复信息，拒绝疑似撕裂记录。
- 支持 `$I30` 的驻留索引根及非驻留索引分配树；有界中序遍历，不通过整卷 MFT 扫描伪造目录。
- 支持嵌套目录、中文文件名和 UTF-16 代理对到 UTF-8 的转换；跳过纯 DOS 短名别名。
- 支持普通未压缩、未加密、非稀疏文件的驻留和非驻留无名数据流；支持按偏移读取，正确处理最后不足请求长度的片段。
- 非驻留流中超过已初始化长度的逻辑区域返回零，不暴露未初始化磁盘内容。
- 文件引用校验 MFT 记录号和序列号；目录条目校验父引用。

文件名采用 UTF-8 精确匹配，区分大小写。未实现 Windows 的 `$UpCase` 字符折叠规则，因此 `README.txt` 和 `readme.txt` 不是同一个路径。原生驱动路径最长 1023 字节、最多 32 个有效目录分量；桌面／Shell 统一路径仍限制为 127 个 UTF-8 字节。调用驱动前需规范化 `..`。

目录树最多 16 层、4096 个被访问索引节点，检测当前祖先路径中的索引循环。磁盘偏移、分区长度、簇范围、属性偏移、记录长度、runlist 编码和流大小都经过边界检查。超过实现上限会报错，不静默截断为成功读取。

## 明确未实现的内容

- 写入、删除、重命名、创建目录、属性更改以及日志重放。
- 压缩、稀疏、EFS 加密流，以及 WOF／其他重解析点。
- `$ATTRIBUTE_LIST` 跨 MFT 记录续接；遇到它会拒绝受影响文件，若 `$MFT` 依赖该特性则拒绝挂载。
- 非默认命名数据流、完整 Windows ACL／权限语义、大小写不敏感查找。
- NTFS 1.x、非 512 字节逻辑扇区、超出上述记录／目录／extent 上限的卷。
- 脏卷恢复、自动修复、BitLocker 解密。检测到卷 dirty 标记会拒绝挂载；应先在具备相应恢复能力的系统中处理。

这些限制意味着这是可读取兼容 NTFS 卷的实验性驱动，不是完整 Windows NTFS 实现。遇到不支持的文件结构会显示错误，不应把错误解释成文件为空。

## 桌面和 Shell 的文本限制

NTFS 驱动本身支持超过 16 KiB 的非驻留文件和二进制偏移读取；实际测试读取了 100000 字节二进制文件。当前统一 VFS 的应用缓存只有 64 个外部条目，每个文本视图最多 16383 字节，并拒绝嵌入 NUL 的二进制文本。大文件可以列出真实大小，但不能在当前记事本／Shell 中完整打开或复制。此限制属于应用缓存接口，不是磁盘文件长度被改写。

`build/ntfs-demo.img` 是 32 MiB 的演示卷，包含 `README.txt`、`中文说明.txt` 和用于展示文本视图限制的 `large.txt`。外部磁盘镜像不是个人目录使用的 ArkFS 数据镜像。

## 接口

`include/ntfs.h` 使用 `include/extfs.h` 定义的回调类型。

```c
bool ntfs_mount(FsReadBlocks read, void *ctx,
                uint64_t partition_lba, uint64_t partition_sectors);
bool ntfs_stat(const char *path, bool *is_dir, uint64_t *size);
bool ntfs_list(const char *path, FsEmit emit, void *ctx);
bool ntfs_read(const char *path, uint64_t offset,
               void *buffer, size_t bytes, size_t *read);
const char *ntfs_error(void);
```

块回调使用设备绝对 LBA，单位为 512 字节。NTFS 路径从卷内 `/` 开始；列表回调收到直接子项名称而不是完整路径。列表回调返回 `false` 可提前停止枚举。

实现使用有界静态工作区，接口不支持重入；列表回调只能接收／缓存结果，不能递归调用 NTFS 接口。挂载失败会清除挂载状态。公开读取函数在成功时返回实际读取字节数，到达 EOF 时为零；错误文本通过 `ntfs_error()` 获取。

## 测试与复现

宿主测试使用真正的驱动代码，读取由独立 `mkntfs` 格式化、`ntfscp` 写入的镜像；这些宿主工具不链接或打包进 ArkOS 内核。测试包含：

- 110 个真实根目录项，强制产生索引分配树；嵌套目录读取。
- ASCII、中文和补充平面字符文件名。
- 驻留文件、100000 字节非驻留文件、跨簇偏移读取和 EOF。
- 将 `$MFT` 映射调整为多段，以及有效负 LCN 增量的测试。
- 损坏引导几何、MFT／索引修复值、属性长度、runlist、目录项边界、压缩／加密／稀疏标志等拒绝路径。
- 所有块读取均由宿主断言限制在传入分区内。

```sh
python3 tests/ntfs_make_fixture.py
# 小型演示卷（用于外部 VFS 的 64 条目缓存）
python3 tests/ntfs_make_fixture.py --demo
gcc -std=c11 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  tests/ntfs_test.c kernel/ntfs.c -o build/ntfs_test
ASAN_OPTIONS=detect_leaks=0 ./build/ntfs_test build/ntfs-test/fixture.img
```

脚本优先使用 `../toolroot/sbin` 和 `../toolroot/bin` 的宿主工具，也可通过 `ARK_TOOLROOT` 指定目录；工具不存在时使用 PATH。LeakSanitizer 在受限宿主环境中关闭，AddressSanitizer 和 UndefinedBehaviorSanitizer 仍然启用。

格式参考：Microsoft 的 [MFT](https://learn.microsoft.com/en-us/windows/win32/devnotes/master-file-table)、[文件记录头](https://learn.microsoft.com/en-us/windows/win32/devnotes/file-record-segment-header)、[属性记录头](https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-record-header)，以及 [NTFS 公开格式说明](https://github.com/libyal/libfsntfs/blob/main/documentation/New%20Technologies%20File%20System%20%28NTFS%29.asciidoc)。代码由 ArkOS 独立实现，没有复制这些项目的驱动源代码。

完整宿主集成测试还覆盖原生 NTFS → 外部卷缓存 → VFS → Shell：列目录、中文读取、复制到个人目录、管线输出、大文件限制，以及所有 NTFS 修改操作拒绝。测试断言整个流程没有调用磁盘写回调。

```sh
gcc -std=c11 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  tests/ntfs_shell_test.c user/shell.c kernel/vfs.c kernel/extfs.c \
  kernel/ntfs.c kernel/fat32.c -o build/ntfs_shell_test
ASAN_OPTIONS=detect_leaks=0 ./build/ntfs_shell_test build/ntfs-demo.img
```
