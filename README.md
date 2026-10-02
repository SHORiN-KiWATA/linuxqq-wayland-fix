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

在 flake 的 `inputs` 里加上：

```nix
inputs = {
  linuxqq-wayland-fix = {
    url = "github:xiaoyintx/linuxqq-wayland-fix";
    # 与系统共用 nixpkgs，避免重复的 glib / QQ（可选，但推荐）
    inputs.nixpkgs.follows = "nixpkgs";
  };
};
```

然后在 `home-manager.users` 下挂到对应用户：

```nix
home-manager.users.<用户名> =
  { pkgs, inputs, ... }:
  {
    home.packages = [
      inputs.linuxqq-wayland-fix.packages.${pkgs.stdenv.hostPlatform.system}.default
    ];
  };
```

QQ 本体是 unfree，需要 `nixpkgs.config.allowUnfree = true;`。安装后从「QQ（Wayland修复版）」启动，用法与其它发行版一致。

### 从源码

依赖：C 编译器、make、pkg-config、wayland-scanner，以及 glib2（gio）、libX11、libwayland-client 的开发文件；libpulse、libpipewire-0.3 的开发文件（只用头文件，运行时不依赖）。

```bash
make
sudo make install PREFIX=/usr
```

## 使用

完全退出QQ（包括托盘），然后从应用菜单打开「**QQ（Wayland修复版）**」。

- 屏幕分享
  
  共享屏幕：在 QQ 自己的选窗里随便选「桌面」→「确定」，然后在合成器弹出的选择框里选真正要共享的屏幕或窗口；需要共享电脑声音时，点共享工具栏上的「共享设备音频」。

- 剪贴板
  
  照常复制粘贴即可。

- 截图
  
  截图应该不再闪退。在平铺式合成器上截图窗口可能显示异常，见「已知问题」。

- 检查环境
  
    检查环境、以及 QQ 更新后修复是否仍然适用：

    ```bash
    linuxqq-wayland-fix --doctor
    ```

## 兼容性

| 项目     | 要求                                                                                                                                             |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| 屏幕共享 | xdg-desktop-portal 的 ScreenCast                                                                     |
| 剪贴板   | 合成器支持 data-control（`ext-data-control-v1` 或 `wlr-data-control-unstable-v1`）：**GNOME 不支持** |               
| XWayland | 需要（QQ 的界面流程和剪贴板仍是 X11）                                                                                                            |

>kde plasma上运行异常，暂不支持，原因未明

## 已知问题

- 使用 Easy Effects 时，需在它的「输入」「输出」排除名单里都加上 `TRAE`，否则 QQ 一开通话/共享就会崩；
- 不要同时运行 linuxqq-clipsync 等其它剪贴板同步工具；
- 截图窗口在 niri 等平铺式合成器上会被平铺，画面重复显示；KDE、GNOME 下截图背景是黑的；
- 共享时 QQ 的全屏蓝色边框会变成一个真实窗口；流畅度取决于 QQ 自己的编码。

详细说明见 [常见问题与排错](docs/常见问题与排错.md)。

## 排错

先运行 `linuxqq-wayland-fix --doctor`。日志在 `$XDG_RUNTIME_DIR/linuxqq-wayland-fix.log`，QQ 的崩溃记录保存在 `~/.cache/linuxqq-wayland-fix/crash/`，反馈问题时请附上。症状对照、单独关掉某个修复的方法见 [常见问题与排错](docs/常见问题与排错.md)。

## 工作原理

启动器通过 `LD_PRELOAD` 向 QQ 注入三个小库，不修改任何 QQ 文件：`libqq-wl-portal.so` 给 QQ 自带的 Wayland 共享代码补上 portal 选择步骤；`libqq-clipbridge.so` 在 QQ 的 X11 剪贴板和 Wayland 剪贴板之间双向桥接；`libqq-screenshot.so` 让 QQ 截全屏时从 Wayland 取画面。详见 [原理详解](docs/原理详解.md)。

## 致谢

[littlekan233/qq-wayland-screenshare](https://github.com/littlekan233/qq-wayland-screenshare)、[xuwd1/wemeet-wayland-screenshare](https://github.com/xuwd1/wemeet-wayland-screenshare)：「截屏中转」思路的先行者。本项目采用了不同的方法，不包含它们的代码。

[@YoungJurry](https://github.com/YoungJurry) 定位了显示器坐标偏移时共享闪退的问题（#1、#2）。

## 许可证

MIT。`protocol/` 下的协议描述文件保留其原有版权声明。
