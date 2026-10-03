# GNU GRUB bootloader source

The ISO is built with GNU GRUB 2.12 from Ubuntu package version
`2.12-1ubuntu7.3`. ArkOS itself is a separate, independently written kernel.

This directory includes the original tarball, Ubuntu/Debian packaging and
patches, source descriptor, copyright information, and GPLv3 text for that
bootloader. To unpack the matching source with Debian tooling:

```sh
dpkg-source -x grub2_2.12-1ubuntu7.3.dsc
```

Build instructions and exact package dependencies are in the extracted
`debian/control`, `debian/rules`, and upstream `INSTALL` files. The ArkOS
Makefile uses the distribution's `grub-mkrescue` without modifying GRUB.

Source snapshot (all three source package files):
https://snapshot.ubuntu.com/ubuntu/20260828T000000Z/pool/main/g/grub2/

The ISO also contains GRUB's bundled font and standard modules; their
licenses remain those of GNU GRUB and the copyright holders. QEMU and OVMF
are test/runtime tools and are not redistributed in this package.


## 0.9.1 macOS 构建证据

本次本机验证的 ISO 使用 Debian GRUB `2.12-9+deb13u2` 的 BIOS rescue image、EFI monolithic image 和对应 EFI 模块。ArkOS 未修改这些模块。上面的 Ubuntu 源码留作旧镜像对应证据；本目录另附 `grub2_2.12-9+deb13u2.dsc`、同版本 Debian patch tar 与 `COPYRIGHT-debian-2.12-9+deb13u2`，复用同一个 `grub2_2.12.orig.tar.xz`。

官方对应来源：[Debian grub2 包目录](https://deb.debian.org/debian/pool/main/g/grub2/)。在具有 Debian 源码工具的环境运行 `dpkg-source -x grub2_2.12-9+deb13u2.dsc` 可解包对应源码。本机工具链入口见 [开发指南](../../CONTRIBUTING.md)。

## 0.10.0 本机混合镜像

继续使用上面的 Debian `2.12-9+deb13u2` 未修改 BIOS/EFI 模块。本机 xorriso 包装器将卷标设为 `ARKOS0100`，添加 EFI El Torito 镜像，并通过 `-boot_image any efi_boot_part=--efi-boot-image` 生成安装器需要的 GPT EFI 分区。普通源码构建仍调用 GNU `grub-mkrescue`。BIOS/UEFI 启动与安装测试入口见 [开发指南](../../CONTRIBUTING.md)，逐次结果保存在对应构建与发布目录。
