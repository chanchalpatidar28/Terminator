/* engine.c - the shell engine: parsing, expansion, pipelines, redirection,
 * built-ins, job control and multiWatch.  It knows nothing about the GUI: it
 * reads lines from in_fd and writes all output to out_fd.                    */
#include "shell.h"
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#define DASHES "----------------------------------------------------"

static pthread_mutex_t fork_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------ small utils */
typedef struct { char *p; size_t len, cap; } Buf;
static void buf_add(Buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->len, s, n);
    b->len += n; b->p[b->len] = 0;
}
static void buf_addc(Buf *b, char c) { buf_add(b, &c, 1); }
static char *buf_take(Buf *b)
{
    char *r = b->p ? b->p : strdup("");
    b->p = NULL; b->len = b->cap = 0;
    return r;
}

static void vout(int fd, const char *fmt, va_list ap) { vdprintf(fd, fmt, ap); }
void outf(Shell *sh, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vout(sh->out_fd, fmt, ap); va_end(ap); }
void errf(Shell *sh, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vout(sh->err_fd, fmt, ap); va_end(ap); }

void abs_path(Shell *sh, const char *p, char *out)
{
    if (p[0] == '/') snprintf(out, PATH_MAX, "%s", p);
    else snprintf(out, PATH_MAX, "%s/%s", sh->cwd, p);
}

/* ------------------------------------------------------------- variables */
static const char *get_var(Shell *sh, const char *name)
{
    static __thread char tmp[32];
    if (!strcmp(name, "?")) { snprintf(tmp, sizeof tmp, "%d", sh->last_status); return tmp; }
    if (!strcmp(name, "#")) { snprintf(tmp, sizeof tmp, "%d", sh->argc_pos); return tmp; }
    if (!strcmp(name, "$")) { snprintf(tmp, sizeof tmp, "%d", (int)getpid()); return tmp; }
    if (isdigit((unsigned char)name[0]) && !name[1]) {
        int d = name[0] - '0';
        if (d == 0) return sh->pos[0] ? sh->pos[0] : "terminator";
        return (d <= sh->argc_pos && sh->pos[d]) ? sh->pos[d] : "";
    }
    if (!strcmp(name, "PWD")) return sh->cwd;
    if (!strcmp(name, "OLDPWD")) return sh->oldpwd;
    for (int i = 0; i < sh->nvars; i++) if (!strcmp(sh->vars[i].name, name)) return sh->vars[i].val;
    const char *e = getenv(name);
    return e ? e : "";
}

void set_var(Shell *sh, const char *name, const char *val, int exported)
{
    for (int i = 0; i < sh->nvars; i++)
        if (!strcmp(sh->vars[i].name, name)) {
            free(sh->vars[i].val); sh->vars[i].val = strdup(val);
            if (exported) sh->vars[i].exported = 1;
            return;
        }
    if (sh->nvars >= MAX_VARS) return;
    sh->vars[sh->nvars++] = (Var){ strdup(name), strdup(val), exported };
}

static void unset_var(Shell *sh, const char *name)
{
    for (int i = 0; i < sh->nvars; i++)
        if (!strcmp(sh->vars[i].name, name)) {
            free(sh->vars[i].name); free(sh->vars[i].val);
            sh->vars[i] = sh->vars[--sh->nvars];
            return;
        }
}

/* --------------------------------------------------- line completeness */
int line_incomplete(const char *s)
{
    Scan sc; scan_init(&sc);
    size_t i = 0;
    while (s[i]) scan_next(&sc, s, &i);
    return sc.n > 1 || sc.trail;
}

/* join a continuation line: "\<newline>" is removed, otherwise keep the newline */
char *join_lines(char *acc, const char *next)
{
    if (!acc) return strdup(next);
    Scan sc; scan_init(&sc);
    size_t i = 0, n = strlen(acc);
    while (acc[i]) scan_next(&sc, acc, &i);
    Buf b = { 0 };
    if (sc.trail && n && acc[n - 1] == '\\') {
        buf_add(&b, acc, n - 1);                         /* drop the backslash */
        if (SPEC_DQ_NEWLINE && sc.st[sc.n - 1] == C_DQ) buf_addc(&b, '\n');   /* ...but keep the newline inside "..." */
    }
    else { buf_add(&b, acc, n); buf_addc(&b, '\n'); }
    buf_add(&b, next, strlen(next));
    free(acc);
    return buf_take(&b);
}

/* pointer to the ')' that closes a "$(" whose body starts at s */
static const char *find_close(const char *s)
{
    Scan sc; scan_init(&sc); scan_push(&sc, C_PAREN);
    size_t i = 0;
    while (s[i]) {
        size_t j = i;
        scan_next(&sc, s, &i);
        if (sc.n == 1) return s + j;
    }
    return NULL;
}

/* pointer to the closing backtick of a `...` whose body starts at s */
static const char *find_bq(const char *s)
{
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1]) { i++; continue; }
        if (s[i] == '`') return s + i;
    }
    return NULL;
}

/* ------------------------------------------------------ child plumbing */
static void child_prepare(Shell *sh, pid_t pgid)
{
    pthread_mutex_init(&fork_lock, NULL);          /* copy may be "locked" */
    if (sh->job_control) setpgid(0, pgid);
    sh->job_control = 0; sh->ctl = NULL;
    signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL); signal(SIGTTOU, SIG_DFL); signal(SIGPIPE, SIG_DFL);
    if (chdir(sh->cwd) < 0) { /* keep going in the old dir */ }
    for (int i = 0; i < sh->nvars; i++)
        if (sh->vars[i].exported) setenv(sh->vars[i].name, sh->vars[i].val, 1);
    setenv("PWD", sh->cwd, 1);
}

/* run `cmd` in a forked child, return everything it printed ($(...)) */
static char *run_capture(Shell *sh, const char *cmd)
{
    int p[2];
    pthread_mutex_lock(&fork_lock);
    if (pipe2(p, O_CLOEXEC) < 0) { pthread_mutex_unlock(&fork_lock); return strdup(""); }
    pid_t pid = fork();
    if (pid == 0) {
        child_prepare(sh, 0);
        close(p[0]);
        sh->err_fd = sh->err_fd;           /* stderr still goes to the tab */
        sh->out_fd = p[1];
        run_line(sh, cmd);
        _exit(sh->last_status);
    }
    close(p[1]);
    pthread_mutex_unlock(&fork_lock);
    Buf b = { 0 }; char tmp[4096]; ssize_t r;
    if (pid > 0)
        while ((r = read(p[0], tmp, sizeof tmp)) > 0 || (r < 0 && errno == EINTR))
            if (r > 0) buf_add(&b, tmp, (size_t)r);
    close(p[0]);
    if (pid > 0) { int st; waitpid(pid, &st, 0); sh->last_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); }
    return buf_take(&b);
}

/* ------------------------------------------------------------ tokenizer */
enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_ERR, T_ERRAPP, T_ERRDUP };
typedef struct { int type; char *text; } Token;
typedef struct { Token *v; int n, cap; } Toks;
static void tok_push(Toks *t, int type, char *text)
{
    if (t->n >= t->cap) { t->cap = t->cap ? t->cap * 2 : 16; t->v = realloc(t->v, t->cap * sizeof(Token)); }
    t->v[t->n++] = (Token){ type, text };
}
static void toks_free(Toks *t) { for (int i = 0; i < t->n; i++) free(t->v[i].text); free(t->v); t->v = NULL; t->n = t->cap = 0; }

static int do_glob(Shell *sh, const char *pat, Toks *t)
{
    char full[PATH_MAX * 2];
    int rel = pat[0] != '/';
    if (rel) snprintf(full, sizeof full, "%s/%s", sh->cwd, pat);
    else snprintf(full, sizeof full, "%s", pat);
    glob_t g;
    if (glob(full, 0, NULL, &g) != 0) { globfree(&g); return 0; }
    size_t pre = rel ? strlen(sh->cwd) + 1 : 0, cnt = g.gl_pathc;
    for (size_t i = 0; i < cnt; i++) tok_push(t, T_WORD, strdup(g.gl_pathv[i] + pre));
    globfree(&g);
    return cnt > 0;
}

static void flush_word(Shell *sh, Toks *t, Buf *cur, int *inword, int *globf)
{
    if (!*inword) return;
    char *w = buf_take(cur);
    if (*globf && do_glob(sh, w, t)) free(w);
    else tok_push(t, T_WORD, w);
    *inword = 0; *globf = 0;
}

static void expand_dollar(Shell *sh, const char **pp, Buf *out, int quoted)
{
    const char *s = *pp;
    if (s[1] == '(') {
        const char *close = find_close(s + 2);
        if (!close) { buf_addc(out, '$'); *pp = s + 1; return; }
        char *inner = strndup(s + 2, (size_t)(close - (s + 2)));
        char *res = run_capture(sh, inner);
        free(inner);
        size_t n = strlen(res);
        while (n && res[n - 1] == '\n') res[--n] = 0;
        if (!quoted) for (char *q = res; *q; q++) if (*q == '\n') *q = ' ';
        buf_add(out, res, n);
        free(res);
        *pp = close + 1;
        return;
    }
    char name[128]; size_t k = 0;
    if (s[1] == '{') {
        const char *e = strchr(s + 2, '}');
        if (!e || (size_t)(e - (s + 2)) >= sizeof name) { buf_addc(out, '$'); *pp = s + 1; return; }
        k = (size_t)(e - (s + 2)); memcpy(name, s + 2, k); name[k] = 0;
        *pp = e + 1;
    } else if (s[1] == '?' || s[1] == '#' || s[1] == '$' || isdigit((unsigned char)s[1])) {
        name[0] = s[1]; name[1] = 0; *pp = s + 2;
    } else if (isalpha((unsigned char)s[1]) || s[1] == '_') {
        const char *e = s + 1;
        while ((isalnum((unsigned char)*e) || *e == '_') && k < sizeof name - 1) name[k++] = *e++;
        name[k] = 0; *pp = e;
    } else { buf_addc(out, '$'); *pp = s + 1; return; }
    const char *v = get_var(sh, name);
    buf_add(out, v, strlen(v));
}

/* `cmd` : s points at the opening backtick; returns the char after the closing one */
static const char *expand_backtick(Shell *sh, const char *s, Buf *out, int quoted)
{
    const char *close = find_bq(s + 1);
    if (!close) { buf_addc(out, '`'); return s + 1; }
    Buf in = { 0 };
    for (const char *q = s + 1; q < close; q++) {            /* \` and \\ and \$ are unescaped */
        if (*q == '\\' && (q[1] == '`' || q[1] == '\\' || q[1] == '$')) q++;
        buf_addc(&in, *q);
    }
    char *res = run_capture(sh, in.p ? in.p : "");
    free(in.p);
    size_t n = strlen(res);
    while (n && res[n - 1] == '\n') res[--n] = 0;
    if (!quoted) for (char *q = res; *q; q++) if (*q == '\n') *q = ' ';
    buf_add(out, res, n);
    free(res);
    return close + 1;
}

static void tokenize(Shell *sh, const char *s, Toks *t)
{
    Buf cur = { 0 };
    int inword = 0, globf = 0;
    while (*s) {
        char c = *s;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { flush_word(sh, t, &cur, &inword, &globf); s++; continue; }
        if (c == '#' && !inword) break;
        if (c == '|') { flush_word(sh, t, &cur, &inword, &globf); tok_push(t, T_PIPE, strdup("|")); s++; continue; }
        if (c == '<') { flush_word(sh, t, &cur, &inword, &globf); tok_push(t, T_IN, strdup("<")); s++; continue; }
        if (c == '>') {
            int fd2 = inword && cur.len == 1 && cur.p[0] == '2';       /* "2>" : redirect stderr */
            if (fd2) { cur.len = 0; cur.p[0] = 0; inword = 0; globf = 0; }
            else flush_word(sh, t, &cur, &inword, &globf);
            if (fd2 && s[1] == '&' && s[2] == '1') { tok_push(t, T_ERRDUP, strdup("2>&1")); s += 3; }
            else if (s[1] == '>') { tok_push(t, fd2 ? T_ERRAPP : T_APPEND, strdup(">>")); s += 2; }
            else { tok_push(t, fd2 ? T_ERR : T_OUT, strdup(">")); s++; }
            continue;
        }
        if (c == '\'') {
            inword = 1; s++;
            while (*s && *s != '\'') buf_addc(&cur, *s++);
            if (*s) s++;
            continue;
        }
        if (c == '"') {
            inword = 1; s++;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1] && strchr("$\"\\`\n", s[1])) { if (s[1] != '\n') buf_addc(&cur, s[1]); s += 2; }
                else if (*s == '$') expand_dollar(sh, &s, &cur, 1);
                else if (*s == '`') s = expand_backtick(sh, s, &cur, 1);
                else buf_addc(&cur, *s++);
            }
            if (*s) s++;
            continue;
        }
        if (c == '\\') {
            if (s[1] == '\n') { s += 2; continue; }
            if (s[1]) { inword = 1; buf_addc(&cur, s[1]); s += 2; } else s++;
            continue;
        }
        if (c == '`') {
            size_t before = cur.len;
            s = expand_backtick(sh, s, &cur, 0);
            if (cur.len > before) inword = 1;
            continue;
        }
        if (c == '$') {
            size_t before = cur.len;
            expand_dollar(sh, &s, &cur, 0);
            if (cur.len > before) inword = 1;
            continue;
        }
        if (c == '~' && !inword && (s[1] == 0 || s[1] == '/' || isspace((unsigned char)s[1]))) {
            const char *h = getenv("HOME"); h = h ? h : "/";
            buf_add(&cur, h, strlen(h)); inword = 1; s++;
            continue;
        }
        inword = 1;
        if (c == '*' || c == '?' || c == '[') globf = 1;
        buf_addc(&cur, c); s++;
    }
    flush_word(sh, t, &cur, &inword, &globf);
    free(cur.p);
}

char **expand_words(Shell *sh, const char *text, int *n)
{
    Toks t = { 0 };
    tokenize(sh, text, &t);
    char **w = calloc((size_t)t.n + 1, sizeof(char *));
    for (int i = 0; i < t.n; i++) { w[i] = t.v[i].text; }
    *n = t.n;
    free(t.v);
    return w;
}
void free_words(char **w, int n) { for (int i = 0; i < n; i++) free(w[i]); free(w); }

/* --------------------------------------------------------------- jobs */
static Job *job_alloc(Shell *sh, const char *cmd)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (!sh->jobs[i].used) {
            Job *j = &sh->jobs[i];
            memset(j, 0, sizeof *j);
            j->used = 1;
            snprintf(j->cmd, sizeof j->cmd, "%s", cmd);
            return j;
        }
    return NULL;
}
static int job_id(Shell *sh, Job *j) { return (int)(j - sh->jobs) + 1; }
static int decode_status(int st) { return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); }

/* Wait for a job's processes. Returns its exit status, or leaves it STOPPED. */
static int wait_job(Shell *sh, Job *j)
{
    int stopped = 0, sigint = 0, other_sig = 0;
    for (int i = 0; i < j->npids; i++) {
        if (!j->alive[i]) continue;
        int st; pid_t r;
        do r = waitpid(j->pids[i], &st, WUNTRACED); while (r < 0 && errno == EINTR);
        if (r < 0) { j->alive[i] = 0; continue; }
        if (WIFSTOPPED(st)) { stopped = 1; continue; }
        j->alive[i] = 0;
        if (WIFSIGNALED(st)) {
            if (WTERMSIG(st) == SIGINT) sigint = 1;
            else if (WTERMSIG(st) != SIGPIPE) other_sig = WTERMSIG(st);
        }
        if (i == j->npids - 1) j->status = decode_status(st);
    }
    if (stopped) {
        j->state = J_STOPPED;
        outf(sh, "\n[%d]+ Stopped %s\n", job_id(sh, j), j->cmd);
        return 128 + SIGTSTP;
    }
    if (sigint) outf(sh, "\n");
    else if (other_sig) outf(sh, "%s\n", strsignal(other_sig));
    int st = j->status;
    j->used = 0;
    return st;
}

static void reap_jobs(Shell *sh)
{
    for (int k = 0; k < MAX_JOBS; k++) {
        Job *j = &sh->jobs[k];
        if (!j->used || j->state != J_RUNNING) continue;
        int any = 0;
        for (int i = 0; i < j->npids; i++) {
            if (!j->alive[i]) continue;
            int st; pid_t r = waitpid(j->pids[i], &st, WNOHANG | WUNTRACED);
            if (r == 0) { any = 1; continue; }
            if (r > 0 && WIFSTOPPED(st)) { j->state = J_STOPPED; any = 1; continue; }
            j->alive[i] = 0;
        }
        if (!any) { outf(sh, "[%d]+ Done %s\n", job_id(sh, j), j->cmd); j->used = 0; }
    }
}

static Job *job_from_arg(Shell *sh, const char *arg)
{
    if (arg) {
        if (*arg == '%') arg++;
        int id = atoi(arg);
        if (id >= 1 && id <= MAX_JOBS && sh->jobs[id - 1].used) return &sh->jobs[id - 1];
        return NULL;
    }
    for (int i = MAX_JOBS - 1; i >= 0; i--) if (sh->jobs[i].used) return &sh->jobs[i];
    return NULL;
}

/* ------------------------------------------------------------ built-ins */
typedef struct { char **argv; int argc; char *in_file, *out_file, *err_file; int append, err_append, err_dup; } Cmd;

static int is_builtin(const char *n)
{
    static const char *b[] = { "cd", "pwd", "exit", "history", "jobs", "fg", "bg", "source", ".",
                               "export", "unset", "bind", "multiWatch", NULL };
    for (int i = 0; b[i]; i++) if (!strcmp(n, b[i])) return 1;
    return 0;
}

static int bi_cd(Shell *sh, int argc, char **argv)
{
    const char *t;
    char tmp[PATH_MAX], real[PATH_MAX];
    if (argc < 2) t = getenv("HOME");
    else if (!strcmp(argv[1], "-")) t = sh->oldpwd;
    else t = argv[1];
    if (!t || !*t) { errf(sh, "cd: no target directory\n"); return 1; }
    abs_path(sh, t, tmp);
    if (!realpath(tmp, real)) { errf(sh, "cd: %s: %s\n", t, strerror(errno)); return 1; }
    struct stat st;
    if (stat(real, &st) < 0 || !S_ISDIR(st.st_mode)) { errf(sh, "cd: %s: Not a directory\n", t); return 1; }
    if (access(real, X_OK) < 0) { errf(sh, "cd: %s: Permission denied\n", t); return 1; }
    snprintf(sh->oldpwd, PATH_MAX, "%s", sh->cwd);
    snprintf(sh->cwd, PATH_MAX, "%s", real);
    if (sh->ctl) snprintf(sh->ctl->cwd, PATH_MAX, "%s", real);
    if (argc > 1 && !strcmp(argv[1], "-")) outf(sh, "%s\n", real);
    return 0;
}

static int bi_history(Shell *sh)
{
    long total = G->hist_total;                 /* benign racy read */
    long avail = hist_count();
    long show = avail < HIST_SHOW ? avail : HIST_SHOW;
    char buf[HIST_LINE];
    for (long k = show - 1; k >= 0; k--)
        if (hist_from_end((int)k, buf, sizeof buf)) outf(sh, "%5ld  %s\n", total - k, buf);
    return 0;
}

static int bi_jobs(Shell *sh)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &sh->jobs[i];
        if (j->used) outf(sh, "[%d]+ %s %s\n", i + 1, j->state == J_STOPPED ? "Stopped" : "Running", j->cmd);
    }
    return 0;
}

static int bi_fg(Shell *sh, int argc, char **argv)
{
    Job *j = job_from_arg(sh, argc > 1 ? argv[1] : NULL);
    if (!j) { errf(sh, "fg: no such job\n"); return 1; }
    outf(sh, "%s\n", j->cmd);
    j->state = J_RUNNING;
    if (sh->ctl && sh->job_control) sh->ctl->fg_pgid = j->pgid;
    if (j->pgid) kill(-j->pgid, SIGCONT);
    int st = wait_job(sh, j);
    if (sh->ctl) sh->ctl->fg_pgid = 0;
    return st;
}

static int bi_bg(Shell *sh, int argc, char **argv)
{
    Job *j = job_from_arg(sh, argc > 1 ? argv[1] : NULL);
    if (!j) { errf(sh, "bg: no such job\n"); return 1; }
    if (j->pgid) kill(-j->pgid, SIGCONT);
    j->state = J_RUNNING;
    outf(sh, "[%d]+ %s &\n", job_id(sh, j), j->cmd);
    return 0;
}

static int bi_bind(Shell *sh, int argc, char **argv)
{
    char err[160];
    if (argc == 1) { bind_list(sh->out_fd); return 0; }
    if (argc != 3) { errf(sh, "usage: bind <action> <key>   e.g. bind clear-screen Ctrl+Shift+L\n"); return 1; }
    if (bind_set(argv[1], argv[2], err, sizeof err) < 0) { errf(sh, "bind: %s (keeping the default)\n", err); return 1; }
    if (bind_persist(argv[1], argv[2]) < 0) errf(sh, "bind: could not save to ~/.terminatorrc\n");
    return 0;
}

static int builtin_multiwatch(Shell *sh, const char *rest);

static int run_builtin(Shell *sh, int argc, char **argv)
{
    const char *n = argv[0];
    int st = 0;
    if (!strcmp(n, "cd")) st = bi_cd(sh, argc, argv);
    else if (!strcmp(n, "pwd")) outf(sh, "%s\n", sh->cwd);
    else if (!strcmp(n, "exit")) { sh->exit_flag = 1; sh->exit_code = argc > 1 ? atoi(argv[1]) : sh->last_status; st = sh->exit_code; }
    else if (!strcmp(n, "history")) st = bi_history(sh);
    else if (!strcmp(n, "jobs")) st = bi_jobs(sh);
    else if (!strcmp(n, "fg")) st = bi_fg(sh, argc, argv);
    else if (!strcmp(n, "bg")) st = bi_bg(sh, argc, argv);
    else if (!strcmp(n, "bind")) st = bi_bind(sh, argc, argv);
    else if (!strcmp(n, "export")) {
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) { *eq = 0; set_var(sh, argv[i], eq + 1, 1); *eq = '='; }
            else set_var(sh, argv[i], get_var(sh, argv[i]), 1);
        }
    }
    else if (!strcmp(n, "unset")) { for (int i = 1; i < argc; i++) unset_var(sh, argv[i]); }
    else if (!strcmp(n, "source") || !strcmp(n, ".")) {
        if (argc < 2) { errf(sh, "source: filename argument required\n"); st = 2; }
        else {
            ScriptResult r = script_run(sh, argv[1], argc > 2 ? argc - 2 : -1, argv + 2);
            st = r.code;
            if (r.exited) { sh->exit_flag = 1; sh->exit_code = r.code; }
        }
    }
    else if (!strcmp(n, "multiWatch")) { errf(sh, "usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n"); st = 1; }
    sh->last_status = st;
    return st;
}

/* builtin in the shell process itself, honouring > and >> */
static int run_inproc(Shell *sh, Cmd *c)
{
    int save = sh->out_fd, fd = -1;
    if (c->out_file) {
        char p[PATH_MAX]; abs_path(sh, c->out_file, p);
        fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC | (c->append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { errf(sh, "terminator: %s: %s\n", c->out_file, strerror(errno)); return sh->last_status = 1; }
        sh->out_fd = fd;
    }
    int save_err = sh->err_fd, efd = -1;
    if (c->err_file) {
        char p[PATH_MAX]; abs_path(sh, c->err_file, p);
        efd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC | (c->err_append ? O_APPEND : O_TRUNC), 0644);
        if (efd >= 0) sh->err_fd = efd;
    }
    if (c->err_dup) sh->err_fd = sh->out_fd;
    int st = run_builtin(sh, c->argc, c->argv);
    sh->out_fd = save; sh->err_fd = save_err;
    if (fd >= 0) close(fd);
    if (efd >= 0) close(efd);
    return st;
}

/* ---------------------------------------------------- script detection */
/* 1: run file via our own interpreter, 2: `bash -c "..."` */
static int resolve_script(Shell *sh, char **argv, int argc, const char **path, int *first)
{
    const char *a0 = argv[0];
    if (!strcmp(a0, "bash") || !strcmp(a0, "sh") || !strcmp(a0, "/bin/bash") || !strcmp(a0, "/bin/sh")) {
        if (argc >= 3 && !strcmp(argv[1], "-c")) return 2;
        if (argc >= 2 && argv[1][0] != '-') { *path = argv[1]; *first = 2; return 1; }
        return 0;
    }
    if (strchr(a0, '/')) {
        char p[PATH_MAX]; abs_path(sh, a0, p);
        struct stat st;
        if (stat(p, &st) < 0 || !S_ISREG(st.st_mode) || access(p, X_OK) < 0) return 0;
        FILE *f = fopen(p, "r");
        if (!f) return 0;
        char l[256] = "";
        if (!fgets(l, sizeof l, f)) l[0] = 0;
        fclose(f);
        if (l[0] == '#' && l[1] == '!' && (strstr(l, "bash") || strstr(l, "/sh") || strstr(l, " sh"))) {
            *path = a0; *first = 1; return 1;
        }
    }
    return 0;
}

static void set_pos(Shell *sh, const char *a0, int nargs, char **args)
{
    sh->pos[0] = strdup(a0);
    for (int i = 0; i < 9; i++) sh->pos[i + 1] = i < nargs ? strdup(args[i]) : NULL;
    sh->argc_pos = nargs;
}

/* ------------------------------------------------------------ pipelines */
static void child_run(Shell *sh, Cmd *c, int i, int n, int (*pipes)[2], pid_t pgid)
{
    int fd;
    child_prepare(sh, pgid);
    if (i > 0) dup2(pipes[i - 1][0], 0);
    else if ((fd = open("/dev/null", O_RDONLY)) >= 0) { if (fd != 0) { dup2(fd, 0); close(fd); } }
    if (i < n - 1) dup2(pipes[i][1], 1);
    else if (sh->out_fd != 1) dup2(sh->out_fd, 1);
    if (sh->err_fd != 2) dup2(sh->err_fd, 2);
    if (c->in_file) {
        fd = open(c->in_file, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "terminator: %s: %s\n", c->in_file, strerror(errno)); _exit(1); }
        dup2(fd, 0); close(fd);
    }
    if (c->out_file) {
        fd = open(c->out_file, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { fprintf(stderr, "terminator: %s: %s\n", c->out_file, strerror(errno)); _exit(1); }
        dup2(fd, 1); close(fd);
    }
    if (c->err_file) {
        fd = open(c->err_file, O_WRONLY | O_CREAT | (c->err_append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { fprintf(stderr, "terminator: %s: %s\n", c->err_file, strerror(errno)); _exit(1); }
        dup2(fd, 2); close(fd);
    }
    if (c->err_dup) dup2(1, 2);                      /* 2>&1 (applied after > so `>f 2>&1` works) */
    for (int k = 0; k < n - 1; k++) { close(pipes[k][0]); close(pipes[k][1]); }
    sh->out_fd = 1; sh->err_fd = 2;

    char **argv = c->argv; int argc = c->argc;
    if (is_builtin(argv[0])) { run_builtin(sh, argc, argv); _exit(sh->last_status); }
    const char *path; int first;
    int kind = resolve_script(sh, argv, argc, &path, &first);
    if (kind == 1) { ScriptResult r = script_run(sh, path, argc - first, argv + first); _exit(r.code); }
    if (kind == 2) { set_pos(sh, "bash", argc > 3 ? argc - 3 : 0, argv + 3); run_line(sh, argv[2]); _exit(sh->last_status); }
    execvp(argv[0], argv);
    if (errno == ENOEXEC) { ScriptResult r = script_run(sh, argv[0], argc - 1, argv + 1); _exit(r.code); }
    fprintf(stderr, "terminator: %s: %s\n", argv[0], errno == ENOENT ? "command not found" : strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static int run_pipeline(Shell *sh, Cmd *cmds, int n, int bg, const char *text)
{
    if (n == 1 && is_builtin(cmds[0].argv[0])) return run_inproc(sh, &cmds[0]);
    int pipes[MAX_PROCS][2];
    Job *job = job_alloc(sh, text);
    if (!job) { errf(sh, "terminator: too many jobs\n"); return sh->last_status = 1; }
    pthread_mutex_lock(&fork_lock);
    for (int i = 0; i < n - 1; i++)
        if (pipe2(pipes[i], O_CLOEXEC) < 0) { errf(sh, "terminator: pipe: %s\n", strerror(errno)); pipes[i][0] = pipes[i][1] = -1; }
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) { errf(sh, "terminator: fork: %s\n", strerror(errno)); break; }
        if (pid == 0) child_run(sh, &cmds[i], i, n, pipes, job->pgid);
        if (sh->job_control) {
            if (i == 0) job->pgid = pid;
            setpgid(pid, job->pgid);
        }
        job->pids[i] = pid; job->alive[i] = 1; job->npids = i + 1;
    }
    for (int i = 0; i < n - 1; i++) { if (pipes[i][0] >= 0) close(pipes[i][0]); if (pipes[i][1] >= 0) close(pipes[i][1]); }
    if (sh->ctl && !bg) sh->ctl->fg_pgid = sh->job_control ? job->pgid : 0;
    pthread_mutex_unlock(&fork_lock);
    if (job->npids == 0) { job->used = 0; return sh->last_status = 1; }

    if (bg) {
        outf(sh, "[%d] %d\n", job_id(sh, job), (int)job->pids[job->npids - 1]);
        return sh->last_status = 0;
    }
    int st = wait_job(sh, job);
    if (sh->ctl) sh->ctl->fg_pgid = 0;
    return sh->last_status = st;
}

/* --------------------------------------------------------- multiWatch */
static int builtin_multiwatch(Shell *sh, const char *rest)
{
    char *cmds[MW_MAX]; int nc = 0;
    const char *p = rest;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '[') goto usage;
    p++;
    for (;;) {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"' && *p != '\'') goto usage;
        char q = *p++;
        Buf b = { 0 };
        while (*p && *p != q) { if (*p == '\\' && p[1]) p++; buf_addc(&b, *p++); }
        if (*p != q) { free(b.p); goto usage; }
        p++;
        if (nc < MW_MAX) cmds[nc++] = buf_take(&b); else free(b.p);
    }
    if (nc == 0) goto usage;

    int rfd[MW_MAX]; pid_t pids[MW_MAX]; pid_t pgid = 0;
    int jc = sh->job_control;
    long max_iter = getenv("TERMINATOR_MW_ITERS") ? atol(getenv("TERMINATOR_MW_ITERS")) : 0;
    pthread_mutex_lock(&fork_lock);
    for (int i = 0; i < nc; i++) {
        int pp[2];
        if (pipe2(pp, O_CLOEXEC) < 0) { rfd[i] = -1; pids[i] = 0; continue; }
        pid_t pid = fork();
        if (pid == 0) {
            child_prepare(sh, pgid);
            close(pp[0]);
            sh->out_fd = sh->err_fd = pp[1];
            for (long it = 0; max_iter == 0 || it < max_iter; it++) {
                run_line(sh, cmds[i]);
                if (write(pp[1], "", 1) < 0) _exit(1);       /* end-of-iteration marker */
                if (max_iter == 0 || it + 1 < max_iter) sleep(MW_INTERVAL);
            }
            _exit(0);
        }
        close(pp[1]);
        rfd[i] = pp[0]; pids[i] = pid;
        if (jc) { if (!pgid) pgid = pid; setpgid(pid, pgid); }
    }
    if (sh->ctl) sh->ctl->fg_pgid = jc ? pgid : 0;
    pthread_mutex_unlock(&fork_lock);

    Buf acc[MW_MAX]; memset(acc, 0, sizeof acc);
    int open_n = 0;
    for (int i = 0; i < nc; i++) if (rfd[i] >= 0) open_n++;
    while (open_n > 0) {
        struct pollfd pf[MW_MAX];
        for (int i = 0; i < nc; i++) { pf[i].fd = rfd[i]; pf[i].events = POLLIN; pf[i].revents = 0; }
        if (poll(pf, (nfds_t)nc, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < nc; i++) {
            if (rfd[i] < 0 || !(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char tmp[4096];
            ssize_t r = read(rfd[i], tmp, sizeof tmp);
            if (r <= 0) { close(rfd[i]); rfd[i] = -1; open_n--; continue; }
            for (ssize_t k = 0; k < r; k++) {
                if (tmp[k] != 0) { buf_addc(&acc[i], tmp[k]); continue; }
                outf(sh, "\"%s\", %ld:\n%s\n", cmds[i], (long)time(NULL), DASHES);
                if (acc[i].len) {
                    if (write(sh->out_fd, acc[i].p, acc[i].len) < 0) { /* ignore */ }
                    if (acc[i].p[acc[i].len - 1] != '\n') outf(sh, "\n");
                }
                outf(sh, "%s\n", DASHES);
                acc[i].len = 0; if (acc[i].p) acc[i].p[0] = 0;
            }
        }
    }
    for (int i = 0; i < nc; i++) { if (pids[i] > 0) { int st; waitpid(pids[i], &st, 0); } free(acc[i].p); free(cmds[i]); }
    if (sh->ctl) sh->ctl->fg_pgid = 0;
    outf(sh, "\n");                    /* ends the line the GUI echoed "^C" on */
    return sh->last_status = 0;
usage:
    errf(sh, "usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n");
    for (int i = 0; i < nc; i++) free(cmds[i]);
    return sh->last_status = 1;
}

/* ------------------------------------------------------ segments / lines */
typedef struct { char *text; int conn; int bg; } Seg;   /* conn: 0 ';'  1 '&&'  2 '||' */

static int split_segments(const char *line, Seg **out)
{
    Seg *v = NULL; int n = 0, cap = 0;
    Scan sc; scan_init(&sc);
    size_t i = 0, start = 0; int conn = 0, bg = 0;
    size_t len = strlen(line);
    for (;;) {
        size_t j = i; int end = 0, next_conn = 0, skip = 1;
        if (i >= len) end = 2;
        else {
            int plain = scan_next(&sc, line, &i);
            char c = line[j];
            if (plain) {
                if (c == ';' || c == '\n') end = 1;
                else if (c == '&' && line[j + 1] == '&') { end = 1; next_conn = 1; skip = 2; }
                else if (c == '|' && line[j + 1] == '|') { end = 1; next_conn = 2; skip = 2; }
                else if (c == '&' && j > 0 && line[j - 1] == '>') { /* part of 2>&1 */ }
                else if (c == '&') { end = 1; bg = 1; }
                else if (c == '#' && (j == 0 || isspace((unsigned char)line[j - 1]))) {
                    while (i < len && line[i] != '\n') i++;       /* comment */
                    continue;
                }
            }
        }
        if (!end) continue;
        size_t stop = (end == 2) ? len : j;
        char *t = strndup(line + start, stop - start);
        char *p = t; while (isspace((unsigned char)*p)) p++;
        if (*p) {
            if (n >= cap) { cap = cap ? cap * 2 : 8; v = realloc(v, cap * sizeof(Seg)); }
            v[n++] = (Seg){ t, conn, bg };
        } else free(t);
        if (end == 2) break;
        if (skip == 2) i = j + 2;
        start = i; conn = next_conn; bg = 0;
    }
    *out = v;
    return n;
}

static int is_assign(const char *w)
{
    if (!(isalpha((unsigned char)*w) || *w == '_')) return 0;
    const char *p = w;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    return *p == '=';
}

static int run_segment(Shell *sh, const char *text, int bg)
{
    const char *p = text;
    while (isspace((unsigned char)*p)) p++;
    char jobtxt[256];
    snprintf(jobtxt, sizeof jobtxt, "%s", p);
    for (size_t k = strlen(jobtxt); k > 0 && isspace((unsigned char)jobtxt[k - 1]); k--) jobtxt[k - 1] = 0;
    if (!strncmp(p, "multiWatch", 10) && (p[10] == 0 || isspace((unsigned char)p[10]) || p[10] == '['))
        return builtin_multiwatch(sh, p + 10);

    Toks t = { 0 };
    tokenize(sh, text, &t);
    Cmd cmds[MAX_PROCS]; memset(cmds, 0, sizeof cmds);
    int nc = 0, syntax = 0;
    cmds[0].argv = calloc(t.n + 2, sizeof(char *));
    for (int i = 0; i < t.n && !syntax; i++) {
        Cmd *c = &cmds[nc];
        switch (t.v[i].type) {
        case T_WORD: c->argv[c->argc++] = t.v[i].text; break;
        case T_PIPE:
            if (c->argc == 0 || nc + 1 >= MAX_PROCS) { syntax = 1; break; }
            nc++; cmds[nc].argv = calloc(t.n + 2, sizeof(char *));
            break;
        case T_ERRDUP: c->err_dup = 1; break;
        default:
            if (i + 1 >= t.n || t.v[i + 1].type != T_WORD) { syntax = 1; break; }
            if (t.v[i].type == T_IN) c->in_file = t.v[i + 1].text;
            else if (t.v[i].type == T_ERR || t.v[i].type == T_ERRAPP) { c->err_file = t.v[i + 1].text; c->err_append = t.v[i].type == T_ERRAPP; }
            else { c->out_file = t.v[i + 1].text; c->append = t.v[i].type == T_APPEND; }
            i++;
        }
    }
    int st;
    if (!syntax && cmds[nc].argc == 0 && (nc > 0 || cmds[0].in_file || cmds[0].out_file)) syntax = 1;
    if (syntax) {
        errf(sh, "terminator: syntax error\n");
        st = sh->last_status = 2;
    } else if (cmds[0].argc == 0) {
        st = sh->last_status;                       /* blank / only expansions to nothing */
    } else {
        nc++;
        /* VAR=value [command...] */
        int skip = 0;
        if (nc == 1)
            while (skip < cmds[0].argc && is_assign(cmds[0].argv[skip])) {
                char *eq = strchr(cmds[0].argv[skip], '=');
                *eq = 0; set_var(sh, cmds[0].argv[skip], eq + 1, 0); *eq = '=';
                skip++;
            }
        if (skip == cmds[0].argc) st = sh->last_status = 0;
        else {
            cmds[0].argv += skip; cmds[0].argc -= skip;
            st = run_pipeline(sh, cmds, nc, bg, jobtxt);
            cmds[0].argv -= skip;
        }
        nc--;
    }
    for (int i = 0; i <= nc; i++) free(cmds[i].argv);
    toks_free(&t);
    return st;
}

int run_line(Shell *sh, const char *line)
{
    Seg *segs; int n = split_segments(line, &segs);
    int st = sh->last_status;
    for (int i = 0; i < n && !sh->exit_flag; i++) {
        if (i > 0 && segs[i].conn == 1 && st != 0) continue;
        if (i > 0 && segs[i].conn == 2 && st == 0) continue;
        st = run_segment(sh, segs[i].text, segs[i].bg);
        sh->last_status = st;
    }
    for (int i = 0; i < n; i++) free(segs[i].text);
    free(segs);
    return st;
}

/* -------------------------------------------------------- shell object */
Shell *shell_new(int in_fd, int out_fd, int tab_idx)
{
    Shell *sh = calloc(1, sizeof *sh);
    sh->in_fd = in_fd; sh->out_fd = out_fd; sh->err_fd = out_fd;
    if (!getcwd(sh->cwd, sizeof sh->cwd)) snprintf(sh->cwd, sizeof sh->cwd, "/");
    snprintf(sh->oldpwd, sizeof sh->oldpwd, "%s", sh->cwd);
    sh->pos[0] = strdup("terminator");
    if (tab_idx >= 0) {
        sh->ctl = &G->tabs[tab_idx];
        sh->job_control = 1;
        snprintf(sh->ctl->cwd, PATH_MAX, "%s", sh->cwd);
    } else sh->job_control = 1;
    return sh;
}

static char *read_line(Shell *sh)
{
    Buf b = { 0 };
    for (;;) {
        while (sh->rpos < sh->rlen) {
            char c = sh->rbuf[sh->rpos++];
            if (c == '\n') return buf_take(&b);
            buf_addc(&b, c);
        }
        ssize_t r;
        do r = read(sh->in_fd, sh->rbuf, sizeof sh->rbuf); while (r < 0 && errno == EINTR);
        if (r <= 0) { if (b.len) return buf_take(&b); free(b.p); return NULL; }
        sh->rpos = 0; sh->rlen = (int)r;
    }
}

void shell_run(int in_fd, int out_fd, int tab_idx)
{
    Shell *sh = shell_new(in_fd, out_fd, tab_idx);
    char *acc = NULL;
    for (;;) {
        reap_jobs(sh);
        outf(sh, "%s", acc ? CONT_PROMPT : PROMPT);
        char *line = read_line(sh);
        if (!line) break;
        if (!strcmp(line, "\x03")) { free(line); free(acc); acc = NULL; continue; }   /* Ctrl+C at the prompt */
        acc = join_lines(acc, line);
        free(line);
        if (line_incomplete(acc)) continue;
        hist_add(acc);
        run_line(sh, acc);
        free(acc); acc = NULL;
        if (sh->exit_flag) break;
    }
    free(acc);
    for (int i = 0; i < MAX_JOBS; i++) {                 /* hang up leftover jobs */
        Job *j = &sh->jobs[i];
        if (j->used && j->pgid) { kill(-j->pgid, SIGHUP); kill(-j->pgid, SIGCONT); }
    }
    if (sh->ctl) sh->ctl->fg_pgid = 0;
}
