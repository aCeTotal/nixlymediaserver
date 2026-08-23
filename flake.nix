{
  description = "Nixly Media Server - Lossless streaming server for movies and TV shows";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }@inputs:
    {
      nixosModules.default = import ./module.nix { inherit self; };
    } //
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "nixly-server";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [ pkgs.pkg-config pkgs.makeWrapper ];

          # Repo can contain stale .o files; store timestamps are all equal
          # so make would link them instead of recompiling.
          preBuild = "make clean";
          buildInputs = with pkgs; [
            ffmpeg-headless
            sqlite
            curl
            cjson
          ];

          installPhase = ''
            mkdir -p $out/bin
            cp nixly-server $out/bin/
            wrapProgram $out/bin/nixly-server \
              --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.xdg-utils ]}
          '';

          meta.mainProgram = "nixly-server";
        };

        devShells.default = pkgs.mkShell {
          name = "nixly-media-server";

          buildInputs = with pkgs; [
            # FFmpeg for media probing (not transcoding - we serve lossless)
            ffmpeg-headless

            # Database - SQLite for fast local database
            sqlite

            # C development
            gcc
            gnumake
            pkg-config

            # Networking and HTTP
            openssl
            curl

            # JSON parsing for TMDB responses
            cjson

            # Development tools
            gdb
            valgrind
          ];

          shellHook = ''
            echo "Nixly Media Server Development Environment"
            echo "==========================================="
            echo "FFmpeg:  $(ffmpeg -version | head -n1)"
            echo "SQLite:  $(sqlite3 --version)"
            echo ""
            echo "Build:   make"
            echo "Run:     ./nixly-server"
          '';
        };
      }
    );
}
