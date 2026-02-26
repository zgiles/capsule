# capsule

[![Build](https://github.com/zgiles/capsule/actions/workflows/build.yml/badge.svg)](https://github.com/zgiles/capsule/actions/workflows/build.yml)

**capsule** lets an unprivileged user execute a program inside a pre-existing
Linux network namespace — no sudo, no setuid root, no persistent daemon.

```
capsule vpn firefox
```

---

## What it does

1. Opens the network namespace file you specify.
2. Verifies the file is actually a network namespace (via `fstatfs` / `NSFS_MAGIC`).
3. Optionally bind-mounts `/etc/netns/<name>/resolv.conf` over `/etc/resolv.conf`
   for DNS isolation (same convention as `ip netns exec`).
4. Calls `setns(CLONE_NEWNET)` to enter the namespace — the only step that requires
   elevated privilege.
5. **Drops every capability** (effective, permitted, inheritable, ambient) using
   libcap before `exec`, so the child process is fully unprivileged.
6. **Drops `cap_sys_admin` and `cap_dac_read_search` from the bounding set**,
   preventing child processes from re-acquiring these capabilities via a setcap binary.
7. `execvp`s your command with the original UID/GID and the original environment
   untouched.

Because `execvp` replaces the capsule process, the child's exit code propagates
directly to the caller.

---

## Why it is safer than sudo

| | `sudo ip netns exec` | capsule |
|---|---|---|
| Privilege model | Full root for the duration | `cap_sys_admin` only, and only for one `setns` call |
| Child privileges | Runs as root unless policy says otherwise | Runs with **zero** capabilities, original UID/GID |
| Attack surface | Full sudo policy engine, PAM, etc. | ~250 lines of C, one syscall |
| Environment | Sanitised by sudo | Fully preserved (your PATH, HOME, etc.) |
| Requires policy file | Yes (`/etc/sudoers`) | No — one `setcap` on the binary |
| Bounding set | Unchanged | `cap_sys_admin` + `cap_dac_read_search` dropped |

capsule is installed with `setcap 'cap_sys_admin,cap_dac_read_search+ep'`.  It is
**not** setuid root.  The file capabilities are active only while capsule is
running; the child has none.

---

## Requirements

- Linux 3.19+ (for `NSFS_MAGIC` / namespace fd verification)
- Linux 4.3+ for ambient capability clearing (gracefully ignored on older kernels)
- `libcap` development headers and library (`libcap-dev` / `libcap-devel`)
- `libcap2-bin` (provides `setcap`) for installation

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

`make install` copies the binary to `/usr/local/bin/capsule`, then runs
`setcap 'cap_sys_admin,cap_dac_read_search+ep'` on it automatically.  No
password is required after that point — any user on the machine can use it.

To install to a different prefix:

```
sudo make install PREFIX=/opt/capsule
```

To remove:

```
sudo make uninstall
```

---

## Nix / NixOS

### Using the flake

Add capsule to your flake inputs and import the module:

```nix
# flake.nix
{
  inputs.capsule.url = "github:zgiles/capsule";

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

### NixOS module configuration

```nix
# configuration.nix
{
  programs.capsule = {
    enable = true;

    namespaces.myvpn = {
      wireguard = {
        privateKeyFile = "/run/secrets/myvpn-wg-key";  # agenix / sops-nix / manual
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
}
```

`bindServices` automatically injects into each listed service:

- `NetworkNamespacePath = /var/run/netns/myvpn` — runs the service inside the VPN namespace
- `After = capsule-netns-myvpn.service` — ensures the namespace is ready before the service starts
- `Requires = capsule-netns-myvpn.service` — stops the service if the namespace goes down

**`privateKeyFile`** is read at service start by the `ExecStartPre` script; the private key
never enters the Nix store.  It is compatible with
[agenix](https://github.com/ryantm/agenix),
[sops-nix](https://github.com/Mic92/sops-nix),
and manual key placement.

The `enable` block installs the binary via `security.wrappers` with
`cap_sys_admin,cap_dac_read_search+ep`, so no manual `setcap` is needed on NixOS.

### Non-NixOS: nix profile install

```bash
nix profile install github:zgiles/capsule
# Then grant capabilities manually (requires root or cap_setfcap):
sudo setcap 'cap_sys_admin,cap_dac_read_search+ep' \
    ~/.nix-profile/bin/capsule
```

Note: nix profile installations do not use `security.wrappers`, so you must
run `setcap` manually after install and after each upgrade.

---

## Setting up a WireGuard network namespace

This is the canonical use-case: route specific applications through a VPN
without affecting the rest of the system.

capsule ships two helper scripts — `capsule-netns-up` / `capsule-netns-down`
— and a systemd service template that automate the full setup.

### 1. Install WireGuard tools

```bash
# Debian/Ubuntu
sudo apt install wireguard-tools iproute2

# Fedora/RHEL
sudo dnf install wireguard-tools iproute
```

### 2. Write a WireGuard config

Place a standard WireGuard config at `/etc/capsule/<name>.conf` (mode `600`).
The `DNS =` field in `[Interface]` is required for DNS isolation:

```ini
# /etc/capsule/myvpn.conf
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

### 3. Enable the systemd service

The service template `capsule-netns@.service` takes the namespace name as its
instance specifier and expects the config at `/etc/capsule/<name>.conf`:

```bash
sudo systemctl enable --now capsule-netns@myvpn
```

On start it calls `capsule-netns-up myvpn /etc/capsule/myvpn.conf`.
On stop (or reboot) it calls `capsule-netns-down myvpn`.

### 4. Verify connectivity

```bash
capsule myvpn ip addr show wg0
capsule myvpn curl -s https://ifconfig.me
```

### 5. Launch applications through the VPN

```bash
capsule myvpn firefox
capsule myvpn chromium --no-sandbox
capsule myvpn transmission-gtk
```

---

### How the WireGuard namespace topology works

`capsule-netns-up` creates the WireGuard interface in the **default namespace**
first, configures it there, then moves it into the VPN namespace:

```
default namespace                    VPN namespace
─────────────────                    ─────────────
eth0 ──► internet                    wg0  (decrypted traffic)
UDP socket ──► VPN peer                │
     ▲                                 │
     └── encrypted tunnel ─────────────┘
```

WireGuard's kernel module binds its UDP socket (the encrypted transport to the
VPN peer) to whichever namespace the interface is in **at the time it is
configured via `wg setconf`**.  After `ip link set wg0 netns <nsname>` moves
the interface, the UDP socket stays in the default namespace.

The sequence in the script is therefore:

```bash
ip link add wg-tmp type wireguard      # create in default namespace
wg setconf wg-tmp /etc/capsule/x.conf  # UDP socket anchored here
ip link set wg-tmp netns vpn name wg0  # move+rename; socket stays behind
# — now configure addr/routes inside vpn namespace —
```

If the interface were created **directly inside** the VPN namespace (which has
no physical interface), the UDP socket would be trapped there with no internet
access and WireGuard could never reach the peer.

---

### DNS isolation

`capsule-netns-up` writes the DNS servers from the `DNS =` field of the
WireGuard config to `/etc/netns/<nsname>/resolv.conf`.  `iproute2`'s
`ip netns exec` automatically bind-mounts this file over `/etc/resolv.conf`
for any process it spawns inside the namespace.

`capsule` enters only the network namespace (not the mount namespace), so
the bind-mount does not happen automatically.  DNS isolation still works,
but with one caveat depending on your system:

- **If `/etc/resolv.conf` lists a public DNS server** (e.g. `8.8.8.8`):
  DNS queries route through `wg0` because all traffic uses the default route
  inside the namespace.  DNS is transparently VPN-isolated.

- **If `/etc/resolv.conf` points to a local stub resolver** (e.g.
  `127.0.0.53` for systemd-resolved, or `127.0.0.1` for dnsmasq): that
  resolver is not running inside the network namespace, so DNS will fail.

  **Fix:** tell systemd-resolved to write its upstream DNS to a real file
  and point capsule-launched apps at it, or configure a per-app resolver:

  ```bash
  # Quick workaround: override resolv.conf with the VPN's DNS server
  # (capsule-netns-up sets /etc/netns/<nsname>/resolv.conf, so use ip netns exec
  #  when you need the bind-mount, or set DNS in the app directly)

  # Alternative: use ip netns exec for DNS-sensitive one-off commands
  sudo ip netns exec myvpn curl -s https://ifconfig.me
  ```

  For `capsule` with a local stub resolver, the recommended long-term
  solution is to add the VPN's DNS server address to the app's own resolver
  config (e.g. Firefox's DNS-over-HTTPS setting), or to use `nscd`/`unbound`
  bound to a non-loopback address reachable inside the namespace.

---

### Manual setup (without systemd)

If you prefer not to use the service template:

```bash
sudo capsule-netns-up   myvpn /etc/capsule/myvpn.conf
# ... use capsule ...
sudo capsule-netns-down myvpn
```

---

## Usage examples

```
capsule <netns> <command> [args...]
```

```bash
# Open Firefox through the VPN namespace (short name)
capsule vpn firefox

# Same using an explicit path
capsule /var/run/netns/vpn firefox

# Run a shell in the namespace for debugging
capsule vpn bash

# Use a namespace from /proc (any process's net namespace)
capsule /proc/1234/ns/net ip route

# Check which external IP is visible
capsule vpn curl -s https://ifconfig.me
```

---

## Inspecting namespaces

### List namespaces

```bash
capsule list          # list all named namespaces in /var/run/netns
capsule list -v       # also show processes running in each namespace
```

Example output of `capsule list -v`:

```
myvpn
  PID       COMMAND
  1234      firefox
  5678      curl

work
  (no processes)

```

### Namespace status

```bash
capsule status vpn              # interfaces + processes for vpn
capsule status /proc/$$/ns/net  # works with full paths too
```

Example output of `capsule status vpn`:

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

## Security notes

- capsule **refuses to run as root** (UID 0).  If you need to enter a namespace
  as root, use `ip netns exec` or `nsenter` directly.
- The child process has **no capabilities** whatsoever — not even `cap_net_raw`
  or `cap_net_bind_service`.  If the child needs those, they must be granted
  separately (e.g. another `setcap` on the child binary).
- After dropping E/P/I/ambient sets, capsule also **drops `cap_sys_admin` and
  `cap_dac_read_search` from the bounding set** via `prctl(PR_CAPBSET_DROP)`.
  This prevents a child process that somehow executes the capsule binary (or
  any other binary with the same file capabilities) from re-acquiring those
  capabilities.
- capsule does not touch the mount, PID, UTS, IPC, or user namespaces — only
  the network namespace is changed.
- The binary should be owned by `root:root` with mode `0755`.  Do not make it
  group- or world-writable.

---

## License

MIT — see [LICENSE](LICENSE).
