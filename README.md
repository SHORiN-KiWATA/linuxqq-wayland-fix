
# linuxqq-wayland-fix

修复 Linux QQ 以 **Wayland** 运行时的屏幕分享、剪贴板和截图异常。

>一分钟视频介绍：[B 站](https://www.bilibili.com/video/BV1MKa66VEPS)

目前以 Wayland 模式运行 QQ 会有以下几个异常：

- 剪贴板
  
  Wayland QQ 依旧使用 X11 的剪贴板，导致 QQ 里复制的内容粘贴不到外面，外面复制的内容也粘贴不到 QQ 里。

  >修复前

  ![](./pics/剪贴板-before.gif)
  
  通常的解决办法是做一个剪贴板双向同步脚本。本项目没有使用类似[Linuxqq Clipsync](https://github.com/SHORiN-KiWATA/linuxqq-clipsync)的同步方式，而是在 QQ 进程里接管 QQ 的剪贴板调用，让它直接读写 Wayland 剪贴板（data-control 协议），不再经过 X11 剪贴板：表情等 QQ 自己的格式也能在多个 QQ 账号之间复制粘贴，不会和 X11 程序或其它 QQ 进程互相争抢，而且仅在 QQ 开启时生效。

  >修复后

  ![](./pics/剪贴板-after.gif)

- 屏幕分享

  在 Wayland QQ 尝试进行屏幕分享会提示 Wayland 下无法使用此功能，或者出现循环打开启动器的情况，总之是无法使用。

  >修复前

  ![](./pics/屏幕分享异常演示-before.gif)

  我利用 Opus 5.5 部署 [littlekan233/qq-wayland-screenshare](https://github.com/littlekan233/qq-wayland-screenshare) 项目时意外发现 QQ 居然有一套近乎完整的 Wayland 屏幕分享，只是缺少了选择器的部分。于是让 Opus 5.5 补全了这部分，屏幕分享就可用了。走的是 PipeWire 加 Portal 的“正规”路径，性能开销极低。

  >修复后

  https://github.com/user-attachments/assets/47fc8378-0bf7-4ef8-ac37-bfc1389dcf22

- 截图

  Wayland QQ 点击截图键会迅速闪退。

  >修复前

  ![](./pics/截图闪退-before.gif)

  这个仓库拦截了 QQ 截图操作，并将其改为 `wlr-screencopy` 截取显示输出。

  https://github.com/user-attachments/assets/36b6b031-19d0-4af2-9def-4e1a63a7ce11
  
>详细的逆向分析和原理见由 Deepseek V4.1 Flash 和 Opus 5.5 排查生成的：[docs/原理详解.md](docs/原理详解.md)。

## 注意事项和兼容性

XWayaland 和各个桌面的 xdg-desktop-portal 需要正常工作，以下是兼容性调查表格。

测试环境：Arch Linux + 7940h + 780M，纯净安装 Niri、Hyprland、KDE Plasma、GNOME 最新版本

| 修复内容 | Niri | Hyprland | KDE Plasma | GNOME |
| -------- | ---- | -------- | ---------- | ----- |
| 屏幕共享 | ✅    | ✅        | ✅          | ✅     |
| 剪贴板   | ✅    | ✅        | ✅          | ✅     |
| 截图     | ✅    | ✅        | ✅          | ✅     |

## 安装

QQ 本体需另外安装（[官方下载](https://im.qq.com/linuxqq/)）。

这个项目目前仅支持原生的 `linuxqq` 和 Appimage 版的 `linuxqq-appimage`，Flatpak之类的沙盒版暂不支持。

- Arch Linux

  ```bash
  paru -S linuxqq-wayland-fix-git
  ```

- Debian 12+ / Ubuntu 24.04+ / Fedora 43+ / Arch

  从 [Releases](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/releases) 下载对应的包：

  ```bash
  sudo apt install ./linuxqq-wayland-fix_*debian12_amd64.deb     # Debian 12+
  sudo apt install ./linuxqq-wayland-fix_*ubuntu24.04_amd64.deb  # Ubuntu 24.04+
  sudo dnf install ./linuxqq-wayland-fix-*.fc43.x86_64.rpm         # Fedora 43+
  sudo pacman -U ./linuxqq-wayland-fix-*.pkg.tar.zst              # Arch（需先装好 linuxqq）
  ```

- 源码

    依赖：C 编译器、make、pkg-config、wayland-scanner，以及 glib2（gio）、libX11、libwayland-client 的开发文件；libpulse、libpipewire-0.3 的开发文件（只用头文件，运行时不依赖）。
  
    ```bash
    make
    sudo make install PREFIX=/usr
    ```
  
- NixOS（Flake）
  
  本仓库自带 `flake.nix`，提供 `packages.default`、`overlays.default` 和 `nixosModules.default`。NixOS 用户推荐直接用模块：
  
  ```nix
  {
    inputs.linuxqq-wayland-fix.url = "github:SHORiN-KiWATA/linuxqq-wayland-fix";
  
    outputs = { self, nixpkgs, linuxqq-wayland-fix, ... }: {
      nixosConfigurations.host = nixpkgs.lib.nixosSystem {
        modules = [
          linuxqq-wayland-fix.nixosModules.default
          {
            nixpkgs.config.allowUnfree = true; # pkgs.qq 是 unfree
            programs.linuxqq-wayland-fix.enable = true;
          }
        ];
      };
    };
  }
  ```
  
  `enable = true` 会装好修复包和 `pkgs.qq`，并把 QQ 的路径告诉启动器；之后从应用菜单打开「**QQ（Wayland修复版）**」即可，自检用 `linuxqq-wayland-fix --doctor`。只想装修复包：`programs.linuxqq-wayland-fix.qq = null`。
  
  不用模块的话：
  
  ```bash
  nix build github:yigexuanmu/linuxqq-wayland-fix-nix
  nix develop   # 进开发环境后直接 make
  ```
  
  打包细节都在 `nix/package.nix`：NixOS 缺少的库搜索路径、EGL 平台、Vulkan ICD 路径等都在构建期补齐，不改动上游的任何文件。
  

## 使用方法

安装后完全退出QQ（包括托盘），然后从应用菜单打开「**QQ（Wayland修复版）**」，一切应当开箱即用，如果有问题先自行排查，或者用 AI 进行排查，然后提交仓库 Issues。

下面这条命令可以用于检查运行环境：

```
linuxqq-wayland-fix --doctor
```

想让应用菜单里原来的「QQ」直接启动修复版，可以覆盖原生 QQ 快捷方式（「QQ（Wayland修复版）」会同时隐藏）：

```
linuxqq-wayland-fix install     # 覆盖原生 QQ 快捷方式
linuxqq-wayland-fix uninstall   # 恢复原样
```

只在 `~/.local/share/applications/` 里放同名快捷方式，不改动系统文件。卸载本工具前请先运行 `uninstall`，否则菜单里的 QQ 会消失；QQ 更新后如果原快捷方式变了（名称、图标等），再运行一次 `install` 即可。

## 已知问题或解决办法

- 使用 Easy Effects 时，需在它的输入和输入排除名单里都加上 `TRAE`，否则 QQ 一开共享就会崩；
- 流畅度取决于 QQ 自己的编码，大概只有 20 帧左右；
- 观看别人共享时出现花屏或者显示不全的情况可以尝试调整`QQ_WAYLAND_FIX_ANGLE`环境变量的值，具体可用的值通过`linuxqq-wayland-fix -h`命令查看。详见 [原理详解](docs/原理详解.md#附观看共享花屏)。

详细说明见 [常见问题与排错](docs/常见问题与排错.md)。

## 致谢

[littlekan233/qq-wayland-screenshare](https://github.com/littlekan233/qq-wayland-screenshare) [xuwd1/wemeet-wayland-screenshare](https://github.com/xuwd1/wemeet-wayland-screenshare)：「截屏中转」思路的先行者。本项目采用了不同的方法，不包含它们的代码。

[Linux Do](https://linux.do/) 中文 Linux 社区。

## 许可证

MIT。`protocol/` 下的协议描述文件保留其原有版权声明。

