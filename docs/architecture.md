# 云浏览器敲定方案：fork `--headless=new` + 保留 HWND + 离屏输出

> 基线：Chromium `main` **M158.0.8111.0**（commit `f05e8113`）。文中 `文件:行号` 均指该版本，已在 sparse checkout 中逐一核对（**未编译、未运行**）。标「PoC」的地方是读源码推不出、必须实测的点。
>
> 选型过程和被否决的方案（Ozone、WSL、WGC、CEF、DLL 注入、headless_shell 等）见 [archive/investigation-notes.md](archive/investigation-notes.md)。

---

## 0. 场景与约束

- 一台 Windows PC，几个人各自远程使用一个**完整的** Chrome：标签栏、地址栏、菜单、扩展、权限气泡都要能看到、能操作。
- 只有一个 Windows 会话（不能每人一个会话或一个 VM）。
- 租户隔离要求低：每人一个 Chrome 实例、独立 profile 即可。
- 后续传输链路延迟已经很高，**前端（渲染 → 编码器输入）要尽量压低延迟**。
- 可以维护自己的 Chromium 分支并自行编译。

## 1. 方案一句话

以 `chrome --headless=new` 为底座：每个窗口照常创建 HWND（被 cloak，只作为 Windows 相关代码的"外壳"），但**不把 HWND 交给 GPU 进程**，GPU 端因此走离屏输出，渲染一帧都不进 DComp / DWM。取帧在 viz 内完成，输入直接送进 aura 事件管线，原生 UI 一律换成远程实现。

为什么这样选：
- `--headless=new` 是完整的 Chrome，而且 `HWNDMessageHandlerHeadless` 让每个窗口都**认为自己可见、激活、有焦点**，与系统焦点无关。所以一个会话里多人同时操作各自的窗口，不会抢 Windows 唯一的键盘焦点。
- 保留 HWND，就不用把 Chrome 里大量默认有 HWND 的 Windows 代码（`BrowserDesktopWindowTreeHostWin`、frame、TSF、UIA 等）移植到无窗口的 backend。
- 只切断"HWND → GPU"这一处，补丁很小，却能拿到"完全不进 DWM、帧节奏自己控制"的全部好处。

## 2. 总体架构

```
 客户端（每人一个）                    服务器（一台 Windows PC，一个登录会话）
┌──────────────┐  网络   ┌──────────────────────────────────────────────────────────────┐
│ 解码 + 显示  │◀────────│ 网关/编码进程（你自己写）                                    │
│ 本地光标     │         │  · 每用户：合成多个窗口纹理 → 硬件编码 → WebRTC              │
│ 本地输入法   │────────▶│  · 输入、文件、剪贴板、光标等控制通道                        │
└──────────────┘         └──────▲──────────────────────────────┬────────────────────────┘
                                │ DXGI NT handle（帧）          │ 本地 IPC（命名管道）
                                │                               ▼
                         ┌──────┴──────────────────────────────────────────────────────┐
                         │ Chrome 实例 × N（每用户一个，--headless=new，自编 fork）      │
                         │  browser 进程：CloudStreamController                        │
                         │    · HostTracker：跟踪所有 aura::WindowTreeHost              │
                         │    · 每个 host 一个 FrameSinkVideoCapturer                   │
                         │    · InputInjector：事件 → host->SendEventToSink()           │
                         │    · 文件对话框 / 下载 / 剪贴板 / 光标 桥接                  │
                         │  GPU 进程：每个窗口 SkiaOutputDeviceOffscreen（不进 DWM）    │
                         │  renderer 进程：不改                                         │
                         └─────────────────────────────────────────────────────────────┘
```

## 3. 全链路推理

### 3.1 启动

1. 网关为每个用户启动：
   `chrome.exe --headless=new --user-data-dir=<用户目录> --cloud-stream-pipe=<管道名> [其他参数，见 §5]`
2. `chrome/app/chrome_main.cc` → `headless::IsHeadlessMode()`（只检查 `--headless` 开关是否存在，不看值）→ `InitHeadlessMode()`（`chrome/browser/headless/headless_mode_init.cc`）。在 Windows 上它只补 incognito / no-first-run 等参数，**不改 GPU 参数**，所以默认用硬件 GPU（Graphite + Dawn D3D11）。
3. `ChromeBrowserMainParts::PreMainMessageLoopRunImpl()`：
   - `chrome/browser/chrome_browser_main.cc:1809` 安装 Chrome 自己的 `ChromeSelectFileDialogFactory`（Windows 上它在 `util_win` 工具进程里弹系统对话框）。
   - 之后 `:2078` 调用 `PreBrowserStart()` → `ChromeBrowserMainExtraPartsHeadless::PreBrowserStart()`，这里原本只设置 headless 剪贴板。
   - **【P2】** 在这里初始化 `CloudStreamController`、覆盖安装我们的 `SelectFileDialogFactory`（因为晚于 `:1809`，会覆盖掉 Chrome 的）、挂上剪贴板桥接。只在带 `--cloud-stream-pipe` 时生效。

### 3.2 窗口创建

1. 任何顶层 UI（浏览器窗口、菜单、气泡、`<select>` 下拉、扩展弹窗、画中画、DevTools）都是 `views::Widget` → `DesktopNativeWidgetAura` → `DesktopWindowTreeHostWin`（浏览器主窗口是子类 `BrowserDesktopWindowTreeHostWin`）。
2. `HWNDMessageHandler` 的工厂按 `display::Screen::IsHeadless()` 选择 `HWNDMessageHandlerHeadless`（`ui/views/win/hwnd_message_handler.cc:431`）：
   - `Init()` 照常创建 HWND，然后 `DwmSetWindowAttribute(DWMWA_CLOAK, TRUE)`；
   - 从不调用 `ShowWindow`，`Show()/Hide()` 只维护内部的 `is_visible_` 并通知上层；
   - 焦点变化不上报（"Headless windows are believed to always have focus"）。
3. `DesktopWindowTreeHostWin` 初始化时调用 `CreateCompositor(params.force_software_compositing)`（`desktop_window_tree_host_win.cc:303`），`ui::Compositor` 的 AcceleratedWidget 就是这个 HWND。
4. `aura::Env` 通知 `EnvObserver::OnHostInitialized(host)` → **HostTracker 登记这个 host**，分配窗口 id，挂 `WindowTreeHostObserver`（resize、move、销毁），并为它创建 capturer（§3.4）。`aura::WindowTreeHost` 在 `--headless` 下不启用 native occlusion（`ui/aura/window_tree_host.cc:480`），窗口不会因为"看不见"而停止绘制。

### 3.3 渲染输出：不进 DWM

1. `VizProcessTransportFactory::OnEstablishedGpuChannel()`（`content/browser/compositor/viz_process_transport_factory.cc`）组装 `RootCompositorFrameSinkParams`：
   - `:423` `root_params->widget = compositor->widget();`
   - **【P1】** 改为：cloud 模式下传 `gfx::kNullAcceleratedWidget`。
   - 注意：空句柄**只出现在发给 GPU 进程的这个参数里，窗口本身仍然存在**。browser 进程的顶层 HWND（`HWNDMessageHandlerHeadless` 创建并 cloak）照常存在，`compositor->widget()` 仍是它，消息循环、TSF、UIA、frame 代码都继续使用它。消失的只是 GPU 进程里由 `DCompPresenter` 创建、挂在顶层 HWND 下的 `ChildWindowWin` 及其 DComp/swap chain。GPU 以为自己在离屏渲染，画进一张不属于任何窗口的纹理。
   - 软件合成（`--disable-gpu`、GPU 被 blocklist、GPU 反复崩溃后的回退、或某个 widget 设了 `force_software_compositing`）**在 headless 下本来就不用 HWND**，所以 P1 不需要区分 GPU/软件：
     - browser 进程把 `--headless` 转发给 GPU 进程（`content/browser/gpu/gpu_process_host.cc:294`，在复制给 GPU 进程的开关列表里）；
     - GPU 进程 `VizCompositorThreadRunnerImpl` 读取它（`components/viz/service/main/viz_compositor_thread_runner_impl.cc:241`），传给 `OutputSurfaceProviderImpl(gpu_service, headless)`；
     - `OutputSurfaceProviderImpl::CreateSoftwareOutputDeviceForPlatform()`（`output_surface_provider_impl.cc:144`）开头就是 `if (headless_) return std::make_unique<SoftwareOutputDevice>();`，即一个只画进内存位图的基础设备，**根本不走** `CreateSoftwareOutputDeviceWin(hwnd, ...)`（那条路径才需要真实 HWND，会建 swap chain 或 layered window）。
     - 换句话说，现有的 `--headless=new` 软件路径已经是"离屏、不进 DWM"了，P1 只是让 GPU 路径也变成同样的行为。
   - 软件模式下取帧仍然可用，但会降级：没有 GPU 上下文，capturer 拿不到带 native handle 的 SharedImage，只能交付共享内存里的 CPU 帧（I420/ARGB），编码进程需要上传到 GPU 再编码。把它当作降级模式，正常运行应保证走 GPU 合成（网关检测到降级时报警）。
   - 同文件里其余用到 `compositor->widget()` 的地方（`HostDisplayClient` 构造、`RenderingWindowManager::RegisterParent/UnregisterParent`、`AddChildWindowToBrowser`）保持不变：离屏模式下 GPU 不会创建子窗口，这些代码只是空转。**PoC：确认没有地方断言句柄非空。**
2. GPU 进程：`RootCompositorFrameSinkImpl::Create()`（`components/viz/service/frame_sinks/root_compositor_frame_sink_impl.cc:132-136`）→ `OutputSurfaceProviderImpl::CreateOutputSurface(widget, gpu_compositing=true, ...)` → `SkiaOutputSurfaceImpl::Create()`。
3. `SkiaOutputSurfaceImplOnGpu::InitializeForDawn()`（Windows 默认 Graphite + Dawn D3D11：`gpu_finch_features.cc` 的 `kSkiaGraphite` 默认开启，`DawnContextProvider::GetDefaultBackendType()` 返回 D3D11）：
   - `dependency_->IsOffscreen()` 即 `surface_handle_ == gpu::kNullSurfaceHandle` → 为真 → `SkiaOutputDeviceOffscreen`。
   - 结果：**不创建 `DCompPresenter`、不创建 `ChildWindowWin`、不调用 `CreateSwapChainForComposition`、不 `Present1`、不 commit DComp**。渲染只发生在 GPU 显存里，DWM 完全不参与。
   - 附带好处：没有 DComp 就没有视频 overlay，`<video>`、canvas、WebGL 全部合成进根 render pass。
4. 帧节奏（同文件 `:200-236`）：离屏设备的 `dc_support_level` 为 `kNone`，不会用 `ExternalBeginFrameSourceWin`（DComp vsync），而是回退到 `DelayBasedBeginFrameSource`（合成 60Hz 时钟）。
   - 第二阶段 **【P7】**：cloud 模式下把 `desktop_window_tree_host_win.cc:303` 改为 `CreateCompositor(force_sw, /*use_external_begin_frame_control=*/true)`，`VizProcessTransportFactory` 就会带上 `external_begin_frame_controller`，GPU 端改用 `ExternalBeginFrameSourceMojo`。这样**由编码器决定什么时候出帧**：空闲时不出帧，需要时立即出帧，每个窗口独立控制。
5. 尺寸：离屏设备按 `Reshape()` 收到的尺寸分配纹理；窗口尺寸由 `Browser.setWindowBounds` 或你的控制命令决定（headless 下 bounds 存在 `aura::client::kHeadlessBoundsKey`）。

### 3.4 取帧（第一阶段：每个窗口一个 capturer）

1. 对每个 host：
   ```
   auto* hfsm = host->compositor()->context_factory()->GetHostFrameSinkManager();
   capturer = hfsm->CreateVideoCapturer();                 // components/viz/host/host_frame_sink_manager.h:256
   capturer->ChangeTarget(viz::VideoCaptureTarget(host->compositor()->frame_sink_id()));
   capturer->SetFormat(media::PIXEL_FORMAT_ARGB);
   capturer->SetResolutionConstraints(size, size, /*use_fixed_aspect_ratio=*/true);
   capturer->SetMinCapturePeriod(帧间隔);
   capturer->SetAutoThrottlingEnabled(false);
   capturer->Start(consumer, viz::mojom::BufferFormatPreference::kPreferSharedImageWithNativeHandle);
   ```
   这和 `content/browser/media/capture/aura_window_video_capture_device.cc` 抓窗口的方式相同（该文件在 `use_aura` 下编译，Windows 也包含），可以照着写。
2. capturer 挂在 frame sink 上，与输出设备无关，所以离屏模式下照常工作。每次该窗口有新的 CompositorFrame 被绘制，viz 在 `Display` 绘制过程中处理 `CopyOutputRequest`，把根 render pass blit 到池里的一张 SharedImage。
3. 交付：`FrameSinkVideoCapturerImpl::MaybeDeliverFrame()`（`frame_sink_video_capturer_impl.cc:1526`）→ `consumer_->OnFrameCaptured(handle, info, content_rect, callbacks)`（`:1609`）。
   - `handle` 是 `VideoBufferHandle::GpuMemoryBufferHandle`，Windows 上里面是 `gfx::DXGIHandle`（NT handle）。
   - consumer（browser 进程）`DuplicateHandle` 给编码进程，编码进程 `ID3D11Device1::OpenSharedResource1()` 打开，直接送硬件编码器，不经过 CPU。
   - 用完后必须调用 `callbacks->Done()` 归还缓冲。帧池容量只有 `kDesignLimitMaxFrames + 1`（`frame_sink_video_capturer_impl.h:176`），编码器拖住不还，capturer 会丢帧。
4. **同步风险【P6】**：支持 `gfx::D3DSharedFence` 的设备上，Chrome 的共享纹理只设 `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED`，**不带 keyed mutex**（`d3d_image_backing_factory.cc:742-745`），同步靠 Chrome 内部的 fence。外部编码器拿到句柄时，blit 可能还没执行完。处理办法：在交付前确保 GPU 端的 copy 已完成（在 `MaybeDeliverFrame` 之前等待，或者把对应的 `D3DSharedFence` 句柄随帧一起导出、由编码器等待）。**PoC：先测不打补丁时是否会撕裂或读到旧帧。**
5. 合成：主窗口和各弹窗各是一路纹理。位置用 `host->GetBoundsInPixels()`（headless 下来自 `HWNDMessageHandlerHeadless` 维护的 bounds）；层级按"后创建的在上面"，菜单和气泡总在主窗口之上。编码进程每帧用 D3D11 画几个带偏移的 quad，合成为该用户的一张画面再编码。

### 3.5 取帧（第二阶段：零拷贝，可选）

**【P8】** 把 `SkiaOutputDeviceOffscreen`（`components/viz/service/display_embedder/skia_output_device_offscreen.cc`）里用 recorder 创建的私有 backend texture，换成带 shared handle 的 SharedImage 环形缓冲（D3D backing，参考 `SkiaOutputDeviceBufferQueue` 的分配方式）：
- 分配时把 NT handle 一次性导出给编码进程；
- 每次 `Present()` 时 signal 一个 `gfx::D3DSharedFence`，编码器等待 fence 后读取。
- 省掉 capturer 那次 blit，P6 的同步问题也随之消失，帧的产生和编码完全对齐（配合 P7 的外部 BeginFrame）。

### 3.6 输入：直接送进 aura/views 事件管线

1. 网关 → 命名管道 → browser 进程 UI 线程 → InputInjector 按窗口 id 找到 `aura::WindowTreeHost`。物理鼠标键盘永远到不了被 cloak 的窗口，所以只有注入的事件。
2. 鼠标、滚轮、触摸：构造 `ui::MouseEvent` / `ui::MouseWheelEvent` / `ui::TouchEvent`，坐标用 host 的**像素坐标**，调用 `host->SendEventToSink(&event)`。这和真实输入走的路径完全一样：`DesktopWindowTreeHostWin::HandleMouseEvent()` 本身就是调 `SendEventToSink(event)`（`desktop_window_tree_host_win.cc:1387-1393`）。之后经 `aura::WindowEventDispatcher` → views（标签栏、地址栏、菜单）或 `RenderWidgetHostViewAura`（网页）。双击要自己设置 click count，拖选靠 aura 自己的 capture client。
3. 键盘：
   - `SendEventToSink(KeyEvent)` → `WindowEventDispatcher::PreDispatchKeyEvent()`（`ui/aura/window_event_dispatcher.cc:1093-1110`）→ `host->GetInputMethod()->DispatchKeyEvent()` → `InputMethodWinBase::DispatchKeyEvent()`（`ui/base/ime/win/input_method_win_base.cc:182`）。
   - 关键细节：对 keydown，`InputMethodWinBase` 会 `PeekMessage` **真实消息队列里**的 `WM_CHAR`，用来生成字符。合成的 keydown 不会产生 `WM_CHAR`，所以**只发 keydown 打不出字**。
   - 做法：
     - 快捷键、方向键、回车、删除等：发 keydown/keyup（`KeyboardCode` + `DomCode` + `DomKey` + flags）。
     - 可打印字符：额外发一个 char 类型的 `ui::KeyEvent`。`MSGFromKeyEvent()` 会把它转成 `WM_CHAR`，走 `OnChar()` 插入文本（`:184-196`）。**PoC：确认合成的 char 事件能正确转换。**
     - 中文等输入法：在客户端本地完成组字，服务器端调用 `host->GetInputMethod()->GetTextInputClient()` 的 `SetCompositionText()` / `InsertText()`，候选框也画在客户端。
4. 光标 **【P4】**：`DesktopWindowTreeHostWin::SetCursorNative()`（`:900`）在设置系统光标前，先通知 controller 光标类型（自定义光标附带位图），发给客户端本地绘制。这对感知延迟的改善最大。

### 3.7 原生 UI：换成远程实现

| 类别 | 处理 | 位点 |
|---|---|---|
| 所有文件对话框（`<input type=file>`、下载另存为、File System Access、Ctrl+O、导入书签、加载扩展、改下载目录） | 自定义 `ui::SelectFileDialogFactory`：打开类请求客户端选文件并上传到用户临时目录，回调 `FileSelected()`；保存类先存服务器，再推给客户端下载 | P2（在 `PreBrowserStart` 覆盖 `chrome_browser_main.cc:1809` 安装的工厂） |
| 系统通知 | `--disable-features=NativeNotifications,SystemNotifications`（两者在 `chrome/common/chrome_features.cc` 中默认开启），退回 Chrome 自绘的通知弹窗，它是普通 aura 窗口，会被 HostTracker 抓到 | 启动参数（哪个开关实际控制 Windows 路径需 PoC） |
| 拖放 | `DesktopWindowTreeHostWin::CreateDragDropClient()`（`:353`）原本返回调用 OLE `::DoDragDrop` 的 `DesktopDragDropClientWin`。那个模态循环读取真实鼠标状态，注入事件驱动不了。cloud 模式下改为返回自己的进程内实现（第一版可以先禁用拖放） | **【P5】** |
| 剪贴板 | headless 剪贴板（`PreBrowserStart` 中设置）↔ 客户端剪贴板桥接 | P2 / P3 |
| 下载 | 每用户下载目录，完成后推给客户端；禁用"打开"和"在文件夹中显示" | P3（`DownloadManagerDelegate` 相关） + 策略 |
| 打印 | 保留 Chrome 的打印预览（WebUI），用策略只允许"另存为 PDF"，生成后推给客户端；禁用系统打印对话框 | 企业策略 |
| 外部协议、证书管理器、WebAuthn（系统 `webauthn.dll`）、EyeDropper（取物理屏幕颜色） | 用策略禁用或隐藏入口 | 企业策略 / 小补丁 |
| 兜底 | HostTracker 同时监视本进程新建的、不属于 aura 的顶层窗口：记日志并关闭 | P3 |
| 不用处理 | tooltip（Windows 上只有 `TooltipAura`）、`<select>`、日期和颜色选择器、右键菜单、权限气泡、HTTP 认证框、客户端证书选择框——都是 aura 窗口，自动被抓取和注入 | — |

### 3.8 音频（未核对，待定）

候选位点：在 audio service 中提供自定义的输出流（替换 `media/audio/win` 的 AudioManager 输出路径），把 PCM 写入管道交给网关；或者让 audio service 跑在 browser 进程内再拦截。**这一部分没有在源码里核对，需要单独调研。**

### 3.9 编码进程和 GPU

- 编码进程和所有 Chrome 实例必须在同一块显卡上：Chrome 用 `--use-adapter-luid=<LUID>`（`ui/gl/gl_switches.cc`），编码器用同一个 LUID 创建 D3D11 设备，否则共享纹理会打开失败或退化成跨卡拷贝。
- GeForce 的 NVENC 有并发会话上限（消费级卡约 8 路，随驱动变化），人多时用 Intel QSV 或专业卡。
- 编码器配置成低延迟：无 B 帧、关 lookahead、小 VBV、用 intra refresh 代替周期关键帧。

## 4. Patch 清单

全部改动只在 `--cloud-stream-pipe` 存在时生效，不影响普通 Chrome 行为。

**已实现的补丁见 [`patches/`](../patches) 和 [interface.md](interface.md)**：P1、P2、P3（除远程输入、剪贴板外）、P4、P5（先禁用拖放）、P7 已写；P6、P8 和远程输入尚未实现。

| ID | 位置（M158） | 改动 | 估计规模 | 阶段 |
|---|---|---|---|---|
| P1 | `content/browser/compositor/viz_process_transport_factory.cc:423` | cloud 模式下 `root_params->widget = gfx::kNullAcceleratedWidget`（软件合成在 headless 下本来就走内存位图设备，无需区分） | ~10 行 | 1 |
| P2 | `chrome/browser/headless/chrome_browser_main_extra_parts_headless.cc` `PreBrowserStart()` | 初始化 `CloudStreamController`，覆盖安装 `SelectFileDialogFactory`，挂剪贴板桥接 | ~20 行 | 1 |
| P3 | 新目录 `chrome/browser/cloud_stream/`（含 BUILD.gn、DEPS） | HostTracker（`aura::EnvObserver` + `WindowTreeHostObserver`）、capturer 管理与 consumer、命名管道 IPC、InputInjector、文件对话框工厂、下载/剪贴板桥接、非 aura 窗口兜底 | 2–4k 行 | 1 |
| P4 | `ui/views/widget/desktop_aura/desktop_window_tree_host_win.cc:900` `SetCursorNative()` | 光标变化通知 controller（通过一个全局 observer 接口，避免 views 依赖 chrome） | ~20 行 | 1 |
| P5 | `ui/views/widget/desktop_aura/desktop_window_tree_host_win.cc:353` `CreateDragDropClient()` | cloud 模式返回自定义拖放 client（v1 可先禁用） | ~10 行 + 实现 | 1（禁用）/ 2（实现） |
| P6 | `components/viz/service/frame_sinks/video_capture/frame_sink_video_capturer_impl.cc` 交付路径（`:1526-1609`） | 保证 blit 完成后再交付，或随帧导出 fence | ~50 行 | 视 PoC 结果 |
| P7 | `ui/views/widget/desktop_aura/desktop_window_tree_host_win.cc:303` | cloud 模式 `CreateCompositor(..., use_external_begin_frame_control=true)`，由编码器驱动出帧 | ~10 行 + controller 端 | 2 |
| P8 | `components/viz/service/display_embedder/skia_output_device_offscreen.cc` | 离屏 backing 改为带 shared handle 的 SharedImage 环形缓冲 + 每帧 signal `D3DSharedFence` | ~300–500 行 | 2 |

对现有文件的修改合计不到 200 行，主体在新目录里，跟版本 rebase 的成本低。

## 5. 运行参数

```
chrome.exe --headless=new
  --user-data-dir=<每用户目录>
  --cloud-stream-pipe=<管道名>
  --use-adapter-luid=<与编码器相同的显卡>
  --disable-renderer-backgrounding
  --disable-background-timer-throttling
  --disable-features=NativeNotifications,SystemNotifications
  --window-size=<分辨率>
```
另外用企业策略限制打印目标、外部协议、下载后打开等（§3.7）。离屏模式下没有 DComp，所以不再需要 `--double-buffer-compositing`、`--disable_direct_composition_video_overlays` 这类与 DComp 相关的开关。

## 6. 风险与 PoC 顺序

| 顺序 | 验证内容 | 失败时的退路 |
|---|---|---|
| 1 | P1：空 widget 后 Chrome 正常启动，GPU 端确实走 `SkiaOutputDeviceOffscreen`，无断言失败（用 `chrome://gpu` 和 trace 确认） | 查找并修补断言非空句柄的位置 |
| 2 | 单个窗口挂 capturer，拿到 DXGI handle，编码进程能打开并显示 | — |
| 3 | P6：不加补丁时是否撕裂、读到旧帧 | 实施 P6 |
| 4 | `SendEventToSink` 注入鼠标：标签栏、地址栏、菜单、网页都能点；多个实例同时操作互不干扰 | — |
| 5 | 键盘：keydown + char 事件能打字，快捷键可用；`TextInputClient` 能输入中文 | 调整 char 事件的构造方式 |
| 6 | 菜单、`<select>` 等弹窗作为独立 host 出现、能抓到、能点、坐标正确 | — |
| 7 | 文件对话框工厂覆盖成功；系统通知走 Chrome 自绘版本 | 调整安装时机 |
| 8 | 延迟测量：页面里放高精度时间戳，在 OnFrameCaptured、编码输出、客户端显示三处读取 | 进入第二阶段（P7、P8） |

## 7. 编译

- 环境：Windows 10/11 x64，VS 2022（含"使用 C++ 的桌面开发"和对应 Windows SDK），depot_tools，至少 100 GB SSD，建议 32 GB 以上内存。
- `fetch chromium` 后切到选定的 tag（例如与本文一致的 M158 分支），把补丁放在自己的分支上。
- `args.gn`：
  ```
  is_debug = false
  is_component_build = false
  symbol_level = 0
  blink_symbol_level = 0
  enable_nacl = false
  ```
- 只编 `chrome` 目标：`autoninja -C out\Release chrome`。8 核机器第一次全量编译需要几个小时，之后的增量编译只要几分钟。
- 升级 Chromium 版本时，主要冲突点就是 §4 表里那几个现有文件的几十行改动。
