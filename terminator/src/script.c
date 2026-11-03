/* script.c - our own bash-subset interpreter: variables, $1..$9, $#, $?,
 * nested if/elif/else/fi, [ ] / test, pipes, redirection, $(...), exit n.
 * Everything that is not control flow is handed to the same engine that
 * runs interactive lines.                                                    */
#include "shell.h"
#include <sys/stat.h>

/* ------------------------------------------------------------ [ ] / test */
static int num(const char *s, long *v)
{
    char *e; errno = 0;
    *v = strtol(s, &e, 10);
    return *s && !*e && !errno;
}

/* returns 0 = true, 1 = false, 2 = error (like test(1)) */
int test_eval(Shell *sh, char **w, int n)
{
    if (n == 0) return 1;
    for (int k = 1; k < n - 1; k++) {
        if (!strcmp(w[k], "-o")) return (test_eval(sh, w, k) == 0 || test_eval(sh, w + k + 1, n - k - 1) == 0) ? 0 : 1;
    }
    for (int k = 1; k < n - 1; k++) {
        if (!strcmp(w[k], "-a")) return (test_eval(sh, w, k) == 0 && test_eval(sh, w + k + 1, n - k - 1) == 0) ? 0 : 1;
    }
    if (!strcmp(w[0], "!")) return test_eval(sh, w + 1, n - 1) == 0 ? 1 : 0;
    if (n == 1) return w[0][0] ? 0 : 1;
    if (n == 2) {
        const char *op = w[0];
        if (!strcmp(op, "-z")) return w[1][0] ? 1 : 0;
        if (!strcmp(op, "-n")) return w[1][0] ? 0 : 1;
        char p[PATH_MAX]; struct stat st;
        abs_path(sh, w[1], p);
        if (!strcmp(op, "-e")) return stat(p, &st) == 0 ? 0 : 1;
        if (!strcmp(op, "-f")) return (stat(p, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        if (!strcmp(op, "-d")) return (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        if (!strcmp(op, "-s")) return (stat(p, &st) == 0 && st.st_size > 0) ? 0 : 1;
        if (!strcmp(op, "-r")) return access(p, R_OK) == 0 ? 0 : 1;
        if (!strcmp(op, "-w")) return access(p, W_OK) == 0 ? 0 : 1;
        if (!strcmp(op, "-x")) return access(p, X_OK) == 0 ? 0 : 1;
        return 2;
    }
    if (n == 3) {
        const char *a = w[0], *op = w[1], *b = w[2];
        if (!strcmp(op, "=") || !strcmp(op, "==")) return strcmp(a, b) == 0 ? 0 : 1;
        if (!strcmp(op, "!=")) return strcmp(a, b) != 0 ? 0 : 1;
        long x, y;
        if (!num(a, &x) || !num(b, &y)) return 2;
        if (!strcmp(op, "-eq")) return x == y ? 0 : 1;
        if (!strcmp(op, "-ne")) return x != y ? 0 : 1;
        if (!strcmp(op, "-lt")) return x < y ? 0 : 1;
        if (!strcmp(op, "-le")) return x <= y ? 0 : 1;
        if (!strcmp(op, "-gt")) return x > y ? 0 : 1;
        if (!strcmp(op, "-ge")) return x >= y ? 0 : 1;
    }
    return 2;
}

/* condition of an if/elif: a [ ] test, `test ...`, or any command (exit status) */
static int eval_cond(Shell *sh, const char *text)
{
    const char *p = text;
    while (isspace((unsigned char)*p)) p++;
    int is_br = p[0] == '[' && isspace((unsigned char)p[1]);
    int is_test = !strncmp(p, "test", 4) && isspace((unsigned char)p[4]);
    if (is_br || is_test) {
        int n; char **w = expand_words(sh, p, &n);
        int r;
        if (is_br) r = (n >= 2 && !strcmp(w[n - 1], "]")) ? test_eval(sh, w + 1, n - 2) : 2;
        else r = test_eval(sh, w + 1, n - 1);
        free_words(w, n);
        sh->last_status = r;
        return r;
    }
    return run_line(sh, p);
}

/* -------------------------------------------------- statement splitting */
/* split a logical line at top-level ';' (and newlines), dropping comments */
static int split_stmts(const char *line, char ***out)
{
    char **v = NULL; int n = 0, cap = 0;
    Scan sc; scan_init(&sc);
    size_t i = 0, start = 0, len = strlen(line);
    for (;;) {
        size_t j = i; int end = 0;
        if (i >= len) end = 2;
        else {
            int plain = scan_next(&sc, line, &i);
            if (plain && (line[j] == ';' || line[j] == '\n')) end = 1;
            else if (plain && line[j] == '#' && (j == 0 || isspace((unsigned char)line[j - 1]))) { end = 2; }
        }
        if (!end) continue;
        char *t = strndup(line + start, (end == 2 ? (line[j] == '#' && j < len ? j : len) : j) - start);
        if (n >= cap) { cap = cap ? cap * 2 : 8; v = realloc(v, cap * sizeof(char *)); }
        v[n++] = t;
        if (end == 2) break;
        start = i;
    }
    *out = v;
    return n;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

static int is_kw(const char *s, const char *kw)
{
    size_t n = strlen(kw);
    return !strncmp(s, kw, n) && (s[n] == 0 || isspace((unsigned char)s[n]) || s[n] == ';');
}

typedef struct { int parent_active, taken, active; } Frame;

typedef struct { Frame st[64]; int depth; ScriptResult res; } Interp;

static int cur_active(Interp *it) { return it->depth == 0 ? 1 : it->st[it->depth - 1].active; }

static void do_statement(Shell *sh, Interp *it, char *stmt)
{
    for (;;) {
        char *s = trim(stmt);
        if (!*s || it->res.exited) return;
        if (is_kw(s, "if")) {
            if (it->depth >= 63) { errf(sh, "terminator: if nested too deep\n"); return; }
            Frame *f = &it->st[it->depth++];
            f->parent_active = it->depth == 1 ? 1 : it->st[it->depth - 2].active;
            if (f->parent_active) { f->active = eval_cond(sh, s + 2) == 0; f->taken = f->active; }
            else { f->active = 0; f->taken = 1; }
            return;
        }
        if (is_kw(s, "elif")) {
            if (!it->depth) { errf(sh, "terminator: unexpected elif\n"); return; }
            Frame *f = &it->st[it->depth - 1];
            if (!f->parent_active || f->taken) f->active = 0;
            else { f->active = eval_cond(sh, s + 4) == 0; f->taken |= f->active; }
            return;
        }
        if (is_kw(s, "else")) {
            if (!it->depth) { errf(sh, "terminator: unexpected else\n"); return; }
            Frame *f = &it->st[it->depth - 1];
            f->active = f->parent_active && !f->taken;
            f->taken = 1;
            stmt = s + 4; continue;
        }
        if (is_kw(s, "then")) { stmt = s + 4; continue; }
        if (is_kw(s, "fi")) {
            if (!it->depth) errf(sh, "terminator: unexpected fi\n"); else it->depth--;
            stmt = s + 2; continue;
        }
        if (!cur_active(it)) return;
        if (is_kw(s, "exit")) {
            int n; char **w = expand_words(sh, s, &n);
            it->res.code = n > 1 ? atoi(w[1]) : sh->last_status;
            it->res.exited = 1;
            free_words(w, n);
            return;
        }
        run_line(sh, s);
        return;
    }
}

/* ---------------------------------------------------- syntax pre-check */
/* Look at the WHOLE script before running anything, so a missing `fi` is
 * reported instead of half-executing the file.  Returns 0 if OK.            */
static int script_check(char **L, int n, char *msg, size_t msz, int *errline)
{
    struct { int need_then, seen_else; } st[64];
    int depth = 0, startline = 1;
    char *acc = NULL;
#define FAIL(m, ln) do { snprintf(msg, msz, "%s", m); *errline = (ln); free(acc); return 1; } while (0)
    for (int ln = 0; ln < n; ln++) {
        if (!acc) startline = ln + 1;
        acc = join_lines(acc, L[ln]);
        if (line_incomplete(acc)) continue;
        char **stmts; int ns = split_stmts(acc, &stmts);
        for (int i = 0; i < ns; i++) {
            char *s0 = stmts[i];
            for (;;) {
                char *s = trim(s0);
                if (!*s) break;
                if (is_kw(s, "if")) {
                    if (depth >= 63) { for (int k = i; k < ns; k++) free(stmts[k]); free(stmts); FAIL("if nested too deeply", startline); }
                    if (!*trim(s + 2)) { for (int k = i; k < ns; k++) free(stmts[k]); free(stmts); FAIL("missing condition after 'if'", startline); }
                    st[depth].need_then = 1; st[depth].seen_else = 0; depth++;
                    break;
                }
                const char *err = NULL;
                if (is_kw(s, "elif")) {
                    if (!depth) err = "'elif' without a matching 'if'";
                    else if (st[depth - 1].seen_else) err = "'elif' after 'else'";
                    else if (st[depth - 1].need_then) err = "expected 'then' before 'elif'";
                    else if (!*trim(s + 4)) err = "missing condition after 'elif'";
                    else st[depth - 1].need_then = 1;
                    if (!err) break;
                } else if (is_kw(s, "else")) {
                    if (!depth) err = "'else' without a matching 'if'";
                    else if (st[depth - 1].need_then) err = "expected 'then' before 'else'";
                    else if (st[depth - 1].seen_else) err = "second 'else' in the same 'if'";
                    else { st[depth - 1].seen_else = 1; s0 = s + 4; continue; }
                } else if (is_kw(s, "then")) {
                    if (!depth || !st[depth - 1].need_then) err = "unexpected 'then'";
                    else { st[depth - 1].need_then = 0; s0 = s + 4; continue; }
                } else if (is_kw(s, "fi")) {
                    if (!depth) err = "'fi' without a matching 'if'";
                    else if (st[depth - 1].need_then) err = "expected 'then' before 'fi'";
                    else { depth--; s0 = s + 2; continue; }
                } else {
                    if (depth && st[depth - 1].need_then) err = "expected 'then' after the condition";
                    else break;
                }
                for (int k = i; k < ns; k++) free(stmts[k]);
                free(stmts);
                FAIL(err, startline);
            }
        }
        for (int i = 0; i < ns; i++) free(stmts[i]);
        free(stmts);
        free(acc); acc = NULL;
    }
    if (acc) FAIL("unexpected end of file (unterminated quote or $( ... ))", startline);
    if (depth) FAIL("unexpected end of file: missing 'fi'", n);
#undef FAIL
    return 0;
}

ScriptResult script_run(Shell *sh, const char *path, int nargs, char **args)
{
    ScriptResult none = { 127, 0 };
    char full[PATH_MAX];
    abs_path(sh, path, full);
    FILE *f = fopen(full, "r");
    if (!f) { errf(sh, "terminator: %s: %s\n", path, strerror(errno)); return none; }

    /* read every line first */
    char **L = NULL; int nl = 0, lcap = 0;
    char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        line[strcspn(line, "\r\n")] = 0;
        if (nl >= lcap) { lcap = lcap ? lcap * 2 : 64; L = realloc(L, (size_t)lcap * sizeof(char *)); }
        L[nl++] = strdup(line);
    }
    free(line); fclose(f);

    ScriptResult res = { 2, 0 };
    char msg[160]; int eline = 0;
    if (script_check(L, nl, msg, sizeof msg, &eline)) {          /* syntax error: run nothing */
        errf(sh, "%s: line %d: syntax error: %s\n", path, eline, msg);
        sh->last_status = 2;
        for (int i = 0; i < nl; i++) free(L[i]);
        free(L);
        return res;
    }

    char *save_pos[10]; int save_n = sh->argc_pos;
    memcpy(save_pos, sh->pos, sizeof save_pos);
    if (nargs >= 0) {
        sh->pos[0] = strdup(path);
        for (int i = 0; i < 9; i++) sh->pos[i + 1] = i < nargs ? strdup(args[i]) : NULL;
        sh->argc_pos = nargs;
    }

    Interp it; memset(&it, 0, sizeof it);
    char *acc = NULL;
    for (int ln = 0; ln < nl && !it.res.exited && !sh->exit_flag; ln++) {
        acc = join_lines(acc, L[ln]);
        if (line_incomplete(acc)) continue;
        char **stmts; int n = split_stmts(acc, &stmts);
        for (int i = 0; i < n; i++) { if (!it.res.exited) do_statement(sh, &it, stmts[i]); free(stmts[i]); }
        free(stmts);
        free(acc); acc = NULL;
    }
    free(acc);
    for (int i = 0; i < nl; i++) free(L[i]);
    free(L);
    if (!it.res.exited) it.res.code = sh->last_status;

    if (nargs >= 0) {
        for (int i = 0; i < 10; i++) free(sh->pos[i]);
        memcpy(sh->pos, save_pos, sizeof save_pos);
        sh->argc_pos = save_n;
    }
    return it.res;
}
