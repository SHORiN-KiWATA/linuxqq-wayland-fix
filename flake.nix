{
  description = "linuxqq-wayland-fix：修复 Linux QQ 在 Wayland 下的屏幕共享、共享电脑声音、剪贴板和截图闪退";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    {
      self,
      nixpkgs,
      ...
    }:
    let
      inherit (nixpkgs) lib;

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      # 上游没有 VERSION 文件：版本只存在于 git tag 里，而 flake 求值看不到 tag
      # （self.sourceInfo.ref 无论按 tag 还是按分支固定都是 null，self 里也没有 ref）。
      # 所以版本串只用 git revision，不再手写上游版本号，也就不会再出现
      # "版本串和代码对不上"（LinuxQQ Wayland Fix v0.2.7-unstable-abc1234 那种）。
      version =
        if self ? shortRev then
          "unstable-${self.shortRev}"
        else if self ? dirtyShortRev then
          "unstable-${self.dirtyShortRev}"
        else
          "unstable";

      # nixpkgs 的 pkgs.qq 是 unfree。只有在 nixpkgs 允许 unfree 时才引用它，
      # 否则 packages / overlays 在默认 nixpkgs 上会直接求值报错。
      # 不允许时 packages.default 不带 QQ，启动器会退回自己的查找顺序：
      # $QQ_WAYLAND_FIX_QQ → PATH 里的 linuxqq → /opt/QQ/qq。
      #
      # 注意 flake 的 nixpkgs 会读 ~/.config/nixpkgs/config.nix，所以
      # { allowUnfree = true; } 写在那里可以让 packages.default 也带上 QQ。
      defaultQQ = pkgs: if pkgs.config.allowUnfree or false then pkgs.qq else null;

      # overrides 让 NixOS 模块把它自己的 qq 选项透传进来（模块那边用的是你系统的
      # pkgs，尊重你的 nixpkgs.config）。
      mkPackage =
        { pkgs, overrides ? { } }:
        pkgs.callPackage ./nix/package.nix (
          {
            source = self;
            inherit version;
            qqPackage = defaultQQ pkgs;
          }
          // overrides
        );

      module = import ./nix/module.nix {
        inherit mkPackage defaultQQ;
      };

      homeModule = import ./nix/home-module.nix {
        inherit mkPackage defaultQQ;
      };

      perSystem = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = perSystem (pkgs: {
        default = mkPackage { inherit pkgs; };
        linuxqq-wayland-fix = mkPackage { inherit pkgs; };
      });

      overlays.default = final: _prev: {
        linuxqq-wayland-fix = mkPackage { pkgs = final; };
      };

      nixosModules = {
        default = module;
        linuxqq-wayland-fix = module;
      };

      homeModules = {
        default = homeModule;
        linuxqq-wayland-fix = homeModule;
      };

      devShells = perSystem (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ (mkPackage { inherit pkgs; }) ];
        };
      });
    };
}
