# 固定的输入法数据

这里保存原生 ArkOS 全拼解码器使用的数据与许可。词典源文件按以下上游提交固定，普通构建离线读取已经生成的 `user/ime_lexicon.h`。

| 来源 | 提交 | 许可与文件 |
|---|---|---|
| [rime-luna-pinyin](https://github.com/rime/rime-luna-pinyin) | 56b934b099dfbeab842320f13aa8b461a6ab3e42 | LGPL-3.0，目录 LICENSE/AUTHORS |
| [rime-essay](https://github.com/rime/rime-essay) | 054920de4f54c9e5994276a96a4fc2a35cb51aa3 | LGPL-3.0，目录 LICENSE/AUTHORS |
| [OpenCC](https://github.com/BYVoid/OpenCC) | 3ac34aa439a9908dd49fa92b5174b46314787ac2 | Apache-2.0，目录 LICENSE；TSCharacters/TSPhrases |

原始数据保留。`scripts/generate-ime.py` 是 ArkOS 原创的离线转换器：选取频率词条、以 OpenCC 表转简体、保留多音字读音和音节边界，辅以项目固定的 Unihan 单字读音，生成成本排序和字符串表。转换是确定的逐词/逐字映射，不等于完整 OpenCC 上下文转换。生成结果 122616 条记录；摘要与大小在 `generated.json`。衍生词典按原始 LGPL-3.0 数据许可分发，许可证、源数据、转换器、完整可重新链接的 ArkOS 源码一并提供。

`user/pinyin.c` 是项目原创的有界全拼组句解码器；Rime 引擎没有被移植。上游数据不代表本项目的候选排序、句法或个人学习质量。重新生成：在项目根运行 `python3 scripts/generate-ime.py`，只依赖 Python 标准库。普通系统构建无需执行此步骤或联网。
