# ArkOS 原生 WebAssembly 运行时

解释器为 wasm3 的静态原生移植，源码固定为 `28ecb9af6d2040e474a70f7cb7f43666740141fb`。上游源码未修改；ArkOS 配置、分配器、数学支持、导入与应用适配位于 `runtime/wasm/`、`user/apps/wasm.c`。保留 MIT 许可及下载归档 SHA-256。没有 Node、浏览器、Linux syscall、宿主 libc 或远程执行依赖。

## 使用

从启动台打开 **WASM**，点击内置示例，或在原生终端执行：

```
wasm builtin:hello
wasm builtin:fibonacci
wasm /mnt/fat32/hello.wasm
wasm /mnt/fat32/app.wasm#entry_name
wasm blob:hello.wasm
```

默认调用无参数 `main`；缺少 `main` 时尝试 WASI `_start`。可返回一个标量。i32/i64 显示整数；浮点返回值显示原始位值，模块也可通过输出导入打印自己格式化的文本。函数不接受界面传入的参数。首次加载文件须通过内核的可信文件授权；内置示例不需要文件权限。

应用中 Ctrl+S 将 Hello 标准模块保存为当前用户的 ArkFS 二进制对象 `hello.wasm`，随后可用 `blob:hello.wasm` 加载；持久化需要挂载可写 ArkFS。FAT32/NTFS 文件使用 `ARK_FILE_READ_BYTES` 分块读取，NTFS 保持只读。二进制对象仍是独立命名空间，不能把它当作普通文本文件编辑。

## 运行边界

- 整个解释器、模块线性内存和输出界面位于独立 Ring3 进程；内核不执行 WASM 字节码。
- 编译/验证所有函数后才运行模块 start 和入口；未调用的非法函数也会导致拒绝。
- 启用字节码类型验证、线性内存边界检查、WASM 栈及本机递归栈限制。没有关闭 W^X/NX；解释器内部元代码存于不可执行数据页。
- 模块文件最大 512KiB；总线性内存最多 4MiB；表最多 4096 元素；WASM 值栈 64KiB；本机执行栈预算 48KiB；解释器私有堆 8MiB。
- 每次执行预算 1,000,000,000 gas units（上游计费单位，不等于 CPU 周期或准确指令数），耗尽会返回 trap。没有 JIT、模块线程或共享内存。
- 每次输出总量最多 64KiB，界面保留前 8191 字节；`ark.log` 单次最多 4096 字节。
- TLS 不受当前 ArkOS ABI 支持；使用 wasm3 官方可配置的无 TLS 路径，保持每个应用单执行线程。构建检查拒绝 TLS 段/节、动态装载依赖和 W+X 段。
- 错误显示在应用中，执行其他模块可恢复。未知函数导入在 start 之前被拒绝；模块没有文件、网络、进程或任意系统调用导入。

## 模块导入

| 模块/名称 | WASM 签名 | 行为 |
|---|---|---|
| `ark.log` | `(i32 offset, i32 bytes) -> ()` | 检查线性内存范围后输出 UTF-8 字节；0 是合法线性内存偏移。 |
| `ark.ticks` | `() -> i64` | ArkOS 100Hz 单调计时 tick。 |
| `wasi_snapshot_preview1.fd_write` | `(i32 fd, i32 iovs, i32 count, i32 written) -> i32` | 仅 stdout=1/stderr=2；最多 64 个 iovec；先检查所有向量及返回地址再输出。 |
| `…proc_exit` | `(i32 code) -> ()` | 终止当前模块并保留退出码，应用继续运行。 |
| `…args_sizes_get` / `…environ_sizes_get` | `(i32 count, i32 bytes) -> i32` | 检查输出地址，返回空参数/空环境。 |
| `…args_get` / `…environ_get` | `(i32 pointers, i32 bytes) -> i32` | 空列表，无写入。 |
| `…clock_time_get` | `(i32 id, i64 precision, i32 result) -> i32` | 仅单调时钟 id=1，写入纳秒值；分辨率 10ms。 |

这是一组 **WASI Preview 1 子集**，没有 WASI 文件描述符/目录、socket、随机源、stdin 交互或组件模型。需要其他导入的应用将被拒绝，不会静默代理给宿主。当前配置关闭 memory64、多内存、原子操作、异常处理、typed references、stack switching；不能宣称实现全部 Wasm 3.0。

## 开发和测试

`examples/wasm/` 包含标准 `.wasm` 文件；`scripts/make-wasm-examples.py` 可离线重建。`make check-wasm-host` 用本机测试夹具运行同一解释器/ArkOS 适配，启用 ASan/UBSan，并用独立 Node 引擎验证模块和结果；Node 仅是开发测试工具，不在目标系统中。

`tests/v9_wasm_test.py bios|uefi` 在生产 ISO 上验证真实磁盘加载（含 40KiB 模块）、拒绝/允许授权、WASI 输出/退出、非法模块/越界/除零/递归/执行预算、保存及重启加载。测试范围不等于上游完整规范测试集或安全认证。

开发者可用支持 wasm32 的编译器生成无系统依赖模块，例如导出 `main`、关闭默认入口并按上表声明需要的导入；资源上限与支持的特性仍适用。SDK 不捆绑完整 C/C++ WASI 工具链。

参考：[WebAssembly 二进制规范](https://webassembly.github.io/spec/core/binary/modules.html)、[wasm3 上游](https://github.com/wasm3/wasm3)、[WASI Preview 1](https://github.com/WebAssembly/WASI/tree/main/legacy/preview1)。
