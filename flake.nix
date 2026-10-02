{
  description = "修复 Linux QQ 在 Wayland 下的屏幕共享、共享电脑声音、剪贴板和截图问题";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);

      mkPackage =
        pkgs: qq:
        pkgs.stdenv.mkDerivation (finalAttrs: {
          pname = "linuxqq-wayland-fix";
          version = self.shortRev or self.dirtyShortRev or "0.0.0";

          # 源码即本 flake（GitHub 上被 pin 到某个 commit，`nix flake update` 即可更新）。
          src = self;

          nativeBuildInputs = with pkgs; [
            gawk
            makeWrapper
            pkg-config
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            glib
            libpulseaudio
            libx11
            pipewire
            wayland
          ];

          enableParallelBuilding = true;

          makeFlags = [
            "PREFIX=${placeholder "out"}"
            "VERSION=${finalAttrs.version}"
            "PKG_CONFIG=${pkgs.pkg-config}/bin/pkg-config"
            "WAYLAND_SCANNER=${pkgs.lib.getExe pkgs.wayland-scanner}"
          ];

          postInstall = ''
            substituteInPlace $out/share/applications/linuxqq-wayland-fix.desktop \
              --replace-fail 'Exec=linuxqq-wayland-fix' "Exec=$out/bin/linuxqq-wayland-fix"
          '';

          # 启动器靠 QQ_WAYLAND_FIX_QQ 找到 nixpkgs 里的 QQ，并把自检所需的工具放进 PATH。
          # 注入库用 dlopen 按 soname 取这些库（不写死在 RUNPATH 里），NixOS 没有默认库搜索路径，
          # 必须加进 LD_LIBRARY_PATH：
          #   libpipewire-0.3.so.0  QQ 的 broadcast-core 采集（否则走不到 Wayland、共享选源框不弹出）；
          #   libXfixes.so.3        clipbridge 用 XFixes 追踪 X11 复制，缺失会退回较粗的判断；
          #   libXRes.so.1          clipbridge 用 XRes 分辨合成器的剪贴板代理窗口，缺失会误抢；
          #   libXrandr.so.2        screenshot 用它拼根窗口画面（QQ 经 GTK 也会加载，保险起见一并给出）。
          postFixup = ''
            wrapProgram $out/bin/linuxqq-wayland-fix \
              --set QQ_WAYLAND_FIX_QQ ${qq}/bin/qq \
              --prefix LD_LIBRARY_PATH : ${pkgs.pipewire}/lib \
              --prefix LD_LIBRARY_PATH : ${pkgs.xorg.libXfixes}/lib \
              --prefix LD_LIBRARY_PATH : ${pkgs.xorg.libXres}/lib \
              --prefix LD_LIBRARY_PATH : ${pkgs.xorg.libXrandr}/lib \
              --prefix PATH : ${
                pkgs.lib.makeBinPath (
                  with pkgs;
                  [
                    bash
                    coreutils
                    findutils
                    gawk
                    gnugrep
                    gnused
                    procps
                    systemd
                    wayland-utils
                  ]
                )
              }
          '';

          meta = {
            description = "Fix Linux QQ screen sharing, device audio, clipboard and screenshots on Wayland";
            homepage = "https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix";
            license = pkgs.lib.licenses.mit;
            platforms = pkgs.lib.platforms.linux;
            mainProgram = "linuxqq-wayland-fix";
          };
        });
    in
    {
      packages = forAllSystems (
        system:
        let
          # QQ 本体是 unfree，构建本包时显式放行。
          pkgs = import nixpkgs {
            inherit system;
            config.allowUnfree = true;
          };
          pkg = mkPackage pkgs pkgs.qq;
        in
        {
          default = pkg;
          linuxqq-wayland-fix = pkg;
        }
      );

      overlays.default = final: _prev: {
        linuxqq-wayland-fix = mkPackage final final.qq;
      };

      homeManagerModules.default =
        {
          config,
          lib,
          pkgs,
          ...
        }:
        {
          options.programs.linuxqq-wayland-fix = {
            enable = lib.mkEnableOption "Linux QQ Wayland 修复";
            package = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
              description = "要安装的 linuxqq-wayland-fix 包。";
            };
          };

          config = lib.mkIf config.programs.linuxqq-wayland-fix.enable {
            home.packages = [ config.programs.linuxqq-wayland-fix.package ];
          };
        };
    };
}
