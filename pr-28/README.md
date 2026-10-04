# 测试包：PR #28（屏幕共享可选 NVENC 硬件编码）

给 [PR #28](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/28) 的测试包，基于 **v0.2.17 + PR #28**（commit `292da96`），版本号 `0.2.17.test.nvenc.1`。

## 包

| 发行版 | 文件 |
|---|---|
| Arch Linux | `linuxqq-wayland-fix-0.2.17.test.nvenc.1-1-x86_64.pkg.tar.zst` |
| Debian / Ubuntu | `linuxqq-wayland-fix_0.2.17.test.nvenc.1-1~test_amd64.deb` |

安装：

```bash
# Arch
sudo pacman -U linuxqq-wayland-fix-0.2.17.test.nvenc.1-1-x86_64.pkg.tar.zst

# Debian / Ubuntu
sudo apt install ./linuxqq-wayland-fix_0.2.17.test.nvenc.1-1~test_amd64.deb
```

## 这个包是什么

QQ 的 AVSDK 只有软件 H.264 编码器；这个包额外带一个**可选**的 NVIDIA NVENC 硬件编码路径，**默认关闭**。需要 NVIDIA 显卡 + 驱动；没有 NVIDIA 卡或不开环境变量时完全无感知。

启动方式（**一个开关**）：

```bash
# 启用 NVENC 编码
QQ_NVENC=1 linuxqq-wayland-fix

# 只挂钩旁观、不接管（排查用）
QQ_NVENC=1 QQ_NVENC_PROBE=1 linuxqq-wayland-fix
```

## 本版包含的修复（相对旧测试包）

- **帧号修复**：`VideoPacket` 的 `+0x00/+0x08` 改用输入帧自带的 64 位帧号（与 `CO264RTEncoder::Encode` 反汇编一致，两处各写 8 字节）。此前用自建计数器，跨编码器对象/会话对不上，下游按帧号做队列匹配失败会让对端收不到画面；实机验证：修复后对端出画面。
- `+0x20` 帧类型补 I 帧映射（1/2/3）；并发创建编码器的槽位竞态修复。
- 开关合并：`QQ_NVENC=1` 即启用（删除 `QQ_NVENC_ACTIVE`）。

## 怎么验证

1. `linuxqq-wayland-fix --doctor` 应显示 `NVENC：CreateH264Encoder 符号存在，入口字节与挂钩点一致`；
2. `QQ_NVENC=1` 启动、开屏幕共享，日志里挂钩成功、无回落：

   ```bash
   grep NVENC $XDG_RUNTIME_DIR/linuxqq-wayland-fix.log
   ```

3. 对端（手机 / 另一账号）能看到画面、流畅；出帧日志里 `帧号=` 连续递增；
4. 编码失败会自动回落软件编码（`nv_dead`），不丢帧、不崩溃。

反馈请发到 [PR #28](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/28)。

> **混合显卡用户注意**：启用 NVENC 需要加载 nvidia 模块，独显的 Vulkan 设备会随之出现；启动器会自动把 QQ 的 Vulkan 限定到显示 GPU，规避「观看共享花屏」（见 [PR #38](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/38) 与 `docs/原理详解.md#附观看共享花屏`）。
