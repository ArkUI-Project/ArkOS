# ArkOS native API v1

The public contract is `include/ark_api.h`; standalone build instructions and a complete example are in `sdk/README.md`. Applications are ELF64 executables loaded into Ring 3. They do not link kernel drivers, receive kernel pointers, or call Linux syscalls.

## Calling convention

`int 0x80`: RAX is syscall number; RDI, RSI, RDX, R10, R8, R9 are six arguments. RAX returns a nonnegative result or negative error. Structured calls pass a pointer and exact `sizeof(request)` in the first two arguments. The kernel validates every page and overflow before copying. API v1 is statically versioned; programs can inspect `ArkSystemInfo.abi`.

| Number | Service | Permission and behavior |
|---|---|---|
| 0, 1, 3 | Exit, yield, process ID | Scheduler-managed identity |
| 2 | Spawn catalog application | PROCESS capability; active session; child caps chosen by kernel |
| 4, 5 | Bounded IPC send/receive | Kernel overwrites sender identity; 128-byte message body |
| 16 | System information | Copied metadata, no framebuffer/MMIO addresses |
| 17 | Raw keyboard/pointer events | SYSTEM only; apps receive routed surface events |
| 18, 19 | Presentation and cursor | SYSTEM only; bounded synchronous presentation |
| 20 | Files | FILES capability; active session; canonical private-home scope |
| 21 | Volume metadata | SYSTEM only; copied text/status |
| 22 | Reboot/shutdown | SYSTEM only |
| 23 | Diagnostic log | Bounded copied text, at most 4096 bytes |
| 24 | Surfaces/events | UI capability; multiple owned surfaces; desktop queries/copies/routes input |
| 25 | Network/HTTP/HTTPS | NETWORK capability; one request owned by initiating PID |
| 26 | Account status/authentication | Public status; all mutations SYSTEM plus session/admin policy |
| 27 | Monotonic ticks | 10 ms units; native monotonic clock |
| 28, 29 | Threads / task snapshots | Native SMP threads; SYSTEM task management |
| 30 | Disk installer | SYSTEM plus active administrator |
| 31 | Binary objects | FILES plus current active UID; persistent ArkFS only |
| 32 | App permissions | Kernel identity; trusted management and consent |
| 33, 34 | Activity / launch argument | Owned activity, private launch metadata |
| 35 | Monotonic milliseconds | Public; native HPET, 1 kHz PIT fallback |
| 36 | Native packages | SYSTEM plus active session; checked ArkPkg admission and persistent grants |
| 38 | Private heap allocation | scalar byte count; committed demand-zero RW/NX region, per-process budget |
| 37 | RTC date and time | Public; copied coherent native RTC snapshot, with validity flag |
| 39 | Configuration registry | Active UID and real application namespace; typed persistent values |
| 40 | Drag/drop | UI; owned source/target token, FILES for file sources; SYSTEM routing |
| 41 | Performance | SYSTEM; copied native allocator and CPU accounting |
| 42 | Anonymous virtual memory | owned reserve/commit/protect/decommit/release/trim and demand paging; see VM.md |
| 43 | Glass compositor | SYSTEM; fixed native VirtIO/VirGL shaders, checked staging/readback; see Fixed GPU glass below |
| 44 | Device inventory | public metadata in an active session; DEVICE capability for block reads; see Device model |
| 45 | Loadable kernel drivers | list/query public; install/remove need SYSTEM + admin session; see DRIVERS.md |

`ArkPresent.flags` uses `ARK_PRESENT_FULL` for the complete display; otherwise
the checked `damage` rectangle selects the update. `ARK_PRESENT_ANIMATION`
streams that selected region without retained-tile hashing/copies. It also
invalidates the retained shadow for the next ordinary presentation. Both paths
still validate the complete source mapping, dimensions, stride and caller's
SYSTEM identity, and consume the pixels synchronously.

The C SDK wraps the ABI without changing its security boundary. Capabilities are SYSTEM=1, FILES=2, UI=4, NETWORK=8, PROCESS=16, ACTIVITY=32. Request fields named UID/PID never grant authority. The trusted desktop has SYSTEM. Eleven independently linked built-in applications have immutable maxima; per-user grants may reduce them. Installed ArkPkg applications declare their maxima within UI|FILES|NETWORK|ACTIVITY and initially receive UI only.

## Fixed GPU glass

`ARK_SYS_COMPOSITOR` takes `ArkGlassRequest`. QUERY=0 initializes and verifies the
fixed pipeline once; it returns `-19` if a usable VirGL device is unavailable.
RENDER=1 samples the caller's current XRGB8888 canvas and updates only the clipped
glass rectangle. Flags select seven-color dispersion and the compositor's
bounded eight-blend rectangular shadow. Four GPU passes prepare saturation and
shadow, blur horizontally and vertically, then apply lens/refraction/highlight
and continuous corner coverage. All retained intermediate textures contain
fresh source pixels for that operation.

Only SYSTEM callers may use the API. The kernel validates every caller page,
stride and arithmetic, copies pixels to supervisor staging, and exposes only
that staging to PCI DMA. No user pointer or arbitrary shader/command stream is
accepted by the device. It waits for the checked readback fence before copying
the result to user memory. A fully offscreen rectangle completes without
altering pixels or enqueueing work. Invalid memory returns `-14`, invalid
geometry/flags `-22`, device failure `-5`; failure disables the GPU path and
the desktop resumes the CPU effect.

Width/height are bounded to 3840×2160 and stride to 3840 pixels. The rectangle
cannot exceed the supplied canvas dimensions; its origin is bounded to one
canvas extent in each direction. Radius cannot exceed half the shorter side;
tint and edge strength are 0–255. Shadow bounds must stay within the canvas.
QUERY and successful RENDER return cumulative jobs, passes, upload/readback
bytes, fences, elapsed milliseconds and per-stage timings. Initialization
probes contribute transfers/fences, but not application jobs/passes.
`ArkSystemInfo.gpu_capabilities` bit 31 identifies this shader path;
`gpu_accelerated` also remains true for verified VMware 2D acceleration.

This API accelerates material pixels. Text, other desktop drawing, curved
window composition and final presentation retain their existing paths.
Ordinary applications use surfaces and receive the material through the
desktop; they do not need SYSTEM access. Full GPU composition and zero-copy
scanout are not implemented by this interface.

## Surfaces and input

The `0.13.0+mouse1` revision adds a native SPICE absolute mouse channel for the
UTM GPU configuration. It produces the existing pointer and `ARK_EV_SCROLL`
events through the trusted desktop, including press/release and disconnect
release. ABI 13 and application event layouts are unchanged. The driver implements dedicated modern VirtIO-serial, display ID 0 and vertical
wheel events; disconnect releases its held buttons. BIOS/UEFI checks are in
`tests/spice_mouse_vm_test.py`.

Applications allocate XRGB8888 pixels in their own writable memory and CREATE a surface. PRESENT copies validated pixel rows into a kernel-owned surface. A generation counter lets the desktop COPY only new content. SEND_EVENT is desktop-only; NEXT_EVENT is available only to the owner. CLOSE revokes the surface. Kernel limit is twelve surfaces up to3840×2160; the desktop copies settled application pixels at1:1, clipping window bounds. SDK apps render logical800×500 at density1 or2, and divide incoming pointer coordinates by that density. Window motion still transforms its cached texture.

## Files

CREATE/STAT/READ/WRITE/MKDIR/REMOVE/RENAME/COPY/LIST/SYNC use bounded UTF-8 paths and text content. Maximum path is127 bytes; maximum file body16383 bytes. Relative client paths resolve against the authenticated home; the kernel resolves and checks the final canonical path again. The own home root cannot be moved or deleted. `/.system`, aliases resolving into it, and another user's home are denied. `/etc` is read-only; `/mnt` delegates writable/read-only status to FAT32/NTFS. These are experimental text-file APIs, not POSIX descriptors, mmap or a general binary-stream filesystem API.

## ArkUI

`include/arkui.h` supplies declarative node trees, stacks, cards, text, icon buttons, toolbars, toggles, sliders, materials, layout, pointer capture, and bounded invalidation. `include/arkui_animation.h` supplies interruptible native transitions. All ArkUI code operates on user-owned memory; presentation goes through the surface API. See `docs/ARKUI.md` and examples.

## SDK applications

Clock displays time and a stopwatch. Paint accepts pointer/touch strokes, undo/clear and exports real SVG to the user's home. Markdown reads/saves real UTF-8 text and renders a limited heading/list/code preview. Each is a separately linked executable with separate address space and process lifetime.

The eleven independent system entries are compiled from include/ark_catalog.h and packaged into actual boot ArkPkg containers. Eight system tools share the trusted desktop ELF with selected entries. Locally installed packages use a growable per-account catalog and the same W^X Ring3 loader. Launch arguments use LAUNCH. TLS1.2 is implemented in NETWORK; this is distinct from unsupported thread-local storage, generalized sockets, AVX, pthreads and POSIX compatibility.

## 0.6 扩展：线程、等待与任务快照

原生 ABI 仍为 1，新增调用不改变已有结构。

| 调用 | 参数结构 | 行为 |
|---|---|---|
| 28 THREAD | ArkThreadRequest | CREATE=0、EXIT=1、JOIN=2、SLEEP=3、ID=4、WAIT=5 |
| 29 TASKS | ArkTaskRequest | LIST=0 或 KILL=1；仅 SYSTEM |

CREATE：entry、argument、trampoline 为用户值，entry/trampoline 必须可执行；返回 tid。退出跳板见 `sdk/thread.h`。JOIN：仅允许本进程的其他子线程；运行中返回 -11，退出后返回 status 并释放任务槽。栈页在进程退出时统一释放。EXIT：只退出调用子线程；主线程应使用 SYS_EXIT 结束整个进程。SLEEP/WAIT：ticks 为 10 ms 单位，上限 8640000；WAIT 可被待处理事件提前唤醒，SLEEP 等待期限。无 POSIX pthread/TLS 兼容承诺。

TASKS/LIST 的 buffer 指向 ArkTaskInfo[capacity]，capacity≤32；返回 count、ticks、online_cpus。记录包含 tid/pid/uid/state、exit_status、thread、capabilities、mapped_bytes、cpu_ticks、switches 和 name[32]。共享线程的 mapped_bytes 显示栈预算，不应与所属进程重复累加。状态 0空闲、1就绪、2运行、3退出、4睡眠、5等事件。KILL 传 tid；核验后终止所属进程组，SYSTEM 和当前进程组受保护。

SYS_YIELD 参数 a1=0 保持旧行为；a1=1 在只有调用者可运行时不 HLT，供短时动画使用。长期空闲应使用事件等待。

新增 ARK_EV_SCROLL=8：x 为水平轮步，y 为垂直轮步，正 y 向下；buttons 保持报告的五键状态。输入 ABI 其余布局不变。


## 0.7 扩展：真实 SMP 与持久化服务

当前 x86 用户代码在最多 8 个 xAPIC CPU 上并行执行，所有内核入口由全局锁串行化。最多 32 个任务槽；线程共享进程页表，具有独立栈和 x87/SSE2 上下文。跨核进程退出先撤销所有线程，确认没有 CPU 使用该地址空间后才回收页。没有用户态 TLS、pthread、AVX、任意 mmap/unmap、CPU 热插拔或细粒度并行内核。C11 原子操作示例见 `sdk/parallel.c`。ARM64 目前只有 EL0 自检，不能运行本 x86 ABI 的应用。

### SYS_BLOB（31）：二进制对象

`sdk/files.h` 提供 `ark_blob()`。传入清零的 `ArkBlobRequest`；内核按认证 UID 确定范围，结构没有可用于伪造 UID 的字段。调用者必须具有 FILES，且属于当前活动会话。SYSTEM 桌面使用当前会话 UID。

| op | 输入 | 成功结果 |
|---|---|---|
| LIST=0 | index：该用户对象的序号，从0开始 | name、size、checksum；count为该用户总数；越界返回-2 |
| READ=1 | name、buffer、offset、capacity | 必须满足 offset+capacity≤size；count为复制字节数；校验整个对象 CRC 后才返回成功 |
| WRITE=2 | name、buffer、capacity | 完整创建/替换，1–8MiB；忽略offset；size/count为字节数，checksum为CRC32 |
| REMOVE=3 | name | 持久化删除索引项 |

名称为 1–63 字节，不含 `/`、反斜杠、控制字符，不接受单独的 `.` 或 `..`；客户端应传有效 UTF-8，当前二进制服务不额外验证 UTF-8 编码。`@pkg.` 前缀为内核包事务私有区，公开 BLOB 对所有调用者隐藏并禁止访问，包括 SYSTEM。对象索引按可用元数据内存增长，所有用户共享磁盘容量。它独立于文本 FILE 命名空间；`cat`/文件管理器不打开这些对象。读取失败时缓冲区可能已被部分写入，必须丢弃结果。

索引位于 ArkFS 卷内 LBA 8192/8200，数据区从8208开始。两份4KiB根与链接索引页面带CRC和代数；写入新区域、flush、提交另一份索引、flush后才返回成功。分配时保留两份有效索引引用的所有区段，因此覆盖旧对象也需要额外空间。错误：-1权限/会话，-2未找到/枚举结束，-5磁盘或CRC失败，-14指针/范围错误，-22名称/操作错误，-12元数据内存不足、-28磁盘空间耗尽。错误文字仅部分路径填写，以返回值为准。未挂载ArkFS时不降级为“已保存”。

### SYS_PERMISSION（32）：应用授权与撤销

目录以 `include/ark_catalog.h` 为准；权限 ID 与桌面图标 ID 不同。记录按账号保存。

| 权限 ID | 程序 | 最大权限 | 新账号默认权限 |
|---:|---|---|---|
| 0 | clock | UI=4 | UI |
| 1 | paint | UI+FILES=6 | UI+FILES |
| 2 | markdown | UI+FILES=6 | UI+FILES |
| 3 | notes | UI+FILES=6 | UI |
| 4 | browser | UI+NETWORK=12 | UI |
| 5 | calculator | UI=4 | UI |
| 6 | todo | UI+FILES=6 | UI |
| 7 | timer | UI+ACTIVITY=36 | UI+ACTIVITY |
| 8 | wasm | UI+FILES=6 | UI |
| 9 | calendar | UI=4 | UI |
| 10 | reminders | UI+ACTIVITY=36 | UI+ACTIVITY |
| 256+稳定安装槽 | pkg.ID，按稳定安装槽 | 包声明的 UI/FILES/NETWORK/ACTIVITY 子集 | UI |

所有请求传清零的 `ArkPermissionRequest` 及其准确大小。会话必须活动。

| op | 调用者、输入及结果 |
|---|---|
| GET=0 | SYSTEM；app=目录权限ID；返回name/maximum/grants |
| SET=1 | SYSTEM；app/grants；拒绝任何超出maximum的位；撤销立即影响整个进程组 |
| REQUEST=2 | 普通目录应用；grants=所需非UI权限；内核从真实进程名、PID和UID确定身份，忽略伪造的app/name；0已授予、-11等待可信弹窗、-1拒绝或身份/范围不合法、-16该应用已有其他请求者 |
| PENDING=3 | SYSTEM；返回一个待处理app/grants/name；空队列返回-2 |
| RESOLVE=4 | SYSTEM；app和与待处理请求完全一致的grants表示允许；grants=0拒绝；过期请求-2，不匹配-22 |

每个目录应用最多一个待处理项。拒绝后，该进程本次运行不会反复弹相同权限；进程退出、会话变更或设置页重新设置权限会清理相应状态。普通应用没有管理、查看其他应用授权或向自己授予UI/SYSTEM的接口。弹窗的名称和图标来自可信目录。

安装包的 GET/SET 使用 256 加槽号；包管理器也可通过 PACKAGE/GRANTS 按 ID 管理。包应用 REQUEST 从真实进程身份推导槽位，只确认已有授权，未授权返回 -1。额外授权由包管理器显式保存到包对象，不进入内置应用的弹窗队列或 ARKP2 记录。

受保护记录 `/.system/grants-UID` 使用ARKP2格式；读取兼容ARKP1的前三个权限ID，新增应用使用上述默认值；下次修改写出ARKP2。损坏记录使所有目录授权归零。无磁盘时记录仅存在于内存。同步失败返回-5，但已经执行的内存撤销不会回滚；应提示未能持久化。

FILES撤销阻止后续文件/二进制请求；NETWORK撤销取消当前HTTP所有权并阻止新访问；ACTIVITY撤销移除活动；UI撤销结束整个应用进程组。已经读入应用内存的数据不会因撤销而被抹除。

这是有界、按账号/应用的粗粒度权限系统：文件范围是该账号主目录、公开只读目录和允许的挂载卷，没有每应用私有文件目录、文件选择器授权、网络域名规则或细分读/写权限。桌面、终端、文件管理、设置、任务管理、截图和安装仍属SYSTEM可信组件，不能称为完整全应用沙箱。

### SYS_ACTIVITY（33）：由内核验证身份的活动提示

传 `ArkActivityRequest`。SET=0只允许当前账号的普通目录或已安装包应用且具有ACTIVITY；active为0/1，kind当前只能为1（计时），deadline为100Hz绝对ticks且不超过当前时间后24小时。app/title由内核根据受保护目录重写，不能伪造其他应用。GET=1仅SYSTEM可调用，app为桌面ID或UINT32_MAX（取第一个active项）。没有活动时active=0。应用完成、暂停应发布active=0，退出或撤销权限也会清除活动。当前实现不承诺后台应用异常时的定时完成通知。

### SYS_LAUNCH（34）：启动参数

`ArkLaunchInfo`只包含128字节的NUL结尾argument。仅当前账号中由目录启动的所属进程/线程可读取自己的参数。没有全局参数枚举。

SPAWN目录应用按当前UID复用一个实例；非空argument更新最新参数，并给已存在surface发送`ARK_EV_OPEN=9`。它是“读取最新参数”的通知，不是历史参数队列。Notes将参数解释为文件路径；Browser将其解释为URL。`run APP [argument]`会启动/激活对应窗口。文件访问仍由FILE服务检查，传递路径本身不授予权限。

`ARK_EV_TEXT=10`的key为Unicode标量，用于屏幕键盘提交；SDK验证标量范围及缓冲区容量。`app_poll_event`直接返回CLOSE，供编辑器保存或拒绝关闭；默认`app_event`仍自动关闭退出。拒绝关闭后桌面会重新显示窗口，任务管理器仍可强制结束进程。

### SYS_INSTALL（30）：原生安装器

`sdk/files.h` 的 `ark_install()`。所有操作均要求SYSTEM、活动会话和管理员身份。普通第三方应用不能获得该权限。

| op | 输入/行为 |
|---|---|
| LIST=0 | disk=0或1，返回present、sectors、in_use、media；不写盘 |
| BEGIN=1 | disk、刚查询的sectors、以NUL结尾的`ERASE`；重新校验容量、占用和ARKOS0100光盘卷标；开始即清除目标MBR |
| STEP=2 | 每次复制最多32个2048B光盘扇区；最后生成ArkFS/GPT并提交引导MBR |
| STATUS=3 | 查询state、done、total和message；进度以2048B光盘扇区计 |
| CANCEL=4 | 停止正在进行的安装；不恢复目标盘 |

state：0未开始、1复制中、2完成、3取消、4失败。驱动要求512B目标扇区，容量128MiB–约2TiB。安装是破坏性全盘操作，不提供分区缩容、双系统或回滚。复制当前账户/文本/设置；二进制媒体不迁移。设置页面已经挂载的ArkFS/FAT32/NTFS所在磁盘会被拒绝。只有state=2才可以报告安装完成。客体安装检查入口为 `tests/install_013_test.py`。

## 0.9 additions

`ARK_FILE_READ_BYTES` (operation 12 in FILE) is a binary-safe positional read. It retains FILES capability, active UID and canonical-path checks, verifies the entire writable caller range before disk I/O, accepts capacity up to ARK_FILE_MAX and a 64-bit offset, and returns count without a terminator. EOF returns zero, including offsets above EOF. External reads bypass the text cache; native text files can also be read. It does not add binary writes or NTFS writes. Large ArkFS binary objects still use BLOB.

Catalog entry 8 (`wasm`, desktop entry 15) has maximum FILES|UI and default UI. Eight-entry ARKP2 records migrate without changing existing grants; the ninth entry receives its default. Corrupt records still deny all. Surface slots are now 12; the public struct ABI remains version 1.

The WebAssembly import ABI is documented separately in WASM.md. It exposes checked logging and monotonic time plus a limited WASI Preview 1 set, not raw int80/syscall access or filesystem/network imports. Module execution remains in Ring3.


## 0.9.1 单调毫秒与高刷调度

新增 `ark_millis()`（35）与 THREAD 操作 `ARK_THREAD_WAIT_MS=6`：请求的 `ticks` 字段在此操作中为毫秒，上限 86400000，超出返回 -22。布局和已有 SLEEP/WAIT 的 10 ms 单位保持兼容。`ark_wait_ms(ms)`、SDK `ark_wait_milliseconds(ms)` 等待期限或本线程的消息／surface 事件；桌面硬件输入仍按最多 10 ms 的等待期限读取。时钟与等待无需 SYSTEM 权限，用户指针仍按线程接口检查。

`ARK_THREAD_WAIT_MSG_MS=7` / `ark_wait_message_ms(ms)` 只等待本线程消息或毫秒期限，长度上限相同。桌面合成工作线程使用它，避免尚未处理的键鼠事件让所有工作线程反复醒来。消息只唤醒指定 TID；普通事件仍按进程唤醒事件等待者。空闲核心收到调度 IPI，副核心本地定时器为 1 ms。

ACPI HPET 主计数器提供不依赖中断交付次数的客体时间；缺少支持的 HPET 时退回 1 kHz PIT，负载下精度会受中断延迟影响。`ark_ticks()` 返回同一时钟除以 10。桌面支持 60/120/144/240 的绝对时间调度并跳过过期帧；这是提交目标，实际帧率由对应 QEMU 场景的采样日志确定。

## 0.10.0 原生包与完整日期

`include/package.h` 提供 `ArkPackageInfo`、`ArkPackageRequest` 与 `ark_package()`。SYS_PACKAGE=36 校验准确结构大小、用户映射、SYSTEM 和活动会话，从内核会话身份取得 UID。操作为 LIST=0、INSPECT=1、INSTALL=2、UPGRADE=3、REMOVE=4、GRANTS=5、FIND=6。LIST 的 index 是稠密枚举序号，前19项为系统包，随后为当前用户包；枚举结束返回 -2。稳定 slot 单独返回，FIND 按 ID 查询。检查和安装传本地 path 或 `blob:NAME`，卸载/授权传 ID。图形安装传入检查时的 `expected_manifest_sha256`，源清单变化返回 -11；直接命令可传全零。输出 info 含版本、最大/当前权限、PID、名称及清单摘要。

包格式、完整生命周期、COW 事务与资源限制见 [PACKAGES.md](PACKAGES.md)。主要错误为 -1 权限/会话、-2 未安装/枚举结束、-5 数据盘或完整性失败、-11 检查后源改变、-16 运行中不能更新/卸载、-17 已安装、-22 格式/版本无效、-12 元数据内存不足、-28 磁盘空间不足。只有提交成功才能报告安装、升级或授权成功。原生启动名是 `pkg.ID`；使用现有 SPAWN，子进程的 UID 与权限由受保护记录确定，原有 LAUNCH 和 OPEN 通知继续生效。

SYS_DATETIME=37 接受 `ArkDateTime`（year、month、day、hour、minute、second、weekday 为 int32，valid 为 uint32）。`ark_datetime()` 复制稳定的原生 CMOS RTC 快照；星期 0=周日，6=周六。valid=0 时调用方不应展示日期。RTC 按 UTC 配置，桌面按账户偏好转换时区；单调等待仍使用 MILLIS/TICKS。

## 私有堆与高分辨率

`SYS_MEMORY=38` 是标量系统调用，RDI 为字节数，返回私有用户地址或负错误。每次最多64 MiB、向上对齐4 KiB，页先清零且固定 RW/NX；不能映射内核地址或自行指定虚拟地址。普通进程所有映射合计64 MiB，SYSTEM512 MiB。失败回滚已分配数据页；进程组退出统一回收。没有独立 free/unmap；SDK 的2倍surface和桌面缓存随进程生命周期保留。

页池从物理128 MiB开始，根据真实 RAM 映射选择连续容量，最多896 MiB；元数据不把全部物理页静态链接到内核。GPU shadow、surface像素按需分配为内核专用页，应用只能通过检查的 COPY/PRESENT 使用。4K 客体验证使用1 GiB RAM、64 MiB虚拟显存。

## 0.12.0 窗口、主题与文本

`ArkSpawn.flags` 接受 0 或 `ARK_SPAWN_NEW=1`。NEW 为目录应用或普通包创建独立实例；启动参数按真实 PID 保存。已有 surface 事件结构保持不变，增加 THEME=11（key=0 浅色、1 深色）、NEW=12、DROP=13（key 为传输 token、x/y 为目标局部物理坐标）与 DRAG_END=14（buttons 为接受动作，0 取消）。SCROLL 的正 y 向下；内核合并冗余移动和滚轮事件，保留按钮边沿、键盘和关闭请求。

SURFACE/RESIZE=7 仅允许拥有者调用，width/height 为物理像素；分配失败保留旧缓冲。桌面发送 RESIZE 事件，SDK 更新私有缓冲和布局。一个进程可拥有多个 surface，每个有独立事件队列。0.13 起系统关闭按钮请求退出该 PID 的全部窗口；SDK `app_event()` 退出进程，`app_poll_event()` 让编辑器先检查保存成功。应用内部 `app_close()` 仍可只关闭一个 surface。总上限 12，每个最多 3840×2160。普通映射预算 64 MiB，SYSTEM 512 MiB；0.13 匿名区可按 SYS_VM 的完整区间释放。

SURFACE/INPUT=8 由拥有者声明 `ARK_SURFACE_TEXT_INPUT=4` 并在 damage 中设置插入点。系统拼音只对声明的文本目标提交 UTF-8 码点 TEXT 事件；认证输入由可信登录界面处理。SDK `app_text_input()` 将逻辑插入点转换为实际像素，`app_codepoint()` 插入码点。任意尺寸布局应读取 `app_width()/app_height()`；用旧 SDK 构建的程序不会自动获得这些能力。

## SYS_REGISTRY（39）

`ArkRegistryRequest` 使用 GET=0、SET=1、DELETE=2、LIST=3、REVISION=4；类型 INTEGER=1、STRING=2、BINARY=3。key[128] 是 NUL 结尾的 ASCII 分层路径；整数为有符号 64 位。字符串最多 511 字节且无内嵌 NUL，二进制最多 512 字节。LIST 的 key 是前缀、index 是稠密序号；返回完整键、类型、值、count 和 generation，结束 -2。REVISION 返回该会话原生存储缓存的修改代数，用于刷新；不是逐项事务版本或跨调用 compare-and-swap。

应用仅能修改 `/apps/<真实进程名>/`；普通包名称为 `pkg.ID`。内核从 ProcessInfo 与当前 UID 推导命名空间，不能以请求中的声明越权。普通应用只读指定的主题、时区和输入偏好。SYSTEM 可访问当前用户的 `/user/` 和应用配置；`/system/` 写入还要求管理员。个人输入学习不公开给普通应用。所有对象存入 UID 私有 `@registry.` 封套，公开 BLOB 不可读写、列举；封套包含原始键和类型并校验摘要身份。持久写失败返回 -5，不报告 RAM 保存成功。元数据缓存预算 4 MiB。

`sdk/config.h` 提供二进制读写；整数和字符串使用 `ark_registry()`。终端 `reg get/list/delete KEY`、`reg set KEY int|string VALUE` 调用同一服务。

## SYS_DEVICE（44）

`ArkDeviceRequest` 的操作 ENUMERATE=0、QUERY=1、STATS=2、READ=3、CONTROL=4；class 为 PLATFORM=1…PCI=11；bus 为 PLATFORM=0…VIRTUAL=3；flag 为 PRESENT=1、READABLE=2、WRITABLE=4、SYSTEM_VOLUME=8、REMOVABLE=16、MODULE=32；state 为 UNKNOWN=0、OK=1、ERROR=2、ABSENT=3。节点最多 64 个，索引在本次启动内稳定，`index` 是用户态唯一可用的标识。

设备清单、查询与统计只需要活动会话，不需要任何能力；READ 需要 DEVICE 能力。读操作把最多 128 个扇区（`ARK_DEV_READ_CAP` 64 KiB）复制到调用者的缓冲区，整个目标区间在搬运任何扇区之前就已校验，暂存副本用后清零。ArkFS 系统卷永不以此方式可读，也没有类具备写路径。CONTROL 的 REFRESH 需要 DEVICE 能力并重跑驱动计数器，FLUSH 需要 SYSTEM；块刷新还要求节点同时 PRESENT、READABLE、WRITABLE，因此系统卷不能从这里刷新。每个进程每 100 ms 最多 16 次尝试，超出返回 -16；拒绝同样消耗预算。

## SYS_DRIVER（45）

`ArkDriverRequest` 的操作 LIST=0、QUERY=1、INSTALL=2、REMOVE=3；state 为 LOADED=1、DISABLED=2、FAILED=3。LIST/QUERY 只需要活动会话；INSTALL 与 REMOVE 需要 SYSTEM 能力、活动会话，并且当前账户是管理员，三者缺一即 -1。请求失败时结构体仍会回写，`count` 因此始终是已安装驱动的总数，越界索引返回 -2。

INSTALL 接受本地规范路径或 `blob:NAME`，先按整份文件校验受保护清单 @drv.manifest 记录的 SHA-256，再校验 ARCO1 头、镜像 CRC-32 与 SHA-256，最后才映射并调用 `arco_entry`；驱动拒绝 INIT 时不写任何持久状态。REMOVE 调用 DEINIT、把该模块注册的节点置为 ABSENT、清空标志，并删除清单条目与镜像 blob，槽位与映射页随即归还内核池，同名驱动可以重新安装。加载格式、隔离方式与限制见 [DRIVERS.md](DRIVERS.md)。

## SYS_DRAG（40）

`ArkDragRequest` 的操作 BEGIN=0、STATUS=1、DELIVER=2、READ=3、CANCEL=4、ACCEPT=5；kind TEXT=1 或 FILE=2；动作 COPY=1。全系统有一个有界传输，文本最多 511 字节，文件路径最多 127 字节，MIME 最多 47 字节。BEGIN 验证真实来源 surface 与 UID；FILE 还检查 FILES、规范路径和文件存在。可信桌面内部来源为 UINT32_MAX。不能用 drag 获得文件权限。

DELIVER 仅 SYSTEM 可调用，验证目标 UID、存活 surface 与局部 x/y。DROP 把 token 给目标，READ/ACCEPT 仅目标拥有者可调用；接收成功调用 ACCEPT/COPY，失败 ACCEPT/0。接收者仍自行读文件、保存数据并处理容量错误。结束时来源收到 POINTER 松开与 DRAG_END；拖动最长 60 秒，投递等待最多 5 秒，会话变更或来源进程退出清理传输。暂存区保留有界副本，只执行复制。接口见 `sdk/drag.h`，完整多窗口示例见 `sdk/workspace.c`。

## SYS_PERFORMANCE（41）

仅 SYSTEM，传 `ArkPerformanceInfo`。返回客体毫秒、8 核累计 busy/total 毫秒、在线 CPU、当前存活任务数，以及实际分配池总量、空闲、内核缓冲与用户映射字节。0.13 追加匿名区预留/提交/交换、缺页/换入/换出、已分配交换区容量、实际块读写和网络字节计数；内核也接受以 reserved_bytes 偏移为长度的旧前缀。差值用于采样 CPU 使用率；不是硬件性能计数器或整机所有内存的统计。TASKS/LIST 过滤 DEAD 记录，仍列举系统任务和线程；已退出线程内部可保留用于 join，列表不显示它。

## 0.13.0 任务与指针

TASKS/METRICS=2 将 `ArkTaskMetrics[capacity]` 写入 buffer，capacity≤32，返回 count/ticks/online_cpus。task 为旧 `ArkTaskInfo` 前缀；后接 reserved_bytes、committed_bytes、swapped_bytes、faults、read_bytes、write_bytes、messages_sent、messages_received。它仍仅供 SYSTEM，过滤退出任务。共享线程的内存、匿名区与 I/O 属于 owner，按 PID 展示时只累计一次；CPU 则累计同 PID 的各 TID。读写计数为该任务实际完成的块操作；缓存命中不产生磁盘字节。没有逐进程功耗与网络 socket 字节统计。

SURFACE/CURSORS=9：拥有者传 pixels=`ArkCursorRegion[capacity]`，capacity≤32；rect 为非空且完全在 surface 内的物理区域，shape<ARK_CURSOR_COUNT。校验整个批次后原子替换，不允许修改其他 PID 的区域。SURFACE/CURSOR_AT=10 仅 SYSTEM，event.x/y 为 surface 局部坐标，返回 flags=形状；重叠采用最后声明的区域，区域外为箭头。SDK 负责逻辑密度转换。

GPU/CURSOR_SHAPE=2 仅 SYSTEM，`ArkGpuRequest.reserved` 为形状编号。MOVE/SCALE 布局不变。硬件 SVGA 指针与软件 AA 指针使用相同几何和热点；硬件路径受二值掩码限制。

SYS_VM=42 传清零 `ArkVMRequest`，完整操作、对齐、交换盘和 -16 并发限制见 [VM.md](VM.md)。SYS_MEMORY=38 保留 scalar ABI，改为匿名区预留加提交，物理页在第一次访问或经检查的系统调用复制时分配。
