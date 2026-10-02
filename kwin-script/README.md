# linuxqq-share-overlay（KWin 脚本）

**仅 KDE Plasma 6 / KWin 需要**，用来解决纯 Wayland 下 QQ 共享悬浮层位置乱掉的问题。

QQ 共享时创建三个普通 Wayland 顶层窗口（class `QQ`，标题「屏幕共享」）：内容/边框层、
悬浮工具栏、参会者面板。Wayland 协议不允许客户端给顶层窗口定位，合成器只能层叠摆放。
这个脚本在合成器侧按窗口尺寸把它们钉到屏幕上的固定位置（逻辑坐标，天然适配各档缩放）。

| 窗口 | 判定 | 目标位置 |
| --- | --- | --- |
| 内容/边框层 | 宽或高超过屏幕 | 当前屏幕 `0,0` 铺满 |
| 悬浮工具栏 | 其余里宽 > 高×2 | 顶部居中，距上边缘 24 |
| 参会者面板 | 其余 | 右上角，距右 50、距上 90 |

参数都在 `linuxqq-share-overlay/contents/code/main.js` 顶部。

已知限制：内容/边框层 QQ 自己固执地保持 2560×1440（比逻辑屏幕大），脚本只能把它挪到
`0,0`，不一定能改小（客户端可能不接受 configure）；它基本是透明层，工具栏和面板位置修好即可。

## 安装

```bash
mkdir -p ~/.local/share/kwin/scripts
cp -r linuxqq-share-overlay ~/.local/share/kwin/scripts/
kwriteconfig6 --file kwinrc --group Plugins --key linuxqq-share-overlayEnabled true
qdbus org.kde.KWin /KWin reconfigure
```

开关：系统设置 → 窗口管理 → KWin 脚本。

## 不安装、快速试用

```bash
id=$(qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript \
        "$PWD/linuxqq-share-overlay/contents/code/main.js" "linuxqq-share-overlay-test")
qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.start
# 停掉：
qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "linuxqq-share-overlay-test"
```

脚本只在窗口创建时摆一次，并用几何变化信号兜底（每个窗口最多尝试 6 次）。

## 其它合成器

niri / Hyprland / sway 等需要各自的窗口规则来做同样的事（按 class + 标题匹配，设置位置）。
