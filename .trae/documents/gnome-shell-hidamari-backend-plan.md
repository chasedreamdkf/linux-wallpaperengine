# GNOME Shell（Hidamari 式）壁纸后端 —— 实施方案

> 目标：在 GNOME 50 Wayland 下复用 linux-wallpaperengine 的完整壁纸解析/渲染内核（Scene / Shader / 粒子 / 视频 / CEF 网页），
> 通过新增「离屏渲染驱动 + GNOME Shell GJS 扩展」把画面注入 Mutter 的 `_backgroundGroup`，
> 完全绕开 Mutter 未实现的 `zwlr_layer_shell_v1`，不依赖 layer-shell，且不创建任何可见 Wayland/X11 窗口。

---

## 1. 概述（Summary）

本方案在原引擎既有的 `VideoDriver` / `Output` 抽象之上新增一个**离屏后端**，把渲染结果经 **POSIX 共享内存（路线 B）** 交给一个 **GNOME Shell 扩展**，由扩展把帧作为 `Clutter.Actor` 插入 `Main.layoutManager._backgroundGroup`。

范围（本次计划覆盖文档阶段 1–5）：
1. 离屏渲染内核 + SHM 输出通道 + 极简 GJS 扩展（原型链路）。
2. 双向 IPC：显示器拓扑、鼠标坐标回传，驱动视差/鼠标特效。
3. 接入完整 WE 渲染管线：Scene / Shader / 粒子 / 视频 / CEF 网页壁纸。
4. 性能优化：帧节流、VSync 对齐、SHM 零拷贝、读回优化。
5. 打包部署：一键安装脚本，自动部署扩展到用户扩展目录。

**定位**：在 `gnome50-dev` 分支上**全新、独立实现**，不改动、不依赖已有 `gnome50` 分支的 XWayland 方案（`GLFWDesktopWindowOutput`），两者可共存为不同后端。

---

## 2. 现状分析（Current State Analysis）

- **输出驱动抽象**：[VideoDriver.h](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/VideoDriver.h) 定义输出驱动接口（`getOutput` / `dispatchEventQueue` / `getFramebufferSize` / `getProcAddress` …）；[Output.h](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/Output/Output.h) 定义输出接口（`getViewports` / `haveImageBuffer` / `getImageBuffer` / `updateRender` …）。
- **驱动注册与选择**：驱动通过 `__attribute__((constructor))` 在 [GLFWOpenGLDriver.cpp](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/GLFWOpenGLDriver.cpp#L169-L187) 里调用 `sVideoFactories.registerDriver(mode, xdgSessionType, factory)` 注册；[VideoFactories.cpp](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/VideoFactories.cpp#L57-L79) 按 `(WINDOW_MODE, XDG_SESSION_TYPE)` 选择，非 `DESKTOP_BACKGROUND` 的模式统一用 `DEFAULT_WINDOW_NAME` 键。
- **主循环与帧流程**：[WallpaperApplication::render](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Application/WallpaperApplication.cpp#L855-L952) 每帧调用 `m_videoDriver->dispatchEventQueue()`；驱动内部渲染后可用 `glReadPixels` 把像素读入 `Output::getImageBuffer()`（参考 [GLFWOpenGLDriver.cpp](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/GLFWOpenGLDriver.cpp#L110-L161)）。
- **非窗口输出的既有先例**：[X11Output](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/Output/X11Output.cpp) 已经不创建可见窗口，而是把像素写入 X root pixmap，是「把帧交给外部合成器显示」最接近的参考。
- **离屏渲染目标**：[CWallpaper::render](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/CWallpaper.cpp#L276-L303) 在 `m_destFramebuffer`（由 [WallpaperApplication::setDestinationFramebuffer](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Application/WallpaperApplication.cpp#L992-L998) 设置，默认 0）上做最终合成；因此把目标 FBO 设为我们自己的离屏 FBO 即可完整离屏渲染。
- **驱动选择入口**：[WallpaperApplication::setupOutput](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Application/WallpaperApplication.cpp#L666-L687) 读取 `XDG_SESSION_TYPE` 后经工厂创建驱动。
- **依赖情况**：CMake 已 `find_package(DBus REQUIRED)`（见 [CMakeLists.txt](file:///home/chasedream/Developer/linux-wallpaperengine/CMakeLists.txt#L31)）并链接 `${DBUS_LIBRARIES}`，D-Bus 控制通道无需新增依赖；EGL 已在 Wayland 分支中链接（`${OPENGL_egl_LIBRARY}`）。
- **已有分支对照**：`gnome50` 分支已实现一套 XWayland 绕行方案（新增 `GLFWDesktopWindowOutput` 等，实机可用）。本方案是**另一条技术路线**，不复用其代码。

---

## 3. 总体设计决策（Key Decisions）

| 项 | 决策 |
|---|---|
| 后端定位 | 新增独立后端，`gnome50-dev` 上从零实现；与 XWayland 方案互不影响 |
| 驱动/模式 | 新增 `WINDOW_MODE::GNOME_SHELL`，CLI 开关 `--gnome-shell` |
| GL 上下文 | **EGL surface-less**（`EGL_MESA_platform_surfaceless`）离屏上下文；不可用时回退到 GLFW 隐藏窗口创建上下文 |
| 像素通道 | **POSIX 共享内存**（`memfd_create` + `/dev/shm` 命名文件），双缓冲 + seqlock，**本次第一版即实现路线 B** |
| 控制通道 | D-Bus 会话总线（扩展持有 well-known name，引擎作客户端连接），承载握手 / 显示器拓扑 / 鼠标 / 生命周期 / `FrameReady` 通知 |
| 多显示器 | 引擎渲染「整桌面包围盒」单帧，扩展据显示器布局在 `_backgroundGroup` 内跨屏摆放（沿用引擎既有 fullWidth/fullHeight 思路） |
| 像素格式 | 引擎 `glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE)`；扩展侧 `Cogl.PixelFormat.RGBA_8888`（如实际倒置则改 `RGBX_8888` 或交换 shader，见阶段 3 校验） |
| 扩展交互 | 扩展发 `PointerMoved`；引擎经 `GnomeShellMouseInput` 注入视差；**不支持壁纸内鼠标点击**（`_backgroundGroup` Actor 不接收点击，与 Hidamari 同限制） |
| 锁屏 | 扩展监听 `Main.sessionMode` / `lock-screen` 状态，锁屏时隐藏 Actor 并通知引擎暂停 |

---

## 4. 组件与文件清单（Proposed Changes）

### 4.1 C++ 引擎侧

#### (A) CLI 与模式枚举
- 文件：[ApplicationContext.h](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Application/ApplicationContext.h#L34-L41)、[ApplicationContext.cpp](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Application/ApplicationContext.cpp)
- 做什么：
  - `WINDOW_MODE` 增加 `GNOME_SHELL = 3`。
  - 新增 `--gnome-shell` 参数（参照现有 `--layer`/`--screen-root` 的 argparse 写法，见 [ApplicationContext.cpp](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Application/ApplicationContext.cpp#L264-L354) 附近），设置 `settings.render.mode = GNOME_SHELL`。
- 为什么：让后端可通过命令行显式启用，不影响其它后端。

#### (B) 离屏帧缓冲 / 共享内存（新）
- 文件：`src/WallpaperEngine/Render/Drivers/GnomeShell/SharedFrameBuffer.h/.cpp`
- 做什么：
  - `memfd_create`（回退 `shm_open`）分配 `header + 2 * (stride*height)`；导出 `/dev/shm/lwe-gnome-<pid>-<n>` 名。
  - 提供 `mapName()`、`beginWrite()/endWrite()`、`publishFrameId()`、`close()`；销毁时 `munmap` + `shm_unlink`（防残留）。
  - 头部结构（POD，小端）：
    ```
    magic, version,
    atomic<uint32_t> seq,      // 奇数=写入中，偶数=稳定
    width, height, stride,
    uint32_t format,           // 0=RGBA8888
    uint32_t backIndex,        // 双缓冲 0/1
    ```
- 为什么：路线 B 的核心，提供可被扩展按名映射的零拷贝像素通道，并用 seqlock 防撕裂。

#### (C) D-Bus 控制通道（新）
- 文件：`src/WallpaperEngine/Render/Drivers/GnomeShell/GnomeShellIpc.h/.cpp`
- 做什么：基于 `libdbus-1`（已是依赖）封装到扩展的调用与信号订阅。接口见 §5.2。
  - 客户端方法：`Register(shmName, width, height, stride, format)`、`Unregister()`、`RequestMonitors()`。
  - 订阅信号：`PointerMoved(dx,dy)`、`MonitorsChanged(a(iiiis))`、`Pause()/Resume()`、`Shutdown()`。
  - 每帧发布 `FrameReady(u frameId)`（轻量，仅唤醒扩展，不带像素）。
- 为什么：控制平面与像素平面分离；逐帧信号负载极小。

#### (D) 输出与视口（新）
- 文件：`src/WallpaperEngine/Render/Drivers/Output/GnomeShellOutput.h/.cpp`（视口可复用 [GLFWOutputViewport](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/Output/GLFWOutputViewport.h) 或新增轻量 viewport）
- 做什么：
  - 由扩展上报的显示器列表构建 viewport 集合（`globalPosition` / `logicalSize` / `viewport`），计算整桌面包围盒 `m_fullWidth/m_fullHeight`。
  - `haveImageBuffer()=true`；`getImageBuffer()` 指向共享内存当前写缓冲；`renderVFlip()=false`；`updateRender()` 调用 `SharedFrameBuffer` 的 `endWrite()` 并发布 `FrameReady`。
- 为什么：复用引擎既有「读回整帧→交给外部显示」的 Output 约定（对齐 [X11Output](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/Output/X11Output.cpp#L71-L79)）。

#### (E) 视频驱动（新）
- 文件：`src/WallpaperEngine/Render/Drivers/GnomeShellOpenGLDriver.h/.cpp`
- 做什么：
  - 构造：`eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA)` → `eglInitialize` → `eglBindAPI(EGL_OPENGL_API)` → `eglCreateContext` → `eglMakeCurrent(EGL_NO_SURFACE)`；随后 `glewInit()`。失败则回退到 GLFW 隐藏窗口路径。
  - 用 `__attribute__((constructor))` 注册：`registerDriver(GNOME_SHELL, DEFAULT_WINDOW_NAME, ...)`，使 [VideoFactories::createVideoDriver](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/VideoFactories.cpp#L70-L78) 的选择逻辑成立。
  - `dispatchEventQueue()` 复刻 [GLFWOpenGLDriver](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/GLFWOpenGLDriver.cpp#L110-L161) 的节流 + 渲染循环，但改为：渲染 → `glReadPixels(GL_RGBA)` 到 SHM 写缓冲 → 发布。
  - `getProcAddress()` 走 `eglGetProcAddress`；`closeRequested()` 由 IPC `Shutdown` 信号或 SIGINT 驱动。
- 为什么：真正的离屏驱动，不创建任何可见窗口。

#### (F) 鼠标输入（新）
- 文件：`src/WallpaperEngine/Input/Drivers/GnomeShellMouseInput.h/.cpp`（实现 [MouseInput](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Input/MouseInput.h)）
- 做什么：缓存扩展上报的指针坐标，`update()` 时刷新；`leftClick/rightClick` 恒为 `Released`（无点击能力）。
- 为什么：支撑视差/鼠标驱动特效（文档明确仅位置可用）。

#### (G) 全屏检测（新，可选）
- 文件：`src/WallpaperEngine/Render/Drivers/Detectors/GnomeShellFullScreenDetector.h/.cpp`
- 做什么：优先由扩展上报的锁屏/概览状态驱动暂停；否则退化为 stub（无全屏检测），并在日志中注明限制。
- 为什么：GNOME Wayland 下 `zwlr-foreign-toplevel` 不可用（见项目记忆）。

#### (H) 应用装配
- 文件：[WallpaperApplication.cpp](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Application/WallpaperApplication.cpp)
- 做什么：
  - `loadBackgrounds()`：把 `GNOME_SHELL` 与 `NORMAL_WINDOW/EXPLICIT_WINDOW` 同列为「单一 `default` 背景」分支，使 `--gnome-shell <path>` 的位置参数生效。
  - `setupOutput()`：`GNOME_SHELL` 时创建 `GnomeShellOpenGLDriver` + `GnomeShellFullScreenDetector`（工厂已可覆盖，必要时直接构造）。
  - `setDestinationFramebuffer`：把壁纸最终合成目标指向离屏 FBO。
- 为什么：让新模式完整接入既有装配流程。

#### (I) 构建
- 文件：[CMakeLists.txt](file:///home/chasedream/Developer/linux-wallpaperengine/CMakeLists.txt)
- 做什么：
  - 新增 `option(GNOME_SHELL_BACKEND "..." ON)`。
  - 当开启时把上述新文件追加进 `COMMON_SOURCES`（或独立 `GNOME_SHELL_SOURCES` 变量并入 `linux-wallpaperengine-lib`），并 `target_compile_definitions(... ENABLE_GNOME_SHELL)`；EGL/D-Bus 已在链接范围内。
- 为什么：可控开关、便于回归与打包。

### 4.2 GNOME Shell GJS 扩展侧

- 目录：`extensions/linux-wallpaperengine-gnome@local/`
- 文件与职责：
  - `metadata.json`：`uuid=linux-wallpaperengine-gnome@local`、`shell-version=["50"]`（参照文档，注意实际数组写法）。
  - `extension.js`：`enable()` 中创建 `Clutter.Actor`，`set_style('background-color:rgba(0,0,0,0.01);')`，`Main.layoutManager._backgroundGroup.add_child(actor)`；`disable()` 清理。
  - `dbus.js`：导出 `org.gnome.Shell.Extensions.LinuxWallpaperEngine`，实现 §5.2 接口；处理 `Register/Unregister/RequestMonitors` 与 `FrameReady` 信号。
  - `shm.js`：用 `GLib.MappedFile`（或回退 `Gio.File.read`）读取共享内存；用 `Clutter.Image.set_data(bytes, Cogl.PixelFormat.RGBA_8888, w, h, stride)` 构建纹理并 `actor.set_content(image)`。
  - `state.js`：监听 `Main.sessionMode` / 锁屏与显示器变化，向上报告 `MonitorsChanged` / `Pause` / `Resume`；鼠标位置经 `global.get_pointer()` + `GLib.timeout_add(…, ~30Hz)` 上报 `PointerMoved`。
- 关键 hack 与风险：`_backgroundGroup`、`rgba(0,0,0,0.01)` 均为非公开约定，仅保证 GNOME 50（见 §7）。

### 4.3 打包与部署脚本

- 目录：`packaging/gnome-shell/`
- 文件：
  - `install.sh`：把扩展复制到 `~/.local/share/gnome-shell/extensions/linux-wallpaperengine-gnome@local/`，`glib-compile-schemas`（如需），提示 `gnome-extensions enable`。
  - `linux-wallpaperengine-gnome.service`（可选）：systemd --user 单元，登录后以 `--gnome-shell` 启动引擎并支持重启。
  - `README`（脚本用法，非 md 文档）：安装/卸载/开机自启说明。

---

## 5. 接口与数据格式

### 5.1 SHM 帧缓冲布局
```
offset 0                    : FrameBufferHeader (POD, see §4.1-B)
offset HDR                  : buffer[0] pixels  (width*height*4)
offset HDR + sz             : buffer[1] pixels  (width*height*4)
```
- 写：等待 `seq` 为偶 → 写 buffer[back] → `seq=back+1`（奇，表示写入中）→ `seq=back`（偶）→ 翻转 `back`。
- 读：读 `seq` → 若为偶则读对应 buffer 并 `Clutter.Image` 更新 → 再次读 `seq` 校验未变，否则重试。

### 5.2 D-Bus 接口（草案）
```
name: org.gnome.Shell.Extensions.LinuxWallpaperEngine
path: /org/linuxwallpaperengine/GnomeBridge
interface: org.linuxwallpaperengine.GnomeBridge

methods (extension -> callable by engine):
  Register(s shmName, u width, u height, u stride, u format) -> (b ok)
  Unregister()
  RequestMonitors()

signals (extension -> engine):
  FrameReady(u frameId)
  PointerMoved(d x, d y)
  MonitorsChanged(a(iiiis) monitors, s primary)
  Pause()
  Resume()
  Shutdown()
```
> `FrameReady` 仅作唤醒；像素经 SHM 读取，避免逐帧大负载。

---

## 6. 分阶段实施（阶段 1–5）

### 阶段 1 — 最小原型验证链路
1. 实现 §4.1(A)(B)(D)(E) 的最小版：EGL 离屏上下文 + `GnomeShellOpenGLDriver` + `SharedFrameBuffer` + `GnomeShellOutput`。
2. 用**视频壁纸**（CVideo，走 MPV 解码，见 [GLPlayer](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/VideoPlayback/MPV/GLPlayer.h)）验证，**先跳过 CEF**。
3. 实现极简扩展 `extension.js` + `dbus.js` + `shm.js`，把帧贴到 `_backgroundGroup`。
4. 验收：屏幕出现随视频变化的壁纸；`/dev/shm` 无残留；Ctrl-C 后扩展 Actor 被清理。

### 阶段 2 — 双向 IPC
1. 完成 §4.1(C)(F)：`Register/RequestMonitors`、`MonitorsChanged`、`PointerMoved`。
2. 扩展上报 `Main.layoutManager.monitors`；引擎据其构建多显示器 viewport 与包围盒。
3. 鼠标接入 `InputContext`，验证视差/鼠标特效。
4. 验收：双屏不同分辨率下画面正确跨屏；鼠标移动驱动视差。

### 阶段 3 — 接入完整 WE 渲染管线
1. 验证 Scene / Shader / 粒子壁纸（[CScene](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Wallpapers/CScene.h)）。
2. 验证 CVideo 编解码/音频同步路径。
3. 打通 CEF 网页壁纸（[CWeb](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/Render/Wallpapers/CWeb.h) / [WebBrowserContext](file:///home/chasedream/Developer/linux-wallpaperengine/src/WallpaperEngine/WebBrowser/WebBrowserContext.h)）；确认 CEF 子进程在无窗口环境可用（必要时补 CEF 无头参数）。
4. 校验像素格式/朝向，必要时在扩展或引擎侧交换 R/B 或翻转 V。
5. 验收：三类壁纸（Scene/视频/网页）均正确显示。

### 阶段 4 — 性能优化
1. 帧节流与 VSync 对齐（复用 `maximumFPS` 设置；对齐扩展帧时钟）。
2. 异步读回：PBO（`glMapBuffer`/`GL_PIXEL_PACK_BUFFER`）替代同步 `glReadPixels`，消除 GPU stall。
3. SHM 零拷贝路径确认（避免任何中间 `memcpy`）。
4. 压测 4K/高刷，记录 CPU/GPU 占用；必要时降级到半分辨率或降帧。
5. 验收：4K 场景下无明显卡顿、概览动画不掉帧（对照文档「shell 主线程压力」约束）。

### 阶段 5 — 打包部署
1. 编写 `packaging/gnome-shell/install.sh` 与 systemd --user 单元（§4.3）。
2. 引擎二进制 + 扩展一键安装；自动启用扩展。
3. 文档化已知限制（§7）与卸载方式。
4. 验收：全新用户环境执行安装脚本后，登录即见动态壁纸。

---

## 7. 假设、决策与风险

**假设**
- 目标环境为 GNOME 50 Wayland，Mutter 仍暴露 `Main.layoutManager._backgroundGroup`。
- 引擎可在 EGL surface-less 上下文完成全部 WebGL2/OpenGL 3.3 渲染（否则回退 GLFW 隐藏窗口）。

**关键决策**
- 像素通道第一版即用 SHM（路线 B），控制通道用 D-Bus。
- 渲染整桌面包围盒单帧、由扩展跨屏摆放，而非每屏独立帧（复杂度与带宽更优）。

**风险与缓解**
1. `_backgroundGroup` / `rgba(0,0,0,0.01)` 为非公开 API，GNOME 升级易失效 → 仅承诺 GNOME 50，代码内集中封装、便于升级修复。
2. GJS 侧读取 mmap 的零拷贝能力不确定 → 首选 `GLib.MappedFile`；若不可用则回退 `Gio.File.read`（有拷贝），并在阶段 4 评估。
3. gnome-shell 主线程压力 → 严格帧节流 + VSync 对齐 + 轻量逐帧信号。
4. CEF 在无窗口环境的兼容性 → 阶段 3 专项验证，必要时补无头参数。
5. 无鼠标点击交互 → 明确写入限制说明，与 Hidamari 对齐。
6. 锁屏/休眠/热插拔 → 阶段 2 处理锁屏与显示器变化；休眠/热插拔在阶段 4 补齐。
7. SHM 残留 → `SharedFrameBuffer` 析构与信号处理中 `shm_unlink`；异常退出由扩展侧清理过期段。

---

## 8. 验证步骤（Verification）

1. **构建**：`cmake -S . -B build -DGNOME_SHELL_BACKEND=ON && cmake --build build -j`（确保 CEF/依赖齐全）。
2. **单测回归**：`cmake --build build --target tests && ./build/tests`（不破坏既有测试）。
3. **阶段 1**：`XDG_SESSION_TYPE=wayland ./build/output/linux-wallpaperengine --gnome-shell <视频壁纸路径>`，确认无可见窗口、画面变化、`/dev/shm` 在退出后无残留。
4. **阶段 2**：双屏环境验证跨屏位置正确、鼠标视差生效。
5. **阶段 3**：分别用 Scene / 视频 / 网页壁纸各验证一次，检查像素格式与朝向。
6. **阶段 4**：4K 壁纸下用 `top`/`radeontop`（或对应工具）观察占用，确认无 GPU stall、概览不掉帧。
7. **阶段 5**：在干净用户环境执行 `packaging/gnome-shell/install.sh`，重登后确认壁纸自动显示；卸载脚本可完全还原。
8. **回归对照**：确认未改动/未破坏 `gnome50` 分支的 XWayland 方案（不同分支，互不影响）。

---

## 9. 交付物清单

- 引擎：新增 `src/WallpaperEngine/Render/Drivers/GnomeShell/`、`.../Output/GnomeShellOutput.*`、`GnomeShellOpenGLDriver.*`、`.../Input/Drivers/GnomeShellMouseInput.*`、`.../Detectors/GnomeShellFullScreenDetector.*`；修改 `ApplicationContext.*`、`WallpaperApplication.cpp`、`CMakeLists.txt`。
- 扩展：`extensions/linux-wallpaperengine-gnome@local/{metadata.json,extension.js,dbus.js,shm.js,state.js}`。
- 打包：`packaging/gnome-shell/{install.sh,uninstall.sh,linux-wallpaperengine-gnome.service,README}`。