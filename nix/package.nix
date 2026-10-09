{
  lib,
  stdenv,
  makeWrapper,
  pkg-config,
  wayland-scanner,
  glib,
  libx11,
  wayland,
  libpulseaudio,
  libva,
  pipewire,
  # 启动器脚本运行时要用的小工具：pgrep、busctl、wayland-info、od、xargs…，
  # 从应用菜单启动时它们不一定在 PATH 里，wrap 的时候补上。
  coreutils,
  findutils,
  gawk,
  gnugrep,
  gnused,
  procps,
  systemd,
  wayland-utils,
  # 名字刻意不叫 src：callPackage 会把 pkgs 里的同名属性自动填进来
  # （pkgs.src 是 simple-revision-control），默认值就永远用不上。
  source ? ../.,
  version ? "0.0.0",
  # 被修复的 QQ（nixpkgs 的 pkgs.qq，unfree 许可）。
  # null 表示不写死：启动器会退回自己在 PATH 里找 linuxqq（或 /opt/QQ/qq），
  # 或者读用户显式设置的 QQ_WAYLAND_FIX_QQ。
  #
  # 名字不叫 qq 是因为 callPackage 会拿 pkgs 里的同名属性自动填充，
  # pkgs.qq 正好存在，会把这个默认值覆盖成 unfree 的 QQ 本体。
  qqPackage ? null,
}:

let
  runtimeTools = [
    coreutils
    findutils
    gawk
    gnugrep
    gnused
    procps # pgrep：--doctor 检查 QQ 是否被注入
    systemd # busctl、systemctl：--doctor 检查 portal 与 clipsync
    wayland-utils # wayland-info：--doctor 检查 data-control 协议
  ];
in
stdenv.mkDerivation (finalAttrs: {
  pname = "linuxqq-wayland-fix";
  inherit version;
  src = source;

  nativeBuildInputs = [
    makeWrapper
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    glib
    wayland
    libx11
  ]
  # 上游只用 libpulse / libpipewire 的头文件，注入库运行时不链接它们，
  # 所以这里只取 dev 输出。（libpipewire 的运行库在 postFixup 里通过
  # LD_LIBRARY_PATH 交给 QQ 进程，不是给注入库链接用的。）
  ++ map lib.getDev [
    libpulseaudio
    pipewire
  ];

  # 上游 Makefile 的 PREFIX 决定 bin/、lib/linuxqq-wayland-fix/、share/ 的落点；
  # 启动器里的 @LIBEXECDIR@ 在安装时展开成 store 里的绝对路径。
  makeFlags = [
    "VERSION=${finalAttrs.version}"
    "PREFIX=${placeholder "out"}"
  ];

  buildPhase = ''
    runHook preBuild
    make $makeFlags
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    make install $makeFlags
    runHook postInstall
  '';

  postFixup = ''
    # 菜单里的 Exec 用绝对路径，profile 不在 PATH 里也能启动。
    substituteInPlace $out/share/applications/linuxqq-wayland-fix.desktop \
      --replace-fail 'Exec=linuxqq-wayland-fix' "Exec=$out/bin/linuxqq-wayland-fix"

    # 下面两个补丁只改装出来的运行时脚本，不动仓库源码。
    #
    # 1. 上游用 `compgen -G` 判断有没有崩溃记录。compgen 属于 bash 的可编程补全
    #    内建，nix 构建出的 bash 没编译它（compgen: command not found），而
    #    keep_crash_records 在每次启动前都会调用。换成等价的 ls 判断。
    substituteInPlace $out/bin/linuxqq-wayland-fix \
      --replace-fail 'compgen -G "$QQ_CRASH_DIR/tomb_*.txt" >/dev/null || return 0' \
      'ls "$QQ_CRASH_DIR"/tomb_*.txt >/dev/null 2>&1 || return 0'

    # 2. QQ 装在 store 里，没有 /opt/QQ，--doctor 的「QQ 内部实现」与共享防闪退
    #    特征码检查会找不到 QQ。QQ_WAYLAND_FIX_QQ_ROOT 由下面的 wrapper 指向
    #    store 里的 .../opt/QQ；没设这个变量时保持上游的 /opt/QQ 行为。
    #    qq_runtime_exe 找不到运行中的 QQ 时的回退路径也要跟着走 store
    #    （qq_runtime_dir、共享防闪退的 qqbin 都由它得来）。
    substituteInPlace $out/bin/linuxqq-wayland-fix \
      --replace-fail 'elif [[ -d /opt/QQ/resources/app ]]; then' \
      'elif [[ -d "''${QQ_WAYLAND_FIX_QQ_ROOT:-/opt/QQ}/resources/app" ]]; then' \
      --replace-fail '        echo /opt/QQ/resources/app' \
      '        echo "''${QQ_WAYLAND_FIX_QQ_ROOT:-/opt/QQ}/resources/app"' \
      --replace-fail '    [[ -x /opt/QQ/qq ]] && echo /opt/QQ/qq' \
      '    [[ -x "''${QQ_WAYLAND_FIX_QQ_ROOT:-/opt/QQ}/qq" ]] && echo "''${QQ_WAYLAND_FIX_QQ_ROOT:-/opt/QQ}/qq"'

    # 3. 上游在 PATH 里只找 linuxqq（AppImage 包的命令名）。nixpkgs 的 QQ 命令
    #    叫 qq，加上它，这样没有写死 QQ 路径时（没开 allowUnfree，见 flake.nix）
    #    启动器仍能从 PATH 里找到 nixpkgs 装的 QQ。
    substituteInPlace $out/bin/linuxqq-wayland-fix \
      --replace-fail '    elif command -v linuxqq >/dev/null 2>&1; then
        command -v linuxqq
' \
      '    elif command -v linuxqq >/dev/null 2>&1; then
        command -v linuxqq
    elif command -v qq >/dev/null 2>&1; then
        command -v qq
'

    # LD_LIBRARY_PATH：QQ 的 resources/app/avsdk/broadcast-core.so 是
    #   dlopen("libpipewire-0.3.so.0") + dlsym("pw_context_connect_fd") 来取 PipeWire 的，
    #   它自己的 RUNPATH 与 QQ bundle 里都没有 libpipewire。Debian/Arch 上 /usr/lib 本来
    #   就在默认搜索路径里，所以上游没管；NixOS 没有全局库目录、ld.so.cache 里也没有，
    #   dlopen 直接失败 → Wayland 采集分支起不来（屏幕共享不弹 portal、对方无画面）。
    #   把 pipewire 的 lib 目录交给 QQ 进程即可。
    #
    # EGL_PLATFORM：NixOS 的 glvnd 在没有任何平台提示时，会把 eglGetDisplay(EGL_DEFAULT_DISPLAY)
    #   交给 Mesa 厂商应答；Mesa 驱动不了 NVIDIA 闭源驱动，只能退化成 llvmpipe 软件渲染。
    #   后果是屏幕共享时插件进程里 12 个 llvmpipe 线程各占约 50%，合计约 5 个核（实测
    #   484%~570%）；采集/转换本可在 GPU 上做。显式声明 Wayland 平台后，同一个调用改由
    #   驱动的 EGL 应答，实测同样的共享降到 22%~81%。
    #   只在 Wayland 会话里设置，且不覆盖用户自己设定的值（X11 会话下保持原样）。
    #   （AMD/Intel 上 Mesa 能直接驱动硬件，这条基本是空操作；出问题的是 NVIDIA 闭源驱动。）
    #
    # VK_DRIVER_FILES：让上游启动器的 Vulkan 探测在 NixOS 上也能命中。它只看
    #   VK_DRIVER_FILES / VK_ICD_FILENAMES、/usr/share/vulkan/icd.d、/etc/vulkan/icd.d、
    #   XDG_DATA_HOME 下的 icd.d，而 NixOS 的 ICD 在 /run/opengl-driver/share/vulkan/icd.d，
    #   于是永远探测不到、拿不到 --use-angle=vulkan（上游在 Arch 上的默认行为，能避开部分
    #   设备上 Wayland + ANGLE 的 GLES 后端把共享画面渲染花的问题）。把该目录下的 ICD
    #   全部（各家厂商）交给启动器即可；用户自己设过就不动，不想要就 QQ_WAYLAND_FIX_ANGLE=off。
    #
    # 硬件编解码库路径（厂商中立，两条）：
    #
    # ① 把 /run/opengl-driver/lib 追加进 LD_LIBRARY_PATH。NixOS 把各家的硬件编解码库都
    #    聚合在这里：NVIDIA 的 libnvidia-encode.so / libnvcuvid.so / libcuda.so
    #    （NVENC/NVDEC/CUDA）、AMD 的 libamfrt64.so.1（AMF，来自 extraPackages 里的 amf）、
    #    Intel 的 libmfx.so.1（oneVPL/QSV，来自 vpl-gpu-rt）等等。
    #    broadcast-core.so 与系统 ffmpeg 的 libavcodec 都是用裸名 dlopen 这些库，
    #    libavcodec 自身没有 RUNPATH，只能靠 LD_LIBRARY_PATH 与 ld.so.cache。
    #    Arch/Debian 的 /usr/lib 本来就在搜索路径里，所以上游没管。不补这一步，采集帧
    #    只能落到 broadcast-core.so 自带的软件编码器（Openh264）。
    #    追加到末尾而不是开头：该目录顶层没有 libGL/libEGL/libgbm/libvulkan 这些加载器，
    #    放末尾既能让厂商库兜底命中，又不会遮蔽前面的 store/系统库。
    #
    # ② 补上 libva。broadcast-core.so 还会 dlopen("libva.so") 走 VA-API，而 NixOS 的
    #    /run/opengl-driver/lib 里不一定有 libva（看用户有没有把它加进
    #    hardware.graphics.extraPackages）。nixpkgs 的 libva 编译时就把驱动目录设成了
    #    /run/opengl-driver/lib/dri（各家驱动都在那儿：radeonsi / iHD / nvidia / nouveau /
    #    r600 …），所以补上它之后，AMD/Intel 的硬件编解码与 NVIDIA 的 nvidia-vaapi-driver
    #    都能直接用。
    #
    # PATH：补上启动器与 --doctor 依赖的小工具。
    # QQ_WAYLAND_FIX_QQ：指向 pkgs.qq 的启动脚本（它会自己处理 libssh2 预加载、
    #   gsettings / GIO 模块、NIXOS_OZONE_WL 等 NixOS 上必需的运行环境）。
    # QQ_WAYLAND_FIX_QQ_ROOT：上面第 2 个补丁用到的 QQ 安装目录。
    wrapProgram $out/bin/linuxqq-wayland-fix \
      --prefix PATH : ${lib.makeBinPath runtimeTools} \
      --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ libva (lib.getLib pipewire) ]} \
      --suffix LD_LIBRARY_PATH : /run/opengl-driver/lib \
      --run 'if [ -z "''${EGL_PLATFORM:-}" ] && [ -n "''${WAYLAND_DISPLAY:-}" ]; then export EGL_PLATFORM=wayland; fi' \
      --run 'if [ -z "''${VK_DRIVER_FILES:-}''${VK_ICD_FILENAMES:-}" ] && [ -d /run/opengl-driver/share/vulkan/icd.d ]; then icds=$(ls /run/opengl-driver/share/vulkan/icd.d/*.json 2>/dev/null | tr "\n" ":"); [ -n "$icds" ] && export VK_DRIVER_FILES="''${icds%:}"; fi' \
      ${lib.optionalString (qqPackage != null) ''
        --set QQ_WAYLAND_FIX_QQ ${lib.getExe' qqPackage "qq"} \
        --set QQ_WAYLAND_FIX_QQ_ROOT "${qqPackage}/opt/QQ"
      ''}
  '';

  meta = {
    description = "修复 Linux QQ 在 Wayland 下的屏幕共享、共享电脑声音、剪贴板和截图闪退";
    longDescription = ''
      通过 LD_PRELOAD 向 Linux QQ 注入四个小库，不修改 QQ 自身的文件：
      libqq-wl-portal.so（屏幕共享与共享设备音频）、libqq-clipbridge.so
      （X11 与 Wayland 剪贴板双向桥接）、libqq-screenshot.so（截图防闪退）、
      libqq-borderfix.so（共享时隐藏全屏边框窗口）。
      从应用菜单的「QQ（Wayland修复版）」启动，或用 linuxqq-wayland-fix --doctor 自检。
    '';
    homepage = "https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "linuxqq-wayland-fix";
  };
})
