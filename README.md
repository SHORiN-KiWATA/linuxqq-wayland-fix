# linuxqq-wayland-fix

修复 Linux QQ 以 **Wayland** 运行时的屏幕分享、剪贴板和截图异常。

>本项目接替 linuxqq-wayland-native-screenshare-fix 和 [linuxqq-clipsync](https://github.com/SHORiN-KiWATA/linuxqq-clipsync)。

## 安装

### Arch Linux（AUR）

```bash
paru -S linuxqq-wayland-fix-git
```

### Debian 12+ / Ubuntu 24.04+ / Fedora 43+ / Arch

从 [Releases](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/releases) 下载对应的包：

```bash
sudo apt install ./linuxqq-wayland-fix_*debian12_amd64.deb     # Debian 12+
sudo apt install ./linuxqq-wayland-fix_*ubuntu24.04_amd64.deb  # Ubuntu 24.04+
sudo dnf install ./linuxqq-wayland-fix-*.fc43.x86_64.rpm         # Fedora 43+
sudo pacman -U ./linuxqq-wayland-fix-*.pkg.tar.zst              # Arch（需先装好 linuxqq）
```

QQ 本体需另外安装（[官方下载](https://im.qq.com/linuxqq/)）。

### NixOS / Nix（flake）

本仓库自带 `flake.nix`，从 GitHub 按 commit 构建，`nix flake update`（或 `nix profile upgrade`）即可更新到最新。

临时试用：

```bash
nix run github:xiaoyintx/linuxqq-wayland-fix
# 或装进 profile：
nix profile install github:xiaoyintx/linuxqq-wayland-fix
```

写进 NixOS / Home Manager 配置：

```nix
{
  inputs.linuxqq-wayland-fix = {
    url = "github:xiaoyintx/linuxqq-wayland-fix";
    # 与系统共用 nixpkgs，避免重复的 glib / QQ。（可选，但推荐）
    inputs.nixpkgs.follows = "nixpkgs";
  };
}
```

```nix
# 直接装包
home.packages = [
  inputs.linuxqq-wayland-fix.packages.${pkgs.stdenv.hostPlatform.system}.default
];

# 或引用模块
# imports = [ inputs.linuxqq-wayland-fix.homeManagerModules.default ];
# programs.linuxqq-wayland-fix.enable = true;

# 或通过 overlay 使用 pkgs.linuxqq-wayland-fix
# nixpkgs.overlays = [ inputs.linuxqq-wayland-fix.overlays.default ];
```

QQ 本体是 unfree，需要 `nixpkgs.config.allowUnfree = true;`。安装后从「QQ（Wayland修复版）」启动，用法与其它发行版一致。

### 从源码

依赖：C 编译器、make、pkg-config、wayland-scanner，以及 glib2（gio）、libX11、libwayland-client 的开发文件；libpulse、libpipewire-0.3 的开发文件（只用头文件，运行时不依赖）。

```bash
make
sudo make install PREFIX=/usr
```

## 使用

- 屏幕分享
  
  1. **完全退出 QQ**（包括托盘）。
  2. 从应用菜单打开「**QQ（Wayland修复版）**」。
  3. 共享屏幕：在 QQ 自己的选窗里随便选「桌面」→「确定」，然后在合成器弹出的选择框里选真正要共享的屏幕或窗口；需要共享电脑声音时，点共享工具栏上的「共享设备音频」。

- 剪贴板
  
  照常复制粘贴即可。

- 截图
  
  照常按截图键（默认 Ctrl+Alt+A）。在平铺式合成器上截图窗口可能显示异常，见「已知问题」。

- 检查环境
  
    检查环境、以及 QQ 更新后修复是否仍然适用：

    ```bash
    linuxqq-wayland-fix --doctor
    ```

    QQ 崩溃时，崩溃记录（Bugly 的 `tomb_*.txt`）会保存到 `~/.cache/linuxqq-wayland-fix/crash/`（原位置会被 `linuxqq` 启动脚本清空），反馈问题时请附上。

## 兼容性

| 项目     | 要求                                                                                                                                             |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| 屏幕共享 | xdg-desktop-portal 的 ScreenCast（niri、KDE、GNOME、wlroots 系都有对应后端）                                                                     |
| 剪贴板   | 合成器支持 data-control（`ext-data-control-v1` 或 `wlr-data-control-unstable-v1`）：niri、KDE Plasma、Hyprland、sway、labwc 等；**GNOME 不支持** |
| 截图     | 合成器支持 `wlr-screencopy-unstable-v1`：niri、Hyprland、sway、labwc 等；KDE、GNOME 下截图背景是黑的（不会闪退）                                   |
| XWayland | 需要（QQ 的界面流程和剪贴板仍是 X11）                                                                                                            |

## 已知问题

### 使用 Easy Effects 时，QQ 一开通话/共享就崩

Easy Effects 会把新出现的音频流移到它自己的设备上，这会触发 QQ 音频模块里的竞态 bug。解决：

- Easy Effects →「输入」和「输出」页 → 排除的应用 → 都加上 **`TRAE`**（QQ 音频流的应用名）；
- QQ「设置 → 音视频通话」里把麦克风选成 **Easy Effects Source**，麦克风照样经过 Easy Effects 处理。

### 不要同时运行其它剪贴板同步工具

本工具已经在 QQ 内部双向同步剪贴板，再运行 linuxqq-clipsync 之类的 X11↔Wayland 同步工具会重复同步。`--doctor` 会检查 linuxqq-clipsync。

### 截图窗口在平铺式合成器上显示异常

QQ 的截图窗口是按「放在桌面左上角、和整个桌面一样大」设计的，Wayland 下程序无法自己指定窗口位置和大小，平铺式合成器（如 niri）会把它当成普通窗口平铺，截到的画面按窗口宽度缩放后会重复铺好几份。可以改用合成器自带的截图（截完复制到剪贴板），再粘贴进 QQ。

### 全屏蓝色边框

QQ 为 X11 设计的共享边框在 Wayland 下会变成一个真实的全屏窗口，目前没有处理，可以在合成器里把它挪到别的工作区。

### 流畅度

共享画面由 QQ 单线程软件 H.264 编码，帧率由 QQ 自己的策略决定，本项目无法改善。在选择框里选**单个窗口**会更流畅。

### niri 上用 Linux QQ 3.2.34 观看共享可能花屏

原版 QQ 同样如此，与本项目无关；同一路共享在手机和 Windows 上看是正常的。

## 排错

日志：`$XDG_RUNTIME_DIR/linuxqq-wayland-fix.log`

```bash
grep -E 'qq-wl-portal|qq-clipbridge' "$XDG_RUNTIME_DIR/linuxqq-wayland-fix.log"
```

正常的输出：

```
[qq-clipbridge] ready (pid 12345, ext-data-control)
[qq-wl-portal] broadcast-core asked to connect fd=1, opening portal       ← 开始共享
[qq-wl-portal] portal ok: pipewire fd=82 node=127
[qq-wl-portal] device audio: report sample format 7 as float32le (5) …   ← 开启共享设备音频
[qq-clipbridge] QQ copied -> Wayland: text/plain;charset=utf-8 …          ← QQ 里复制
[qq-clipbridge] Wayland clipboard changed -> X11 for QQ: image/png        ← 别处复制
```

| 症状                                                                     | 原因 / 办法                                                                                |
| ------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------ |
| 提示「Wayland桌面环境暂时无法使用屏幕分享功能」                          | 不是从「QQ（Wayland修复版）」打开的                                                        |
| 点共享没反应，`coredumpctl` 有 QQ 的 SIGTRAP，栈里有 `PulseAudioWrapper` | Easy Effects，见上文                                                                       |
| 点「确定」没反应，栈里有 `ZSTD_` / `libgallium`                          | QQ 自带的 zstd 与 Mesa 冲突；启动器已设置 `MESA_SHADER_CACHE_DISABLE=true`，请确认没被覆盖 |
| 日志里没有任何 `qq-wl-portal` / `qq-clipbridge`                          | QQ 没被注入（旧 QQ 没退干净），或 QQ 更新后改了实现，运行 `--doctor`                       |
| `compositor supports neither …`                                          | 合成器不支持 data-control（如 GNOME），剪贴板修复不可用                                    |
| `response=1`                                                             | 在 portal 选择框里点了取消                                                                 |
| 按截图键 QQ 闪退，日志里有 `X_GetImage` 的 `BadMatch`                     | 截图修复没有生效（旧 QQ 没退干净，或没从「QQ（Wayland修复版）」打开）；正常时日志里有 `[qq-screenshot] captured …` |
| 点「确定」开始共享时 QQ 闪退，崩溃记录里是 `signal: 5 (SIGTRAP)`         | 显示器坐标不从 0 开始时 QQ 算出空的窗口几何（#1）；本工具会自动处理，日志里应有 `empty-geometry fix: patched`，若是 `not patching` 说明 QQ 更新改了实现，请反馈 |

排查时可以单独关掉某个修复：`QQ_WL_NATIVE_DISABLE=1`（屏幕共享）、`QQ_CLIPBOARD_FIX_DISABLE=1`（剪贴板）、`QQ_WL_GEOMETRY_FIX_DISABLE=1`（共享时防闪退）、`QQ_SCREENSHOT_FIX_DISABLE=1`（截图）。

## 工作原理

启动器通过 `LD_PRELOAD` 向 QQ 注入三个小库，不修改任何 QQ 文件。

**屏幕共享（`libqq-wl-portal.so`）**：QQ 的采集库 `broadcast-core.so` 其实自带一套 portal + PipeWire 的 Wayland 采集代码，但缺少「选择共享源」这一步，从未启用。本库只对 broadcast-core 发起的调用生效：让它走 Wayland 分支、在它连接 PipeWire 时自己走一遍 portal 选择流程，并修正两个 QQ 自身的 bug（声卡格式不是 s16le/f32le 时设备音频静默失败；共享内存帧忽略行跨度导致画面斜切）。另外，显示器坐标不从 0 开始时（如 Hyprland 单屏 `position=1920x0`），QQ 会给某个窗口算出空的几何并主动崩溃；本库在 QQ 主进程里把这处崩溃改为跳过该请求（#1，由 [@YoungJurry](https://github.com/YoungJurry) 最早定位）。

**剪贴板（`libqq-clipbridge.so`）**：QQ 的剪贴板代码（`wrapper.node` 里的 `ClipBoardHelper`）只用 Xlib，所以 QQ 在 Wayland 下只读写 X11 剪贴板。本库在 QQ 进程里起一个后台线程，用自己的 X 连接和 data-control 协议双向桥接：QQ 复制时把格式提供给 Wayland，别的程序复制时接管 X11 剪贴板；数据都在粘贴时按需传输一次。

**截图（`libqq-screenshot.so`）**：启动器为了让屏幕共享可用，给 QQ 的是 `XDG_SESSION_TYPE=x11`，于是 QQ 用 X11 的方式对根窗口 `XGetImage` 截全屏；而 Wayland 下的 XWayland 是 rootless 的，根窗口没有内容，这一步必然失败，QQ 不检查返回值就直接崩溃。本库拦截对根窗口的截取，改为通过 `wlr-screencopy` 截取各个 Wayland 输出，按 X 的显示器布局拼好交给 QQ；合成器不支持时给一张黑图，至少不再闪退。

详细的逆向分析见 [docs/原理详解.md](docs/原理详解.md)。

## 致谢

[littlekan233/qq-wayland-screenshare](https://github.com/littlekan233/qq-wayland-screenshare)、[xuwd1/wemeet-wayland-screenshare](https://github.com/xuwd1/wemeet-wayland-screenshare)：「截屏中转」思路的先行者。本项目采用了不同的方法，不包含它们的代码。

## 许可证

MIT。`protocol/` 下的协议描述文件保留其原有版权声明。
