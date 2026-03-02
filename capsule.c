/*
 * capsule — enter a network namespace and exec a command, then drop all caps.
 *
 * Install with:  setcap 'cap_sys_admin,cap_dac_read_search,cap_setpcap+ep' /usr/local/bin/capsule
 *
 * The binary is NOT setuid.  It acquires three capabilities via file caps:
 *   cap_dac_read_search — open the mode-000 namespace file (nsfs inodes
 *                         do not support setattr, so chmod is not possible)
 *   cap_sys_admin       — call setns(2) to enter the network namespace
 *   cap_setpcap         — drop cap_sys_admin, cap_dac_read_search, and
 *                         cap_setpcap itself from the bounding set before exec
 * All three are dropped before exec so the child runs fully unprivileged.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/capability.h>

/* Magic number for the kernel nsfs (Linux ≥ 3.19). */
#ifndef NSFS_MAGIC
# define NSFS_MAGIC 0x6e736673
#endif

/* ------------------------------------------------------------------ helpers */

static void __attribute__((noreturn))
die_errno(const char *ctx)
{
    fprintf(stderr, "capsule: %s: %s\n", ctx, strerror(errno));
    exit(1);
}

static void __attribute__((noreturn))
die(const char *msg)
{
    fprintf(stderr, "capsule: %s\n", msg);
    exit(1);
}

/* -------------------------------------------------------------- drop_all_caps */

/*
 * Clear the effective, permitted, and inheritable capability sets.
 * Called by list/status paths after their privileged work is done, and by
 * the exec path just before execvp.  Does NOT clear the ambient set — callers
 * that exec must do that separately with PR_CAP_AMBIENT_CLEAR_ALL.
 */
static void
drop_all_caps(void)
{
    cap_t empty = cap_init();
    if (!empty)
        die_errno("cap_init");
    if (cap_set_proc(empty) < 0)
        die_errno("cap_set_proc");
    cap_free(empty);
}

/* ---------------------------------------------------------------- open_netns */

/*
 * Open a network namespace file, verify it is really a namespace, and return
 * an open fd.  The resolved path is written into resolved[0..rsz-1].
 *
 * If arg contains no '/' the short-name form is assumed and the path is
 * constructed as /var/run/netns/<arg>.  Full paths are used verbatim.
 * Calls die() on any error.
 */
static int
open_netns(const char *arg, char *resolved, size_t rsz)
{
    int n;
    if (strchr(arg, '/') == NULL)
        n = snprintf(resolved, rsz, "/var/run/netns/%s", arg);
    else
        n = snprintf(resolved, rsz, "%s", arg);

    if (n <= 0 || n >= (int)rsz)
        die("namespace path too long");

    /* O_RDONLY: setns(2) requires a real open fd — O_PATH fds are rejected with
     * EBADF because the kernel checks that the fd has ns_file_operations, which
     * are replaced by a stub for O_PATH.  cap_dac_read_search is held at this
     * point specifically to open mode-000 nsfs files. */
    int fd = open(resolved, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        die_errno(resolved);

    struct statfs sfs;
    if (fstatfs(fd, &sfs) < 0)
        die_errno("fstatfs on netns file");

    if ((long)sfs.f_type != (long)NSFS_MAGIC) {
        fprintf(stderr,
                "capsule: %s: not a network namespace "
                "(f_type=0x%lx, expected 0x%lx)\n",
                resolved,
                (unsigned long)sfs.f_type,
                (unsigned long)NSFS_MAGIC);
        exit(1);
    }

    return fd;
}

/* --------------------------------------------------------------- list_procs */

/*
 * Scan /proc for processes whose network namespace inode matches (ns_ino,
 * ns_dev) and print a PID / COMMAND table.  Prints "(no processes)" if none
 * are found.
 */
static void
list_procs(ino_t ns_ino, dev_t ns_dev)
{
    DIR *proc = opendir("/proc");
    if (!proc)
        die_errno("opendir /proc");

    int found = 0;
    struct dirent *de;
    while ((de = readdir(proc)) != NULL) {
        /* Only purely-numeric names are PIDs. */
        char *p = de->d_name;
        if (*p < '1' || *p > '9')
            continue;
        int ok = 1;
        for (p++; *p; p++) {
            if (*p < '0' || *p > '9') { ok = 0; break; }
        }
        if (!ok)
            continue;

        long pid = strtol(de->d_name, NULL, 10);

        char ns_path[48];
        snprintf(ns_path, sizeof(ns_path), "/proc/%ld/ns/net", pid);
        struct stat st;
        if (stat(ns_path, &st) < 0)
            continue;
        if (st.st_ino != ns_ino || st.st_dev != ns_dev)
            continue;

        char comm_path[48];
        snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", pid);
        char comm[64] = "(unknown)";
        FILE *f = fopen(comm_path, "r");
        if (f) {
            if (fgets(comm, sizeof(comm), f))
                comm[strcspn(comm, "\n")] = '\0';
            fclose(f);
        }

        if (!found)
            printf("  %-10s%s\n", "PID", "COMMAND");
        printf("  %-10s%s\n", de->d_name, comm);
        found = 1;
    }
    closedir(proc);

    if (!found)
        printf("  (no processes)\n");
}

/* -------------------------------------------------------------- list_ifaces */

/*
 * Temporarily enter the network namespace referenced by nsfd, enumerate
 * interfaces via /proc/net/dev, then restore the original namespace.
 */
static void
list_ifaces(int nsfd)
{
    int orig = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    if (orig < 0)
        die_errno("open /proc/self/ns/net");

    if (setns(nsfd, CLONE_NEWNET) < 0)
        die_errno("setns(CLONE_NEWNET)");

    FILE *f = fopen("/proc/net/dev", "r");
    if (f) {
        char line[256];
        int lineno = 0;
        while (fgets(line, sizeof(line), f)) {
            if (++lineno <= 2)
                continue;   /* skip two header lines */
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            *colon = '\0';
            char *name = line;
            while (*name == ' ' || *name == '\t')
                name++;
            printf("  %s\n", name);
        }
        fclose(f);
    }

    if (setns(orig, CLONE_NEWNET) < 0)
        die_errno("setns back to original namespace");
    close(orig);
}

/* ---------------------------------------------------------------- cmd_list  */

static int
cmd_list(int argc, char *argv[])
{
    /* list needs no elevated capabilities; drop them all before doing anything. */
    drop_all_caps();

    int verbose = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--details") == 0)
            verbose = 1;
    }

    DIR *d = opendir("/var/run/netns");
    if (!d) {
        if (errno == ENOENT)
            printf("no named network namespaces found\n");
        else
            die_errno("opendir /var/run/netns");
        return 0;
    }

    int found = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        printf("%s\n", de->d_name);
        if (verbose) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "/var/run/netns/%s", de->d_name);
            struct stat st;
            if (stat(path, &st) == 0)
                list_procs(st.st_ino, st.st_dev);
            printf("\n");
        }
        found = 1;
    }
    closedir(d);

    if (!found)
        printf("no named network namespaces found\n");

    return 0;
}

/* --------------------------------------------------------------- cmd_status */

static int
cmd_status(int argc, char *argv[])
{
    const char *name = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            ;   /* accepted but reserved; status always shows full detail */
        else if (!name)
            name = argv[i];
    }

    if (!name) {
        fprintf(stderr, "capsule status: missing namespace argument\n");
        return 1;
    }

    char resolved[PATH_MAX];
    int nsfd = open_netns(name, resolved, sizeof(resolved));

    /* Drop cap_dac_read_search immediately after opening the namespace file —
     * same as the exec path.  list_ifaces only needs cap_sys_admin (for setns). */
    {
        cap_t caps = cap_get_proc();
        if (!caps)
            die_errno("cap_get_proc");
        cap_value_t drop_dac[] = { CAP_DAC_READ_SEARCH };
        if (cap_set_flag(caps, CAP_EFFECTIVE, 1, drop_dac, CAP_CLEAR) < 0 ||
            cap_set_flag(caps, CAP_PERMITTED, 1, drop_dac, CAP_CLEAR) < 0 ||
            cap_set_proc(caps) < 0)
            die_errno("cap_set_proc: failed to drop cap_dac_read_search");
        cap_free(caps);
    }

    struct stat st;
    if (fstat(nsfd, &st) < 0)
        die_errno("fstat on namespace fd");

    printf("Namespace: %s (%s)\n\n", name, resolved);
    printf("Interfaces:\n");
    list_ifaces(nsfd);
    /* cap_sys_admin no longer needed. */
    drop_all_caps();
    printf("\nProcesses:\n");
    list_procs(st.st_ino, st.st_dev);
    printf("\n");

    close(nsfd);
    return 0;
}

/* -------------------------------------------------------------------- usage */

static void __attribute__((noreturn))
usage(void)
{
    fputs("usage: capsule [-H hostname] <netns> <command> [args...]\n"
          "       capsule list [-v]\n"
          "       capsule status [-v] <netns>\n"
          "\n"
          "  -H hostname  unshare UTS namespace and set hostname\n"
          "  netns    namespace name (looks up /var/run/netns/<name>)\n"
          "           or full path (/var/run/netns/vpn, /proc/<pid>/ns/net)\n"
          "  command  program to execute inside that namespace\n"
          "  args     optional arguments forwarded to command\n"
          "  list     list all named network namespaces\n"
          "  status   show interfaces and processes for a namespace\n"
          "  -v       with list: show processes per namespace\n",
          stderr);
    exit(1);
}

/* -------------------------------------------------------------------- main  */

int
main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "list") == 0)
        return cmd_list(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "status") == 0)
        return cmd_status(argc, argv);

    /* ----------------------------------------------------------------
     * Parse optional -H <hostname> flag.
     *
     * When present, capsule will unshare a new UTS namespace and set
     * the hostname before exec, so the child sees an isolated hostname
     * rather than the host's.  argi tracks the position of the netns
     * argument after consuming any flags.
     * ---------------------------------------------------------------- */
    const char *uts_hostname = NULL;
    int         argi         = 1;
    if (argc > 1 && strcmp(argv[1], "-H") == 0) {
        if (argc < 4)
            usage();
        uts_hostname = argv[2];
        if (uts_hostname[0] == '\0')
            die("-H: hostname must not be empty");
        if (strlen(uts_hostname) > HOST_NAME_MAX)
            die("-H: hostname too long");
        argi = 3;
    }

    if (argc < argi + 2)
        usage();

    /* ----------------------------------------------------------------
     * Safety check: refuse to run when the real UID is 0.
     *
     * This is checked before any capability-assisted operations so that
     * a root caller cannot use file capabilities to bypass the guard.
     * capsule is designed for unprivileged users; root should use
     * `ip netns exec` or nsenter directly.
     * ---------------------------------------------------------------- */
    if (getuid() == 0)
        die("refusing to run as root (UID 0)");

    char **cmd = &argv[argi + 1];

    /* ----------------------------------------------------------------
     * Resolve the netns argument and open the namespace file.
     *
     * If the argument contains no '/' treat it as a short name and
     * use /var/run/netns/<name>.  Paths (anything containing '/') are
     * used directly, so /var/run/netns/vpn and /proc/<pid>/ns/net
     * all work as-is.
     * ---------------------------------------------------------------- */
    char        netns_resolved[PATH_MAX];
    int         nsfd      = open_netns(argv[argi], netns_resolved,
                                       sizeof(netns_resolved));
    const char *netns_path = netns_resolved;

    /* Drop cap_dac_read_search immediately — only needed to open the namespace
     * file above.  The remaining steps (unshare, mount, setns) need only
     * cap_sys_admin; cap_setpcap is needed later for bounding-set drops. */
    {
        cap_t caps = cap_get_proc();
        if (!caps)
            die_errno("cap_get_proc");
        cap_value_t drop_dac[] = { CAP_DAC_READ_SEARCH };
        if (cap_set_flag(caps, CAP_EFFECTIVE, 1, drop_dac, CAP_CLEAR) < 0 ||
            cap_set_flag(caps, CAP_PERMITTED, 1, drop_dac, CAP_CLEAR) < 0 ||
            cap_set_proc(caps) < 0)
            die_errno("cap_set_proc: failed to drop cap_dac_read_search");
        cap_free(caps);
    }

    /* ----------------------------------------------------------------
     * Set up DNS isolation via a private mount namespace.
     *
     * Convention (same as ip-netns(8)): if /etc/netns/<name>/resolv.conf
     * exists, bind-mount it over /etc/resolv.conf so the child sees the
     * VPN's DNS servers rather than the host's.
     *
     * We derive <name> from the basename of netns_path, so this works
     * automatically for paths under /var/run/netns/.  For other paths
     * (e.g. /proc/<pid>/ns/net) the file won't exist and the step is
     * silently skipped — no DNS isolation, but everything else works.
     *
     * Steps:
     *   1. unshare(CLONE_NEWNS)  — private copy of the mount namespace.
     *   2. MS_PRIVATE|MS_REC on / — fully isolated, no propagation either way.
     *   3. MS_BIND the resolv.conf into place.
     * ---------------------------------------------------------------- */
    {
        /* basename without modifying netns_path */
        const char *nsname = strrchr(netns_path, '/');
        nsname = nsname ? nsname + 1 : netns_path;

        /* Only look up /etc/netns/<name>/resolv.conf for a simple, non-empty
         * basename with no path separators and not '.' or '..'.  A crafted
         * full path (e.g. /proc/<pid>/ns/net) can yield an unusual basename;
         * skip the bind-mount rather than risk targeting an unexpected path. */
        char resolv_src[PATH_MAX];
        int  have_resolv = 0;
        if (nsname[0] != '\0' &&
            strcmp(nsname, ".")  != 0 &&
            strcmp(nsname, "..") != 0 &&
            strchr(nsname, '/') == NULL) {
            int resolv_len = snprintf(resolv_src, sizeof(resolv_src),
                                      "/etc/netns/%s/resolv.conf", nsname);
            struct stat st;
            have_resolv = (resolv_len > 0 &&
                           resolv_len < (int)sizeof(resolv_src) &&
                           stat(resolv_src, &st) == 0);
        }

        if (have_resolv) {
            if (unshare(CLONE_NEWNS) < 0)
                die_errno("unshare(CLONE_NEWNS)");

            /* Fully isolate the mount namespace — no propagation either way. */
            if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
                die_errno("mount --make-rprivate /");

            if (mount(resolv_src, "/etc/resolv.conf", NULL, MS_BIND, NULL) < 0)
                die_errno("bind-mount /etc/resolv.conf");
        }
    }

    /* ----------------------------------------------------------------
     * UTS namespace isolation (-H flag).
     *
     * Unshare a new UTS namespace and set the requested hostname so
     * the child process cannot observe or leak the host's real hostname.
     * cap_sys_admin is required for unshare(CLONE_NEWUTS).
     * ---------------------------------------------------------------- */
    if (uts_hostname != NULL) {
        if (unshare(CLONE_NEWUTS) < 0)
            die_errno("unshare(CLONE_NEWUTS)");
        if (sethostname(uts_hostname, strlen(uts_hostname)) < 0)
            die_errno("sethostname");
    }

    /* ----------------------------------------------------------------
     * Enter the network namespace.
     *
     * cap_sys_admin is required for both unshare(CLONE_NEWNS) above
     * and setns(CLONE_NEWNET) here.
     * ---------------------------------------------------------------- */
    if (setns(nsfd, CLONE_NEWNET) < 0)
        die_errno("setns(CLONE_NEWNET)");

    close(nsfd);

    /* ----------------------------------------------------------------
     * Drop capsule's file capabilities from the bounding set.
     *
     * prctl(PR_CAPBSET_DROP) requires CAP_SETPCAP in the caller's
     * effective set, so this step MUST come before cap_set_proc()
     * clears the effective set below.
     *
     * We drop the three caps that appear in capsule's file capability
     * set (sys_admin, dac_read_search, setpcap).  All others remain in
     * the bounding set so that tools inside the child (sudo, ping, etc.)
     * continue to work normally.
     *
     * Even though E/P/I will be empty after the next step, removing
     * these from the bounding set closes the re-activation path: a
     * child that executes a copy of capsule (or any binary carrying the
     * same file caps) cannot re-acquire these capabilities and escape
     * the network namespace.
     * ---------------------------------------------------------------- */
    if (prctl(PR_CAPBSET_DROP, CAP_SYS_ADMIN, 0, 0, 0) < 0)
        die_errno("prctl(PR_CAPBSET_DROP, CAP_SYS_ADMIN)");

    if (prctl(PR_CAPBSET_DROP, CAP_DAC_READ_SEARCH, 0, 0, 0) < 0)
        die_errno("prctl(PR_CAPBSET_DROP, CAP_DAC_READ_SEARCH)");

    if (prctl(PR_CAPBSET_DROP, CAP_SETPCAP, 0, 0, 0) < 0)
        die_errno("prctl(PR_CAPBSET_DROP, CAP_SETPCAP)");

    /* ----------------------------------------------------------------
     * Drop every capability (E/P/I), then clear the ambient set.
     *
     * Ambient caps can only be raised when both the inheritable and
     * permitted sets contain the capability, so clearing those above is
     * already sufficient — but belt-and-suspenders never hurts.
     * Requires Linux ≥ 4.3; EINVAL on older kernels is ignored.
     * ---------------------------------------------------------------- */
    drop_all_caps();

    /* Clear ambient capability set (best-effort; ignore EINVAL on old kernels). */
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0
            && errno != EINVAL)
        die_errno("prctl(PR_CAP_AMBIENT_CLEAR_ALL)");

    /* ----------------------------------------------------------------
     * Exec the requested command.
     *
     * execvp searches PATH and inherits the current environment
     * unchanged, which is exactly what we want.  The child replaces
     * this process, so its exit status propagates directly to the
     * caller — no fork/wait needed.
     * ---------------------------------------------------------------- */
    execvp(cmd[0], cmd);

    /* execvp only returns on error. */
    die_errno(cmd[0]);
    return 1; /* unreachable */
}
