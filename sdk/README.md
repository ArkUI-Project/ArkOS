# ArkOS Native SDK 0.13.0 — ABI 1

应用是静态 ELF64，在 ArkOS 自研内核的 Ring3、私有页表与真实 UID 下执行。公开 ABI 以 `include/ark_api.h` 为准，完整行为与错误语义见 [API](../docs/API.md)。SDK 是本项目 x86-64 原生接口；普通构建离线完成。

## 构建与安装

在完整源码根目录执行，需要 x86-64 GCC/binutils 或配置交叉 Clang/LLD、Python 3 和 POSIX shell：

```sh
make user-programs
sh sdk/build-app.sh sdk/workspace.c build/workspace.elf
python3 scripts/arkpkg.py build build/workspace.elf build/workspace.arkpkg \
  --id workspace --title '拖拽工作区' --version 0.13.0 --permissions ui
python3 scripts/arkpkg.py inspect build/workspace.arkpkg
```

客体中 `pkg install /mnt/fat32/workspace.arkpkg`、`pkg run workspace`。可以设置 `CC`、`LD` 为单个编译器和链接器路径。`build-app.sh` 编译源码目录中的运行时、字体、ArkUI 和动画；单独复制 sdk 目录不足以构建。随附 [workspace.c](workspace.c) 是可执行的多窗口、拖放与输入示例，最多请求四个窗口；申请仍受实际内存和全局 surface 数量限制。

ELF 使用独立 RX/R/RW 段、NX 栈、入口 0x40000000；内核拒绝 RWX、非法段、动态链接与 TLS。用户栈 128 KiB，普通映射总预算 64 MiB，SYSTEM 512 MiB。`ark_memory(bytes)` 预留并提交 RW/NX 匿名区，首次访问才分配清零物理页，单次最多 64 MiB；`ark_memory_release(pointer,bytes)` 释放完整区间。内存不足时可换到客体数据盘，详见 [虚拟内存](../docs/VM.md)。每个进程可拥有多个 surface；全系统共十二个 surface、三十二个任务/线程槽、最多八个 xAPIC 核。

## 窗口、布局与生命周期

使用 `ArkApp`、`app_open()`、`app_event()`、`app_present()` 和 `app_wait(100)`。像素为 0x00RRGGBB，私有缓冲由用户绘制，提交到内核管理的 surface，桌面取得自己的副本。无事件时等待会被本进程事件唤醒；100 ticks 是最长 1 秒。不要在空闲时持续轮询或无条件 present。

`app_width()` 与 `app_height()` 返回逻辑尺寸；ArkApp.width/height、pixels 与 stride 是物理像素。在 1440p/4K `app_open()` 选 density=2，文字从轮廓按实际尺寸光栅化。窗口 RESIZE 事件更新 SDK 缓冲，应用应按最新逻辑尺寸重新布局；最大物理 surface 为 3840×2160。稳态按 1:1 合成，高级窗口动效继续使用曲面纹理变形。直接操作 pixels、构建 ArkUISurface 的应用自行处理密度与裁剪。旧 SDK 二进制需重新编译才能获得新布局、字体与主题支持。

多个 `ArkApp` 各自有事件与布局。系统关闭按钮向同一 PID 的窗口发送 CLOSE；`app_event()` 处理 CLOSE 并退出整个进程，内核回收其他 surface 与页。编辑器使用 `app_poll_event()` 收到原始 CLOSE，先保存并检查成功再退出；保存失败保留草稿。`app_close()` 仍是应用内部关闭单个 surface 的接口，可用于应用自己的窗口管理。最小化保留 surface 与进程。`ARK_SPAWN_NEW=1` 请求独立进程实例，flags=0 激活已有实例；其他 PID 的同名实例拥有独立生命周期。

窗口边框、Dock 和菜单玻璃由可信桌面调用 `SYS_COMPOSITOR=43` 绘制，
在已验证的 VirtIO/VirGL 设备上使用实时 GPU 着色器。此接口要求 SYSTEM，
普通应用继续绘制自己的私有 surface，不能提交原始 GPU 命令。接口与
资源上限见 [API](../docs/API.md#fixed-gpu-glass)。

## 输入、中文与主题

`0.13.0+mouse1` 支持 UTM 的原生 SPICE 绝对鼠标通道，应用继续通过原有
POINTER／SCROLL 接口接收事件，ABI 13 和系统包版本保持兼容。协议接入和事件边界见 [API](../docs/API.md#surfaces-and-input)。

POINTER 的 buttons&1 表示按下，坐标已经转换为逻辑尺寸；按住拖出窗口仍可能收到外部坐标，绘制必须裁剪。`app_click_hit()` 仅在按下和松开都位于同一控件时为真，拖放结束会取消点击。画笔和拖动继续处理原始按键边沿。SCROLL 正 y 向下，列表需自行调整首行。KEY 使用 KEY_* 或 ASCII/快捷键码。系统可能先处理窗口与全局快捷键。

`app_cursor(app,x,y,w,h,shape)` 声明最多 32 个逻辑区域；`app_present()` 发布这一帧的区域，后声明的区域优先。`app_clear()` 清空区域，`app_button()` 自动声明手形指针。形状常量见 `include/cursor.h`，支持箭头、手形、文本、抓取、四种缩放和十字；每帧重绘后重新声明文本及拖动区域。桌面根据最上层窗口选择形状，应用只能修改自己 surface 的声明。

输入字段调用 `app_text_input(app,true,x,y,height)` 声明焦点和插入点。Ctrl+I 切全拼，空格/Enter 或 1–9 选候选，方括号/PageUp/PageDown 翻页；系统以 TEXT 事件提交 Unicode 码点，用 `app_codepoint()` 插入。有界编辑辅助在 `sdk/text.h`。密码界面由可信会话处理。原生全拼支持组句与当前账户的个人学习，数据与许可证见 [输入数据](../third_party/rime-data/README.md)。

SDK 初始读取当前主题，THEME 更新 ArkApp.night 并标记 dirty。标准 app_clear/round/text/button 依据语义中性色映射主题；原始图像、画布与用户选择的画笔颜色使用 `app_rect_raw()` 等保留像素。自绘颜色应根据 night 选择可读配色，不能假设所有任意 RGB 都会自动转换。

## 拖放与配置

```c
#include "drag.h"
/* 超过拖动阈值后发起 COPY。FILE 要求 FILES 与实际存在的路径。 */
app_drag_begin(&view, ARK_DRAG_TEXT, text, "text/plain");
/* 收到事件后读取真实 token，并按实际插入结果确认。 */
if (event.type == ARK_EV_DROP) {
    ArkDragRequest drop;
    if (app_drop_read(&view, &event, &drop)) {
        bool accepted = drop.kind == ARK_DRAG_TEXT &&
            app_insert(text, &length, &cursor, sizeof text, drop.data);
        app_drop_accept(&view, &drop, accepted);
    }
}
```

来源收到 POINTER 松开与 DRAG_END，应清除本地拖动状态。全系统一个在途传输，TEXT 最多 511 字节、FILE 路径 127 字节、MIME 47 字节。拖动 60 秒、投递确认 5 秒；失败可重试。文件 payload 是路径，目标仍通过文件 API 检查权限和读取结果；复制不会自动执行文字或文件。顶部暂存区是当前会话的有界副本。

SYS_REGISTRY=39 存储 INTEGER、STRING、BINARY 配置。应用写自己的 `/apps/<真实进程名>/`，包为 `/apps/pkg.ID/`；普通应用可读明确公开的主题/时区，个人输入学习由系统保护。`sdk/config.h` 提供最多 512 字节的二进制 helper；整数、字符串和枚举使用 `ark_registry()`。GET/SET/DELETE/LIST/REVISION 的大小和错误见 API。配置在 ArkFS 原生 COW 存储内按 UID 保存，写失败须提示，不能报告已持久化。

## 文件、时间与权限

`app_read_text()` 返回字节数并补 NUL，失败 -1；`app_write_text()` 创建/覆盖并 sync，检查 bool 与 error。路径最多 127 字节，文本文件 16383 字节；二进制分块用 READ_BYTES。相对路径由内核按当前用户 home 解析，应用不能以 ..、UID 或缓存索引绕过权限。FAT32 有限读写、NTFS 只读。

`sdk/datetime.h` 提供原生 RTC 日期、UTC epoch 与账户时区转换；`ark_ticks()` 为 100Hz、`ark_millis()` 为客体单调毫秒。`sdk/thread.h` 提供线程、join、sleep 和等待消息；C11 release/acquire 用于工作缓冲发布，无 pthread/TLS/AVX。`sdk/parallel.c` 为共享地址空间线程示例。

FILES、NETWORK、ACTIVITY 由内核选定的权限记录决定。内置目录应用通过 `app_request_permission()` 请求可信授权，-11 表示等弹窗，-1 拒绝；保留界面并等事件，避免忙等。普通安装包初始 UI，其他权限由包管理器显式授予；权限不足保留用户输入。SYSTEM、PROCESS 不能由磁盘包声明。后台活动、启动参数分别使用 ACTIVITY 和 LAUNCH。

## 随附独立应用

| 应用 | 实际功能与边界 |
|---|---|
| Notes | UTF-8 编辑、系统全拼、滚轮、独立新文档、文字/文件拖放；文本最多 16383 字节 |
| Browser | 原生 HTTP/TLS1.2 与 HTML 文字阅读、滚轮；无 CSS/JS |
| Calculator | 有符号 64 位整数与括号，溢出/除零检查 |
| Clock | UTC/上海显示、真实单调秒表 |
| Paint | 输入事件分批处理、画笔/撤销、导出 SVG；最多 180 段，画布文档 750×300 |
| Markdown | 本地文件、轻量格式预览、滚轮；非完整 CommonMark |
| Todo | 文件保存、完成/删除与滚轮；旧格式最多 12 项 |
| Timer | 1/5/25 分钟计时、暂停与原生活动 |
| WASM | wasm3 原生解释器、磁盘模块与有限 WASI；有燃料/内存限制 |
| Calendar | 月历、选日、新建/删除、每窗口加载最多 256 条；当前日程时间为 09:00 |
| Reminders | 可编辑日期时间、完成/删除、后台到期提示；每窗口最多加载 128 条 |

十九个实际系统包包含上述十一个独立 ELF 与八个可信桌面工具入口。工具共享桌面进程，用户应用保有各自地址空间；系统包随系统镜像更新。原始字体、BearSSL、wasm3、Rime/OpenCC 数据的许可随源码提供，项目原创实现与原生移植分别说明。

主机验证覆盖真实 SDK 循环、指针/长度、配置 UID/COW、组句、主题/尺寸和拖放边界；Ring3 隔离、多核和真实持久化必须用客体验证。测试入口见 [开发指南](../CONTRIBUTING.md)，实际日志与载荷摘要保存在对应构建与发布目录。ARM64 是串口开发目标，未移植本 SDK 的图形应用执行环境。
