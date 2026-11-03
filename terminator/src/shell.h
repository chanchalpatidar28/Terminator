#ifndef SHELL_H
#define SHELL_H
#include "common.h"
#include <ctype.h>

typedef struct { char *name, *val; int exported; } Var;
enum { J_RUNNING = 0, J_STOPPED };
typedef struct {
    int used; pid_t pgid; pid_t pids[MAX_PROCS]; int alive[MAX_PROCS];
    int npids; int state; int status; char cmd[256];
} Job;

typedef struct Shell {
    int in_fd, out_fd, err_fd;
    TabCtl *ctl;
    int job_control;
    char cwd[PATH_MAX], oldpwd[PATH_MAX];
    int last_status;
    Var vars[MAX_VARS]; int nvars;
    char *pos[10]; int argc_pos;           /* $0..$9, $# */
    Job jobs[MAX_JOBS];
    int exit_flag, exit_code;
    char rbuf[4096]; int rpos, rlen;
} Shell;

typedef struct { int code; int exited; } ScriptResult;

/* ---- quote-aware scanner shared by the engine and the script layer ---- */
typedef struct { char st[64]; int n; int trail; } Scan;
enum { C_CMD, C_DQ, C_SQ, C_PAREN, C_BQ };
static inline void scan_init(Scan *sc) { sc->n = 1; sc->st[0] = C_CMD; sc->trail = 0; }
static inline void scan_push(Scan *sc, int k) { if (sc->n < 63) sc->st[sc->n++] = (char)k; }
/* Look at s[*i], advance *i. Returns 1 when that char is a plain, unquoted,
 * top-level character (eligible to be an operator such as ; | &).          */
static inline int scan_next(Scan *sc, const char *s, size_t *i)
{
    char c = s[*i];
    int top = sc->st[sc->n - 1];
    if (top == C_SQ) { if (c == '\'') sc->n--; (*i)++; return 0; }
    if (c == '\\') {
        if (!s[*i + 1]) sc->trail = 1;
        *i += s[*i + 1] ? 2 : 1; return 0;
    }
    if (top == C_BQ) { if (c == '`') sc->n--; (*i)++; return 0; }
    if (top == C_DQ) {
        if (c == '`') { scan_push(sc, C_BQ); (*i)++; }
        else if (c == '"') { sc->n--; (*i)++; }
        else if (c == '$' && s[*i + 1] == '(') { scan_push(sc, C_PAREN); *i += 2; }
        else (*i)++;
        return 0;
    }
    if (c == '`')  { scan_push(sc, C_BQ); (*i)++; return 0; }
    if (c == '\'') { scan_push(sc, C_SQ); (*i)++; return 0; }
    if (c == '"')  { scan_push(sc, C_DQ); (*i)++; return 0; }
    if (c == '$' && s[*i + 1] == '(') { scan_push(sc, C_PAREN); *i += 2; return 0; }
    if (c == ')' && top == C_PAREN) { sc->n--; (*i)++; return 0; }
    (*i)++;
    return sc->n == 1;
}

Shell *shell_new(int in_fd, int out_fd, int tab_idx);
int    run_line(Shell *sh, const char *line);
int    line_incomplete(const char *s);
char  *join_lines(char *acc, const char *next);
char **expand_words(Shell *sh, const char *text, int *n);
void   free_words(char **w, int n);
void   set_var(Shell *sh, const char *name, const char *val, int exported);
void   abs_path(Shell *sh, const char *p, char *out);
void   outf(Shell *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void   errf(Shell *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
ScriptResult script_run(Shell *sh, const char *path, int nargs, char **args);
int    test_eval(Shell *sh, char **w, int n);
#endif
