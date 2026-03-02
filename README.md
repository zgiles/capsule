# capsule

[![Build](https://github.com/zgiles/capsule/actions/workflows/build.yml/badge.svg)](https://github.com/zgiles/capsule/actions/workflows/build.yml)

**capsule** executes a program inside a pre-existing Linux network namespace — no sudo, no setuid root, no persistent daemon. A Nix flake and NixOS module are included.

```
capsule vpn firefox
```

---

## How it works

1. Opens the namespace file with `O_RDONLY | O_CLOEXEC` and verifies it is a network namespace via `fstatfs` / `NSFS_MAGIC`. Drops `cap_dac_read_search` from E and P immediately — it is held only for this single `open` call.
2. If `/etc/netns/<name>/resolv.conf` exists, and `<name>` is a simple non-empty basename (not `.`, `..`, or containing `/`), creates a fully private mount namespace (`MS_PRIVATE`) and bind-mounts the file over `/etc/resolv.conf` for DNS isolation.
3. If `-H hostname` is given, calls `unshare(CLONE_NEWUTS)` and `sethostname(2)` to give the child an isolated hostname, preventing hostname leakage through network traffic.
4. Calls `setns(CLONE_NEWNET)` to enter the network namespace.
5. Drops `cap_sys_admin`, `cap_dac_read_search`, and `cap_setpcap` from the capability bounding set via `prctl(PR_CAPBSET_DROP)` — before the E/P/I clear, because `PR_CAPBSET_DROP` requires `cap_setpcap` in the effective set.
6. Clears all capabilities: effective, permitted, inheritable, and ambient.
7. `execvp`s the command with the original UID/GID and unmodified environment.

`execvp` replaces the capsule process; the child's exit code propagates directly to the caller.

---

## Security

capsule is installed `setcap 'cap_sys_admin,cap_dac_read_search,cap_setpcap+ep'` — not setuid root.

| Property | Detail |
|---|---|
| File capabilities | `cap_sys_admin`, `cap_dac_read_search`, `cap_setpcap` in E+P only |
| `cap_dac_read_search` scope | Dropped from E+P immediately after the namespace `open` call |
| Namespace fd | `O_RDONLY | O_CLOEXEC` — `setns(2)` requires a real open; `cap_dac_read_search` covers mode-000 nsfs files |
| Bounding set | `cap_sys_admin`, `cap_dac_read_search`, `cap_setpcap` removed before exec |
| Child capabilities | None — E, P, I, ambient all empty; bounding set reduced |
| `list` / `status` paths | Drop all capabilities before unprivileged work |
| resolv.conf basename | Validated: non-empty, no `/`, `.`, or `..` |
| Mount isolation | `MS_PRIVATE` — no propagation in either direction |
| Root execution | Refused (UID 0 checked before any privileged operation) |
| Namespaces changed | Network only; mount, PID, UTS, IPC, user unaffected |

A child executing any binary with the same file capabilities cannot re-acquire `cap_sys_admin`, `cap_dac_read_search`, or `cap_setpcap` — all three are removed from the bounding set before exec.

### vs. `sudo ip netns exec`

| | `sudo ip netns exec` | capsule |
|---|---|---|
| Privilege model | Full root for the duration | Three file caps, each scoped to the minimum operation |
| Child privileges | Root unless policy says otherwise | Zero capabilities, original UID/GID |
| Attack surface | sudo engine, PAM, policy files | ~300 lines of C |
| Environment | Sanitised by sudo | Preserved (PATH, HOME, etc.) |
| Policy file | Required (`/etc/sudoers`) | None — one `setcap` on the binary |
| Bounding set | Unchanged | `cap_sys_admin`, `cap_dac_read_search`, `cap_setpcap` removed |

---

## Requirements

- Linux 3.19+ (`NSFS_MAGIC` namespace verification)
- Linux 4.3+ for ambient capability clearing (silently skipped on older kernels)
- `libcap` development headers (`libcap-dev` / `libcap-devel`)
- `libcap2-bin` (provides `setcap`) for non-Nix installation

```
# Debian/Ubuntu
sudo apt install libcap-dev libcap2-bin

# Fedora/RHEL
sudo dnf install libcap-devel libcap
```

---

## Build and install

```
make
sudo make install
```

`make install` copies the binary to `/usr/local/bin/capsule` and runs `setcap 'cap_sys_admin,cap_dac_read_search,cap_setpcap+ep'` automatically.

```
sudo make install PREFIX=/opt/capsule
sudo make uninstall
```

---

## Nix / NixOS

A Nix flake is included. The NixOS module manages namespace lifecycle as systemd services and sets file capabilities via `security.wrappers` — no manual `setcap` needed.

### Flake input

```nix
# flake.nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    capsule = {
      url = "github:zgiles/capsule";
      inputs.nixpkgs.follows = "nixpkgs";  # reuse your nixpkgs, avoids duplicate eval
    };
  };

  outputs = { self, nixpkgs, capsule }: {
    nixosConfigurations.mymachine = nixpkgs.lib.nixosSystem {
      modules = [
        capsule.nixosModules.default
        ./configuration.nix
      ];
    };
  };
}
```

### NixOS configuration

```nix
programs.capsule = {
  enable = true;  # installs binary + sets cap_sys_admin,cap_dac_read_search,cap_setpcap+ep

  namespaces.myvpn = {
    wireguard = {
      privateKeyFile = "/run/secrets/myvpn-wg-key";  # read at service start, never in Nix store
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
};
```

`bindServices` injects into each listed systemd service:

- `NetworkNamespacePath = /var/run/netns/myvpn` — service runs inside the namespace
- `After = capsule-netns-myvpn.service`
- `Requires = capsule-netns-myvpn.service`

`privateKeyFile` is compatible with [agenix](https://github.com/ryantm/agenix), [sops-nix](https://github.com/Mic92/sops-nix), and manual placement.

### Non-NixOS: nix profile

```bash
nix profile install github:zgiles/capsule
sudo setcap 'cap_sys_admin,cap_dac_read_search,cap_setpcap+ep' ~/.nix-profile/bin/capsule
```

`setcap` must be re-run after each upgrade (`security.wrappers` is not available outside NixOS).

---

## Setting up a WireGuard network namespace

Route specific applications through a VPN without affecting the rest of the system.

capsule ships `capsule-netns-up` / `capsule-netns-down` and a systemd service template.

### 1. Install dependencies

```bash
# Debian/Ubuntu
sudo apt install wireguard-tools iproute2

# Fedora/RHEL
sudo dnf install wireguard-tools iproute
```

### 2. Write a WireGuard config

Place a standard WireGuard config at `/etc/capsule/<name>.conf` (mode `600`). The `DNS =` field is required for DNS isolation:

```ini
[Interface]
PrivateKey = <your-private-key>
Address    = 10.0.0.2/24
DNS        = 10.0.0.1

[Peer]
PublicKey  = <server-public-key>
Endpoint   = vpn.example.com:51820
AllowedIPs = 0.0.0.0/0, ::/0
```

```bash
sudo install -d -m 750 /etc/capsule
sudo install -m 600 wg0.conf /etc/capsule/myvpn.conf
```

### 3. Start the namespace

```bash
sudo systemctl enable --now capsule-netns@myvpn
```

On start: `capsule-netns-up myvpn /etc/capsule/myvpn.conf`.
On stop: `capsule-netns-down myvpn`.

### 4. Verify and use

```bash
capsule myvpn ip addr show wg0
capsule myvpn curl -s https://ifconfig.me
capsule myvpn firefox
capsule myvpn transmission-gtk
```

---

### WireGuard namespace topology

`capsule-netns-up` creates the WireGuard interface in the default namespace, configures it there (binding the UDP transport socket to the default namespace), then moves the interface into the VPN namespace:

```
default namespace                    VPN namespace
─────────────────                    ─────────────
eth0 ──► internet                    wg0  (decrypted traffic)
UDP socket ──► VPN peer                │
     ▲                                 │
     └── encrypted tunnel ─────────────┘
```

The WireGuard kernel module binds its UDP socket to whichever namespace the interface is in **at the time `wg setconf` is called**. Creating the interface directly inside the VPN namespace would trap the UDP socket there with no internet access.

```bash
ip link add wg-tmp type wireguard      # create in default namespace
wg setconf wg-tmp /etc/capsule/x.conf  # UDP socket anchored in default ns
ip link set wg-tmp netns vpn name wg0  # move+rename; socket stays behind
```

---

### DNS isolation

`capsule-netns-up` writes the VPN's DNS servers to `/etc/netns/<nsname>/resolv.conf`. When capsule runs with a simple named namespace and that file exists, it bind-mounts it over `/etc/resolv.conf` in a private mount namespace before entering the network namespace.

If `/etc/resolv.conf` points to a local stub resolver (`127.0.0.53`, `127.0.0.1`), that resolver is unreachable inside the network namespace and DNS will fail. Options:

- `sudo ip netns exec myvpn curl -s https://ifconfig.me` — uses iproute2's bind-mount
- Configure per-app DNS (e.g. Firefox DNS-over-HTTPS)
- Run `unbound` or `nscd` bound to a non-loopback address reachable inside the namespace

---

### Manual setup

```bash
sudo capsule-netns-up   myvpn /etc/capsule/myvpn.conf
# ... use capsule ...
sudo capsule-netns-down myvpn
```

---

## Usage

```
capsule [-H hostname] <netns> <command> [args...]
capsule list [-v]
capsule status [-v] <netns>
```

```bash
capsule vpn firefox                        # short namespace name
capsule /var/run/netns/vpn firefox         # explicit path
capsule vpn bash                           # interactive shell for debugging
capsule /proc/1234/ns/net ip route         # any process's network namespace
capsule vpn curl -s https://ifconfig.me
capsule -H vpn-host vpn firefox            # isolated hostname to prevent leakage
```

---

## Inspecting namespaces

```bash
capsule list          # list all namespaces in /var/run/netns
capsule list -v       # also show processes in each
capsule status vpn    # interfaces + processes for vpn
capsule status /proc/$$/ns/net
```

`capsule list -v`:

```
myvpn
  PID       COMMAND
  1234      firefox
  5678      curl

work
  (no processes)

```

`capsule status vpn`:

```
Namespace: vpn (/var/run/netns/vpn)

Interfaces:
  lo
  wg0

Processes:
  PID       COMMAND
  1234      firefox
  5678      curl

```

---

## License

MIT — see [LICENSE](LICENSE).
