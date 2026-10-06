# 开发与验证

先阅读 [AGENTS.md](AGENTS.md)、[源码结构](docs/ARCHITECTURE.md)和
[接口约定](docs/API.md)。运行中的 ArkOS 使用自研内核；下列工具用于
交叉编译、生成测试盘和验证客体行为。

## Linux 构建环境

Ubuntu 24.04 x86-64 可准备以下工具：

```sh
sudo apt-get update
sudo apt-get install -y build-essential python3-pil nodejs grub-pc-bin \
  grub-efi-amd64-bin xorriso mtools dosfstools ntfs-3g \
  qemu-system-x86 ovmf gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
make -j4
make run
```

依赖安装属于开发环境准备；普通 `make` 使用仓库内固定的组件和数据。
输出位于 `build/`，默认启动 ISO 是 `build/arkos-0.13.0.iso`。账户和
文件保存在客体数据盘。源码目录与生成文件的边界由 `.gitignore` 管理。

macOS 可使用 x86-64 freestanding 交叉 Clang/LLD，并通过 `CC`、`LD`
覆盖命令。GRUB 适配器读取 `ARKOS_GRUB_CACHE` 指定的已解包组件，
其来源与解包说明见 [GRUB](third_party/grub/README.md)。ISO 由
`scripts/mkiso.py` 直接组装，不依赖 `grub-mkrescue` 或 mtools。

## 验证入口

```sh
make check
make check-security-host check-package-host check-v7-host check-v8-host
make check-arkfs2 check-arkfs2-seal
make check-device-host check-module-host check-ioapic-host check-msi-host check-registry-host
make check-wasm-host
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd make check-runtime
python3 tests/arco_vm_test.py bios
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd python3 tests/arco_vm_test.py uefi
python3 tests/ioapic_vm_test.py
python3 tests/nvme_vm_test.py
python3 tests/spice_mouse_vm_test.py bios
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd python3 tests/spice_mouse_vm_test.py uefi
make arm64
```

客体测试通过 `tests/fixtures.py` 为各自的 `build/test-*` 目录生成空白
ArkFS 和独立 FAT32/NTFS 卷。文件系统夹具使用 mtools、dosfstools 与
ntfs-3g 的格式化工具。持久化检查会复用该测试自己的盘。

历史版本测试要求相应 ISO、SDK 包或诊断载荷；实际执行日志、载荷摘要
与结果写入各自的 `build/test-*` 目录。当前 GitHub Actions 配置验证 x86 构建、
主机边界（含设备模型、可加载模块、IOAPIC 与 MSI-X）、独立文件系统夹具、
客体 IOAPIC 与 MSI-X/NVMe 冒烟门和 BIOS/UEFI 原生鼠标输入，
ARM64 步骤只检查串口开发目标编译。

## 变更与发布

保持每进程页表、Ring3、W^X/NX 和内核身份校验。接口或应用变化同步
SDK、目录、权限迁移和接口说明；性能改动提供相同场景的实测数据。
第三方内容保留固定来源、许可证与修改说明，见
[组件清单](third_party/README.md)。

提交源代码、构建脚本、必要数据和验证说明。启动 ISO、空白盘与逐次
验证证据按版本生成独立发布包，摘要对应实际载荷。发布操作保留旧
快照，账户盘继续作为本地运行数据。
