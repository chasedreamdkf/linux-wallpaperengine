# linux-wallpaperengine 使用方法（GNOME Shell / Wayland）

本后端把完整渲染核心放在离屏 EGL 上下文中运行，再通过 POSIX 共享内存把每帧交给一个 GNOME Shell 扩展，由扩展把画面画进 Mutter 的背景层。不依赖 wlr-layer-shell，也不需要切换到 Xorg。

---

## 1. 前置条件

- GNOME Shell 47 及以上（已在 GNOME Shell 50.1 / Mutter 18 上验证），会话必须是 Wayland
- 已用 `-DGNOME_SHELL_BACKEND=ON` 编译引擎
- Wallpaper Engine 的素材目录（Steam 安装）
- 壁纸 ID 取自 Wallpaper Engine 创意工坊，例如 `2976403740`

## 2. 编译

```bash
cmake -S . -B build -DGNOME_SHELL_BACKEND=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

产物：`build/output/linux-wallpaperengine`

## 3. 一键安装并运行（推荐）

```bash
# 首次：安装扩展 + 写入并启用 systemd 用户服务
./packaging/gnome-shell/run.sh 2976403740

# 之后不传参数即可复用上次的壁纸
./packaging/gnome-shell/run.sh
```

脚本会做四件事：

1. 把扩展复制到 `~/.local/share/gnome-shell/extensions/linux-wallpaperengine-gnome@local`
2. 生成 `~/.config/systemd/user/linux-wallpaperengine-gnome.service`
3. 执行 `systemctl --user enable`，因此**每次登录都会自动启动**
4. 扩展已加载时直接重启服务；未加载时提示你注销重登一次

第一次安装或扩展文件有改动后，**必须注销并重新登录一次**。GNOME Shell 对扩展模块只导入一次，`gnome-extensions disable/enable` 不会重新读取磁盘上的代码。

## 4. 壁纸参数格式

```bash
# 单个壁纸铺满整个桌面
./packaging/gnome-shell/run.sh 2976403740

# 每台显示器一个壁纸，<index> 为 0 起的显示器序号
./packaging/gnome-shell/run.sh 0=2976403740 1=3023217614
```

- 纯数字形式与 `<index>=<background>` 形式不能混用
- 纯数字形式只能给一个壁纸
- 不传参数时从 `~/.config/linux-wallpaperengine-gnome/args` 读取上次的配置

## 5. 每屏独立壁纸（直接调用引擎）

`--gnome-shell-screen` 是可重复的，它同时选中 GNOME Shell 后端，并让后面的 `--bg` / `--playlist` / `--scaling` / `--clamp` 作用于该显示器：

```bash
linux-wallpaperengine \
    --gnome-shell-screen 0 --bg 2976403740 --scaling fill \
    --gnome-shell-screen 1 --bg 3023217614 --scaling fill
```

显示器序号在启动日志里打印：

```
GNOME Shell desktop bounding box is 3840x1285 at (0,0) across 2 monitor(s), rendering frames at 3840x1285
  monitor 0 -> 1920x85:1920x1200
  monitor 1 -> 0x0:1920x1080
```

GNOME Shell 的 JS 接口不暴露显示器连接器名，因此只能按序号选择。

## 6. 性能调优

每帧都要经过「独显渲染 → 读回 → 共享内存 → Shell 拷贝 → 上传纹理 → 合成」。以 3840x1285、30 FPS 计算约 1.2 GB/s，是桌面卡顿的主要来源。

`--frame-scale`（0.1 - 1.0，默认 1.0）让引擎按更低分辨率渲染并发布，扩展在合成时放大：

```bash
# 通过 run.sh
LWE_FRAME_SCALE=0.75 ./packaging/gnome-shell/run.sh 2976403740

# 直接调用引擎
linux-wallpaperengine --gnome-shell 2976403740 --frame-scale 0.75
```

实测（3840x1285，30 FPS，Shell 空载 24%）：

| frame-scale | gnome-shell CPU | 说明 |
|---|---|---|
| 1.0 | 67% | 原始画质 |
| 0.75 | 53% | 轻微变软，几乎看不出 |
| 0.5 | 36% | 明显变软，最流畅 |

其他降低负载的方式：

```bash
# 降低帧率
linux-wallpaperengine --gnome-shell 2976403740 --fps 24

# 关闭粒子 / 鼠标 / 视差
linux-wallpaperengine --gnome-shell 2976403740 --disable-particles --disable-mouse --disable-parallax
```

引擎会打印实际使用的 GPU，可用于确认是否跑在独显上：

```
GNOME Shell OpenGL renderer: NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2
```

## 7. 手动运行（调试用）

绕过 systemd：

```bash
XDG_SESSION_TYPE=wayland ./build/output/linux-wallpaperengine --gnome-shell 2976403740
```

常用参数：

| 参数 | 作用 |
|---|---|
| `--gnome-shell <id>` | 整个桌面使用一个壁纸 |
| `--gnome-shell-screen <index>` | 选中后端并指定后续壁纸作用于哪台显示器，可重复 |
| `--bg <id>` | 指定壁纸 |
| `--scaling <mode>` | 缩放模式：`stretch`、`fit`、`fill`、`default` |
| `--clamp <mode>` | UV 夹取模式：`clamp`、`border`、`repeat` |
| `--frame-scale <0.1-1.0>` | 降低发布分辨率以减轻 Shell 负载 |
| `--fps <n>` | 帧率上限，默认 30 |
| `--assets-dir <path>` | 素材目录，自动探测失败时手动指定 |
| `--disable-particles` / `--disable-mouse` / `--disable-parallax` | 关闭对应效果 |
| `--list-properties` / `--set-property` | 查看与修改壁纸属性 |

## 8. 服务管理

```bash
# 查看状态
systemctl --user status linux-wallpaperengine-gnome.service

# 实时查看日志
journalctl --user -u linux-wallpaperengine-gnome.service -f

# 重启 / 停止 / 关闭开机自启
systemctl --user restart linux-wallpaperengine-gnome.service
systemctl --user stop linux-wallpaperengine-gnome.service
systemctl --user disable --now linux-wallpaperengine-gnome.service
```

单独安装扩展和服务（自行管理启停）：

```bash
./packaging/gnome-shell/install.sh [引擎二进制路径] [壁纸路径]
gnome-extensions enable linux-wallpaperengine-gnome@local
systemctl --user enable --now linux-wallpaperengine-gnome.service
```

## 9. 卸载

```bash
./packaging/gnome-shell/uninstall.sh
```

该脚本会停用并删除服务、删除扩展、清理配置目录和残留的 `/dev/shm/lwe-gnome-*`。注销重登后扩展才彻底卸载。

## 10. 已知限制

- **需要注销一次**：扩展模块只在登录时导入，更新扩展文件后必须重新登录
- **只能按序号选屏**：GNOME Shell JS 接口没有显示器连接器名
- **全屏暂停不可用**：Mutter 不提供 `zwlr-foreign-toplevel`，Wayland 下无法检测全屏窗口
- **鼠标交互有限**：扩展只回注指针位置，不支持点击事件
- **非零拷贝**：每帧都经过 CPU 拷贝，高分辨率 + 高帧率会明显占用核显和 Shell 主线程
- 壁纸只渲染扩展上报过的显示器；上报失败时引擎会直接报错退出
