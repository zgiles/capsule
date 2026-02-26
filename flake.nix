{
  description = "capsule — capability-based network namespace executor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }: let
    systems = [ "x86_64-linux" "aarch64-linux" ];
    forAll  = f: nixpkgs.lib.genAttrs systems f;
  in {
    overlays.default = final: _: {
      capsule = final.callPackage ./nix/package.nix { src = self; };
    };

    packages = forAll (system: {
      default = (nixpkgs.legacyPackages.${system}.extend self.overlays.default).capsule;
    });

    nixosModules.default = { ... }: {
      imports = [ ./nix/module.nix ];
      nixpkgs.overlays = [ self.overlays.default ];
    };
    nixosModules.capsule = self.nixosModules.default;
  };
}
