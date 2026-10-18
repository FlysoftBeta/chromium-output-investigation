# Windows 云浏览器：Chromium 输出/输入接入与多租户 GPU 方案调研

> 范围：Chromium latest（M14x），Windows 宿主。目标是把渲染输出接到自定义管线（如 WebRTC），并能注入输入；对比 Windows 原生方案与 WSL/Linux 容器方案；评估 GPU（NVIDIA / Intel）接入方式，以及 WGC 是否可行。暂不讨论具体 codec。
>
> 调研日期：2026-10。源码结论已对照 Chromium `main`（**M158.0.8111.0**，commit `f05e8113`）的 sparse checkout 核对过（未编译），见 §7。标注「需 PoC 验证」的结论来自源码结构推断或二手资料，没有实测。

---

## 0. 结论速览

| 问题 | 结论 |
|---|---|
| Windows 上实现 Ozone platform？ | **不可行或代价极高。** Ozone 只支持 Linux / ChromeOS / Fuchsia，Windows 走 `ui/views/win` + `DesktopWindowTreeHostWin` + DirectComposition，没有 Ozone 抽象层。在 Windows 上"写一个 Ozone platform"等于把整个 Windows UI 栈移植到 Ozone，不现实。 |
| Windows 上最佳 hack 层面 | **viz 显示合成器的输出设备层（`SkiaOutputDevice`）+ 无窗口宿主（`//headless` 的 headless_shell；注意 `chrome --headless` 在 Windows 上会创建隐藏的真实 HWND，走的是 DComp 路径，不适合）**。输出用 D3D11 共享纹理，零拷贝交给编码器；输入走 `RenderWidgetHost::Forward*Event`。次选是 **`FrameSinkVideoCapturer`**（tab capture 用的同一套机制，基本不用改 Chromium，但多一次 GPU 拷贝/格式转换）。 |
| WGC 是否可行 | **能做 PoC，不建议用于生产。** 帧在 DWM 合成之后才到 WGC，比在 viz 内取帧多出约 1–2 个 vsync（60Hz 下约 16–35 ms），另外还有可见窗口、交互式会话、输入只能走 SendInput/PostMessage、无法做多租户等结构性问题（见 §3）。 |
| "WSL3" | **Microsoft 已公开否认 WSL 3 存在**；Build 2026 发布的是 **WSL Containers（`wslc.exe`）**，"WSL 3 近原生 GPU" 的文章是误报。现有可用的是 WSL2 的 GPU-PV（`/dev/dxg`）。 |
| WSL2 + Linux Chromium | Ozone 可以随意定制（这是 Ozone 的设计初衷），但 **WSL 下 GPU 只能走 Mesa d3d12（GL）/ dzn（Vulkan）翻译层，没有 DRM/GBM/dmabuf**，渲染到编码无法零拷贝；所有 distro/容器**共享同一个 utility VM 和内核**，不是安全边界。**不建议作为多租户生产方案。** |
| 如果接受 Linux | 最主流也最省事的是 **Linux 物理机 / Linux VM（DDA 直通或 SR-IOV vGPU）+ 自定义 Ozone（以 headless 为基础）+ dmabuf 零拷贝**。宿主必须是 Windows 时，用 Hyper-V Linux VM + GPU-P/DDA，**而不是 WSL**。 |
| 多租户 GPU | Windows 原生：所有进程共享 WDDM 调度，没有硬隔离；进程隔离的 Windows 容器可以挂 DirectX 设备类。硬隔离要用 Hyper-V GPU-P（SR-IOV：NVIDIA vGPU A16/L4/L40 等）或 DDA 整卡直通。 |

**推荐路线**

1. **短期 PoC（1–2 周）**：Windows 原生 Chromium，基于 CEF 或 Electron 的 OSR shared-texture（底层就是 `FrameSinkVideoCapturer`），拿到 D3D11 纹理 → libwebrtc 自定义 `VideoTrackSource`；输入用 CEF 的 `SendMouse/KeyEvent`。先跑通端到端，测出延迟基线。
2. **生产（自维护 Chromium 分支）**：基于 `//headless`（headless_shell）或 content embedder，在 Windows 上启用 `SkiaOutputDeviceBufferQueue` 并配一个自定义 `OutputPresenter`（或改造 `SkiaOutputDeviceOffscreen`），让根 render pass 直接画进可跨进程共享的 D3D SharedImage，用 `gfx::D3DSharedFence` 同步，外部 BeginFrame 控制帧节奏；补丁集中在 `components/viz/service/display_embedder/`，rebase 成本可控。
3. **隔离**：每租户一个独立 browser 进程树，跑在独立低权限用户 + Job Object 下，按密度/安全等级再叠加 Windows 进程隔离容器或 Hyper-V GPU-P VM。

---

## 1. Chromium 在 Windows 上的输出栈（要 hack 的地方在哪）

```
Renderer (Blink/cc)  ──CompositorFrame──▶  GPU 进程 / viz Display
Browser UI (views/cc)──CompositorFrame──▶      │
                                               ▼
                            SkiaRenderer / SkiaOutputSurfaceImpl
                                               │
                         SkiaOutputSurfaceImplOnGpu 选择 OutputDevice
                                               │
          ┌──────────────────────┬─────────────┴────────────┬──────────────────────┐
          ▼                      ▼                          ▼                      ▼
 SkiaOutputDeviceDComp   SkiaOutputDeviceGL(ANGLE)   SkiaOutputDeviceOffscreen   (软件) LayeredWindowUpdater
 (有 HWND，DirectComposition  (旧路径)                (surface handle 为空时：
  + MPO 硬件 overlay)                                 headless / 无窗口)
```

关键源码位置（M14x）：

- `components/viz/service/display_embedder/skia_output_surface_impl_on_gpu.cc`：`InitializeForGL / InitializeForVulkan / InitializeForDawn` 三条初始化路径，每条都是先判断 `dependency_->IsOffscreen()`（即 `surface_handle_ == gpu::kNullSurfaceHandle`），是则用 `SkiaOutputDeviceOffscreen`；否则在 Windows 上用 `SkiaOutputDeviceDComp`（GL 路径在 DComp 关闭时退回 `SkiaOutputDeviceGL`）。`SkiaOutputDeviceBufferQueue` 被 `#if !BUILDFLAG(IS_WIN)` 排除在 Windows 之外。
- `components/viz/service/display_embedder/skia_output_device_offscreen.cc`：离屏输出（约 260 行）。**注意：它是直接用 Graphite recorder / GrContext 创建私有 backend texture，不是 SharedImage，所以不能直接跨进程共享**，要改成 SharedImage 分配才能用。
- `components/viz/service/display_embedder/skia_output_device_buffer_queue.cc` + `output_presenter.h`：BufferQueue 负责分配 SharedImage 环形缓冲，`OutputPresenter` 接口（`Reshape / Present / ScheduleOverlayPlane`）拿到主平面的 `OverlayImageRepresentation::ScopedReadAccess`。在 Windows 上，D3D backing 的 overlay representation 带 `GetAcquireFence()/SetReleaseFence()`（`gfx::D3DSharedFence`），**正好可以用来做跨进程生产者/消费者同步**。这是比改 Offscreen 更"顺着架构"的切入点。
- `components/viz/service/display_embedder/skia_output_device_dcomp.cc`、`ui/gl/dc_layer_tree.cc`、`ui/gl/direct_composition_support.cc`：Windows 正常窗口的 DComp 路径。视频可能走硬件 overlay，**不经过根 render pass**，在窗口模式下截根 surface 可能漏掉视频。可以用 `--disable-direct-composition` 整体关闭 DComp；源码里**没有**单独关闭视频 overlay 的开关（只有 `--enable-direct-composition-video-overlays`），但所有 GPU workaround 都可以用同名开关强制打开，例如 `--disable_direct_composition_video_overlays=1`。
- `components/viz/service/frame_sinks/video_capture/frame_sink_video_capturer_impl.cc`：tab capture、`getDisplayMedia(preferCurrentTab)`、CEF/Electron shared-texture OSR 都用这套机制，基于 `CopyOutputRequest`。`mojom::BufferFormatPreference::kPreferSharedImageWithNativeHandle` 时输出不可 CPU 映射的 GPU SharedImage，以 `GpuMemoryBufferHandle` 交付（Windows 上即 DXGI shared handle）。支持 I420/ARGB/RGBAF16/NV12；**NV12 只能配 `kPreferMappableSharedImage`**（源码中有 CHECK）；帧池容量为 `kDesignLimitMaxFrames + 1`。
- `gpu/command_buffer/service/shared_image/d3d_image_backing.cc` + `dxgi_shared_handle_manager.h`：D3D11 SharedImage backing，通过 `DXGISharedHandleState` 管理 NT shared handle，是跨进程零拷贝的基础。
- `headless/lib/browser/headless_window_tree_host.cc`：headless_shell 的 `HeadlessWindowTreeHost::GetAcceleratedWidget()` 返回空 widget → surface handle 为空 → **Offscreen 设备**。headless_shell 在 Windows 上可以编译（`headless/BUILD.gn` 中有 `is_win` 分支）。
- `ui/views/win/hwnd_message_handler_headless.h`：**`chrome --headless`（new headless）在 Windows 上用 `HWNDMessageHandlerHeadless`，"platform windows exist but are never shown"**，即仍然有真实 HWND → 有 surface handle → **走 DComp 路径**，不是 Offscreen。所以生产方案应基于 `//headless`（headless_shell）或自己的 content embedder，而不是 `chrome --headless`。
- **渲染后端（M158 已核对）**：`gpu/config/gpu_finch_features.cc` 中 `kSkiaGraphite` 在 Windows/Apple 上 **默认开启**；`DawnContextProvider::GetDefaultBackendType()` 在 Windows 上默认 **D3D11**（`--skia-graphite-dawn-backend=d3d12` 可切 D3D12）。Ganesh/ANGLE 只是回退路径。所以新增的输出设备必须**基于 SharedImage 抽象**，同时兼容 Graphite/Dawn，不要写死 ANGLE EGL。好消息是默认后端仍然是 D3D11，D3D11 共享纹理/keyed mutex/shared fence 的方案依然成立。

### 1.0 `chrome --headless`（new headless）在 Windows 上的实际路径（源码核对）

> `--headless`、`--headless=new` 走的是同一条路径：M158 的 `headless::IsHeadlessMode()` 只调用 `HasSwitch(kHeadless)`，不看值，`chrome/app` 里也没有按值分支。old headless 早已从 chrome.exe 里移除，拆成了单独的 headless_shell / chrome-headless-shell（即下表右列）。

```
chrome.exe --headless
  │ chrome/browser/headless/headless_mode_init.cc
  │   只追加 --incognito / --no-first-run / --no-error-dialogs / 临时 user-data-dir
  │   （Linux 额外加 --ozone-platform=headless，并默认 --use-angle=swiftshader-webgl；
  │    Windows 不改任何 GPU 开关 → 默认硬件 GPU）
  ▼
完整的 Chrome 浏览器（Browser / BrowserView / 扩展 / views UI 都在）
  │ DesktopWindowTreeHostWin
  │ HWNDMessageHandler 工厂按 display::Screen::IsHeadless() 选择 HWNDMessageHandlerHeadless
  ▼
真实的顶层 HWND：WindowImpl::Init() 创建，但
  - 从不 ShowWindow，Show()/Hide() 只维护 is_visible_ 并回调 HandleVisibilityChanged
  - DwmSetWindowAttribute(DWMWA_CLOAK, TRUE)：告诉 DWM 永远不显示
  - aura::WindowTreeHost 在 --headless 下关闭 native occlusion 计算（不会因"不可见"而停绘）
  ▼
ui::Compositor 的 AcceleratedWidget = 这个 HWND（非空 surface handle）
  ▼
GPU 进程 viz：Graphite/Dawn D3D11 → SkiaOutputDeviceDComp → DCompPresenter
  - DCompPresenter 在 GPU 进程里创建 ChildWindowWin，经 AddChildWindowToBrowser 挂到被 cloak 的 HWND 下
  - DCLayerTree 正常建 swap chain / DComp surface，视频照常可以走 overlay
  - 每帧照常 Present/Commit，但 DWM 不合成这个窗口 → 这部分 GPU 工作是白做的
  ▼
取帧只能通过 viz 的 CopyOutputRequest：CDP Page.captureScreenshot / Page.startScreencast、
tab capture / FrameSinkVideoCapturer
```

对云浏览器的影响：

| 方面 | new headless（`chrome --headless`） | headless_shell（`//headless`） |
|---|---|---|
| 输出设备 | `SkiaOutputDeviceDComp`（往看不见的窗口 present） | `SkiaOutputDeviceOffscreen` |
| GPU | 默认硬件 | 需显式开启（默认软件/SwiftShader 路径，以版本为准） |
| 浏览器功能 | 完整 Chrome（扩展、策略、Chrome 的 permission/下载逻辑） | 精简的 content embedder，很多 UI 需自己实现 |
| 外部 BeginFrame（`--enable-begin-frame-control` / `HeadlessExperimental.beginFrame`） | **没有**（只在 `headless/lib` 里实现，`chrome/browser/headless` 中找不到） | 有 |
| 不改代码取帧 | 可以：`FrameSinkVideoCapturer`（tab capture / CDP screencast） | 可以 |
| WGC | **基本不可用**：窗口被 cloak 且不可见，DWM 不合成（预期，需验证） | 无窗口，不可用 |
| `SendInput` | 不可用（窗口不在前台、不可见） | 无窗口 |
| 输入 | CDP `Input.*` 或进程内 `RenderWidgetHost::Forward*Event` | 同左 |
| 做 L4 hack | 要么改 `SkiaOutputDeviceDComp`/`DCompPresenter`，要么打补丁让 headless 模式下 surface handle 为空、退回 Offscreen | 直接改 Offscreen 或 BufferQueue+Presenter |

结论：new headless 适合"**不改 Chromium + 用 FrameSinkVideoCapturer 取帧 + CDP 注入输入**"的方案，好处是完整 Chrome 功能（扩展等）；代价是每帧多一次没用的 DComp present，而且没有外部 BeginFrame 来控制出帧节奏。要做 L4 零拷贝 + 按需出帧，headless_shell 是阻力更小的底座；如果又想要完整 Chrome 功能，就在 new headless 上打一个"headless 时用空 surface handle"的补丁。

### 1.1 各 hack 层面对比

| 层面 | 做法 | 改 Chromium？ | 额外延迟（相对 viz 出帧） | 零拷贝 | 维护成本 | 评价 |
|---|---|---|---|---|---|---|
| L0 CDP `Page.startScreencast` | DevTools 推 JPEG | 否 | 高（编码 JPEG + base64 + IPC，几十 ms） | 否 | 低 | 只适合调试 |
| L1 tab capture → 浏览器内 WebRTC | 扩展 `chrome.tabCapture` / `getDisplayMedia`，直接在页面里建 RTCPeerConnection | 否 | 约 1 帧 | GPU 内部是 | 低 | 最快能出 demo；但编码/拥塞控制被 Chromium 内置 WebRTC 限死，难以统一调度多租户 |
| L2 CEF / Electron OSR shared texture | CEF `shared_texture_enabled` → `OnAcceleratedPaint`；Electron `offscreen.useSharedTexture` | 否（用 CEF/Electron） | 约 1 帧（capturer 拷贝 + 可能的格式转换） | 是（D3D11 shared handle） | 中：跟随 CEF/Electron 版本 | **PoC 首选**。注意社区报告过的问题：某些版本退回 `OnPaint`、NV12 时不回调、`OpenSharedResource` 崩溃等，**锁定版本并做回归测试** |
| L3 自定义 `FrameSinkVideoCapturer` 消费者 | embedder 里直接调 capturer（不经过 CEF） | 少量 | 约 1 帧 | 是 | 中 | L2 的"自己掌控"版本 |
| **L4 自定义 `SkiaOutputDevice`** | 方案 a：在 Windows 上放开 `SkiaOutputDeviceBufferQueue` + 新增 `OutputPresenterSharedTexture`（BufferQueue 分配 D3D SharedImage 环形缓冲，presenter 在 `Present` 时通过 `D3DSharedFence` 把帧交给编码进程）；方案 b：把 `SkiaOutputDeviceOffscreen` 的私有 backend texture 换成带 shared handle 的 SharedImage | 是（viz 内约 1–2k 行） | **≈0**（画完就是最终帧） | 是，无多余拷贝 | 中高：每个大版本 rebase，补丁比较集中 | **生产首选** |
| L5 改 DComp/DWM 层 | 截 DComp visual tree | 是，且侵入大 | — | — | 很高 | 不推荐 |
| WGC | 见 §3 | 否 | +1–2 vsync，外加 DWM 合成 | 是（拿到 D3D11 纹理） | 低 | 只做 PoC / 兜底 |

**为什么推荐 L4**：

- 不再多出一次 `CopyOutputRequest` 的 blit 和 RGB→NV12 转换（可以把转换放进编码器的 compute/VPP，或者编码器直接吃 ARGB）。
- 可以配合**外部 BeginFrame**（`ExternalBeginFrameSource`；headless 已有 `--enable-begin-frame-control` / CDP `HeadlessExperimental.beginFrame`）做"按需出帧"：编码器 / 网络拥塞控制决定何时出帧，空闲页面不出帧，多租户密度明显更高。
- headless 下没有 HWND / DComp，视频没有 overlay 分支，**所有内容（含 `<video>`、WebGL）都合成进根 surface**。

**L4 的注意点**：

- `<select>` 下拉、日期选择器等是**独立的 popup widget**（`RenderWidgetHostViewAura::InitAsPopup`），有自己的 CompositorFrameSink。
  - 在 **headless_shell** 里，popup 的 aura window 通过 `ParentWindowWithContext` 交给 `HeadlessWindowParentingClient`，后者直接返回 root window，所以 popup 是同一个 `WindowTreeHost` / `ui::Compositor` 下的子窗口，**应该会被合成进根 surface，也就是 L4 能直接拿到**（源码推断，需 PoC 验证）。
  - 在 **L1–L3（`FrameSinkVideoCapturer`，包括 CEF/Electron OSR）** 下，capturer 只抓 WebContents 的 frame sink，**popup 不在画面里**，需要像 CEF 那样单独拿 popup 帧（`PET_POPUP`）自己叠加。
- 浏览器 UI（权限气泡、下载栏、对话框、文件选择器）在 headless 里不存在，需要 embedder 自己实现策略（自动拒绝/允许，或做成 Web UI 推给客户端）。
- 跨进程同步：D3D11 用 `IDXGIKeyedMutex`，或用 D3D11.4/12 shared fence（`ID3D11Fence` / `ID3D12Fence` 共享句柄）。编码进程最好和 Chromium GPU 进程在**同一个 adapter（LUID 一致）**，否则共享会失败或退化成跨卡拷贝。

### 1.2 输入注入

| 方式 | 说明 | 适用 |
|---|---|---|
| 进程内 `RenderWidgetHostImpl::ForwardMouseEvent / ForwardWheelEvent / ForwardKeyboardEvent / ForwardGestureEvent`，IME 用 `ImeSetComposition / ImeCommitText` | CEF 的 `SendMouseClickEvent` 等就是这么做的；延迟最低；坐标和 DPI 自己管 | **生产** |
| CDP `Input.dispatchMouseEvent / dispatchKeyEvent / insertText / dispatchTouchEvent` | 无需改代码；多一次 IPC，约 1–3 ms | PoC / 外部控制 |
| `SendInput` / `PostMessage` 到 HWND | `SendInput` 作用于整个会话且要求前台窗口；`PostMessage` 键盘消息在 Chromium 下不可靠（焦点、IME、raw input） | 只有 WGC 方案才不得不用，**不推荐** |

还需要处理的反向通道：光标形状（`RenderWidgetHostViewBase::UpdateCursor`）、剪贴板（替换 `ui::Clipboard` 实现，按租户隔离）、IME 候选框位置（`TextInputManager`）、文件上传/下载、音频（`AudioOutputStream` → WebRTC audio，可以替换音频输出设备实现，headless 下可以完全不碰系统声卡）。

### 1.3 不编译 Chromium：OBS 式 DXGI Present hook（DLL 注入拿纹理）

思路和 OBS game capture 一样：往 Chromium 的 **GPU 进程**注入 DLL，hook `IDXGISwapChain::Present/Present1` 的 COM vtable，在 Present 时把 backbuffer `CopyResource` 进一张共享纹理（NT handle + keyed mutex / fence），交给编码进程。hook 的是 DXGI 公开 ABI，**不依赖 Chromium 内部符号，跨版本稳定**。

**能 hook 到什么（源码核对）**：
- `--headless=new` 默认走 `SkiaOutputDeviceDComp` → `DCompPresenter`，GPU 进程里有 flip-model swap chain，绑在 GPU 进程自己的 `ChildWindowWin` 上。
- DComp 不可用时走 `SkiaOutputDeviceDawnSwapChain`（Dawn 的 `wgpu::Surface`）。源码注释写明"flip model swap chains can't bind to a foreign-process HWND"，所以 **swap chain 一定在 GPU 进程里**，注入目标是 GPU 进程，不是 browser 进程。

**难点**：

| # | 问题 | 依据 / 对策 |
|---|---|---|
| 1 | **GPU 进程沙箱拒绝加载第三方 DLL** | `sandbox_win.cc`：沙箱进程的 delayed mitigation 里有 `MITIGATION_FORCE_MS_SIGNED_BINS`（只允许微软签名的 DLL），但**加 `--allow-third-party-modules` 就不再设置这一项**。其余保留：`EXTENSION_POINT_DISABLE`（只封 AppInit / 全局 hook，不影响 `CreateRemoteThread`）、`IMAGE_LOAD_NO_REMOTE`（DLL 放本地盘）、`IMAGE_LOAD_NO_LOW_LABEL`（DLL 文件不能带低完整性标签）。`gpu_process_host.cc`：GPU 进程 token 为 `USER_LIMITED`、**低完整性级别**、job 限制。中/高完整性的启动器向低完整性进程注入是允许的。所以**很可能只需 `--allow-third-party-modules`，不用关 GPU 沙箱**（需 PoC 验证，否则退回 `--disable-gpu-sandbox`）。 |
| 2 | **视频 overlay 会让 hook 失效** | 已核对：`overlay_processor_win.cc` 一旦有 DC layer（视频 overlay），就给根 render pass 设 `needs_synchronous_dcomp_commit`，`skia_renderer.cc` 于是把根画面从 `DXGISwapChainImageBacking`（composition swap chain，`Present1`）**切换成 `DCompSurfaceImageBacking`（`IDCompositionSurface`，没有 Present）**，视频本身则在另一个 swap chain 里。所以**必须加 `--disable_direct_composition_video_overlays=1`**，保证根画面一直走 swap chain。 |
| 3 | GPU 进程会重建 | context lost / GPU 崩溃后 browser 会重启 GPU 进程，hook 跟着丢失。需要监听子进程创建并重新注入；也可以用 `--in-process-gpu` 把 GPU 放进 browser 进程，只注入一次（这是测试用开关，稳定性需验证）。 |
| 4 | 只解决输出 | 输入仍走 CDP `Input.*`（无需注入）。光标形状、剪贴板这类反向通道 CDP 覆盖不全。 |
| 5 | present 节奏 | 页面内容不变时不 Present，编码端要能重复上一帧；隐藏或 cloak 窗口上的 flip-model Present 会不会被节流（`DXGI_STATUS_OCCLUDED`），需 PoC 验证。 |
| 6 | **一个窗口不等于一张纹理** | 主窗口的 swap chain 里有标签栏、工具栏和页面；菜单、权限气泡等是独立的顶层 widget（独立 HWND → 独立 DComp presenter / swap chain），要按各自窗口位置自己合成。**系统原生对话框（文件选择、另存为、Windows Hello / WebAuthn 系统 UI 等）不经过 Chrome 的 swap chain，hook 抓不到。** 见 §1.4。 |
| 7 | **一个 Chrome 实例共用一个 GPU 进程** | 多租户按"每租户一个 Chrome 实例（独立 `--user-data-dir`）"，每个实例一个 GPU 进程、一个 hook。 |

### 1.4 目标是"完整浏览器体验"时的重新评估

"完整体验"指浏览器 UI（标签栏、地址栏、菜单、扩展弹窗、权限气泡）和系统原生对话框都要能看到、能操作。在这个前提下，前面几种方案要重新排序：

**headless 不是合适的底座**（源码核对）：
- CDP `Input.*` 只能把事件送进**页面**（RenderWidgetHost），点不到标签栏、地址栏、菜单这些 views UI。
- new headless 会装上进程内的 headless 剪贴板（`chrome_browser_main_extra_parts_headless` 依赖 `//components/headless/clipboard`），行为和普通 Chrome 不同。
- 窗口的可见性、焦点都是"伪造"的（`HWNDMessageHandlerHeadless`），依赖窗口状态的 UI 行为可能和有头 Chrome 不一致。
- 结论：完整体验应该用**有头（headful）Chrome**。

**Windows 的根本约束：一个会话只有一个输入焦点。** `SendInput` 作用于整个会话的输入桌面，键盘焦点也是每会话一个。所以在 Windows 上，**"每租户一个完整浏览器"基本等于"每租户一个交互式会话/桌面"**，无论画面怎么抓。

| 方案 | 画面 | 输入 | 原生对话框 | 隔离 | 评价 |
|---|---|---|---|---|---|
| **W1. 每租户一个 Windows VM**（Hyper-V GPU-P / GPU-PV）+ 有头 Chrome + 虚拟显示器（IddCx）+ DDA/WGC 抓整屏 | 整个桌面，一切可见 | `SendInput`（VM 内只有一个用户） | ✅ | VM 级 | **Windows 上最完整、最稳妥**，就是 Parsec / Sunshine 的模式；抓屏多约 1 个合成帧，云浏览器可以接受。缺点是 VM 开销大，密度低。 |
| **L1. Linux 容器**（每租户一个容器：有头 Chrome + 独立的 headless Wayland/X 显示服务）| 显示服务端抓整屏（dmabuf，零拷贝） | 显示服务端的虚拟输入设备 | ✅（Linux 下文件选择框等也由 Chrome/显示服务端绘制） | 容器级 | 每个容器有自己的显示服务和输入焦点，天然解决"一会话一焦点"的问题；业界云浏览器的主流做法。前提是 Linux 宿主（或 Linux VM）+ 原生 GPU 驱动，不能是 WSL。 |
| W2. 共享桌面 + 多个 headless Chrome + 纹理 hook（§1.3）| 只有 Chrome 自己画的部分 | 页面靠 CDP，浏览器 UI 难以操作 | ❌ | 弱 | 密度最高，但达不到"完整体验"，只适合"只要网页内容"的场景。 |

**建议**：要完整体验，Windows 上选 W1（每租户一个 VM，整桌面抓取），能接受 Linux 就选 L1。§1.3 的纹理 hook 只在"只要网页内容、追求密度"时才值得。

---

## 2. Windows 原生方案的多租户与 GPU

### 2.1 隔离层次（由弱到强）

1. **同一用户、多 browser 实例**：每租户独立 `--user-data-dir`。Chromium 自身的 renderer 沙箱保护的是"网页 → 系统"，**不保护租户之间**（共用 GPU 进程时尤其如此）。
2. **每租户独立 Windows 低权限账户 + Job Object**（限制 CPU/内存/进程数）+ 可选 AppContainer：每个租户有自己的 browser、GPU、utility 进程树，**不同租户之间不共享 GPU 进程**。成本最低，推荐作为基线。
3. **Windows 进程隔离容器（process-isolated）**：可以用 `--device class/5B45201D-F2F2-4F3B-85BB-30FF1F953599` 挂 DirectX 设备（GPU 加速，支持 D3D11/12，ANGLE/Dawn 可用）。只支持 process isolation，Hyper-V 隔离容器不支持 GPU。Chromium 需要完整的 `windows` / `windows/server` 基础镜像（Server Core/Nano 缺 UI 组件）。共享宿主内核，隔离强度大致等于"加强版第 2 层"。
4. **Hyper-V VM + GPU-P（SR-IOV）**：Windows Server 2025 / Azure Local 支持 GPU 分区，并支持带 GPU-P 的 VM 热迁移（需要 IOMMU DMA bit tracking；AMD Rome 不支持热迁移）。NVIDIA 侧是 vGPU（A2/A16/L4/L40 等，需要 vGPU 许可）；社区有 A16 集群热迁移失败的报告。**Intel Flex / Arc Pro 在 Hyper-V GPU-P 下的支持情况需向厂商确认**（Intel Flex 的 SR-IOV 主要面向 KVM）。
5. **DDA 整卡直通**：一卡一 VM，最强隔离，不能热迁移，密度最低。

### 2.2 GPU 侧要点

- **WDDM 调度没有硬配额**：第 2、3 层下，某个租户跑重 WebGL/WebGPU 可以拖慢所有租户。缓解手段：开 HAGS、按租户设置进程 GPU 优先级（`D3DKMTSetProcessSchedulingPriorityClass`）、Chromium 侧限帧（外部 BeginFrame）、限制或禁用 WebGPU。
- **编码器并发**：GeForce 有 NVENC 会话数限制（消费级卡目前为 8 路左右，随驱动版本变化），数据中心卡（L4/L40/A16）没有这个限制。Intel QSV 不限会话数，但受媒体引擎吞吐约束。多租户密度往往**先卡在编码器上，而不是渲染**。
- **多卡**：每个 browser 用 `--use-adapter-luid=`（`ui/gl/gl_switches.cc`，已核对）绑定到指定 GPU，编码进程也绑定到同一 LUID，保证共享纹理零拷贝。
- **会话**：headless / OSR 方案不需要交互式桌面，可以在服务会话里跑（GPU 在 Session 0 下的 D3D 渲染是可用的），这是相对 WGC 的一大优势。

---

## 3. WGC（Windows.Graphics.Capture）评估

**原理**：WGC 从 DWM 拿某个窗口的 redirection surface 或整个显示器的合成结果，以 `Direct3D11CaptureFrame` 的形式交给调用方（Chromium 自己的 `third_party/webrtc/modules/desktop_capture/win/wgc_capturer_win.cc` 也在用它）。

**延迟构成**（定性分析，**需 PoC 实测**）：

1. Chromium 出帧 → DComp commit → **等 DWM 下一次合成**（最多 1 个 vsync）
2. DWM 合成 → 帧进入 WGC 的 frame pool → `FrameArrived` 回调（通常再多半帧到 1 帧；frame pool 缓冲数越多，排队越多）
3. 如果 `MinUpdateInterval` 设置不当会被限帧：24H2（26100）的 `IGraphicsCaptureSession5::MinUpdateInterval`，社区报告设为 <1 ms 或 0 时反而被限到约 50fps，应显式设成 1 ms 或帧间隔。

整体相比 L4 大约**多 16–35 ms（60Hz）**；在 144Hz 虚拟显示器上可以缩短，但 GPU 开销随之上升。

**结构性问题**（比延迟更致命）：

- 必须有**交互式桌面会话**和真实 HWND。窗口不能最小化或 cloaked；被遮挡时 WGC 仍能抓到，但 **Chromium 的原生遮挡检测会节流/停绘**，需要 `--disable-backgrounding-occluded-windows --disable-features=CalculateNativeWinOcclusion`（`kCalculateNativeWinOcclusion` 默认开启，已核对）。
- 多租户需要多个会话（RDS 多会话或多用户登录），每个会话都要 DWM 和桌面，开销大，管理也复杂。
- 输入只能 `SendInput`（需前台，整会话共享）或 `PostMessage`（不可靠），**基本无法支撑多租户**。
- 抓的是窗口，带浏览器 UI；要 kiosk/app 模式，而且没法拿到 popup 与主页面分离的信息。
- 有黄色边框（Win11 可以通过 `IsBorderRequired=false` 关掉，需要相应权限），光标要单独处理（`IsCursorCaptureEnabled`）。

**结论**：WGC 适合"零改动 Chromium，先看效果"的 PoC，或者作为"抓第三方桌面应用"的通用兜底；**Chromium 云浏览器的主路径不应依赖 WGC**。

---

## 4. WSL / Linux 容器方案评估

### 4.1 现状澄清

- **WSL 3 不存在**：Microsoft WSL PM 已公开否认（Windows Latest，2026-06-28）。Build 2026 实际发布的是 **WSL Containers**（`wslc.exe` + NuGet API）。GPU 访问在其文档里是"目标"之一，隔离设计细节**尚未公开**，需要跟进官方文档。
- 当前可用的是 WSL2 的 **WDDM GPU-PV**：Linux 内核驱动 `dxgkrnl` 暴露 `/dev/dxg`，用户态是 `libdxcore` / `libd3d12`，图形 API 由 **Mesa d3d12 Gallium（OpenGL/GLES）** 和 **dzn（Vulkan → D3D12）** 翻译。NVIDIA 的 CUDA/NVENC 在 WSL 中可用（驱动提供 `libcuda`/`libnvidia-encode` 的 WSL 版本）；Mesa d3d12 还提供 VA-API（D3D12 Video）编解码前端，Intel/NVIDIA/AMD 都可以走。

### 4.2 在 Linux 侧怎么 hack（Ozone 方案）

Ozone 正是为这个场景设计的抽象层，以 `ui/ozone/platform/headless/` 为模板新增一个 platform：

- `OzonePlatform` 子类：注册新的 platform 名（`--ozone-platform=cloud`）。
- `PlatformWindow`：不创建真实窗口，持有 `PlatformWindowDelegate`。**输入注入**：把网络收到的事件构造成 `ui::MouseEvent / KeyEvent / TouchEvent`，调 `delegate->DispatchEvent()`，这是最干净的注入点；键盘布局可以复用 `ui/events/ozone/layout`。
- `SurfaceFactoryOzone`：
  - GL：Ozone headless 的 `GetAllowedGLImplementations()` 只允许 `kGLImplementationEGLANGLE`，对应的 `GLOzone` 成员名就叫 `swiftshader_implementation_`；view surface 是把帧写进文件的 `FileGLSurface`（调试用）。自定义平台需要自己实现 GLOzone 和 Vulkan surface（headless 已有 `vulkan_implementation_headless.cc` 可参考）。
  - 取帧：**Ozone headless 的 `CreateNativePixmap()` 返回的是 `TestPixmap`（假 pixmap），不能导出 dmabuf。** 要零拷贝，必须移植 `ui/ozone/platform/drm` 或 wayland 里基于 GBM 的 `NativePixmap` 实现（render node 即可，不需要 KMS），这样 dmabuf 才能直接导入 VA-API/NVENC。
- `CreateClipboard / CursorFactory / InputMethod`：按租户实现。

### 4.3 在 WSL 里的致命问题

1. **没有 DRM/KMS/GBM，没有 dmabuf**：`/dev/dxg` 不是 DRM 设备，Ozone 的 `NativePixmap` 零拷贝链路走不通。渲染结果只能 `glReadPixels` / CopyOutputRequest 读回 CPU，再上传给编码器。1080p RGBA 约 8 MB/帧，60fps 约 0.5 GB/s 的 PCIe 往返，每帧 1–3 ms，还占 CPU 和带宽，多租户下会被放大。（Mesa d3d12 的 GL 与 VA-API 之间是否能在 Mesa 内部共享 D3D12 资源，**需 PoC 验证**，就算可行也是非标准路径。）
2. **翻译层兼容性**：Chromium 的 GPU 黑名单/特性检测在 Mesa d3d12（GL 4.x 子集）和 dzn（Vulkan 一致性有限）上表现不稳定，WebGL2/WebGPU 可能被降级或禁用；NVIDIA 原生 Linux GL/Vulkan 驱动在 WSL 中**不可用**。
3. **隔离**：所有 WSL2 distro 和其中的容器**共享同一个 utility VM 和同一个 Linux 内核**，不构成租户间安全边界；WSL 定位是开发者工具，不是服务器虚拟化平台（许可、服务化运行、无人值守启动都有坑）。
4. **WSLg** 用的是 Weston + RDP（VAIL 共享内存）把窗口带回 Windows，属于"另一个远程显示管线"，不适合直接复用作为输出通道。

### 4.4 "Windows 宿主 + Linux Chromium" 的正确姿势

如果坚持要 Linux 版 Chromium（Ozone 生态更好，云浏览器业界主流也是 Linux），在 Windows 宿主上应该：

- 用 **Hyper-V Linux VM**（而不是 WSL），GPU 通过 **DDA 整卡直通** 或 **SR-IOV GPU-P（NVIDIA vGPU）**，guest 里装原生 Linux 驱动，从而拿到 DRM/GBM/dmabuf 和原生 VA-API / NVENC，Ozone 零拷贝链路可以完整走通。（Linux guest 在 Hyper-V GPU-P 下的官方支持矩阵**需按具体 GPU/驱动版本确认**。）
- VM 内部再用容器（Docker/containerd + gVisor/Kata 视安全要求）做租户隔离，每租户独立 browser，GPU 在 VM 内共享。
- 或者干脆：**宿主直接换成 Linux + KVM**，NVIDIA vGPU / Intel Flex SR-IOV 的支持在 KVM 下最成熟。

---

## 5. 方案对比总表

| 方案 | 出帧延迟 | 零拷贝到编码 | 输入 | 多租户隔离 | GPU 支持 | Chromium 改动 | 推荐度 |
|---|---|---|---|---|---|---|---|
| A. Win 原生 + 自定义 SkiaOutputDevice（L4） | 最低 | ✅ D3D 共享纹理 | ✅ 进程内注入 | 用户/Job/进程容器/GPU-P VM | NVIDIA/Intel 原生 D3D 驱动 | 中（viz 补丁 + embedder） | ⭐⭐⭐⭐⭐ |
| B. Win 原生 + CEF/Electron OSR shared texture | 低（+1 帧内） | ✅ | ✅ | 同 A | 同 A | 无（跟随 CEF） | ⭐⭐⭐⭐（PoC/早期产品） |
| C. Win 原生 + WGC | +1–2 vsync | ✅（D3D11） | ❌ SendInput | 差（需多会话） | 同 A | 无 | ⭐⭐（PoC） |
| D. WSL2 / WSL Containers + 自定义 Ozone | 中（读回 CPU） | ❌ 无 dmabuf | ✅ Ozone 注入 | ❌ 共享内核 | 翻译层（d3d12/dzn），兼容性差 | 中 | ⭐ |
| E. Hyper-V Linux VM（DDA/GPU-P）+ 自定义 Ozone | 最低 | ✅ dmabuf | ✅ | ✅ VM 级 | 原生 Linux 驱动 | 中 | ⭐⭐⭐⭐（接受 Linux 时） |
| F. 裸金属 Linux + KVM/容器 + Ozone | 最低 | ✅ | ✅ | ✅ | 最成熟 | 中 | ⭐⭐⭐⭐⭐（不限 Windows 时） |

---

## 6. 建议的 PoC 计划

1. **延迟基线**（方案 B vs C）：同一页面（时间戳 + 帧计数器 canvas），在编码器输入端读回时间戳并与渲染时间比较，分别测 OSR shared texture 和 WGC 的 glass-to-encoder 延迟，以及 1/4/16 租户下的 GPU 占用。
2. **方案 A 原型**：在 chrome-headless-shell 上加 `SkiaOutputDeviceSharedTexture` + 外部 BeginFrame；验证 popup、`<video>`、WebGL、WebGPU 都能正确出现在输出里；验证 Graphite/Dawn 打开时的行为。
3. **GPU 隔离**：在 Windows Server 2025 上分别测进程隔离容器（DirectX 设备类）和 GPU-P VM，跑 NVIDIA L4/A16 与 Intel（Arc Pro / Flex，先确认驱动是否支持 GPU-P），记录密度、编码会话上限，以及租户间的 GPU 干扰。
4. **（可选）方案 D 验证**：只为确认"WSL 下读回链路的开销与 WebGL 兼容性"，用于向决策方说明为什么不走 WSL。

---

## 6.5 决定编译时的具体方案（一台 Windows、多人各用一个完整浏览器）

场景：一台 Windows PC，几个人各自远程使用一个**完整的** Chrome（标签栏、地址栏、菜单、扩展都要能用），隔离要求低，前端延迟要尽量压。

### 底座：fork `--headless=new`

理由（均已在源码中核对）：
- 它是完整的 Chrome（`Browser` / `BrowserView` / 扩展都在），不是精简的 content shell。
- `HWNDMessageHandlerHeadless` 让所有窗口**都认为自己处于激活、有焦点的状态**，不随系统焦点变化（`hwnd_message_handler.cc`："Headless windows are believed to always have focus"）。所以多个人同时操作各自的窗口，不会抢 Windows 那唯一的键盘焦点。**这正好解决了"一个会话一个焦点"这个最大的障碍。**
- 窗口被 cloak，不会在物理桌面上叠成一片；native occlusion 节流在 `--headless` 下默认关闭。
- new headless 用的是进程内的 headless 剪贴板，每个实例互不干扰，正好需要按用户桥接。

每人一个 Chrome 实例（独立的 `--user-data-dir`）。

### 输出：每个顶层窗口挂一个 capturer（补丁小，延迟低于 WGC）

- 枚举每个实例的顶层 aura 窗口：浏览器主窗口、菜单、气泡、下拉框、扩展弹窗，各自有自己的 `ui::Compositor`。用 `views::WidgetObserver`、`aura::EnvObserver` 跟踪窗口的创建、销毁和位置变化。
- 每个窗口：`compositor->context_factory()->GetHostFrameSinkManager()->CreateVideoCapturer()`，再 `ChangeTarget(compositor->frame_sink_id())`。做法和 `content/browser/media/capture/aura_window_video_capture_device.cc` 一样，该文件在 `use_aura` 下编译，Windows 也包含在内。
- 参数：`BufferFormatPreference::kPreferSharedImageWithNativeHandle` + ARGB，帧以 `GpuMemoryBufferHandle`（DXGI shared handle）交付，编码进程打开后零拷贝送进硬件编码器。`SetMinCapturePeriod` 设为显示帧间隔，`SetAutoThrottlingEnabled(false)`。
- 合成：按每个窗口的屏幕坐标（headless 下保存在 `aura::client::kHeadlessBoundsKey`）和层级，把弹出窗口叠到主窗口画面上。可以在编码进程里用 D3D11 合成，开销很小。
- 延迟：帧在 viz 聚合后马上 copy 出来，**不经过 DWM 合成**，比 WGC 少一个合成帧。
- 以后要更极致：第二阶段再把 capturer 换成自定义 `OutputPresenter`（§1.1 的 L4），省掉 capturer 那次 GPU blit。

### 输入：直接送进 aura/views 事件管线（全部 UI 可用）

- 在 browser 进程里开一个本地 IPC（named pipe 或 mojo 都可以；也可以在 `chrome/browser/devtools` 加一个自定义 CDP domain，复用现有的 websocket 通道）。
- 收到 `(窗口 id, 事件)` 后，构造 `ui::MouseEvent` / `ui::MouseWheelEvent` / `ui::KeyEvent` / `ui::TouchEvent`，调用该窗口 `aura::WindowTreeHost` 的 `ui::EventSource::SendEventToSink()`。事件走完整的 aura → views → `RenderWidgetHostViewAura` 管线，**标签栏、地址栏、菜单、页面都能点**，不依赖系统焦点。
- 中文输入：不要把按键交给本地 IME，而是在客户端完成组字，再通过当前焦点的 `ui::TextInputClient`（`SetCompositionText` / `InsertText`）送入，候选框画在客户端本地。
- 光标：监听光标变化（`DesktopWindowTreeHostWin::SetCursor` 这一层），把光标类型发给客户端本地绘制，感知延迟最低。

### 原生对话框和系统交互：改成远程版本

- 文件选择 / 另存为：实现一个自定义的 `ui::SelectFileDialogFactory`，弹窗请求转给客户端，用户从自己的电脑选文件上传。这比在服务器上弹 Windows 对话框体验更好。
- 下载：每个用户一个下载目录，下载完成后推给客户端。
- 剪贴板：桥接 headless 剪贴板和客户端剪贴板。
- 打印：Chrome 的打印预览是 WebUI，可以正常显示；禁用"使用系统对话框打印"。
- 权限气泡、扩展弹窗、对话框都是 views 绘制的，按上面的方法自然能抓到，也能点。

### 延迟相关开关（已核对）

`--double-buffer-compositing`、`--disable-gpu-vsync`（不建议用 `--disable-frame-rate-limit`，多实例时 GPU 压力过大）、`--disable-renderer-backgrounding`、`--disable-background-timer-throttling`。如果后面要按需出帧，把外部 BeginFrame（`ui::Compositor` 的 `ExternalBeginFrameController`）也接进来。

### 补丁规模与编译

| 部分 | 位置 | 估计规模 |
|---|---|---|
| 窗口跟踪 + capturer 管理 + IPC 服务 | 新目录，例如 `chrome/browser/cloud_stream/`，在 headless 启动路径（`chrome_browser_main_extra_parts_headless`）里初始化 | 1–2k 行 |
| 输入注入 + IME + 光标 | 同上，加上少量 `ui/views/win` 或 `desktop_window_tree_host_win` 的钩子 | 0.5–1k 行 |
| 文件对话框 / 下载 / 剪贴板桥接 | `SelectFileDialogFactory` 实现 + `DownloadManagerDelegate` 设置 + headless clipboard 适配 | 0.5–1k 行 |
| 现有代码的修改 | 尽量只在初始化处插入几行 | < 100 行 |

核心逻辑放在新文件里，对现有文件只改几行，跟版本 rebase 的成本很低。

编译（Windows）：VS 2022 + Windows SDK + depot_tools，磁盘至少 100 GB（建议 SSD），内存建议 32 GB 以上。args.gn 用 `is_debug=false is_component_build=false symbol_level=0 blink_symbol_level=0 enable_nacl=false`。8 核机器第一次全量编译需要几个小时，之后的增量编译只要几分钟。

## 7. 源码核对记录（Chromium M158.0.8111.0，commit `f05e8113`，未编译）

用 `git clone --depth=1 --filter=blob:none --sparse` 只检出了相关目录（约 150 MB），未编译，也未拉取 third_party。

| 结论 | 源码依据 | 结果 |
|---|---|---|
| Ozone 只在 Linux/ChromeOS/Fuchsia 启用 | `build/config/ozone.gni`: `use_ozone = is_chromeos \|\| is_fuchsia \|\| is_linux` | ✅ |
| 空 surface handle → Offscreen 设备 | `skia_output_surface_dependency_impl.cc` `IsOffscreen()`；`skia_output_surface_impl_on_gpu.cc` GL/Vulkan/Dawn 三条路径 | ✅ |
| Windows 窗口模式 → DComp；BufferQueue 不在 Windows 上用 | 同上，`#if !BUILDFLAG(IS_WIN)` | ✅ |
| Offscreen 设备用的是私有纹理，不是 SharedImage | `skia_output_device_offscreen.cc`（`createBackendTexture`） | ✅（**修正**了初版"直接改 Offscreen 就能共享"的表述） |
| headless_shell 无 HWND，`chrome --headless` 有隐藏 HWND | `headless_window_tree_host.cc`；`hwnd_message_handler_headless.h`；`hwnd_message_handler.cc` 中按 `Screen::IsHeadless()` 选择 | ✅（**新增**区分） |
| headless_shell 可在 Windows 上编译 | `headless/BUILD.gn` 的 `is_win` 分支 | ✅ |
| Windows 默认 Graphite + Dawn D3D11 | `gpu_finch_features.cc` `kSkiaGraphite`；`dawn_context_provider.cc` `GetDefaultBackendType()` | ✅（**修正**了初版"正在迁移"） |
| D3D SharedImage 带 shared handle 和 shared fence | `d3d_image_backing.cc`、`dxgi_shared_handle_manager.h`；`shared_image_representation.h` 的 `GetAcquireFence/SetReleaseFence` | ✅ |
| capturer 能输出带 native handle 的 GPU 帧 | `frame_sink_video_capture.mojom` `kPreferSharedImageWithNativeHandle`；`mappable_shared_image_video_frame_pool.cc` | ✅；NV12 仅限 mappable 模式 |
| 外部 BeginFrame | `headless/public/switches.h` `enable-begin-frame-control` | ✅ |
| 输入注入接口 | `render_widget_host_impl.h` `ForwardMouseEvent/ForwardWheelEvent/ForwardKeyboardEvent/ForwardGestureEvent`、`ImeSetComposition/ImeCommitText` | ✅ |
| headless_shell 中 popup 与主窗口共用 root window | `headless_window_parenting_client.cc` 返回 `root_window_`；`render_widget_host_view_aura.cc` `InitAsPopup` | ✅（是否被合成进同一帧需 PoC） |
| Ozone headless 不支持真实 dmabuf | `headless_surface_factory.cc` `CreateNativePixmap` → `TestPixmap` | ✅ |
| 开关名 | `--disable-direct-composition`、`--use-adapter-luid`（`ui/gl/gl_switches.cc`）；`--disable-backgrounding-occluded-windows`（`content_switches.cc`）；`CalculateNativeWinOcclusion`（`ui_base_features.cc`）；workaround 可用同名开关强制（`gpu_util_unittest.cc`） | ✅ |

没有在源码里核对的：WGC 延迟（`third_party/webrtc` 未检出，且需要实测）、CEF/Electron 的实现（不在 Chromium 仓库）、所有 GPU 虚拟化和 WSL 相关结论（属于 Windows/驱动层面）。

## 参考资料

- Microsoft Build 2026 Windows Developer Blog：<https://blogs.windows.com/windowsdeveloper/2026/06/02/build-2026-furthering-windows-as-the-trusted-platform-for-development/>
- Microsoft 否认 WSL 3 存在（Windows Latest）：<https://www.windowslatest.com/2026/06/28/microsoft-denies-wsl-3-exists-reveals-windows-11s-wsl-containers-ship-next-week/>
- WSL Containers 实际变化：<https://falcao.org/posts/wsl-containers-what-changes-for-wsl2-users/>
- Build 2026 WSL demo 仓库：<https://github.com/microsoft/Build26-DEM346-whats-new-in-windows-subsystem-for-linux>
- WSL2 GPU 计算链路（dxgkrnl / `/dev/dxg`）：<https://danielcosenza.com/posts/wsl-gpu-compute/>
- Hyper-V GPU 分区（Windows Server 2025）：<https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/gpu-partitioning>
- Azure Local GPU 分区：<https://learn.microsoft.com/en-us/azure-stack/hci/manage/partition-gpu>
- NVIDIA A16 GPU-P 热迁移失败报告：<https://forums.developer.nvidia.com/t/gpu-p-live-migration-fails-in-hyper-v-2025-cluster-with-nvidia-a16-cards/369354>
- WGC `MinUpdateInterval` 行为报告：<https://github.com/robmikh/Win32CaptureSample/issues/82>
- Chromium Ozone 平台范围说明：<https://chromium.googlesource.com/chromium/src/+/a01655045bf5f3340a62e55758cfb7b80be37202>
- CEF shared texture OSR 原始 PR：<https://bitbucket.org/chromiumembedded/cef/pull-requests/158>
- CEF `OnAcceleratedPaint` 回退问题（OBS 论坛）：<https://obsproject.com/forum/threads/obsproject-cef-onacceleratedpaint2-is-called-once.165222/>
