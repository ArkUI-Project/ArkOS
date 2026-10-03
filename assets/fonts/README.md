# 捆绑字体与原生字库

源字体为 WenQuanYi Micro Hei 0.2.0-beta 的 `wqy-microhei.ttc`，采用其中 Mono face（索引 1）。版权、上游地址和 Apache-2.0 / GPL-3+ Font exception 信息保留在 [LICENSE.txt](LICENSE.txt)。本项目的字体生成和客体绘制代码为 ArkOS 实现；字形轮廓来自该许可字体。

源 TTC 的 SHA-256 为 `2420e8078af796b19a3f6ef13de527a1a91c1e7171eea115926c614ced1009b3`。字体没有修改。构建直接将 `assets/fonts/wqy-microhei.ttc`（5,177,387字节）链接到 ELF 的只读 `.arkfont` 段；系统容器在构建时共用同一份不可变字体载荷。

运行 `python3 scripts/generate_unicode_font.py` 离线验证固定 SHA 并生成字体元数据头文件，只需 Python 标准库。客体的user/unicode.c以MIT许可的stb_truetype1.26直接从轮廓在目标像素尺寸光栅化，支持1–6倍文字尺度，保持ASCII8、CJK16和行高22的逻辑字距。256字形缓存和256KiB单字形工作区有界。没有宿主字体运行时依赖。

只解析不可变捆绑字体；不接收用户字体文件。支持范围以TTC的cmap为准，缺失字形显示方框。主机验证142种覆盖率、放大轮廓、裁剪/stride和目标高字节保留；实际字体与窗口合成由客体测试验证，入口见[开发指南](../../CONTRIBUTING.md)。旧SDK位图字体应用须重新构建后获得新的光栅方式。
