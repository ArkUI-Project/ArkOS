# ArkOS — 原生实验操作系统

这是一款由C语言编写的自研 x86-64 内核、Ring3 图形桌面、原生存储/输入/网络和独立应用的操作系统，不使用 Linux 内核。

## 启动

推荐 QEMU x86-64、512MiB 以上内存、4 个 CPU、VMware SVGA（64 MiB 显存）、AHCI/ATA、E1000 和 VirtIO 触控/数位板。ISO 支持 BIOS/UEFI。

```sh
make -j4
make run
```

已有镜像时运行 `sh scripts/run-qemu.sh`；脚本保留项目的数据盘。

GPU 玻璃使用 `sh scripts/run-qemu-gpu.sh`。本机 macOS 启动器会打开已安装的 UTM 的 QEMU 配置；在 UTM 中点击运行。首次创建独立的 `build/ArkOS-0.13.0-GPU.utm` 数据盘，后续启动保留其账户与数据；支持 GL 的独立 QEMU 可通过 `QEMU_BIN` 指定。
玻璃材质由 GPU 加速；其接口、资源范围和失败处理见 [API](docs/API.md#fixed-gpu-glass)。

启动时按 Escape 显示分辨率菜单，可选择 1920×1080、2560×1440 或 3840×2160。4K 使用 `ARKOS_MEMORY=1024M sh scripts/run-qemu.sh`。

SDK 应用在 1440p/4K 创建 2 倍物理像素 surface，鼠标坐标返回逻辑单位；桌面菜单和窗口控制目前仍采用 100% 尺寸。

系统暂不支持运行时切换分辨率。

HTTPS 在客体中执行 TLS 1.2、证书链/域名/有效期检查，使用固定的 150 个公共根证书。Wi-Fi 扫描、认证与连接等待具体无线设备目标。详见 [网络实现](docs/NETWORK.md)。

本机 Homebrew QEMU 11.1.2 可选 `sh scripts/run-qemu-high-refresh.sh`，将已验证开发副本的活跃显示轮询改为 4 ms。工具校验固定版本的完整摘要，具体变更见[准备脚本](scripts/prepare-qemu-high-refresh.py)。60/120 为调度目标，实际显示帧率需在运行环境测量。

构建依赖 GCC/binutils（或配置 x86-64 交叉 Clang/LLD）、Python 3、GRUB（BIOS 与 EFI）、xorriso；QEMU 用于验证。字体和解释器源码均随项目提供。重新生成字体元数据只需 Python 标准库；图形测试另需 Pillow、mtools、ASan/UBSan，TLS 诊断需 OpenSSL 生成临时证书。

安装器在 ArkOS 内读取本机 ISO、写入受支持的目标磁盘；支持安装后移除 ISO 启动。

当前是**清空整盘的安装方式**，不支持已有分区缩容/双系统迁移。验证仅使用专门创建的镜像，真机支持取决于具体控制器。

## 许可

TLS 使用 MIT 许可的 BearSSL 0.6，字体轮廓光栅使用 stb_truetype 1.26；输入词典使用固定 Rime LGPL-3.0 与 OpenCC Apache-2.0 数据，解码器为原创实现；WASM 使用 MIT 许可的 wasm3 原生移植；GRUB、字体和 Unicode 数据也保留各自许可。

## 开发与验证

从 [开发指南](CONTRIBUTING.md)、[源码结构](docs/ARCHITECTURE.md)和 [AGENTS.md](AGENTS.md) 开始；接口见 [API](docs/API.md)、[SDK](sdk/README.md)、[ArkUI](docs/ARKUI.md)。固定组件与许可见 [第三方清单](third_party/README.md)，源码上传步骤见 [GitHub](docs/GITHUB.md)。当前为静态链接的原生 ABI，不是 POSIX/Linux 兼容层。

```sh
make check check-security-host check-package-host check-v7-host check-v8-host
python3 tests/interaction_013_test.py bios
python3 tests/interaction_013_test.py uefi
python3 tests/lifecycle_013_test.py bios
python3 tests/lifecycle_013_test.py uefi
python3 tests/vm_memory_test.py
python3 tests/highres_013_test.py
python3 tests/install_013_test.py bios
python3 tests/install_013_test.py uefi
python3 tests/package_count_vm_test.py
python3 tests/tls_vm_test.py
make check-wasm-host
python3 tests/v9_wasm_test.py bios
python3 tests/v9_wasm_test.py uefi
make check-protection
make check-vm
make check-runtime
make arm64
python3 tests/arm64_test.py
```

`make run-arm64` 启动独立串口开发目标：MMU/EL0/SVC/多核启动和保护探针已经实现，完整桌面没有移植。
