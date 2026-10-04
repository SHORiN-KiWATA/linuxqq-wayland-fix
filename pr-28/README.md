# 测试包：PR #28（屏幕共享可选 NVENC 硬件编码）

给 [PR #28](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/28) 的测试包，基于 **v0.2.17 + PR #28**（commit `9b816b0`），版本号 `0.2.17.test.nvenc.2`。

## 包

| 发行版 | 文件 | sha256 |
|---|---|---|
| Arch Linux | `linuxqq-wayland-fix-0.2.17.test.nvenc.2-1-x86_64.pkg.tar.zst` | `abd5a1ca7ed376e71319e58701016e462dbea1fb87a1e3851b8eab288255dba2` |
| Debian / Ubuntu | `linuxqq-wayland-fix_0.2.17.test.nvenc.2-1~test_amd64.deb` | `54ff64739440b560eaf98b70d0f7e5bd42d9ce9fa082c1f43d2c6d5859bb78d0` |

安装：

```bash
# Arch
sudo pacman -U linuxqq-wayland-fix-0.2.17.test.nvenc.2-1-x86_64.pkg.tar.zst

# Debian / Ubuntu
sudo apt install ./linuxqq-wayland-fix_0.2.17.test.nvenc.2-1~test_amd64.deb
```

## 这个包是什么

QQ 的 AVSDK 只有软件 H.264 编码器；这个包额外带一个**可选**的 NVIDIA NVENC 硬件编码路径，**默认关闭**。需要 NVIDIA 显卡 + 驱动；没有 NVIDIA 卡或不开环境变量时完全无感知。

启动方式（**一个开关**）：

```bash
QQ_NVENC=1 linuxqq-wayland-fix
```

## 本版相对 0.2.17.test.nvenc.1 的变化（重要）

上一版把帧号来源改成「输入帧 +0x10 的 64 位值」后，实机反馈**对端看不到画面**（[FJKiXfaR](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/commit/15739529055e90bf5ae4b87f5b8ec6bb8e0208ec#commitcomment-203369129)、ljm-233），而旧版计数器方案正常。本版：

- **帧号/帧类型回退到实机验证过的计数器方案**（4 字节写入，与 0.2.15.test2 相同）；
- 保留：编码器槽位竞态修复、单开关（`QQ_NVENC=1`）；
- 新增调试开关 `QQ_NVENC_SEQ=frame`：切回帧号来源，可在同一构建上做 A/B 对比；
- 出帧日志同时打印计数器与 `frame+0x10` 的运行时取值（前 4 帧）。

## 怎么验证

1. `linuxqq-wayland-fix --doctor` 应显示 `NVENC：CreateH264Encoder 符号存在，入口字节与挂钩点一致`；
2. 默认启动、开屏幕共享，**对端应能看到画面**（与 0.2.15.test2 一致）：

   ```bash
   QQ_NVENC=1 linuxqq-wayland-fix
   grep NVENC $XDG_RUNTIME_DIR/linuxqq-wayland-fix.log
   ```

3. A/B 排查（可能出现对端无画面，用于对比数据）：

   ```bash
   # 帧号来源：输入帧 +0x10（8 字节写入）
   QQ_NVENC=1 QQ_NVENC_SEQ=frame linuxqq-wayland-fix
   ```

4. 旁观原实现写进 packet 的值（不影响正式路径）：

   ```bash
   QQ_NVENC=1 QQ_NVENC_PROBE=1 QQ_NVENC_PROBE_DUMP=1 linuxqq-wayland-fix
   ```

反馈请发到 [PR #28](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/28)。

> **混合显卡提示**：本包基于 v0.2.17，不含 [PR #38](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/38) 的「把 QQ 的 Vulkan 限定到显示 GPU」；开启独显后观看共享若出现花屏，属于 #38 的范畴。
