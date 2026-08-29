#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <curl/curl.h>
#include <json-c/json.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_MODEL "gpt-4.1-mini"
#define DEFAULT_MAX_STEPS 8
#define MAX_MAX_STEPS 100
#define TOOL_OUTPUT_LIMIT (64U * 1024U)
#define HTTP_RESPONSE_LIMIT (16U * 1024U * 1024U)
#define CONFIG_FILE_LIMIT (16U * 1024U * 1024U)
#define COMMAND_TIMEOUT_SECONDS 30

#define EXIT_LOCAL 1
#define EXIT_API 2
#define EXIT_AGENT 3

static const char *SYSTEM_PROMPT =
    "You are a codebase search agent.\n\n"
    "Answer the user's question using evidence from the repository.\n\n"
    "You have a bash tool. Use normal read-only Unix commands such as rg,\n"
    "find, sed, awk, git, and cat to search and inspect the codebase.\n\n"
    "Search iteratively. Do not speculate when you can inspect the code.\n"
    "Avoid unnecessarily large outputs.\n"
    "Stop once you have enough evidence.\n\n"
    "Return a concise answer with relevant file:line references.\n"
    "All repository paths in the final answer should be relative.";

struct config {
    char *endpoint;
    char *api_key;
    char *model;
    char *repo;
    char *question;
    int max_steps;
    bool verbose;
    bool yolo;
    json_object *extra;
};

struct buffer {
    char *data;
    size_t len;
    size_t limit;
    bool truncated;
};

struct run_context {
    const struct config *cfg;
    char *temp_dir;
#ifdef __APPLE__
    char *profile_path;
#endif
};

struct command_result {
    struct buffer out;
    struct buffer err;
    int exit_code;
    bool timed_out;
};

static void die_oom(void)
{
    fputs("lmg: out of memory\n", stderr);
    exit(EXIT_LOCAL);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die_oom();
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p)
        die_oom();
    return p;
}

static char *xasprintf(const char *format, ...)
{
    va_list ap;
    char *result = NULL;
    int n;
    va_start(ap, format);
    n = vasprintf(&result, format, ap);
    va_end(ap);
    if (n < 0 || !result)
        die_oom();
    return result;
}

static bool buffer_init(struct buffer *b, size_t limit)
{
    memset(b, 0, sizeof(*b));
    b->limit = limit;
    b->data = calloc(1, limit + 1);
    return b->data != NULL;
}

static bool buffer_append(struct buffer *b, const void *src, size_t n)
{
    size_t keep = n;
    if (b->len >= b->limit) {
        b->truncated = b->truncated || n > 0;
        return true;
    }
    if (keep > b->limit - b->len) {
        keep = b->limit - b->len;
        b->truncated = true;
    }
    if (keep)
        memcpy(b->data + b->len, src, keep);
    b->len += keep;
    b->data[b->len] = '\0';
    return true;
}

static bool buffer_append_cstr(struct buffer *b, const char *s)
{
    return buffer_append(b, s, strlen(s));
}

static int replace_string(char **dst, const char *src)
{
    char *copy = xstrdup(src);
    *dst = copy;
    return 0;
}

static const char *plain_json_string(json_object *obj)
{
    const char *s;
    if (!json_object_is_type(obj, json_type_string))
        return NULL;
    s = json_object_get_string(obj);
    if (!s || strlen(s) != (size_t)json_object_get_string_len(obj))
        return NULL;
    return s;
}

static bool parse_positive_int(const char *s, int *out)
{
    char *end = NULL;
    long n;
    errno = 0;
    n = strtol(s, &end, 10);
    if (errno || !s[0] || !end || *end || n < 1 || n > MAX_MAX_STEPS)
        return false;
    *out = (int)n;
    return true;
}

static json_object *parse_json_strict(const char *text, size_t len)
{
    json_tokener *tok = json_tokener_new();
    json_object *obj;
    size_t end;
    if (!tok)
        die_oom();
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    obj = json_tokener_parse_ex(tok, text, (int)len);
    if (json_tokener_get_error(tok) != json_tokener_success) {
        return NULL;
    }
    end = json_tokener_get_parse_end(tok);
    while (end < len && isspace((unsigned char)text[end]))
        end++;
    if (end != len) {
        return NULL;
    }
    return obj;
}

static int read_file(const char *path, char **out, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    char *buf;
    size_t used = 0;
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > CONFIG_FILE_LIMIT) {
        close(fd);
        return -1;
    }
    buf = xmalloc((size_t)st.st_size + 1);
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        used += (size_t)n;
    }
    buf[used] = '\0';
    close(fd);
    *out = buf;
    *out_len = used;
    return 0;
}

static void merge_extra(struct config *cfg, json_object *extra)
{
    json_object *merged = json_object_new_object();
    json_object *override;
    if (!merged)
        die_oom();
    {
        json_object_object_foreach(cfg->extra, key, value) {
            if (!json_object_object_get_ex(extra, key, &override))
                json_object_object_add(merged, key, json_object_get(value));
        }
    }
    {
        json_object_object_foreach(extra, key, value) {
            json_object_object_add(merged, key, json_object_get(value));
        }
    }
    cfg->extra = merged;
}

static bool apply_config_object(struct config *cfg, json_object *root)
{
    json_object *v;
    const char *s;
    if (!json_object_is_type(root, json_type_object))
        return false;
    if (json_object_object_get_ex(root, "endpoint", &v)) {
        if (!(s = plain_json_string(v))) return false;
        replace_string(&cfg->endpoint, s);
    }
    if (json_object_object_get_ex(root, "api_key", &v)) {
        if (!(s = plain_json_string(v))) return false;
        replace_string(&cfg->api_key, s);
    }
    if (json_object_object_get_ex(root, "model", &v)) {
        if (!(s = plain_json_string(v))) return false;
        replace_string(&cfg->model, s);
    }
    if (json_object_object_get_ex(root, "max_steps", &v)) {
        int64_t n;
        if (!json_object_is_type(v, json_type_int)) return false;
        n = json_object_get_int64(v);
        if (n < 1 || n > MAX_MAX_STEPS) return false;
        cfg->max_steps = (int)n;
    }
    if (json_object_object_get_ex(root, "extra", &v)) {
        if (!json_object_is_type(v, json_type_object)) return false;
        merge_extra(cfg, v);
    }
    return true;
}

static char *home_directory(void)
{
    const char *home = getenv("HOME");
    struct passwd *pw;
    if (home && home[0])
        return xstrdup(home);
    pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0])
        return xstrdup(pw->pw_dir);
    return NULL;
}

static int load_file_config(struct config *cfg)
{
    char *home = home_directory();
    char *path;
    char *text = NULL;
    size_t len = 0;
    json_object *root = NULL;
    int rc = 0;
    if (!home)
        return 0;
    path = xasprintf("%s/.lmg.json", home);
    if (access(path, F_OK) != 0) {
        return 0;
    }
    if (read_file(path, &text, &len) < 0 ||
        !(root = parse_json_strict(text, len)) ||
        !apply_config_object(cfg, root)) {
        fputs("lmg: invalid ~/.lmg.json\n", stderr);
        rc = -1;
    }
    return rc;
}

static int load_environment(struct config *cfg)
{
    const char *s;
    json_object *extra;
    s = getenv("LMG_ENDPOINT");
    if (s) replace_string(&cfg->endpoint, s);
    s = getenv("LMG_API_KEY");
    if (s) replace_string(&cfg->api_key, s);
    s = getenv("LMG_MODEL");
    if (s) replace_string(&cfg->model, s);
    s = getenv("LMG_MAX_STEPS");
    if (s && !parse_positive_int(s, &cfg->max_steps)) {
        fputs("lmg: invalid LMG_MAX_STEPS\n", stderr);
        return -1;
    }
    s = getenv("LMG_EXTRA_JSON");
    if (s) {
        extra = parse_json_strict(s, strlen(s));
        if (!extra || !json_object_is_type(extra, json_type_object)) {
            fputs("lmg: invalid LMG_EXTRA_JSON\n", stderr);
            return -1;
        }
        merge_extra(cfg, extra);
    }
    return 0;
}

static void usage(FILE *f)
{
    fputs("usage: lmg [-C DIR] [-m MODEL] [-e URL] [-k N] [--verbose] [--yolo] QUESTION\n", f);
}

static int parse_cli(struct config *cfg, int argc, char **argv)
{
    static const struct option options[] = {
        {"verbose", no_argument, NULL, 1000},
        {"yolo", no_argument, NULL, 1001},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "C:m:e:k:h", options, NULL)) != -1) {
        switch (c) {
        case 'C': replace_string(&cfg->repo, optarg); break;
        case 'm': replace_string(&cfg->model, optarg); break;
        case 'e': replace_string(&cfg->endpoint, optarg); break;
        case 'k':
            if (!parse_positive_int(optarg, &cfg->max_steps)) {
                fputs("lmg: invalid maximum round count\n", stderr);
                return -1;
            }
            break;
        case 1000: cfg->verbose = true; break;
        case 1001: cfg->yolo = true; break;
        case 'h': usage(stdout); exit(0);
        default: usage(stderr); return -1;
        }
    }
    if (optind + 1 != argc) {
        usage(stderr);
        return -1;
    }
    cfg->question = xstrdup(argv[optind]);
    return 0;
}

static int resolve_repo(struct config *cfg)
{
    char *resolved;
    struct stat st;
    if (!cfg->repo)
        cfg->repo = xstrdup(".");
    resolved = realpath(cfg->repo, NULL);
    if (!resolved || stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "lmg: invalid repository directory: %s\n", cfg->repo);
        return -1;
    }
    cfg->repo = resolved;
    return 0;
}

static int remove_tree_callback(const char *path, const struct stat *st,
                                int type, struct FTW *ftw)
{
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

static void cleanup_run_context(struct run_context *ctx)
{
    if (!ctx) return;
#ifdef __APPLE__
#endif
    if (ctx->temp_dir) {
        nftw(ctx->temp_dir, remove_tree_callback, 32, FTW_DEPTH | FTW_PHYS);
    }
    memset(ctx, 0, sizeof(*ctx));
}

#ifdef __APPLE__
static char *seatbelt_quote(const char *s)
{
    size_t len = 2;
    const unsigned char *p;
    char *out, *q;
    for (p = (const unsigned char *)s; *p; p++)
        len += (*p == '\\' || *p == '"') ? 2 : 1;
    out = xmalloc(len + 1);
    q = out;
    *q++ = '"';
    for (p = (const unsigned char *)s; *p; p++) {
        if (*p == '\\' || *p == '"') *q++ = '\\';
        *q++ = (char)*p;
    }
    *q++ = '"';
    *q = '\0';
    return out;
}

static int write_macos_profile(struct run_context *ctx)
{
    char *repo = seatbelt_quote(ctx->cfg->repo);
    char *tmp = seatbelt_quote(ctx->temp_dir);
    char *profile;
    int fd;
    size_t written = 0, profile_len;
    ctx->profile_path = xasprintf("%s/sandbox.sb", ctx->temp_dir);
    profile = xasprintf(
        "(version 1)\n"
        "(deny default)\n"
        "(allow process*)\n"
        "(allow signal (target self))\n"
        "(allow sysctl-read)\n"
        "(allow mach-lookup)\n"
        "(allow file-read*\n"
        "  (subpath %s)\n"
        "  (subpath %s)\n"
        "  (subpath \"/System\")\n"
        "  (subpath \"/usr\")\n"
        "  (subpath \"/bin\")\n"
        "  (subpath \"/sbin\")\n"
        "  (subpath \"/Library/Apple\")\n"
        "  (subpath \"/private/etc\")\n"
        "  (subpath \"/private/var/db/dyld\")\n"
        "  (subpath \"/dev\"))\n"
        "(allow file-write* (subpath %s))\n",
        repo, tmp, tmp);
    fd = open(ctx->profile_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    profile_len = strlen(profile);
    while (written < profile_len) {
        ssize_t n = write(fd, profile + written, profile_len - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        written += (size_t)n;
    }
    close(fd);
    return 0;
}
#endif

static int init_run_context(struct run_context *ctx, const struct config *cfg)
{
    char template[] = "/tmp/lmg.XXXXXX";
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = cfg;
    ctx->temp_dir = xstrdup(template);
    if (!mkdtemp(ctx->temp_dir)) {
        fputs("lmg: cannot create private temporary directory\n", stderr);
        cleanup_run_context(ctx);
        return -1;
    }
#ifdef __APPLE__
    if (!cfg->yolo && write_macos_profile(ctx) < 0) {
        fputs("lmg: cannot create sandbox profile\n", stderr);
        cleanup_run_context(ctx);
        return -1;
    }
#endif
    return 0;
}

static double monotonic_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static const char *bash_path(void)
{
    if (access("/bin/bash", X_OK) == 0) return "/bin/bash";
    if (access("/usr/bin/bash", X_OK) == 0) return "/usr/bin/bash";
    return NULL;
}

#ifdef __linux__
static const char *bwrap_path(void)
{
    if (access("/usr/bin/bwrap", X_OK) == 0) return "/usr/bin/bwrap";
    if (access("/bin/bwrap", X_OK) == 0) return "/bin/bwrap";
    return NULL;
}
#endif

static char **minimal_environment(const struct run_context *ctx)
{
    char **env = calloc(6, sizeof(char *));
    if (!env) die_oom();
    env[0] = xstrdup("PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin");
    env[1] = xstrdup("LANG=C.UTF-8");
    env[2] = xstrdup("LC_ALL=C.UTF-8");
    env[3] = xasprintf("HOME=%s", ctx->temp_dir);
    env[4] = xasprintf("TMPDIR=%s", ctx->temp_dir);
    return env;
}

static void exec_unsandboxed(const struct run_context *ctx, const char *command,
                             char **env)
{
    const char *bash = bash_path();
    char *const argv[] = {(char *)"bash", (char *)"-lc", (char *)command, NULL};
    if (!bash) {
        dprintf(STDERR_FILENO, "bash not found\n");
        _exit(127);
    }
    if (chdir(ctx->cfg->repo) < 0) {
        dprintf(STDERR_FILENO, "cannot enter repository: %s\n", strerror(errno));
        _exit(127);
    }
    execve(bash, argv, env);
    dprintf(STDERR_FILENO, "cannot execute bash: %s\n", strerror(errno));
    _exit(127);
}

#ifdef __linux__
static void push_arg(char **argv, size_t *n, char *arg)
{
    argv[(*n)++] = arg;
}

static void exec_linux_sandbox(const struct run_context *ctx, const char *command,
                               char **env)
{
    const char *bwrap = bwrap_path();
    const char *bash = bash_path();
    const char *mounts[] = {
        "/usr", "/usr/local", "/bin", "/lib", "/lib64", "/sbin", "/etc",
        "/opt", "/nix", NULL
    };
    char *argv[96];
    size_t n = 0, i;
    (void)env;
    if (!bwrap || !bash) _exit(127);
    push_arg(argv, &n, (char *)"bwrap");
    push_arg(argv, &n, (char *)"--unshare-all");
    push_arg(argv, &n, (char *)"--die-with-parent");
    push_arg(argv, &n, (char *)"--new-session");
    push_arg(argv, &n, (char *)"--clearenv");
    for (i = 0; mounts[i]; i++) {
        if (access(mounts[i], F_OK) == 0) {
            push_arg(argv, &n, (char *)"--ro-bind");
            push_arg(argv, &n, (char *)mounts[i]);
            push_arg(argv, &n, (char *)mounts[i]);
        }
    }
    push_arg(argv, &n, (char *)"--ro-bind");
    push_arg(argv, &n, ctx->cfg->repo);
    push_arg(argv, &n, (char *)"/repo");
    push_arg(argv, &n, (char *)"--tmpfs");
    push_arg(argv, &n, (char *)"/tmp");
    push_arg(argv, &n, (char *)"--proc");
    push_arg(argv, &n, (char *)"/proc");
    push_arg(argv, &n, (char *)"--dev");
    push_arg(argv, &n, (char *)"/dev");
    push_arg(argv, &n, (char *)"--chdir");
    push_arg(argv, &n, (char *)"/repo");
    push_arg(argv, &n, (char *)"--setenv");
    push_arg(argv, &n, (char *)"HOME");
    push_arg(argv, &n, (char *)"/tmp");
    push_arg(argv, &n, (char *)"--setenv");
    push_arg(argv, &n, (char *)"TMPDIR");
    push_arg(argv, &n, (char *)"/tmp");
    push_arg(argv, &n, (char *)"--setenv");
    push_arg(argv, &n, (char *)"PATH");
    push_arg(argv, &n, (char *)"/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin");
    push_arg(argv, &n, (char *)"--setenv");
    push_arg(argv, &n, (char *)"LANG");
    push_arg(argv, &n, (char *)"C.UTF-8");
    push_arg(argv, &n, (char *)"--setenv");
    push_arg(argv, &n, (char *)"LC_ALL");
    push_arg(argv, &n, (char *)"C.UTF-8");
    push_arg(argv, &n, (char *)bash);
    push_arg(argv, &n, (char *)"-lc");
    push_arg(argv, &n, (char *)command);
    argv[n] = NULL;
    execve(bwrap, argv, env);
    dprintf(STDERR_FILENO, "cannot execute bwrap: %s\n", strerror(errno));
    _exit(127);
}
#endif

#ifdef __APPLE__
static void exec_macos_sandbox(const struct run_context *ctx, const char *command,
                               char **env)
{
    const char *bash = bash_path();
    char *const argv[] = {
        (char *)"sandbox-exec", (char *)"-f", ctx->profile_path,
        (char *)bash, (char *)"-lc", (char *)command, NULL
    };
    if (!bash || access("/usr/bin/sandbox-exec", X_OK) != 0) _exit(127);
    if (chdir(ctx->cfg->repo) < 0) _exit(127);
    execve("/usr/bin/sandbox-exec", argv, env);
    dprintf(STDERR_FILENO, "cannot execute sandbox-exec: %s\n", strerror(errno));
    _exit(127);
}
#endif

static void close_pipe(int p[2])
{
    if (p[0] >= 0) close(p[0]);
    if (p[1] >= 0) close(p[1]);
}

static void drain_fd(int fd, struct buffer *dst, bool *open_flag)
{
    char chunk[8192];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            if (!buffer_append(dst, chunk, (size_t)n)) die_oom();
            continue;
        }
        if (n == 0) {
            close(fd);
            *open_flag = false;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(fd);
            *open_flag = false;
        }
        return;
    }
}

static int execute_command(const struct run_context *ctx, const char *command,
                           struct command_result *result)
{
    int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    pid_t pid;
    bool out_open = true, err_open = true, child_done = false;
    int status = 0;
    double start;
    char **env;
    memset(result, 0, sizeof(*result));
    if (!buffer_init(&result->out, TOOL_OUTPUT_LIMIT) ||
        !buffer_init(&result->err, TOOL_OUTPUT_LIMIT))
        die_oom();
    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
        close_pipe(out_pipe); close_pipe(err_pipe);
        return -1;
    }
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(out_pipe[1], F_SETFD, FD_CLOEXEC);
    env = minimal_environment(ctx);
    pid = fork();
    if (pid < 0) {
        close_pipe(out_pipe); close_pipe(err_pipe);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close_pipe(out_pipe); close_pipe(err_pipe);
        if (ctx->cfg->yolo) {
            exec_unsandboxed(ctx, command, env);
        } else {
#ifdef __linux__
            exec_linux_sandbox(ctx, command, env);
#elif defined(__APPLE__)
            exec_macos_sandbox(ctx, command, env);
#else
            dprintf(STDERR_FILENO, "sandbox unsupported on this platform\n");
            _exit(127);
#endif
        }
    }
    setpgid(pid, pid);
    close(out_pipe[1]); out_pipe[1] = -1;
    close(err_pipe[1]); err_pipe[1] = -1;
    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);
    start = monotonic_seconds();
    while (out_open || err_open || !child_done) {
        struct pollfd fds[2];
        nfds_t nfds = 0;
        if (out_open) { fds[nfds].fd = out_pipe[0]; fds[nfds].events = POLLIN | POLLHUP; nfds++; }
        if (err_open) { fds[nfds].fd = err_pipe[0]; fds[nfds].events = POLLIN | POLLHUP; nfds++; }
        if (!result->timed_out && monotonic_seconds() - start >= COMMAND_TIMEOUT_SECONDS) {
            result->timed_out = true;
            kill(-pid, SIGKILL);
        }
        if (nfds)
            poll(fds, nfds, 100);
        if (out_open) drain_fd(out_pipe[0], &result->out, &out_open);
        if (err_open) drain_fd(err_pipe[0], &result->err, &err_open);
        if (!child_done) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) child_done = true;
            else if (w < 0 && errno != EINTR) child_done = true;
        }
    }
    /* Do not leave background descendants from the command group running. */
    kill(-pid, SIGKILL);
    if (!child_done) waitpid(pid, &status, 0);
    if (result->timed_out)
        result->exit_code = 124;
    else if (WIFEXITED(status))
        result->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result->exit_code = 128 + WTERMSIG(status);
    else
        result->exit_code = 1;
    return 0;
}

static char *format_command_result(const struct command_result *r)
{
    struct buffer b;
    char header[128];
    size_t budget = TOOL_OUTPUT_LIMIT - 128;
    size_t out_take = r->out.len < budget ? r->out.len : budget;
    size_t err_take = r->err.len < budget - out_take ? r->err.len : budget - out_take;
    bool out_cut, err_cut;
    if (err_take < r->err.len && out_take > budget * 3 / 4) {
        out_take = budget * 3 / 4;
        err_take = r->err.len < budget - out_take ? r->err.len : budget - out_take;
    }
    if (out_take < r->out.len && err_take < r->err.len &&
        out_take + err_take < budget) {
        size_t more = budget - out_take - err_take;
        size_t available = r->out.len - out_take;
        if (more > available) more = available;
        out_take += more;
    }
    out_cut = r->out.truncated || out_take < r->out.len;
    err_cut = r->err.truncated || err_take < r->err.len;
    if (!buffer_init(&b, TOOL_OUTPUT_LIMIT)) die_oom();
    snprintf(header, sizeof(header), "exit_code: %d\n", r->exit_code);
    buffer_append_cstr(&b, header);
    if (r->timed_out) buffer_append_cstr(&b, "timed_out: true\n");
    buffer_append_cstr(&b, "stdout:\n");
    buffer_append(&b, r->out.data, out_take);
    if (out_take && r->out.data[out_take - 1] != '\n') buffer_append_cstr(&b, "\n");
    if (out_cut) buffer_append_cstr(&b, "[output truncated]\n");
    buffer_append_cstr(&b, "stderr:\n");
    buffer_append(&b, r->err.data, err_take);
    if (err_take && r->err.data[err_take - 1] != '\n') buffer_append_cstr(&b, "\n");
    if (err_cut) buffer_append_cstr(&b, "[output truncated]\n");
    return b.data;
}

static bool sandbox_available(const struct run_context *ctx)
{
    struct command_result result;
    bool ok;
    if (ctx->cfg->yolo)
        return true;
#ifdef __linux__
    if (!bwrap_path()) {
        fputs("lmg: bwrap not found; use --yolo to run unsandboxed\n", stderr);
        return false;
    }
#elif defined(__APPLE__)
    if (access("/usr/bin/sandbox-exec", X_OK) != 0) {
        fputs("lmg: sandbox-exec failed; use --yolo to run unsandboxed\n", stderr);
        return false;
    }
#else
    fputs("lmg: sandboxing unsupported on this platform; use --yolo to run unsandboxed\n", stderr);
    return false;
#endif
    if (execute_command(ctx, ":", &result) < 0) {
        fputs("lmg: sandbox initialization failed; use --yolo to run unsandboxed\n", stderr);
        return false;
    }
    ok = !result.timed_out && result.exit_code == 0;
    if (!ok) {
#ifdef __APPLE__
        fputs("lmg: sandbox-exec failed; use --yolo to run unsandboxed\n", stderr);
#else
        fputs("lmg: bwrap failed; use --yolo to run unsandboxed\n", stderr);
#endif
        if (ctx->cfg->verbose && result.err.len)
            fprintf(stderr, "lmg: sandbox: %.*s", (int)result.err.len, result.err.data);
    }
    return ok;
}

static size_t http_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct buffer *b = userdata;
    size_t n;
    if (size && nmemb > SIZE_MAX / size)
        return 0;
    n = size * nmemb;
    if (n > b->limit - b->len)
        return 0;
    if (!buffer_append(b, ptr, n))
        return 0;
    return n;
}

static int post_chat(const struct config *cfg, json_object *request,
                     json_object **response)
{
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    struct buffer body;
    char *auth = NULL;
    CURLcode cc;
    long status = 0;
    json_object *parsed;
    const char *payload = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
    if (!curl || !buffer_init(&body, HTTP_RESPONSE_LIMIT)) {
        die_oom();
    }
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (cfg->api_key && cfg->api_key[0]) {
        auth = xasprintf("Authorization: Bearer %s", cfg->api_key);
        headers = curl_slist_append(headers, auth);
    }
    curl_easy_setopt(curl, CURLOPT_URL, cfg->endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(payload));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lmg/1");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    cc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (cc != CURLE_OK) {
        fprintf(stderr, "lmg: API request failed: %s\n", curl_easy_strerror(cc));
        return -1;
    }
    if (status < 200 || status >= 300) {
        fprintf(stderr, "lmg: API returned HTTP %ld\n", status);
        return -1;
    }
    parsed = parse_json_strict(body.data, body.len);
    if (!parsed || !json_object_is_type(parsed, json_type_object)) {
        fputs("lmg: API returned invalid JSON\n", stderr);
        return -1;
    }
    *response = parsed;
    return 0;
}

static json_object *make_tools(void)
{
    json_object *tools = json_object_new_array();
    json_object *tool = json_object_new_object();
    json_object *function = json_object_new_object();
    json_object *parameters = json_object_new_object();
    json_object *properties = json_object_new_object();
    json_object *command = json_object_new_object();
    json_object *required = json_object_new_array();
    json_object_object_add(command, "type", json_object_new_string("string"));
    json_object_object_add(properties, "command", command);
    json_object_object_add(parameters, "type", json_object_new_string("object"));
    json_object_object_add(parameters, "properties", properties);
    json_object_array_add(required, json_object_new_string("command"));
    json_object_object_add(parameters, "required", required);
    json_object_object_add(function, "name", json_object_new_string("bash"));
    json_object_object_add(function, "description",
        json_object_new_string("Run a shell command in the repository to search or inspect the codebase."));
    json_object_object_add(function, "parameters", parameters);
    json_object_object_add(tool, "type", json_object_new_string("function"));
    json_object_object_add(tool, "function", function);
    json_object_array_add(tools, tool);
    return tools;
}

static json_object *make_message(const char *role, const char *content)
{
    json_object *message = json_object_new_object();
    json_object_object_add(message, "role", json_object_new_string(role));
    json_object_object_add(message, "content", json_object_new_string(content));
    return message;
}

static json_object *make_request(const struct config *cfg, json_object *messages,
                                 json_object *tools)
{
    json_object *request = json_object_new_object();
    json_object_object_foreach(cfg->extra, key, value) {
        if (strcmp(key, "model") != 0 && strcmp(key, "messages") != 0 &&
            strcmp(key, "tools") != 0 && strcmp(key, "tool_choice") != 0 &&
            strcmp(key, "stream") != 0)
            json_object_object_add(request, key, json_object_get(value));
    }
    json_object_object_add(request, "model", json_object_new_string(cfg->model));
    json_object_object_add(request, "messages", json_object_get(messages));
    json_object_object_add(request, "tools", json_object_get(tools));
    json_object_object_add(request, "tool_choice", json_object_new_string("auto"));
    json_object_object_add(request, "stream", json_object_new_boolean(false));
    return request;
}

static json_object *response_message(json_object *response)
{
    json_object *choices, *choice, *message;
    if (!json_object_object_get_ex(response, "choices", &choices) ||
        !json_object_is_type(choices, json_type_array) ||
        json_object_array_length(choices) < 1)
        return NULL;
    choice = json_object_array_get_idx(choices, 0);
    if (!choice || !json_object_is_type(choice, json_type_object) ||
        !json_object_object_get_ex(choice, "message", &message) ||
        !json_object_is_type(message, json_type_object))
        return NULL;
    return message;
}

static int parse_bash_call(json_object *call, const char **id, char **command)
{
    json_object *id_obj, *function, *name, *arguments, *args_obj, *command_obj;
    const char *args_text;
    size_t args_len;
    if (!json_object_is_type(call, json_type_object) ||
        !json_object_object_get_ex(call, "id", &id_obj) ||
        !plain_json_string(id_obj) ||
        !json_object_object_get_ex(call, "function", &function) ||
        !json_object_is_type(function, json_type_object) ||
        !json_object_object_get_ex(function, "name", &name) ||
        !plain_json_string(name) ||
        strcmp(plain_json_string(name), "bash") != 0 ||
        !json_object_object_get_ex(function, "arguments", &arguments) ||
        !plain_json_string(arguments))
        return -1;
    args_text = plain_json_string(arguments);
    args_len = strlen(args_text);
    args_obj = parse_json_strict(args_text, args_len);
    if (!args_obj || !json_object_is_type(args_obj, json_type_object) ||
        !json_object_object_get_ex(args_obj, "command", &command_obj) ||
        !plain_json_string(command_obj)) {
        return -1;
    }
    *id = plain_json_string(id_obj);
    *command = xstrdup(plain_json_string(command_obj));
    return 0;
}

static int run_agent(const struct config *cfg, const struct run_context *ctx)
{
    json_object *messages = json_object_new_array();
    json_object *tools = make_tools();
    int round, rc = EXIT_AGENT;
    json_object_array_add(messages, make_message("system", SYSTEM_PROMPT));
    json_object_array_add(messages, make_message("user", cfg->question));
    for (round = 1; round <= cfg->max_steps; round++) {
        json_object *request = make_request(cfg, messages, tools);
        json_object *response = NULL;
        json_object *message, *calls, *content;
        size_t i, call_count = 0;
        if (post_chat(cfg, request, &response) < 0) {
            rc = EXIT_API;
            break;
        }
        message = response_message(response);
        if (!message) {
            fputs("lmg: API response has no assistant message\n", stderr);
            rc = EXIT_API;
            break;
        }
        json_object_array_add(messages, json_object_get(message));
        if (json_object_object_get_ex(message, "tool_calls", &calls)) {
            if (!json_object_is_type(calls, json_type_array)) {
                fputs("lmg: API returned invalid tool calls\n", stderr);
                rc = EXIT_API; break;
            }
            call_count = json_object_array_length(calls);
        }
        if (call_count) {
            for (i = 0; i < call_count; i++) {
                json_object *call = json_object_array_get_idx(calls, i);
                json_object *tool_message;
                struct command_result result;
                const char *id;
                char *command = NULL;
                char *result_text;
                if (parse_bash_call(call, &id, &command) < 0) {
                    fputs("lmg: malformed arguments for bash\n", stderr);
                    rc = EXIT_LOCAL;
                    break;
                }
                if (cfg->verbose)
                    fprintf(stderr, "lmg: bash: %s\n", command);
                if (execute_command(ctx, command, &result) < 0) {
                    fputs("lmg: cannot execute bash\n", stderr);
                    rc = EXIT_LOCAL; break;
                }
                if (cfg->verbose)
                    fprintf(stderr, "lmg: bash exited %d%s\n", result.exit_code,
                            result.timed_out ? " (timed out)" : "");
                result_text = format_command_result(&result);
                tool_message = json_object_new_object();
                json_object_object_add(tool_message, "role", json_object_new_string("tool"));
                json_object_object_add(tool_message, "tool_call_id", json_object_new_string(id));
                json_object_object_add(tool_message, "content", json_object_new_string(result_text));
                json_object_array_add(messages, tool_message);
            }
            if (rc == EXIT_LOCAL) break;
            continue;
        }
        if (json_object_object_get_ex(message, "content", &content) &&
            json_object_is_type(content, json_type_string) &&
            json_object_get_string_len(content) > 0) {
            const char *answer = json_object_get_string(content);
            size_t answer_len = (size_t)json_object_get_string_len(content);
            fwrite(answer, 1, answer_len, stdout);
            if (answer[answer_len - 1] != '\n') fputc('\n', stdout);
            rc = 0;
            break;
        }
        fputs("lmg: agent failed to produce an answer\n", stderr);
        rc = EXIT_AGENT;
        break;
    }
    if (round > cfg->max_steps && rc == EXIT_AGENT)
        fprintf(stderr, "lmg: agent exceeded %d rounds\n", cfg->max_steps);
    return rc;
}

int main(int argc, char **argv)
{
    struct config cfg;
    struct run_context run;
    int rc = EXIT_LOCAL;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model = xstrdup(DEFAULT_MODEL);
    cfg.max_steps = DEFAULT_MAX_STEPS;
    cfg.extra = json_object_new_object();
    if (!cfg.extra) die_oom();
    signal(SIGPIPE, SIG_IGN);
    if (load_file_config(&cfg) < 0 || load_environment(&cfg) < 0 ||
        parse_cli(&cfg, argc, argv) < 0 || resolve_repo(&cfg) < 0)
        goto done;
    if (!cfg.endpoint || !cfg.endpoint[0]) {
        fputs("lmg: endpoint is not configured\n", stderr);
        goto done;
    }
    if (!cfg.model || !cfg.model[0]) {
        fputs("lmg: model is not configured\n", stderr);
        goto done;
    }
    if (init_run_context(&run, &cfg) < 0)
        goto done;
    if (!sandbox_available(&run)) {
        cleanup_run_context(&run);
        goto done;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fputs("lmg: cannot initialize HTTP client\n", stderr);
        cleanup_run_context(&run);
        goto done;
    }
    rc = run_agent(&cfg, &run);
    cleanup_run_context(&run);
done:
    return rc;
}
