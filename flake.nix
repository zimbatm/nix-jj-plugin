{
  description = "Jujutsu input scheme for Nix";

  # The plugin links against Nix's internals, so it must be built against the
  # very Nix that loads it. localRepoURL() is NixOS/nix#16507.
  inputs.nix.url = "github:zimbatm/nix/local-root-detection";

  outputs =
    { self, nix }:
    let
      forAllSystems = nix.inputs.nixpkgs.lib.genAttrs [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = nix.inputs.nixpkgs.legacyPackages.${system};
          nixPkgs = nix.packages.${system};
        in
        {
          default = self.packages.${system}.nix;

          # A plugin and the Nix that loads it have to be the same build, or
          # dlopen fails on an undefined symbol. Handing out one command that
          # already points at both is the only way to make that unmissable.
          nix = pkgs.writeShellScriptBin "nix" ''
            exec ${nixPkgs.nix}/bin/nix \
              --plugin-files ${self.packages.${system}.nix-jj-plugin}/lib/jj-plugin.so \
              "$@"
          '';

          nix-jj-plugin = pkgs.stdenv.mkDerivation {
            pname = "nix-jj-plugin";
            version = "0.1.0";
            src = ./src;

            nativeBuildInputs = [ pkgs.pkg-config ];
            buildInputs = [
              nixPkgs.nix-fetchers
              nixPkgs.nix-store
              nixPkgs.nix-util
              pkgs.nlohmann_json
              pkgs.boost
            ];

            buildPhase = ''
              $CXX -shared -fPIC -std=c++23 \
                $(pkg-config --cflags nix-fetchers nix-store nix-util) \
                -o jj-plugin.so jj.cc
            '';

            installPhase = ''
              install -Dm555 jj-plugin.so $out/lib/jj-plugin.so
            '';

            meta = {
              description = "Fetch Jujutsu workspaces as Nix flake inputs";
              platforms = pkgs.lib.platforms.unix;
            };
          };
        }
      );

      devShells = forAllSystems (system: {
        default = nix.devShells.${system}.default;
      });
    };
}
