# ArkUI 原生声明式界面框架

ArkUI 是 ArkOS 的 C 界面框架，代码位于 `user/`，在桌面用户进程中运行。系统设置使用它的视图树、布局、状态绑定和输入捕获；文件管理器使用公共工具栏和图标按钮；窗口、启动器及 Dock 预览共用公共动画接口。绘制产生真实像素，不依赖浏览器、视频播放或宿主界面。

ArkUI 借鉴 SwiftUI 的视图组合与绑定方式，使用 C 编译器构建。**它不是 SwiftUI 移植版，没有 Swift 编译器、Swift 运行时、Swift ABI 或 Apple 框架。** 控件库不直接调用硬件；进程、窗口呈现、文件、权限和系统调用由调用应用与 ArkOS 内核负责。

## 视图与组合

| 概念 | ArkUI 接口 | 当前行为 |
| --- | --- | --- |
| VStack / HStack | `arkui_vstack()`、`arkui_hstack()` | 纵向／横向组合，支持内边距、间距和交叉轴对齐 |
| Text | `arkui_text()` | UTF-8 文本，使用实际中文字库测宽 |
| Button | `arkui_button()` | 鼠标／触控捕获，同一按钮内释放时产生 action ID |
| Toggle / Slider | `arkui_toggle()`、`arkui_slider()` | 绑定 `bool *` / `int *`，直接更新有界状态 |
| Spacer | `arkui_spacer()` | 按权重分配剩余空间 |
| Card | `arkui_card()` | 纵向容器与圆角主题背景 |
| Toolbar | `arkui_toolbar()` | 水平控制容器，默认高 44、纵向居中 |
| IconButton | `arkui_icon_button()` | 原生符号、稳定 action ID、标签、悬停和按压状态 |
| Symbol / Separator | `arkui_symbol_view()`、`arkui_separator()` | 非交互图标／分隔线 |
| Material | `arkui_material()` | 内容、侧栏、工具栏和浮层的语义背景 |
| 修饰器 | `arkui_size()`、`arkui_flex()`、节点 `style` | 明确／自动／填充尺寸，最小／最大尺寸、内边距和间距 |

`arkui_init()` 初始化树，节点 0 始终为根 VStack。创建函数返回节点编号；`arkui_node()` 可取得节点，设置样式、次要文字色、选中状态或启用状态。只能向容器添加子节点。

```c
static ArkUI views;
static bool dark;
static int tint = 170;

void build_preferences(void) {
    arkui_init(&views);
    int toolbar = arkui_toolbar(&views, 0, 4, 6);
    arkui_icon_button(&views, toolbar, ARKUI_SYMBOL_BACK, "返回", 1);
    arkui_spacer(&views, toolbar, 1);
    arkui_icon_button(&views, toolbar, ARKUI_SYMBOL_SHARE, "共享", 2);
    int card = arkui_card(&views, 0, 16, 12);
    arkui_material(&views, card, ARKUI_MATERIAL_CONTENT);
    arkui_text(&views, card, "外观设置");
    arkui_toggle(&views, card, "深色模式", &dark, 3);
    arkui_slider(&views, card, "玻璃染色", &tint, 70, 235, 4);
}
```

可编译示例位于 [arkui_demo.c](../examples/arkui_demo.c) 和 [arkui_transition_demo.c](../examples/arkui_transition_demo.c)。

## 布局与边界

`arkui_layout(&views, bounds)` 先测量内容，再计算每个节点的 `frame` 与 `clip`。尺寸均为整数像素：正文行高 22，普通按钮和图标按钮高 36，开关高 40，带标签滑块高 58。图标按钮默认宽 36，内部符号默认 20。

`ARKUI_AUTO` 使用内容尺寸，`ARKUI_FILL` 消费主轴剩余空间；`arkui_flex()` 设置伸展权重。VStack 和 Card 默认横向拉伸子项。HStack / Toolbar 的 `style.align` 控制纵向的 START、CENTER、END 对齐；交叉轴最大尺寸仍有效。

如果父容器空间小于子项的总需求，框架压缩主轴尺寸和间距，使子项不重叠、不超出父容器；此时不能同时满足所有最小尺寸。文本、符号与控件按实际可见区域裁剪。当前没有自动滚动容器，应由调用应用分组、缩减内容或实现滚动。

单树最多 **128 个节点、8192 字节标签副本**，单节点尺寸限制为 0～16384 像素。容量不足、非法父节点、重复 action ID 和非法符号会拒绝创建，返回 `-1` 并置 `ui.overflow`。标签创建时复制；绑定指针在视图使用期间必须持续有效。

## 主题、材料与绘制

`arkui_theme(dark)` 返回统一的浅色或深色主题。内容材料使用不透明背景；侧栏、工具栏和浮层使用不同的默认染色透明度，使阅读内容与导航控制有明确区分。

```c
ArkUISurface surface = arkui_surface(pixels, stride_pixels, width, height);
ArkUIPainter painter = arkui_canvas_painter(&surface);
ArkUITheme theme = arkui_theme(dark);
arkui_layout(&views, (ArkUIRect){0, 0, width, height});
arkui_draw(&views, &painter, &theme);
```

`ArkUISurface` 使用 XRGB8888，`stride` 的单位是 **32 位像素**。默认 CPU painter 提供抗锯齿圆角、`unicode_draw()` 中文文本和 ArkUI 符号绘制，并遵守画布和节点两层裁剪。没有动态内存分配，也没有字体或图标文件的运行时读取。

调用应用也可提供 `ArkUIPainter.rect`、`.text`、`.symbol`、`.material` 回调；**每个回调都必须遵守 clip**，并使用指定字段初始化器。材料回调是可选的，未提供时绘制相应透明度的染色背景；默认 painter **不额外计算背景模糊**。桌面窗口已有真实的 CPU 背景模糊、折射和染色，文件工具栏使用 `ARKUI_MATERIAL_NONE` 保留窗口提供的玻璃背景。材料 API 本身不能声称提供 GPU 毛玻璃着色器。

## 原生图标与符号

`arkui_app_icon()` 提供十六个统一应用图标，包含终端、文件、文本编辑、设置、关于、计算器、浏览器、时钟、画板、Markdown、任务、截图录屏、安装、待办、计时器和 WASM。编号与桌面应用注册顺序相同。`arkui_symbol()` 提供 26 个导航和工具符号，包括后退、前进、新文稿、文件夹、共享、删除、重命名、复制、搜索、网格、列表、锁、用户、网络及窗口控制。

应用图标使用原创几何、渐变、柔和投影与统一圆角。整数光栅器在第一次使用时生成 96×96 的预乘 alpha 缓存；符号采用 32×32 alpha 缓存。线条和多边形使用 4×4 子像素覆盖率；任意目标尺寸使用双线性采样。绘制尺寸限制为 1～512，支持有间距的目标行和局部裁剪。全部缓存约 **386 KiB**，不按显示器分辨率增长。

这些图标没有嵌入 SF Symbols、Apple 图标字体、Apple 商标或提取的系统图片。图标按钮保存可读标签，`arkui_hover_label()` 可供调用应用显示提示；该标签接口尚不是完整的无障碍服务。

## 输入、状态与重绘

`arkui_navigation()` 创建带图标、文字与右侧箭头的整行入口，使用按钮相同的 action、禁用与按下/释放规则。设置的分组入口由实际导航 action 驱动。`ARKUI_SEPARATOR` 在横向容器中画竖向分隔，在其他容器中画横向分隔，分隔线厚度为一个像素。设置内容视口统一裁剪矩形、文字和符号，并由桌面维护滚动位置。

每个交互组件使用唯一的正数 action ID，0 表示无动作。调用应用对鼠标或触控更新调用：

```c
ArkUIAction action;
bool consumed = arkui_pointer(&views, x, y, down, &action);
if (action.changed) handle_action(action.id, action.value);
if (arkui_take_dirty(&views)) request_redraw();
```

按下根据可见区域命中并捕获 action ID。普通／图标按钮和开关在同一控件内释放才生效；移出后释放取消。滑块捕获后可拖到控件外，值被限制在上下界，直到释放。外部按住再移入控件不会被当作新按下，禁用容器的后代不接收输入。

外部直接改变状态时调用 `arkui_invalidate()`；框架没有属性观察线程或自动依赖图。树可保留，也可用 `arkui_reset()` 重建；捕获通过稳定 action ID 保留。切换页面或取消手势时调用 `arkui_cancel_pointer()`。

`ArkUIDamage` 是独立的有界重绘区域队列：`arkui_damage_add()` 裁剪并合并相交／相邻矩形，最多保留 8 个不相交区域；第 9 个区域自动退回整个视口。`arkui_damage_take()` 取出并清空队列。它可供调用应用批量提交更新，**不自带滚动、列表虚拟化或显示后端，也不会替代桌面的现有合成器**。

## 公共动画与状态过渡

`ArkUIAnimation` 将声明状态、时间线、减弱动态效果与缓存像素变换统一在公共 API 中。内部复用原生 `MotionTrack` 与像素合成器，时间单位统一为 **毫秒**；0.9.1 使用 `ark_millis()` 的客体内时钟。

| 预设 | 显示／隐藏 | 像素变化 |
| --- | --- | --- |
| `ARKUI_TRANSITION_SHEET` | 230 / 330 ms | 窗口沿 Dock 锚点展开／收拢 |
| `ARKUI_TRANSITION_LAUNCHER` | 280 / 230 ms | 以给定内容中心从 1.20 缩到 1.00，并渐显／渐隐 |
| `ARKUI_TRANSITION_HOVER` | 120 / 100 ms | 预览从 0.94 缩放到 1.00，向上移动并渐显 |

```c
static ArkUIAnimation visibility;
arkui_animation_init(&visibility, ARKUI_TRANSITION_SHEET, false);
/* 状态改变时调用；相同目标不会重启动画。 */
arkui_animation_set(&visibility, true, now_ms, reduced_motion);
/* 每帧先恢复背景，再画缓存内容。 */
arkui_animation_update(&visibility, now_ms);
arkui_transition_draw(&visibility, dst, width, height, cached, cw, ch,
                      (ArkUIRect){x, y, cw, ch}, dock_x, dock_y);
bool more_frames = arkui_animation_active(&visibility);
```

动画中反向设置目标会先采样当前位置，再按剩余路程缩短时长，不会跳回起点。“减少动态效果”立即到达目标。`arkui_animation_progress()` 返回 0～65536 的 Q16 进度；`arkui_animation_finish()` 立即完成当前目标。

过渡源与目标必须是**不重叠、紧密排列**的 XRGB8888 缓冲区。Sheet 的锚点是收拢位置；Launcher 的锚点是缩放中心；Hover 忽略锚点。调用应用负责缓存内容、恢复背景和安排下一帧。过渡不自动保存窗口，也不实现通用布局插值或物理弹簧。

桌面 `user/desktop_motion.inc` 使用 Sheet、Launcher、Hover 预设。十六个桌面入口共享 **4 个最近使用的窗口像素缓存槽**，槽被复用时旧映射失效，避免按每个应用永久保留全屏副本。启动器按最多五列布局，十六个应用形成四行；行距按可用高度收缩，避开搜索框，方向键与命中测试使用同一列数。

## 实际接入与测试

`user/desktop.c` 的 `settings_build()` 构建外观、输入、显示、存储、系统五类设置。控件连接真实偏好和系统操作；框架本身不决定持久化位置。文件管理器的 `draw_files_toolbar()` 使用七个公共 IconButton 和弹性 Spacer，保持原有文件操作入口和命中区域，标签在状态栏显示。

在项目根目录运行：

```sh
mkdir -p build/tests
gcc -std=c11 -Wall -Wextra -Werror -g -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  tests/arkui_test.c user/arkui.c user/arkui_icons.c \
  user/raster.c user/unicode.c -o build/tests/arkui_test
ASAN_OPTIONS=detect_leaks=0 ./build/tests/arkui_test

gcc -std=c11 -Wall -Wextra -Werror -g -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  tests/arkui_v5_test.c user/arkui.c user/arkui_icons.c \
  user/arkui_animation.c user/motion.c user/ribbon.c \
  user/raster.c user/unicode.c -o build/tests/arkui_v5_test
ASAN_OPTIONS=detect_leaks=0 ./build/tests/arkui_v5_test
```

两套测试编译真实实现，覆盖中文布局、状态绑定、指针捕获与重建、父级禁用、容量限制、小窗口裁剪，以及全部图标／符号、有间距画布、过渡边界、动画反向与完成、文件工具栏位置、材料绘制和区域合并。v0.5 的上述宿主测试已通过 AddressSanitizer 和 UndefinedBehaviorSanitizer；启动／GPU／应用进程结果见对应系统测试记录。

当前仍没有 Swift 语法或 ABI、完整无障碍树、通用文本输入控件、自动焦点／键盘导航、列表虚拟化、自动滚动、通用布局动画或线程安全调度。图标缓存和视图状态由同一 UI 线程持有。

## 设计参考

本次参考 Apple 官方 [Toolbars](https://developer.apple.com/design/human-interface-guidelines/toolbars)、[Materials](https://developer.apple.com/design/human-interface-guidelines/materials)、[Icons](https://developer.apple.com/design/human-interface-guidelines/icons) 指南：按任务组织水平控制组，将导航控制的材料层与内容层区分，统一图标尺寸、细节与笔画。[macOS 官方页面](https://www.apple.com/os/macos/) 提供当前系统外观参考。ArkUI 的 C 接口、光栅器、几何图标与过渡实现均为本项目代码。


## 0.7 原生活动胶囊和图标

`ARKUI_APP_COUNT`为15，新增Capture、Installer、Todo、Timer四个原创图标，仍通过相同的裁剪/抗锯齿绘制接口生成。Dock只固定前11个应用，启动台可检索全部15个。

顶部活动胶囊的数据直接来自ArkOS录屏、安装和计时器状态。展开/收起使用公共`ArkUIAnimation`的HOVER预设，在33–106像素高度间插值；同样服从减弱动态效果设置。组件由`user/desktop_native.inc`组合，调用方负责状态、背景恢复和命中区域；并未新增绑定外部媒体服务的特权接口。桌面图标、菜单与胶囊区域参加动态壁纸损伤排除，避免壁纸更新擦掉静止控件。

## 0.9 独立应用与可信提示

九个目录应用提供各自surface，桌面统一转发本地输入、复用动画和显示窗口。隐藏窗口不复制新的像素内容；重新显示时复制最新代际。计时器通过ACTIVITY发布状态，内核指定身份，桌面用既有ArkUI过渡展开/收起顶部提示。授权弹窗由SYSTEM绘制，存在时拦截键盘、指针、滚轮和背景交互。新增app_poll_event/OPEN/TEXT协议见API文档。

本版没有把ArkUI重写为Swift语言运行时；它仍是C声明式布局/事件/动画库，窗口内容以本机像素缓冲绘制。独立应用的颜色目前不随全局深色模式自动重绘。


## 0.9.1 渲染和桌面接入

圆角采用三次超椭圆，曲率与直线边连续，边缘使用 8×8 覆盖率；直线内部仍按连续像素段绘制。`arkui_transition_bounds()` 返回含边缘覆盖的保守矩形，调用方恢复、上传新旧矩形的并集，避免每帧恢复整屏。Dock 对应真实的 16 个应用和启动台按钮，使用玻璃底板、运行点、分隔线和缓存悬停动效。

桌面窗口保留 Sheet 的 24×24 曲面网格、纹理和卷曲光影；启动台保留 Launcher 的 1.20→1.00 中心缩放与渐显／渐隐，Dock 几何固定。预览与活动胶囊使用 Hover。`MotionFrameClock` 用绝对毫秒期限安排 60/120/144/240 Hz，跳过落后的时隙。原生 SSE2 混合和缩放保留标量路径，SSE2 上下文仍由内核隔离保存。双线性采样、整数舍入与原先光栅器通过独立参考实现比对。

圆角缩放的内部划分为三个完整覆盖的 SIMD 区域，仅边缘和角部计算覆盖率。横向采样计划预先保存权重与相邻列置换；纯色样本跳过无效插值。曲面光栅器按精确半平面裁剪扫描线，内部像素省去重复边缘判定；8×8 纹理元数据仅在像素确实同色时直接返回该颜色。

`motion_texture_tiles()` 将紧密排列的 XRGB 源纹理生成 `ceil(sw/8)×ceil(sh/8)` 个 32 位元数据项；源像素变化后必须重建。`motion_blit_cached()`、`ribbon_draw_cached()` 和 `arkui_transition_draw_cached()` 接受该只读表，原接口仍使用相同的完整采样路径。多个工作线程只能共享已经生成且在当前任务完成前保持不变的表。

曲面窗口使用 `gpu_present_animation_damage()` 直接上传损伤并集，省去动画帧的保留像素复制与逐块哈希；启动台使用完整区域。后续普通提交重建保留缓存，静止软件指针在每次提交后恢复。

桌面最多使用七个 Ring3 工作线程，与主线程一起合成互不重叠的画布行；四核推荐配置使用三个工作线程。动画和源像素在完成屏障前保持不变。单核退回主线程，创建线程失败时使用已创建的工作线程。空闲和完成屏障通过消息等待，先消耗通知、再检查完成状态，避免丢失唤醒。BSP 在毫秒期限到期时唤醒空闲 CPU；AP 保留 10 ms 抢占定时器。

鼠标覆盖率按尺寸缓存。同步提交前暂时把软件指针叠加到干净画布，提交后恢复底图；动态壁纸和局部光标提交也沿用此路径。移动时先提交新位置，再清理旧位置，重叠区域仍包含新指针。下一次事件等待前完成指针状态更新。硬件指针仍由设备能力决定。实际帧率需要使用对应运行场景的采样日志验证。

## 0.10.0 字体、顶栏与按需活动

0.10.0 字库的记录由 161 字节改为 321 字节：一字节 advance，后接 16×20 的 8 位覆盖率。该历史版捆绑的 WenQuanYi Micro Hei Mono 在构建工具中以 4 倍尺寸光栅化，再进行一次 LANCZOS 下采样；客体读取生成的字库。0.11.0 起字体改为从固定 TTC 轮廓按目标尺寸绘制，当前来源和边界见 [字体说明](../assets/fonts/README.md)。ASCII 8/CJK 16 像素字距和 22 像素行高保持一致。

Dock 为 17 个固定应用加启动台，增加包管理器。底板转角使用 34 像素三次超椭圆范围，8×8 抗锯齿、相同轮廓阴影、半透明内层和低亮度单描边。`glass_edge()` 允许组件选择材料边缘强度，Dock 关闭原材料的重复亮边。窗口沿用原有材料。`ARKUI_APP_COUNT` 为 18，增加原创包管理器与通用安装包图标；八个运行时应用槽复用通用图标，只显示真实已安装记录。

`desktop_topbar.inc` 组合 32 像素菜单栏：活动应用、菜单、真实网络/账号状态、屏幕键盘、系统控制和完整 RTC 日期时间。状态按秒缓存；空闲时只上传顶栏损伤区域，经同一软件指针恢复路径提交。编辑菜单按当前应用可用能力显示，窗口菜单继续调用原有曲面动效。

`ARKUI_TRANSITION_ISLAND` 增加活动展开 300 ms/收起 240 ms 的时间预设。录屏、安装或 ACTIVITY 存在时，顶栏出现点；点击后从 10×10 的点展开为 340×142 的面板。面板内容来自真实活动状态，以有界纹理缓存和原生双线性混合绘制；点击外部或 Escape 收起，活动结束后隐藏，支持中途连续反向及减弱动态效果。它由桌面持有状态、缓存、背景恢复和命中区域，不创建宿主窗口。

Sheet 的 24×24 曲面与光影、Launcher 的 1.20→1.00 中心缩放、Dock 悬停及预览仍使用原实现。客体功能检查与动效采样入口见[开发指南](../CONTRIBUTING.md)；实际 60/120 帧显示验收需要相应场景的帧时间测量。
