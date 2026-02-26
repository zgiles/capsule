{ config, lib, pkgs, ... }:

let
  cfg = config.programs.capsule;

  peerOpts = { ... }: {
    options = {
      publicKey = lib.mkOption {
        type        = lib.types.str;
        description = "WireGuard peer public key (base64).";
      };
      endpoint = lib.mkOption {
        type        = lib.types.str;
        example     = "vpn.example.com:51820";
        description = "WireGuard peer endpoint (host:port).";
      };
      allowedIPs = lib.mkOption {
        type        = lib.types.listOf lib.types.str;
        default     = [ "0.0.0.0/0" "::/0" ];
        description = "IP prefixes routed through this peer.";
      };
    };
  };

  wgOpts = { ... }: {
    options = {
      privateKeyFile = lib.mkOption {
        type        = lib.types.str;
        description = "Path to the WireGuard private key file. Read at service start; never stored in the Nix store. Compatible with agenix, sops-nix, and manual placement.";
      };
      address = lib.mkOption {
        type        = lib.types.str;
        example     = "10.0.0.2/24";
        description = "WireGuard interface address (CIDR). Comma-separated for dual-stack, e.g. \"10.0.0.2/24,fd00::2/128\".";
      };
      dns = lib.mkOption {
        type        = lib.types.nullOr lib.types.str;
        default     = null;
        example     = "10.0.0.1";
        description = "DNS server(s) written to /etc/netns/<name>/resolv.conf. Comma-separated for multiple servers.";
      };
      peers = lib.mkOption {
        type        = lib.types.listOf (lib.types.submodule peerOpts);
        default     = [];
        description = "WireGuard peer list.";
      };
    };
  };

  nsOpts = { ... }: {
    options = {
      wireguard = lib.mkOption {
        type        = lib.types.submodule wgOpts;
        description = "WireGuard configuration for this namespace.";
      };
      bindServices = lib.mkOption {
        type        = lib.types.listOf lib.types.str;
        default     = [];
        example     = [ "transmission.service" "myapp.service" ];
        description = ''
          Systemd service names to run inside this network namespace.
          Each listed service automatically receives:
            NetworkNamespacePath = /var/run/netns/<name>
            After                = capsule-netns-<name>.service
            Requires             = capsule-netns-<name>.service
          Bind a service to only one namespace.
        '';
      };
    };
  };

  # Build the oneshot service that owns a namespace's lifecycle.
  mkNsService = name: nsCfg: let
    confPath = "/run/capsule-netns/${name}.conf";

    genConf = pkgs.writeShellScript "capsule-netns-gen-${name}" ''
      set -eu
      {
        printf '[Interface]\n'
        printf 'PrivateKey = %s\n' "$(< ${lib.escapeShellArg nsCfg.wireguard.privateKeyFile})"
        printf 'Address = %s\n'    ${lib.escapeShellArg nsCfg.wireguard.address}
        ${lib.optionalString (nsCfg.wireguard.dns != null)
          "printf 'DNS = %s\n'     ${lib.escapeShellArg nsCfg.wireguard.dns}"
        }
        ${lib.concatMapStrings (p: ''
          printf '\n[Peer]\n'
          printf 'PublicKey = %s\n'   ${lib.escapeShellArg p.publicKey}
          printf 'Endpoint = %s\n'    ${lib.escapeShellArg p.endpoint}
          printf 'AllowedIPs = %s\n'  ${lib.escapeShellArg (lib.concatStringsSep ", " p.allowedIPs)}
        '') nsCfg.wireguard.peers}
      } > ${confPath}
      chmod 600 ${confPath}
    '';
  in {
    description = "capsule network namespace: ${name}";
    wantedBy    = [ "multi-user.target" ];
    after       = [ "network-online.target" ];
    wants       = [ "network-online.target" ];
    path        = [ pkgs.iproute2 pkgs.wireguard-tools ];
    serviceConfig = {
      Type                 = "oneshot";
      RemainAfterExit      = true;
      User                 = "root";
      ExecStartPre         = "${genConf}";
      ExecStart            = "${pkgs.capsule}/bin/capsule-netns-up ${name} ${confPath}";
      ExecStop             = "${pkgs.capsule}/bin/capsule-netns-down ${name}";
      CapabilityBoundingSet = [ "CAP_NET_ADMIN" "CAP_SYS_ADMIN" ];
      NoNewPrivileges      = true;
      ProtectSystem        = "strict";
      ProtectHome          = true;
      PrivateTmp           = true;
      ReadWritePaths       = [ "/etc/netns" "/run/netns" "/run/capsule-netns" ];
      RuntimeDirectory     = "capsule-netns";
      RuntimeDirectoryMode = "0700";
    };
  };

  # Extra ordering/namespace config merged into an existing service.
  mkBindOverride = name: svcName: {
    requires      = [ "capsule-netns-${name}.service" ];
    after         = [ "capsule-netns-${name}.service" ];
    serviceConfig = { NetworkNamespacePath = "/var/run/netns/${name}"; };
  };

in {
  options.programs.capsule = {
    enable = lib.mkEnableOption "capsule capability-based network namespace executor";

    namespaces = lib.mkOption {
      type        = lib.types.attrsOf (lib.types.submodule nsOpts);
      default     = {};
      description = "Network namespaces managed by capsule.";
      example     = lib.literalExpression ''
        {
          myvpn = {
            wireguard = {
              privateKeyFile = "/run/secrets/myvpn-wg-key";
              address        = "10.0.0.2/24";
              dns            = "10.0.0.1";
              peers = [{
                publicKey  = "server-public-key=";
                endpoint   = "vpn.example.com:51820";
                allowedIPs = [ "0.0.0.0/0" "::/0" ];
              }];
            };
            bindServices = [ "transmission.service" ];
          };
        }
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ pkgs.capsule ];

    security.wrappers.capsule = {
      source       = "${pkgs.capsule}/bin/capsule";
      capabilities = "cap_sys_admin,cap_dac_read_search+ep";
      owner        = "root";
      group        = "root";
    };

    systemd.services = lib.mkMerge (
      # Per-namespace lifecycle services
      (lib.mapAttrsToList (name: nsCfg:
        { "capsule-netns-${name}" = mkNsService name nsCfg; }
      ) cfg.namespaces)
      ++
      # bindServices — inject NetworkNamespacePath into each listed service
      (lib.concatLists (lib.mapAttrsToList (name: nsCfg:
        map (svcName:
          { ${svcName} = mkBindOverride name svcName; }
        ) nsCfg.bindServices
      ) cfg.namespaces))
    );
  };
}
