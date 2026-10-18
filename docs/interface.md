# 接口与补丁说明

方案背景见 [architecture.md](architecture.md)。本文只列补丁和接口：Chrome 一侧有哪些入口，网关一侧要实现什么。

> 补丁基于 Chromium `main` **M158.0.8111.0**（commit `f05e8113`）。已确认能在该提交上用 `git am` 干净应用，**但没有编译过**，第一次在 Windows 上编译肯定会有错误要修。

## 1. 补丁

| 文件 | 内容 | 对应 architecture.md |
|---|---|---|
| `patches/0001-…offscreen….patch` | 新增开关 `--cloud-stream-pipe`、`--cloud-stream-external-begin-frame`（`ui/base/ui_base_switches.h`）；cloud 模式下不把 HWND 交给 GPU 进程（`viz_process_transport_factory.cc`） | P1 |
| `patches/0002-…CloudStreamHooksWin….patch` | `ui/views` 里的 hook 接口 `views::CloudStreamHooksWin`；`DesktopWindowTreeHostWin` 的三个挂点：建 compositor 时是否启用外部 BeginFrame、光标变化、拖放 client | P4、P5、P7 |
| `patches/0003-…CloudStreamController….patch` | 新目录 `chrome/browser/cloud_stream/`，并在 `ChromeBrowserMainExtraPartsHeadless::PreBrowserStart()` 中初始化 | P2、P3 |

应用：
```
cd src
git checkout f05e8113
git am /path/to/patches/*.patch
```

## 2. Chrome 一侧的接口

### 2.1 启动参数

| 开关 | 作用 |
|---|---|
| `--headless=new` | 必需。cloud 模式只在 headless 下初始化 |
| `--cloud-stream-pipe=<name>` | 启用 cloud 模式；Chrome 连接网关创建的 `\\.\pipe\<name>` |
| `--cloud-stream-external-begin-frame` | 可选。由网关发 `kBeginFrame` 驱动出帧；不带时用默认的 60Hz 合成时钟 |
| `--use-adapter-luid=<luid>` | 必需。与网关编码用的显卡一致 |

其余推荐参数见 architecture.md §5。

### 2.2 `views::CloudStreamHooksWin`（`ui/views/widget/desktop_aura/cloud_stream_hooks_win.h`）

views 不能依赖 chrome，所以由 chrome 实现这个接口、再用 `Set()` 注册。没有注册时 `DesktopWindowTreeHostWin` 的行为完全不变。

| 方法 | 调用位置 | 当前实现（`CloudStreamController`） |
|---|---|---|
| `UseExternalBeginFrameControl()` | `DesktopWindowTreeHostWin::Init()` 调 `CreateCompositor()` 时 | 返回是否带 `--cloud-stream-external-begin-frame` |
| `OnCursorChanged(host, cursor)` | `SetCursorNative()`，设系统光标之前 | 发 `kCursor`，自定义光标附带 BGRA 像素 |
| `CreateDragDropClient(host)` | `CreateDragDropClient()`，替换 OLE 拖放 | 返回 `NoopDragDropClient`（拖放暂时禁用） |

### 2.3 `chrome/browser/cloud_stream/` 模块

| 类 | 职责 |
|---|---|
| `CloudStreamController` | 单例，UI 线程。用 `aura::EnvObserver` 跟踪所有 `WindowTreeHost`，分配窗口 id；收到 `kHello` 后为每个窗口建 `WindowStream`；分发协议消息；实现 `CloudStreamHooksWin`；处理文件对话框请求 |
| `WindowStream` | 每个窗口一个。对 `compositor->frame_sink_id()` 建 `FrameSinkVideoCapturer`（ARGB + `kPreferSharedImageWithNativeHandle`），把 DXGI NT handle `DuplicateHandle` 到网关进程后发 `kFrame`；收到 `kFrameDone` 才归还缓冲 |
| `PipeTransport` | 命名管道。读在专用线程，消息投递回 UI 线程；写加锁，任意线程可调用 |
| `CloudSelectFileDialog(Factory)` | 替换所有文件对话框，转成 `kFileChooserRequest` |
| `RemoteInput` | **只有声明，未实现**（见 §4） |

## 3. 网关一侧要实现的协议

协议头：`protocol/cloud_stream_protocol.h`（与 `chrome/browser/cloud_stream/public/cloud_stream_protocol.h` 是同一份文件，网关直接 include）。每条消息是 `MessageHeader { type, payload_size }` 加上 payload，结构体都是 `#pragma pack(1)`，小端。

### 3.1 连接流程

```
网关                                         Chrome
 │ CreateNamedPipe(\\.\pipe\<name>)            │
 │ 启动 chrome.exe --cloud-stream-pipe=<name>  │
 │◀──────────────── 连接 ──────────────────────│ PreBrowserStart: CloudStreamController::Start()
 │── kHello {version, gateway_pid, luid} ─────▶│ OpenProcess(PROCESS_DUP_HANDLE, gateway_pid)
 │◀── kHelloAck {version, chrome_pid} ─────────│
 │◀── kWindowCreated × N（已有窗口，按创建顺序）│
 │◀── kWindowBounds / kFrame …（持续）─────────│
```

### 3.2 帧

- `kFrame`：`texture_handle` 非 0 时，是**已经复制进网关进程**的 NT handle，网关用 `ID3D11Device1::OpenSharedResource1()` 打开，并按 `(texture_token_high, texture_token_low)` 缓存；为 0 时表示这张纹理之前发过，直接用缓存。capturer 的缓冲池很小，同一张纹理会反复出现。
- 有效区域是 `visible_*`，纹理尺寸是 `coded_*`。
- **每一帧都必须回 `kFrameDone {window_id, frame_seq}`**，否则缓冲池耗尽，capturer 会停止出帧。
- 风险（P6）：在支持 `D3DSharedFence` 的设备上，纹理没有 keyed mutex，网关拿到时 blit 可能还没完成。先实测是否撕裂。

### 3.3 窗口

- `kWindowCreated {id, kind, parent_window_id, x, y, w, h}`：`kind` 为 `kNormal`（带边框的窗口，如浏览器、DevTools）或 `kPopup`（菜单、气泡、`<select>`、扩展弹窗）。网关按坐标和层级（后创建的在上）把弹窗叠到父窗口上。
- `kWindowBounds`：窗口移动或改变大小时发送。
- `kWindowDestroyed`：窗口关闭。
- 网关发 `kSetWindowBounds` 可以改变窗口大小，例如跟随客户端分辨率。

### 3.4 其他

| 消息 | 方向 | 说明 |
|---|---|---|
| `kBeginFrame {window_id(0=全部), frame_time_us, interval_us}` | 网关 → Chrome | 仅在 `--cloud-stream-external-begin-frame` 时有效 |
| `kCursor` | Chrome → 网关 | 客户端在本地绘制光标 |
| `kFileChooserRequest {request_id, window_id, mode, title}` | Chrome → 网关 | 网关让客户端选文件、上传到服务器上该用户的目录 |
| `kFileChosen {request_id, canceled, paths}` | 网关 → Chrome | 服务器上的路径，`'\0'` 分隔 |
| `kMouse` / `kWheel` / `kKey` / `kText` / `kComposition` | 网关 → Chrome | **结构已定义，Chrome 侧尚未实现**（§4） |
| `kClipboardText` / `kClipboardChanged` | 双向 | **结构已定义，尚未实现** |

## 4. 尚未实现

| 项 | 位置 | 做法 |
|---|---|---|
| 远程输入 | `remote_input.h` 已声明，缺 `remote_input.cc`；`CloudStreamController::OnMessage` 里对应分支是 TODO | architecture.md §3.6：鼠标和滚轮构造 `ui::MouseEvent` / `ui::MouseWheelEvent`，调用 `host->SendEventToSink()`；按键发 keydown/keyup；文字用 `ui::KeyEvent::FromCharacter()`（会转成 `WM_CHAR` 走 `OnChar`）或 `TextInputClient::InsertText()`；输入法组字用 `SetCompositionText()` |
| 剪贴板桥接 | `OnMessage` 的 `kClipboardText` 分支 | 读写 headless 剪贴板（`components/headless/clipboard`），变化时发 `kClipboardChanged` |
| 拖放 | `NoopDragDropClient` | 实现由远程指针驱动的进程内拖放 |
| 软件合成降级 | `WindowStream::OnFrameCaptured` 丢弃非 GPU 帧 | 转发共享内存帧 |
| P6 帧同步 | `WindowStream::OnFrameCaptured` 的 TODO | 视实测结果决定 |
| P8 零拷贝 | — | 改造 `SkiaOutputDeviceOffscreen` |
| 下载、系统通知、策略 | — | 见 architecture.md §3.7 |

## 5. 编译后预计要修的地方

没编译过，以下是最可能出错的位置：
- `#include` 路径和 GN `deps`：`//ui/base/cursor/mojom:cursor_type`、`//ui/base/dragdrop/mojom` 等 target 名是按惯例写的，未核对。
- `chrome/browser/cloud_stream/DEPS` 的 include 规则，以及 `ui/views` 对 `ui/aura/client/drag_drop_client.h` 的前向声明。
- `CloudStreamController::HandleFileChosen` 里 `std::u16string` → `base::FilePath` 的转换。
- `WindowStream` 里 `dxgi.token().value()` 的类型（`base::TokenType` 的 `value()` 返回 `base::Token`）。
- `NoopDragDropClient` 是否还需要实现 Windows 下其他纯虚函数。
