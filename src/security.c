/* Northstar Web Browser — privilege refusal, OS sandboxes and security primitives.
 * Landlock/seccomp (Linux), Seatbelt (macOS), Win32 mitigations, CSPRNG, SRI, download tagging.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "security.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Returns TRUE if env var name is set to anything but empty, 0, false, no or off. */
gboolean
ns_env_flag(const char *name)
{
    const char *v = name ? g_getenv(name) : NULL;
    if (!v) return FALSE;
    while (*v == ' ' || *v == '\t') v++;
    return *v &&
           strcmp(v, "0") != 0 &&
           g_ascii_strcasecmp(v, "false") != 0 &&
           g_ascii_strcasecmp(v, "no") != 0 &&
           g_ascii_strcasecmp(v, "off") != 0;
}

static gboolean ns_tool_mode;

void
ns_security_set_tool_mode(gboolean on)
{
    ns_tool_mode = on;
}

gboolean
ns_security_tool_mode(void)
{
    return ns_tool_mode;
}

/* Where downloads are saved, and the one place in $HOME the sandbox lets
 * the browser write: the XDG download directory if it lies strictly
 * below $HOME (an XDG_DOWNLOAD_DIR of $HOME itself, or outside it,
 * would widen that grant), otherwise ~/Downloads. */
const char *
ns_security_downloads_dir(void)
{
    static char *dir;
    if (dir) return dir;
#ifdef G_OS_UNIX
    char *home_real = g_get_home_dir() ? realpath(g_get_home_dir(), NULL) : NULL;
    const char *dl_raw = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    char *dl_real = dl_raw && *dl_raw ? realpath(dl_raw, NULL) : NULL;
    const char *home = home_real ? home_real : g_get_home_dir();
    const char *dl = dl_real ? dl_real : dl_raw;
#else
    const char *home = g_get_home_dir();
    const char *dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
#endif
    gsize home_len = home ? strlen(home) : 0;
    while (home_len > 1 && home[home_len - 1] == G_DIR_SEPARATOR)
        home_len--;
    /* Not a hidden directory either (~/.config, ~/.ssh, ...): those hold
     * other programs' configuration, which a writable grant would expose
     * to the sandboxed process. */
    if (dl && *dl && home && strncmp(dl, home, home_len) == 0 &&
        dl[home_len] == G_DIR_SEPARATOR && dl[home_len + 1] != '\0' &&
        dl[home_len + 1] != '.' && !strstr(dl + home_len, "/.")
#ifdef G_OS_UNIX
        && dl_real
#endif
        )
        dir = g_strdup(dl);
    else if (home)
        dir = g_build_filename(home, "Downloads", NULL);
#ifdef G_OS_UNIX
    free(home_real);
    free(dl_real);
#endif
    return dir;
}


#ifndef G_OS_WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/random.h>
#include <linux/landlock.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#ifdef NS_HAVE_SECCOMP
#include <linux/seccomp.h>
#include <seccomp.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#ifndef SHM_EXEC
#define SHM_EXEC 0100000
#endif
#include <sched.h>
#ifndef CLONE_NEWNS
#define CLONE_NEWNS     0x00020000
#endif
#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000
#endif
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS    0x04000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC    0x08000000
#endif
#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER   0x10000000
#endif
#ifndef CLONE_NEWPID
#define CLONE_NEWPID    0x20000000
#endif
#ifndef CLONE_NEWNET
#define CLONE_NEWNET    0x40000000
#endif
#endif
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/random.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif

#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef G_OS_WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

/* Fills buf from the OS CSPRNG (BCrypt, getrandom, getentropy, /dev/urandom). */
gboolean
ns_security_csprng_fill(void *buf, gsize len)
{
    if (!buf || len == 0) return TRUE;
#if defined(G_OS_WIN32)
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return TRUE;
#elif defined(__linux__)
    gsize off = 0;
    while (off < len) {
        ssize_t n = getrandom((char *)buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (gsize)n;
    }
    if (off == len) return TRUE;
#elif defined(__APPLE__) || defined(__FreeBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    gsize off = 0;
    while (off < len) {
        gsize take = len - off;
        if (take > 256) take = 256;
        if (getentropy((char *)buf + off, take) != 0) break;
        off += take;
    }
    if (off == len) return TRUE;
#endif
#ifndef G_OS_WIN32
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return FALSE;
    gsize got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        got += (gsize)n;
    }
    close(fd);
    return got == len;
#else
    return FALSE;
#endif
}

/* Returns TRUE if the base64 digest of body under type equals want_b64. */
static gboolean
ns_sri_digest_equal_b64(GChecksumType type,
                        const char *want_b64, gsize want_len,
                        const void *body, gsize body_len)
{
    GChecksum *cs = g_checksum_new(type);
    g_checksum_update(cs, (const guchar *)body, (gssize)body_len);
    guint8 raw[64];
    gsize  raw_len = sizeof raw;
    g_checksum_get_digest(cs, raw, &raw_len);
    g_checksum_free(cs);
    g_autofree char *got = g_base64_encode(raw, raw_len);
    if (!got) return FALSE;
    gsize got_len = strlen(got);
    if (got_len != want_len) return FALSE;
    return memcmp(got, want_b64, want_len) == 0;
}

/* Subresource Integrity check: body must match a digest of the strongest listed algorithm. */
gboolean
ns_security_sri_check(const char *integrity_attr,
                      const void *body, gsize body_len)
{
    if (!integrity_attr || !*integrity_attr) return TRUE;
    if (!body && body_len) return FALSE;

    int strongest = 0;
    g_auto(GStrv) tokens = g_strsplit_set(integrity_attr, " \t\r\n", -1);
    for (int i = 0; tokens[i]; i++) {
        const char *t = tokens[i];
        if (g_str_has_prefix(t, "sha256-") && strongest < 256) strongest = 256;
        else if (g_str_has_prefix(t, "sha384-") && strongest < 384) strongest = 384;
        else if (g_str_has_prefix(t, "sha512-") && strongest < 512) strongest = 512;
    }
    if (strongest == 0) return TRUE;

    GChecksumType ctype = G_CHECKSUM_SHA256;
    const char *prefix = "sha256-";
    if (strongest == 384) { ctype = G_CHECKSUM_SHA384; prefix = "sha384-"; }
    else if (strongest == 512) { ctype = G_CHECKSUM_SHA512; prefix = "sha512-"; }
    gsize prefix_len = strlen(prefix);

    for (int i = 0; tokens[i]; i++) {
        const char *t = tokens[i];
        if (!g_str_has_prefix(t, prefix)) continue;
        const char *b64 = t + prefix_len;
        const char *qmark = strchr(b64, '?');
        gsize b64_len = qmark ? (gsize)(qmark - b64) : strlen(b64);
        if (b64_len == 0) continue;
        if (ns_sri_digest_equal_b64(ctype, b64, b64_len, body, body_len))
            return TRUE;
    }
    return FALSE;
}

#ifdef G_OS_WIN32
/* Returns TRUE if the process token is a member of the Administrators group. */
static gboolean
ns_win_is_elevated(void)
{
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    PSID admins_sid = NULL;
    BOOL is_member = FALSE;
    if (!AllocateAndInitializeSid(&nt_auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admins_sid))
        return FALSE;
    if (!CheckTokenMembership(NULL, admins_sid, &is_member))
        is_member = FALSE;
    FreeSid(admins_sid);
    return is_member ? TRUE : FALSE;
}

/* Relaunches this command line with the shell's unelevated token; TRUE if started. */
static gboolean
ns_win_relaunch_deelevated(void)
{
    HWND shell = GetShellWindow();
    if (!shell) return FALSE;
    DWORD shell_pid = 0;
    GetWindowThreadProcessId(shell, &shell_pid);
    if (!shell_pid) return FALSE;

    HANDLE shell_proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, shell_pid);
    if (!shell_proc) return FALSE;

    HANDLE shell_tok = NULL;
    gboolean relaunched = FALSE;
    if (OpenProcessToken(shell_proc, TOKEN_DUPLICATE, &shell_tok)) {
        HANDLE user_tok = NULL;
        DWORD access = TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE |
                       TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID;
        if (DuplicateTokenEx(shell_tok, access, NULL, SecurityImpersonation,
                             TokenPrimary, &user_tok)) {
            TOKEN_ELEVATION elev;
            DWORD got = 0;
            gboolean shell_elevated =
                GetTokenInformation(user_tok, TokenElevation, &elev,
                                    sizeof elev, &got) && elev.TokenIsElevated;
            if (!shell_elevated) {
                wchar_t exe[4096];
                DWORD n = GetModuleFileNameW(NULL, exe, G_N_ELEMENTS(exe));
                LPWSTR app = (n > 0 && n < G_N_ELEMENTS(exe)) ? exe : NULL;
                LPWSTR src = GetCommandLineW();
                gsize bytes = ((gsize)lstrlenW(src) + 1) * sizeof(wchar_t);
                LPWSTR cmd = g_malloc(bytes);
                memcpy(cmd, src, bytes);
                SetEnvironmentVariableW(L"NS_DEELEVATED", L"1");
                STARTUPINFOW si;
                memset(&si, 0, sizeof si);
                si.cb = sizeof si;
                PROCESS_INFORMATION pi;
                memset(&pi, 0, sizeof pi);
                if (CreateProcessWithTokenW(user_tok, 0, app, cmd, 0,
                                            NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    relaunched = TRUE;
                }
                g_free(cmd);
            }
            CloseHandle(user_tok);
        }
        CloseHandle(shell_tok);
    }
    CloseHandle(shell_proc);
    return relaunched;
}
#endif

/* Returns FALSE to refuse running as root/admin unless NS_ALLOW_ROOT or the user agrees. */
gboolean
ns_security_refuse_root(void)
{
#if defined(__linux__) || defined(__APPLE__)
    if (geteuid() != 0 && getuid() != 0) return TRUE;
    if (ns_env_flag("NS_ALLOW_ROOT")) {
        g_warning("northstar: running as root because NS_ALLOW_ROOT is set");
        return TRUE;
    }
    fprintf(stderr,
        "northstar: refusing to run as root.\n"
        "  Web browsers process untrusted content; running as root exposes\n"
        "  the whole system if the renderer is compromised.\n"
        "  Re-run as an unprivileged user, or set NS_ALLOW_ROOT=1 to override.\n");
    return FALSE;
#elif defined(G_OS_WIN32)
    if (!ns_win_is_elevated()) return TRUE;
    if (ns_env_flag("NS_ALLOW_ROOT")) {
        g_warning("northstar: running as Administrator because "
                  "NS_ALLOW_ROOT is set");
        return TRUE;
    }
    if (!ns_env_flag("NS_DEELEVATED") && ns_win_relaunch_deelevated()) {
        g_warning("northstar: dropped Administrator rights by relaunching "
                  "as the current desktop user");
        return FALSE;
    }

    const char *msg =
        "Northstar Web Browser is running as administrator.\n"
        "\n"
        "Northstar Web Browser tried to restart itself without administrator rights but "
        "could not. You do not need administrator rights to browse the web, and "
        "it is much safer without them: if a web page exploited a flaw in the "
        "browser while it has administrator rights, it could take over your "
        "whole PC instead of being limited to your own user account.\n"
        "\n"
        "How to start Northstar Web Browser normally:\n"
        "  -  Close this window, then open Northstar Web Browser with a normal click on "
        "its Start-menu or desktop icon. Do not choose \"Run as "
        "administrator\".\n"
        "  -  If you started it from a Command Prompt or PowerShell, use an "
        "ordinary window that is not marked \"Administrator\".\n"
        "  -  If a shortcut keeps starting it elevated, right-click the "
        "shortcut, choose Properties, then Advanced, and turn off \"Run as "
        "administrator\".\n"
        "\n"
        "Do you want to run as administrator anyway?\n"
        "  -  No (recommended): quit, then reopen Northstar Web Browser normally.\n"
        "  -  Yes: run as administrator this once, at your own risk.";
    fprintf(stderr, "northstar: %s\n", msg);

    int choice = MessageBoxA(NULL, msg,
        "Northstar Web Browser - running as administrator",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
        MB_SETFOREGROUND | MB_TOPMOST);
    if (choice == IDYES) {
        g_warning("northstar: running as Administrator at the user's "
                  "request");
        return TRUE;
    }
    return FALSE;
#else
    return TRUE;
#endif
}

static GPtrArray *ns_extra_writable_dirs = NULL;
#ifdef __linux__
static gboolean ns_gpu_devices_allowed = FALSE;
#endif
/* Set once Landlock is enforced, which limits reads as well as writes;
 * the macOS profile confines writes only. */
static gboolean ns_sandbox_reads_limited = FALSE;
static char *ns_sandbox_readable_downloads = NULL;

gboolean
ns_security_sandbox_limits_reads(void)
{
    return ns_sandbox_reads_limited;
}

const char *
ns_security_sandbox_readable_downloads(void)
{
    return ns_sandbox_reads_limited ? ns_sandbox_readable_downloads : NULL;
}

void
ns_security_allow_gpu_devices(void)
{
#ifdef __linux__
    ns_gpu_devices_allowed = TRUE;
#endif
}

void
ns_security_add_writable_dir(const char *dir)
{
    if (!dir || !*dir) return;
    if (!ns_extra_writable_dirs)
        ns_extra_writable_dirs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ns_extra_writable_dirs, g_strdup(dir));
}

#ifdef __linux__

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif

static int
landlock_create_ruleset_(const struct landlock_ruleset_attr *attr,
                         size_t size, guint32 flags)
{
    return (int)syscall(SYS_landlock_create_ruleset, attr, size, flags);
}

static int
landlock_add_rule_(int ruleset_fd, enum landlock_rule_type type,
                   const void *rule_attr, guint32 flags)
{
    return (int)syscall(SYS_landlock_add_rule, ruleset_fd, type,
                        rule_attr, flags);
}

static int
landlock_restrict_self_(int ruleset_fd, guint32 flags)
{
    return (int)syscall(SYS_landlock_restrict_self, ruleset_fd, flags);
}

/* Adds a Landlock path-beneath rule granting allowed access under path. */
static void
add_path_rw(int rfd, guint64 allowed, const char *path)
{
    if (!path) return;
    int pfd = open(path, O_PATH | O_CLOEXEC);
    if (pfd < 0) return;
    struct landlock_path_beneath_attr pb = {
        .allowed_access = allowed,
        .parent_fd      = pfd,
    };
    (void)landlock_add_rule_(rfd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(pfd);
}

/* Grants Landlock access to /dev/dri and /dev/nvidia* device nodes. */
static void
add_gpu_devices(int rfd, guint64 fs_read, guint64 fs_dev)
{
    add_path_rw(rfd, fs_read | LANDLOCK_ACCESS_FS_WRITE_FILE, "/dev/dri");
    GDir *dev = g_dir_open("/dev", 0, NULL);
    if (!dev) return;
    const char *name;
    while ((name = g_dir_read_name(dev))) {
        if (!g_str_has_prefix(name, "nvidia")) continue;
        g_autofree char *path = g_build_filename("/dev", name, NULL);
        gboolean is_dir = g_file_test(path, G_FILE_TEST_IS_DIR);
        add_path_rw(rfd, is_dir ? fs_read | LANDLOCK_ACCESS_FS_WRITE_FILE : fs_dev,
                    path);
    }
    g_dir_close(dev);
}

/* Confines filesystem access (Landlock on Linux, Seatbelt on macOS); no-op elsewhere. */
void
ns_security_sandbox_init(const char *self_exe)
{
    if (ns_env_flag("NS_NO_SANDBOX")) return;

    int abi = landlock_create_ruleset_(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) {
        if (errno != ENOSYS && errno != EOPNOTSUPP)
            g_info("landlock: version probe failed: %s", g_strerror(errno));
        return;
    }
    guint64 fs_read =
        LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR;
    guint64 fs_write_file = LANDLOCK_ACCESS_FS_WRITE_FILE;
    if (abi >= 3) fs_write_file |= LANDLOCK_ACCESS_FS_TRUNCATE;
    guint64 fs_write =
        fs_write_file |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR;
    if (abi >= 2) fs_write |= LANDLOCK_ACCESS_FS_REFER;
    guint64 fs_exec = LANDLOCK_ACCESS_FS_EXECUTE;
    guint64 fs_rw   = fs_read | fs_write;
    guint64 fs_all  = fs_read | fs_write | fs_exec | LANDLOCK_ACCESS_FS_MAKE_SYM;

    struct landlock_ruleset_attr attr = { .handled_access_fs = fs_all };
    int rfd = landlock_create_ruleset_(&attr, sizeof(attr), 0);
    if (rfd < 0) {
        g_info("landlock: create_ruleset failed: %s", g_strerror(errno));
        return;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        g_info("landlock: PR_SET_NO_NEW_PRIVS failed: %s", g_strerror(errno));
    }

    static const char *const system_exec_dirs[] = {
        "/usr", "/usr/local", "/lib", "/lib64", NULL,
    };
    static const char *const system_read_dirs[] = {
        "/etc", "/var/lib/ca-certificates", "/var/cache/fontconfig",
        "/proc", "/sys", "/run",
        "/dev/shm", "/dev/dri",
        "/tmp/.X11-unix", "/tmp/.ICE-unix",
        NULL,
    };
    add_path_rw(rfd, LANDLOCK_ACCESS_FS_READ_FILE, "/dev/urandom");
    add_path_rw(rfd, LANDLOCK_ACCESS_FS_READ_FILE | fs_write_file, "/dev/null");
    for (gsize i = 0; system_exec_dirs[i]; i++)
        add_path_rw(rfd, fs_read | fs_exec, system_exec_dirs[i]);
    for (gsize i = 0; system_read_dirs[i]; i++)
        add_path_rw(rfd, fs_read, system_read_dirs[i]);

    guint64 fs_dev = LANDLOCK_ACCESS_FS_READ_FILE | fs_write_file;
    for (int vi = 0; vi < 64; vi++) {
        char vdev[32];
        g_snprintf(vdev, sizeof vdev, "/dev/video%d", vi);
        if (g_file_test(vdev, G_FILE_TEST_EXISTS))
            add_path_rw(rfd, fs_dev, vdev);
    }
    if (ns_gpu_devices_allowed)
        add_gpu_devices(rfd, fs_read, fs_dev);
    if (g_file_test("/dev/snd", G_FILE_TEST_IS_DIR))
        add_path_rw(rfd, fs_read | fs_write_file, "/dev/snd");

    const char *xauth = g_getenv("XAUTHORITY");
    if (xauth && *xauth)
        add_path_rw(rfd, LANDLOCK_ACCESS_FS_READ_FILE, xauth);

    const char *home = g_get_home_dir();

    add_path_rw(rfd, fs_read, g_get_user_config_dir());
    add_path_rw(rfd, fs_read, g_get_user_data_dir());
    add_path_rw(rfd, fs_read, g_get_user_cache_dir());
    /* The runtime dir holds other services' state (systemd --user,
     * gvfs, ...); only dconf's shared-memory flag file is written. */
    add_path_rw(rfd, fs_read, g_get_user_runtime_dir());
    {
        g_autofree char *dconf =
            g_build_filename(g_get_user_runtime_dir(), "dconf", NULL);
        if (g_file_test(dconf, G_FILE_TEST_IS_DIR))
            add_path_rw(rfd, fs_rw, dconf);
    }

    char *ns_cfg_root =
        g_build_filename(g_get_user_config_dir(), "northstar", NULL);
    g_mkdir_with_parents(ns_cfg_root, 0700);
    add_path_rw(rfd, fs_rw, ns_cfg_root);
    g_free(ns_cfg_root);

    char *ns_data_root =
        g_build_filename(g_get_user_data_dir(), "northstar", NULL);
    g_mkdir_with_parents(ns_data_root, 0700);
    add_path_rw(rfd, fs_rw, ns_data_root);
    g_free(ns_data_root);

    char *ns_cache_top =
        g_build_filename(g_get_user_cache_dir(), "northstar", NULL);
    g_mkdir_with_parents(ns_cache_top, 0700);
    add_path_rw(rfd, fs_rw, ns_cache_top);
    g_free(ns_cache_top);

    char *ns_cache_root =
        g_build_filename(g_get_user_cache_dir(), "northstar", "cache", NULL);
    g_mkdir_with_parents(ns_cache_root, 0700);
    add_path_rw(rfd, fs_rw, ns_cache_root);
    g_free(ns_cache_root);

    {
        const char *dl_dir = ns_security_downloads_dir();
        if (dl_dir) {
            g_mkdir_with_parents(dl_dir, 0700);
            /* Landlock grants what the path resolves to, so the fallback
             * ~/Downloads must resolve, like the XDG directory, strictly
             * below $HOME and outside hidden directories (a symlink to
             * $HOME or ~/.ssh would otherwise widen the grant). */
            char *dl_real = realpath(dl_dir, NULL);
            char *home_real = realpath(home, NULL);
            gsize hl = home_real ? strlen(home_real) : 0;
            gboolean ok = dl_real && home_real &&
                strncmp(dl_real, home_real, hl) == 0 &&
                dl_real[hl] == '/' && dl_real[hl + 1] != '\0' &&
                dl_real[hl + 1] != '.' && !strstr(dl_real + hl, "/.");
            if (ok && g_file_test(dl_real, G_FILE_TEST_IS_DIR)) {
                add_path_rw(rfd, fs_rw, dl_real);
                g_free(ns_sandbox_readable_downloads);
                ns_sandbox_readable_downloads = g_strdup(dl_real);
            } else
                g_info("sandbox: not granting the download directory %s",
                       dl_dir);
            free(dl_real);
            free(home_real);
        }
    }

    static const char *const home_ro_subdirs[] = {
        ".fonts", ".fontconfig", ".icons", ".themes", NULL,
    };
    for (gsize i = 0; home_ro_subdirs[i]; i++) {
        g_autofree char *p = g_build_filename(home, home_ro_subdirs[i], NULL);
        add_path_rw(rfd, fs_read, p);
    }

    if (self_exe) {
        g_autofree char *exe_dir = g_path_get_dirname(self_exe);
        add_path_rw(rfd, fs_read | fs_exec, exe_dir);
        const char *const dev_data_rel[] = {
            "../data",
            "../../data",
            "../../../data",
            "../share/northstar",
            NULL,
        };
        for (gsize i = 0; dev_data_rel[i]; i++) {
            g_autofree char *p = g_build_filename(exe_dir, dev_data_rel[i], NULL);
            if (g_file_test(p, G_FILE_TEST_IS_DIR))
                add_path_rw(rfd, fs_read, p);
        }
        const char *const prefix_lib_rel[] = {
            "../lib",
            "../lib64",
            NULL,
        };
        for (gsize i = 0; prefix_lib_rel[i]; i++) {
            g_autofree char *p = g_build_filename(exe_dir, prefix_lib_rel[i], NULL);
            if (g_file_test(p, G_FILE_TEST_IS_DIR))
                add_path_rw(rfd, fs_read | fs_exec, p);
        }
        const char *const dev_root_markers[] = {
            "../meson.build",
            "../../meson.build",
            "../../../meson.build",
            NULL,
        };
        for (gsize i = 0; dev_root_markers[i]; i++) {
            g_autofree char *marker =
                g_build_filename(exe_dir, dev_root_markers[i], NULL);
            if (g_file_test(marker, G_FILE_TEST_IS_REGULAR)) {
                g_autofree char *root = g_path_get_dirname(marker);
                add_path_rw(rfd, fs_read, root);
                break;
            }
        }
    }

    if (ns_extra_writable_dirs) {
        for (guint i = 0; i < ns_extra_writable_dirs->len; i++) {
            const char *p = g_ptr_array_index(ns_extra_writable_dirs, i);
            add_path_rw(rfd, fs_rw, p);
        }
    }

    if (landlock_restrict_self_(rfd, 0) != 0) {
        g_info("landlock: restrict_self failed: %s", g_strerror(errno));
    } else {
        ns_sandbox_reads_limited = TRUE;
    }
    close(rfd);
}

#ifdef NS_HAVE_SECCOMP
static const char *const ns_seccomp_allowed_names[] = {
    "accept",
    "accept4",
    "access",
    "arch_prctl",
    "bind",
    "brk",
    "capget",
    "chdir",
    "chmod",
    "clock_getres",
    "clock_getres_time64",
    "clock_gettime",
    "clock_gettime64",
    "clock_nanosleep",
    "clock_nanosleep_time64",
    "clone",
    "clone3",
    "close",
    "close_range",
    "connect",
    "copy_file_range",
    "creat",
    "dup",
    "dup2",
    "dup3",
    "epoll_create",
    "epoll_create1",
    "epoll_ctl",
    "epoll_pwait",
    "epoll_pwait2",
    "epoll_wait",
    "eventfd",
    "eventfd2",
    "exit",
    "exit_group",
    "faccessat",
    "faccessat2",
    "fadvise64",
    "fadvise64_64",
    "fallocate",
    "fchdir",
    "fchmod",
    "fchmodat",
    "fcntl",
    "fcntl64",
    "fdatasync",
    "flock",
    "fstat",
    "fstat64",
    "fstatat64",
    "fstatfs",
    "fstatfs64",
    "fsync",
    "ftruncate",
    "ftruncate64",
    "futex",
    "futex_time64",
    "futex_waitv",
    "futimesat",
    "getcpu",
    "getcwd",
    "getdents",
    "getdents64",
    "getegid",
    "getegid32",
    "geteuid",
    "geteuid32",
    "getgid",
    "getgid32",
    "getgroups",
    "getgroups32",
    "getitimer",
    "getpeername",
    "getpgid",
    "getpgrp",
    "getpid",
    "getppid",
    "getpriority",
    "getrandom",
    "getresgid",
    "getresgid32",
    "getresuid",
    "getresuid32",
    "getrlimit",
    "get_robust_list",
    "getrusage",
    "getsid",
    "getsockname",
    "getsockopt",
    "gettid",
    "gettimeofday",
    "getuid",
    "getuid32",
    "getxattr",
    "inotify_add_watch",
    "inotify_init",
    "inotify_init1",
    "inotify_rm_watch",
    "ioctl",
    "kill",
    "lgetxattr",
    "link",
    "linkat",
    "listen",
    "listxattr",
    "_llseek",
    "llistxattr",
    "lseek",
    "lstat",
    "lstat64",
    "madvise",
    "mbind",
    "membarrier",
    "memfd_create",
    "memfd_secret",
    "mincore",
    "mkdir",
    "mkdirat",
    "mlock",
    "mlock2",
    "mlockall",
    "mmap",
    "mmap2",
    "mprotect",
    "mremap",
    "msync",
    "munlock",
    "munlockall",
    "munmap",
    "nanosleep",
    "newfstatat",
    "open",
    "openat",
    "openat2",
    "pause",
    "pidfd_open",
    "pidfd_send_signal",
    "pipe",
    "pipe2",
    "pkey_alloc",
    "pkey_free",
    "pkey_mprotect",
    "poll",
    "ppoll",
    "ppoll_time64",
    "prctl",
    "pread64",
    "preadv",
    "preadv2",
    "prlimit64",
    "pselect6",
    "pselect6_time64",
    "pwrite64",
    "pwritev",
    "pwritev2",
    "read",
    "readahead",
    "readlink",
    "readlinkat",
    "readv",
    "recv",
    "recvfrom",
    "recvmmsg",
    "recvmmsg_time64",
    "recvmsg",
    "remap_file_pages",
    "rename",
    "renameat",
    "renameat2",
    "restart_syscall",
    "rmdir",
    "rseq",
    "rt_sigaction",
    "rt_sigpending",
    "rt_sigprocmask",
    "rt_sigqueueinfo",
    "rt_sigreturn",
    "rt_sigsuspend",
    "rt_sigtimedwait",
    "rt_sigtimedwait_time64",
    "rt_tgsigqueueinfo",
    "sched_getaffinity",
    "sched_getattr",
    "sched_getparam",
    "sched_get_priority_max",
    "sched_get_priority_min",
    "sched_getscheduler",
    "sched_rr_get_interval",
    "sched_setaffinity",
    "sched_setattr",
    "sched_setparam",
    "sched_setscheduler",
    "sched_yield",
    "select",
    "semctl",
    "semget",
    "semop",
    "semtimedop",
    "send",
    "sendfile",
    "sendfile64",
    "sendmmsg",
    "sendmsg",
    "sendto",
    "setitimer",
    "setpgid",
    "setpriority",
    "setrlimit",
    "set_robust_list",
    "setsid",
    "setsockopt",
    "set_tid_address",
    "shmat",
    "shmctl",
    "shmdt",
    "shmget",
    "shutdown",
    "sigaltstack",
    "signalfd",
    "signalfd4",
    "socket",
    "socketpair",
    "splice",
    "stat",
    "stat64",
    "statfs",
    "statfs64",
    "statx",
    "symlink",
    "symlinkat",
    "sync",
    "sync_file_range",
    "syncfs",
    "sysinfo",
    "tee",
    "tgkill",
    "time",
    "timer_create",
    "timer_delete",
    "timer_getoverrun",
    "timer_gettime",
    "timer_gettime64",
    "timer_settime",
    "timer_settime64",
    "timerfd_create",
    "timerfd_gettime",
    "timerfd_gettime64",
    "timerfd_settime",
    "timerfd_settime64",
    "times",
    "tkill",
    "truncate",
    "truncate64",
    "umask",
    "uname",
    "unlink",
    "unlinkat",
    "utime",
    "utimensat",
    "utimensat_time64",
    "utimes",
    "wait4",
    "waitid",
    "waitpid",
    "write",
    "writev",
};
#endif

/* Loads seccomp argument deny filters and the syscall allowlist; no-op without seccomp. */
void
ns_security_seccomp_init(void)
{
    if (ns_env_flag("NS_NO_SANDBOX")) return;
    if (ns_env_flag("NS_NO_SECCOMP")) return;

#ifdef NS_HAVE_SECCOMP
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        g_info("seccomp: PR_SET_NO_NEW_PRIVS failed: %s", g_strerror(errno));
        return;
    }

    /* A few calls the allowlist permits are refused for particular
     * arguments.  Filters stack and the strictest verdict wins, so these
     * go in a filter of their own, loaded before the allowlist:
     * - TIOCSTI/TIOCLINUX would type commands into the terminal the
     *   browser was started from;
     * - shmat(..., SHM_EXEC) gives executable SysV shared memory that a
     *   second, writable attach can fill, past the exec-memory filter. */
    scmp_filter_ctx deny = seccomp_init(SCMP_ACT_ALLOW);
    if (deny) {
        (void)seccomp_attr_set(deny, SCMP_FLTATR_CTL_TSYNC, 1);
        int ioctl_nr = seccomp_syscall_resolve_name("ioctl");
        if (ioctl_nr != __NR_SCMP_ERROR) {
            (void)seccomp_rule_add(deny, SCMP_ACT_ERRNO(EPERM), ioctl_nr, 1,
                SCMP_A1(SCMP_CMP_MASKED_EQ, 0xffffffffu, TIOCSTI));
            (void)seccomp_rule_add(deny, SCMP_ACT_ERRNO(EPERM), ioctl_nr, 1,
                SCMP_A1(SCMP_CMP_MASKED_EQ, 0xffffffffu, TIOCLINUX));
        }
        int shmat_nr = seccomp_syscall_resolve_name("shmat");
        if (shmat_nr != __NR_SCMP_ERROR)
            (void)seccomp_rule_add(deny, SCMP_ACT_ERRNO(EACCES), shmat_nr, 1,
                SCMP_A2(SCMP_CMP_MASKED_EQ, SHM_EXEC, SHM_EXEC));
        /* No new namespaces (a user namespace is a large kernel attack
         * surface), and clone3, whose flags live in memory a filter
         * cannot read: glibc falls back to clone on ENOSYS. */
#if !defined(__s390__) && !defined(__s390x__)
        int clone_nr = seccomp_syscall_resolve_name("clone");
        static const unsigned long ns_flags[] = {
            CLONE_NEWUSER, CLONE_NEWNS, CLONE_NEWNET, CLONE_NEWPID,
            CLONE_NEWIPC, CLONE_NEWUTS, CLONE_NEWCGROUP,
        };
        for (gsize i = 0; clone_nr != __NR_SCMP_ERROR &&
                          i < G_N_ELEMENTS(ns_flags); i++)
            (void)seccomp_rule_add(deny, SCMP_ACT_ERRNO(EPERM), clone_nr, 1,
                SCMP_A0(SCMP_CMP_MASKED_EQ, ns_flags[i], ns_flags[i]));
#endif
        int clone3_nr = seccomp_syscall_resolve_name("clone3");
        if (clone3_nr != __NR_SCMP_ERROR)
            (void)seccomp_rule_add(deny, SCMP_ACT_ERRNO(ENOSYS), clone3_nr, 0);
        static const char *const ns_unshare[] = { "unshare", "setns" };
        for (gsize i = 0; i < G_N_ELEMENTS(ns_unshare); i++) {
            int nr = seccomp_syscall_resolve_name(ns_unshare[i]);
            if (nr != __NR_SCMP_ERROR)
                (void)seccomp_rule_add(deny, SCMP_ACT_ERRNO(EPERM), nr, 0);
        }
        /* Signals only to this process: kill(-1, SIGKILL) would take down
         * every process the user owns. */
        pid_t self = getpid();
        int kill_nr = seccomp_syscall_resolve_name("kill");
        if (kill_nr != __NR_SCMP_ERROR)
            (void)seccomp_rule_add(deny, SCMP_ACT_ERRNO(EPERM), kill_nr, 1,
                SCMP_A0(SCMP_CMP_NE, (scmp_datum_t)self));
        int tgkill_nr = seccomp_syscall_resolve_name("tgkill");
        if (tgkill_nr != __NR_SCMP_ERROR)
            (void)seccomp_rule_add(deny, SCMP_ACT_ERRNO(EPERM), tgkill_nr, 1,
                SCMP_A0(SCMP_CMP_NE, (scmp_datum_t)self));
        if (seccomp_load(deny) != 0)
            g_info("seccomp: argument filter not loaded");
        seccomp_release(deny);
    }

    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ERRNO(EPERM));
    if (!ctx) {
        g_info("seccomp: seccomp_init failed");
        return;
    }
    (void)seccomp_attr_set(ctx, SCMP_FLTATR_CTL_TSYNC, 1);

    for (size_t i = 0; i < G_N_ELEMENTS(ns_seccomp_allowed_names); i++) {
        int nr = seccomp_syscall_resolve_name(ns_seccomp_allowed_names[i]);
        if (nr == __NR_SCMP_ERROR) continue;
        (void)seccomp_rule_add(ctx, SCMP_ACT_ALLOW, nr, 0);
    }
    int filter_nr = seccomp_syscall_resolve_name("seccomp");
    if (filter_nr != __NR_SCMP_ERROR)
        (void)seccomp_rule_add(ctx, SCMP_ACT_ALLOW, filter_nr, 1,
                               SCMP_A0(SCMP_CMP_EQ, SECCOMP_SET_MODE_FILTER, 0));

    int rc = seccomp_load(ctx);
    if (rc != 0) {
        g_warning("seccomp: load failed, process is NOT syscall-sandboxed: %s",
                  g_strerror(-rc));
    }
    seccomp_release(ctx);
#endif
}

#ifdef NS_HAVE_SECCOMP
/* Adds a seccomp rule refusing PROT_EXEC for a call, optionally only for anonymous maps. */
static void
seccomp_deny_exec(scmp_filter_ctx ctx, const char *name, gboolean anonymous_only)
{
    int nr = seccomp_syscall_resolve_name(name);
    if (nr == __NR_SCMP_ERROR) return;
    if (anonymous_only)
        (void)seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EACCES), nr, 2,
                               SCMP_A2(SCMP_CMP_MASKED_EQ, PROT_EXEC, PROT_EXEC),
                               SCMP_A3(SCMP_CMP_MASKED_EQ, MAP_ANONYMOUS,
                                       MAP_ANONYMOUS));
    else
        (void)seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EACCES), nr, 1,
                               SCMP_A2(SCMP_CMP_MASKED_EQ, PROT_EXEC, PROT_EXEC));
}

/* Executable shared mappings are never needed (libraries are mapped
 * private) and would let writes through another mapping become code. */
static void
seccomp_deny_exec_shared(scmp_filter_ctx ctx, const char *name)
{
    int nr = seccomp_syscall_resolve_name(name);
    if (nr == __NR_SCMP_ERROR) return;
    (void)seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EACCES), nr, 2,
                           SCMP_A2(SCMP_CMP_MASKED_EQ, PROT_EXEC, PROT_EXEC),
                           SCMP_A3(SCMP_CMP_MASKED_EQ, MAP_SHARED, MAP_SHARED));
}
#endif

/* Seccomp filter refusing executable mappings and memfds; TSYNC when whole_process. */
void
ns_security_deny_exec_memory(gboolean whole_process)
{
    if (ns_env_flag("NS_NO_SANDBOX")) return;
    if (ns_env_flag("NS_NO_SECCOMP")) return;

#ifdef NS_HAVE_SECCOMP
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return;
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (!ctx) return;
    if (whole_process)
        (void)seccomp_attr_set(ctx, SCMP_FLTATR_CTL_TSYNC, 1);
#ifndef __i386__
    seccomp_deny_exec(ctx, "mmap", TRUE);
    seccomp_deny_exec_shared(ctx, "mmap");
#endif
    seccomp_deny_exec(ctx, "mmap2", TRUE);
    seccomp_deny_exec_shared(ctx, "mmap2");
    seccomp_deny_exec(ctx, "mprotect", FALSE);
    seccomp_deny_exec(ctx, "pkey_mprotect", FALSE);
    /* Writable-then-executable memory can also be had from an anonymous
     * file mapped twice; content threads have no use for memfds or for
     * re-arranging file mappings. */
    static const char *const no_memfd[] = {
        "memfd_create", "memfd_secret", "remap_file_pages",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(no_memfd); i++) {
        int nr = seccomp_syscall_resolve_name(no_memfd[i]);
        if (nr != __NR_SCMP_ERROR)
            (void)seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), nr, 0);
    }
    int rc = seccomp_load(ctx);
    if (rc != 0)
        g_info("seccomp: executable-memory filter not loaded: %s",
               g_strerror(-rc));
    seccomp_release(ctx);
#else
    (void)whole_process;
#endif
}

#elif defined(__APPLE__)

#include <limits.h>
#include <sandbox.h>
#include <stdlib.h>

/* Appends a quoted, realpath-resolved (subpath ...) clause to a Seatbelt profile. */
static void
sb_append_subpath(GString *prof, const char *dir)
{
    if (!dir || !*dir)
        return;
    char resolved[PATH_MAX];
    const char *use = realpath(dir, resolved) ? resolved : dir;
    g_string_append(prof, "  (subpath \"");
    for (const char *c = use; *c; c++) {
        if (*c == '"' || *c == '\\')
            g_string_append_c(prof, '\\');
        g_string_append_c(prof, *c);
    }
    g_string_append(prof, "\")\n");
}

void
ns_security_sandbox_init(const char *self_exe)
{
    (void)self_exe;
    if (ns_env_flag("NS_NO_SANDBOX"))
        return;

    GString *prof = g_string_new(
        "(version 1)\n"
        "(allow default)\n"
        "(deny file-write*)\n"
        "(allow file-write*\n"
        "  (subpath \"/private/var/folders\")\n"
        "  (subpath \"/private/tmp\")\n"
        "  (subpath \"/tmp\")\n"
        "  (subpath \"/dev\")\n");

    sb_append_subpath(prof, g_get_user_runtime_dir());

    char *cfg = g_build_filename(g_get_user_config_dir(), "northstar", NULL);
    sb_append_subpath(prof, cfg);
    g_free(cfg);
    char *data = g_build_filename(g_get_user_data_dir(), "northstar", NULL);
    sb_append_subpath(prof, data);
    g_free(data);
    char *cache = g_build_filename(g_get_user_cache_dir(), "northstar", NULL);
    sb_append_subpath(prof, cache);
    g_free(cache);

    sb_append_subpath(prof, ns_security_downloads_dir());

    if (ns_extra_writable_dirs) {
        for (guint i = 0; i < ns_extra_writable_dirs->len; i++)
            sb_append_subpath(prof,
                              g_ptr_array_index(ns_extra_writable_dirs, i));
    }

    g_string_append(prof, ")\n");

    char *err = NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    int rc = sandbox_init(prof->str, 0, &err);
#pragma clang diagnostic pop
    if (rc != 0) {
        g_warning("sandbox: macOS Seatbelt init failed, process is NOT "
                  "filesystem-confined: %s", err ? err : "unknown error");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (err)
            sandbox_free_error(err);
#pragma clang diagnostic pop
    } else {
        g_info("sandbox: macOS Seatbelt filesystem write-confinement active");
    }
    g_string_free(prof, TRUE);
}

void
ns_security_seccomp_init(void)
{
}

void
ns_security_deny_exec_memory(gboolean whole_process)
{
    (void)whole_process;
}

#else

void
ns_security_sandbox_init(const char *self_exe)
{
    (void)self_exe;
}

void
ns_security_seccomp_init(void)
{
}

void
ns_security_deny_exec_memory(gboolean whole_process)
{
    (void)whole_process;
}

#endif

#ifdef G_OS_WIN32

typedef BOOL (WINAPI *ns_smp_fn)(PROCESS_MITIGATION_POLICY policy,
                                 PVOID buffer, SIZE_T length);

#define NS_ASLR_FORCE_RELOCATE_IMAGES        0x02u
#define NS_DYNAMIC_CODE_PROHIBIT             0x01u
#define NS_STRICT_HANDLE_RAISE_AND_LOCK      0x03u
#define NS_EXTENSION_POINTS_DISABLE          0x01u
#define NS_IMAGE_LOAD_NO_REMOTE_OR_LOW_IL    0x07u
#define NS_CHILD_PROCESS_NO_CREATION         0x01u

static void
ns_win32_apply_mitigation(ns_smp_fn fn, PROCESS_MITIGATION_POLICY policy,
                          DWORD flags)
{
    if (!fn) return;
    DWORD m = flags;
    (void)fn(policy, &m, sizeof m);
}

/* Applies Windows process mitigation policies; dynamic code is barred when not interactive. */
void
ns_security_win32_mitigations_init(gboolean interactive)
{
    if (ns_env_flag("NS_NO_WIN32_MITIGATIONS")) return;

    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (!k) return;
    ns_smp_fn fn = (ns_smp_fn)(void *)GetProcAddress(k, "SetProcessMitigationPolicy");
    if (!fn) return;

    ns_win32_apply_mitigation(fn, ProcessASLRPolicy,
                              NS_ASLR_FORCE_RELOCATE_IMAGES);
    ns_win32_apply_mitigation(fn, ProcessStrictHandleCheckPolicy,
                              NS_STRICT_HANDLE_RAISE_AND_LOCK);
    ns_win32_apply_mitigation(fn, ProcessExtensionPointDisablePolicy,
                              NS_EXTENSION_POINTS_DISABLE);
    ns_win32_apply_mitigation(fn, ProcessImageLoadPolicy,
                              NS_IMAGE_LOAD_NO_REMOTE_OR_LOW_IL);
    ns_win32_apply_mitigation(fn, ProcessChildProcessPolicy,
                              NS_CHILD_PROCESS_NO_CREATION);
    if (!interactive)
        ns_win32_apply_mitigation(fn, ProcessDynamicCodePolicy,
                                  NS_DYNAMIC_CODE_PROHIBIT);
}

#else

void
ns_security_win32_mitigations_init(gboolean interactive)
{
    (void)interactive;
}

#endif

/* Tags a download with its origin: quarantine xattr, xdg.origin.url or Zone.Identifier. */
void
ns_security_mark_download_origin(const char *path, const char *url)
{
    if (!path || !*path)
        return;
#if defined(__APPLE__)
    (void)url;
    char value[96];
    g_snprintf(value, sizeof value, "0001;%08x;Northstar Web Browser;",
               (unsigned)(g_get_real_time() / G_USEC_PER_SEC));
    setxattr(path, "com.apple.quarantine", value, strlen(value), 0, 0);
#elif defined(__linux__)
    if (url && *url)
        setxattr(path, "user.xdg.origin.url", url, strlen(url), 0);
#elif defined(G_OS_WIN32)
    char *stream = g_strconcat(path, ":Zone.Identifier", NULL);
    wchar_t *wstream = (wchar_t *)g_utf8_to_utf16(stream, -1, NULL, NULL, NULL);
    if (wstream) {
        HANDLE h = CreateFileW(wstream, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            GString *zone = g_string_new("[ZoneTransfer]\r\nZoneId=3\r\n");
            if (url && *url) {
                g_string_append_printf(zone, "HostUrl=%s\r\n", url);
                g_string_append_printf(zone, "ReferrerUrl=%s\r\n", url);
            }
            DWORD wrote = 0;
            WriteFile(h, zone->str, (DWORD)zone->len, &wrote, NULL);
            CloseHandle(h);
            g_string_free(zone, TRUE);
        }
        g_free(wstream);
    }
    g_free(stream);
#else
    (void)url;
#endif
}

/* Sets glibc M_PERTURB so freed and new heap memory is filled with a pattern. */
void
ns_security_harden_allocator(void)
{
#ifdef __GLIBC__
    mallopt(M_PERTURB, 0xAA);
#endif
}
