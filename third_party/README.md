# 固定第三方组件与许可

ArkOS 原创部分采用根目录 MIT 许可。以下组件和派生数据各自保留
来源、版本、许可证和修改记录；具体固定提交与文件摘要以对应目录
说明为准。

| 目录 | 用途 | 来源与许可记录 |
|---|---|---|
| `grub/` | BIOS/UEFI 引导工具对应源码 | [GRUB 说明](grub/README.md)，GPLv3 与分发包版权记录 |
| `bearssl/` | 客体 TLS 1.2 | [移植说明](bearssl/ARKOS-PORT.md)、LICENSE.txt、公共根证书与上游公开测试样例 |
| `stb/` | 客体字体轮廓光栅 | stb_truetype 1.26，源码头内双许可证 |
| `wasm3/` | 独立 Ring3 WASM 解释器 | [固定来源](wasm3/UPSTREAM.txt)、MIT LICENSE 与 ArkOS 移植说明 |
| `unicode/` | Unicode/Unihan 字符与拼音数据 | [数据说明](unicode/README.md)、Unicode LICENSE.txt |
| `rime-data/` | 离线全拼词典 | [固定词典](rime-data/README.md)，Rime LGPL-3.0、OpenCC Apache-2.0 |
| `android-liquid-glass/` | 原生液态玻璃数学参照 | [来源与修改](android-liquid-glass/README.md)，Apache-2.0 |
| `virgl-protocol/` | 原生 GPU 固定协议头 | [固定协议来源](virgl-protocol/README.md)，MIT LICENSE |

字体源文件与许可位于 [assets/fonts](../assets/fonts/README.md)。仓库中
上游压缩源码、公开证书样例和有效/无效 WASM 测试载荷用于许可对应、
构建与边界验证，属于可复现源码的一部分。
