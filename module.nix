{ self }:
{ config, lib, pkgs, ... }:

let
  cfg = config.services.nixly-server;
  pkg = self.packages.${pkgs.system}.default;

  configFile = pkgs.writeText "nixly-server.conf" ''
    port = ${toString cfg.port}
    db_path = /var/lib/nixly-server/nixly.db
    cache_dir = /var/cache/nixly-server
    upload_mbps = ${toString cfg.uploadMbps}
    server_name = ${cfg.serverName}
    tmdb_api_key = ${cfg.tmdbApiKey}
    tmdb_language = ${cfg.tmdbLanguage}
    tv_download_path = ${cfg.tvDownloadPath}
    movie_download_path = ${cfg.movieDownloadPath}
    tv_download_path2 = ${cfg.tvDownloadPath2}
    movie_download_path2 = ${cfg.movieDownloadPath2}
    auth_user = ${cfg.authUser}
    auth_password = ${cfg.authPassword}
    ${lib.concatMapStringsSep "\n"
      (p: "media_path = ${p}") cfg.mediaPaths}
  '';
in {
  options.services.nixly-server = {
    enable = lib.mkEnableOption "Nixly Media Server";

    port = lib.mkOption {
      type = lib.types.port;
      default = 8080;
      description = "HTTP listen port.";
    };

    serverName = lib.mkOption {
      type = lib.types.str;
      default = "nixly";
      description = "Human-readable server name shown to clients.";
    };

    uploadMbps = lib.mkOption {
      type = lib.types.int;
      default = 500;
      description = "Advertised upload bandwidth (informational; no per-stream throttle).";
    };

    tmdbApiKey = lib.mkOption {
      type = lib.types.str;
      default = "d415e076cfcbbe11dd7366a6e2f63321";
      description = "TMDB v3 API key for metadata fetching.";
    };

    tmdbLanguage = lib.mkOption {
      type = lib.types.str;
      default = "en-US";
      description = "TMDB language code.";
    };

    mediaPaths = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [];
      example = [ "/srv/media/TV" "/srv/media/Movies" ];
      description = "Directories scanned + watched for media files.";
    };

    tvDownloadPath = lib.mkOption {
      type = lib.types.str;
      default = "/var/lib/nixly-server/TV";
      description = "Destination for TV downloads via /wget.";
    };

    movieDownloadPath = lib.mkOption {
      type = lib.types.str;
      default = "/var/lib/nixly-server/Movies";
      description = "Destination for movie downloads via /wget.";
    };

    tvDownloadPath2 = lib.mkOption {
      type = lib.types.str;
      default = "/mnt/bigdisk2/media/TV";
      description = "Secondary TV destination on another disk; per download the disk with the most free space wins. Empty disables.";
    };

    movieDownloadPath2 = lib.mkOption {
      type = lib.types.str;
      default = "/mnt/bigdisk2/media/Movies";
      description = "Secondary movie destination on another disk; per download the disk with the most free space wins. Empty disables.";
    };

    authUser = lib.mkOption {
      type = lib.types.str;
      default = "nixly";
      description = "HTTP Basic Auth username.";
    };

    authPassword = lib.mkOption {
      type = lib.types.str;
      default = "nixlyadmin";
      description = "HTTP Basic Auth password. Change for non-private deployments.";
    };

    openFirewall = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Open the HTTP port + UDP discovery port (8081) in the firewall.";
    };
  };

  config = lib.mkIf cfg.enable {
    users.users.nixly = {
      isSystemUser = true;
      group = "nixly";
      home = "/var/lib/nixly-server";
      createHome = false;
    };
    users.groups.nixly = {};

    networking.firewall = lib.mkIf cfg.openFirewall {
      allowedTCPPorts = [ cfg.port ];
      allowedUDPPorts = [ 8081 ];
    };

    # Raise inotify watch limit — large libraries blow past the 8192 default.
    boot.kernel.sysctl."fs.inotify.max_user_watches" = lib.mkDefault 524288;

    systemd.tmpfiles.rules = [
      "d ${cfg.tvDownloadPath} 0755 nixly nixly - -"
      "d ${cfg.movieDownloadPath} 0755 nixly nixly - -"
    ];

    systemd.services.nixly-server = {
      description = "Nixly Media Server";
      after = [ "network.target" ];
      wantedBy = [ "multi-user.target" ];

      environment = {
        NIXLY_NO_BROWSER = "1";
        HOME = "/var/lib/nixly-server";
      };

      serviceConfig = {
        ExecStart = "${pkg}/bin/nixly-server -c ${configFile}";
        Restart = "on-failure";
        RestartSec = "5s";

        User = "nixly";
        Group = "nixly";

        StateDirectory = "nixly-server";
        StateDirectoryMode = "0750";
        CacheDirectory = "nixly-server";
        CacheDirectoryMode = "0750";

        # Allow streaming many concurrent files + watching big libraries.
        LimitNOFILE = 65536;

        # Reasonable hardening — server is read-mostly with writes to its
        # state + cache dirs and the configured media/download paths.
        NoNewPrivileges = true;
        ProtectSystem = "strict";
        ProtectHome = true;
        PrivateTmp = true;
        ReadWritePaths = [
          "/var/lib/nixly-server"
          "/var/cache/nixly-server"
          cfg.tvDownloadPath
          cfg.movieDownloadPath
        ] ++ cfg.mediaPaths;
      };
    };
  };
}
