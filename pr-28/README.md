# 测试包：PR #28（屏幕共享可选 NVENC 硬件编码）

给 [PR #28](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/28) 的测试包，基于 **v0.2.17 + PR #28**（commit `bc2ec7e`），版本号 `0.2.15.test2`。

## 包

| 发行版 | 文件 |
|---|---|
| Arch Linux | `linuxqq-wayland-fix-0.2.15.test2-1-x86_64.pkg.tar.zst` |
| Debian / Ubuntu | `linuxqq-wayland-fix_0.2.15.test2-1~test_amd64.deb` |

安装：

```bash
# Arch
sudo pacman -U linuxqq-wayland-fix-0.2.15.test2-1-x86_64.pkg.tar.zst

# Debian / Ubuntu
sudo apt install ./linuxqq-wayland-fix_0.2.15.test2-1~test_amd64.deb
```

## 这个包是什么

QQ 的 AVSDK 只有软件 H.264 编码器；这个包额外带一个**可选**的 NVIDIA NVENC 硬件编码路径，**默认关闭**。需要 NVIDIA 显卡 + 驱动（能加载 `libcuda.so.1` / `libnvidia-encode.so.1`）；没有 NVIDIA 卡或不开环境变量时完全无感知。

启动方式：

```bash
# 只挂钩旁观：行为与不注入完全一致，用来确认挂钩点匹配
QQ_NVENC=1 linuxqq-wayland-fix

# 真正启用 NVENC 编码
QQ_NVENC=1 QQ_NVENC_ACTIVE=1 linuxqq-wayland-fix
```

## 怎么验证

1. `linuxqq-wayland-fix --doctor` 应显示：
   `NVENC：CreateH264Encoder 符号存在，入口字节与挂钩点一致`。
2. `QQ_NVENC=1` 启动、开屏幕共享，日志里挂钩成功、没有「拒绝挂钩」：

   ```bash
   grep NVENC $XDG_RUNTIME_DIR/linuxqq-wayland-fix.log
   ```

3. `QQ_NVENC=1 QQ_NVENC_ACTIVE=1` 启动、共享整屏（4K 最明显），确认：
   - 日志出现 `NVENC: 会话就绪 ...`、`NVENC: 出流 ...`；
   - ppapi 进程里映射了 `libnvidia-encode` / `libcuda`；
   - **对端能看到画面**（这个包已包含「补齐 VideoPacket 帧序号/帧类型」的修复）；
   - 编码失败自动回落软件编码（`nv_dead`），不丢帧、不崩溃。
4. 结果 / 日志发到 [PR #28](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/pull/28)。

> 包里的 `libqq-nvenc.so` 用 nv-codec-headers `n13.1.15.0` 的头文件构建；运行时通过 dlopen 加载 NVIDIA 驱动库。
