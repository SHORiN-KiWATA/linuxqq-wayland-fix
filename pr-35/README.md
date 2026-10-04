# 测试包：PR #35（分数缩放下的共享批注窗口）

给 [PR #35](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/35) 的测试包，基于 **v0.2.17 + PR #35**（commit `9c9e021`），版本号 `0.2.15.test1`。

## 包

| 发行版 | 文件 |
|---|---|
| Arch Linux | `linuxqq-wayland-fix-0.2.15.test1-1-x86_64.pkg.tar.zst` |
| Debian / Ubuntu | `linuxqq-wayland-fix_0.2.15.test1-1~test_amd64.deb` |

安装：

```bash
# Arch
sudo pacman -U linuxqq-wayland-fix-0.2.15.test1-1-x86_64.pkg.tar.zst

# Debian / Ubuntu
sudo apt install ./linuxqq-wayland-fix_0.2.15.test1-1~test_amd64.deb
```

## 这个包修什么

分数缩放（1.25 / 1.3 / 1.5…）下共享屏幕时，批注 / 激光笔的绘制窗口（标题「屏幕共享」）会把**物理分辨率**当成逻辑尺寸：

- 画布比屏幕大 scale 倍（如 1.3 倍下 2560×1440 的画布铺在 1969×1108 的屏幕上）；
- 工具条跑到屏幕外 /「逻辑中心」，笔迹落点偏移。

本包在 `libqq-screenshot.so` 里拦截 AVSDK 对 `XRRGetMonitors` 的调用，按 xdg-output 的逻辑尺寸返回（仅覆盖「共享屏幕」，窗口共享不受影响）。

## 怎么验证

1. 在分数缩放的显示器上开始共享（共享整个屏幕），点「批注」或「激光笔」。
2. 绘制窗口应该正好是逻辑分辨率大小（1.3 倍：1969×1108；1.5 倍：1707×960），不再是 2560×1440。工具条在屏幕内，笔迹落点和鼠标一致。
3. 日志确认：

   ```bash
   grep "drawing rect" $XDG_RUNTIME_DIR/linuxqq-wayland-fix.log
   # [qq-screenshot] drawing rect: eDP-1 X11 2560x1440+0+0 -> logical 1969x1108
   ```

4. 对照测试（关掉这个修复，画布应重新变大）：

   ```bash
   QQ_DRAWING_RECT_FIX_DISABLE=1 linuxqq-wayland-fix
   ```

反馈请发到 [PR #35](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/35) 或 issue [#32](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/issues/32) / [#34](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/issues/34)。
