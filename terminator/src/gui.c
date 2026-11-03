/* gui.c - the X11 window, the tab bar, the line editor and the event loop.
 *
 * One thread/process talks to X (this one).  Each tab owns two pipes:
 *     GUI --in_fd--> tab shell      (lines the user typed)
 *     GUI <--out_fd-- tab shell     (everything the shell and its jobs print)
 * Thread mode runs the tab shell in a pthread; process mode forks it.  Either
 * way the GUI only ever sees two fds, so both modes behave identically.       */
#include "shell.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAXLINES 20000
#define INPUT_MAX 4000

typedef struct { char *s; int len, cap; } Ln;
typedef struct { char *full; char *disp; } Cand;

typedef struct Tab {
    int slot, in_fd, out_fd; pid_t pid;
    Ln *lines; int nlines, lcap, esc;          /* output buffer; last line = partial/prompt */
    int scroll;
    char input[INPUT_MAX + 8]; int ilen, icur;
    int hpos; char hsave[INPUT_MAX + 8];       /* history browsing */
    int searching; char sbuf[256]; int slen;   /* Ctrl+R */
    int comp; Cand *cand; int ncand, cstart; char numbuf[8];   /* Tab completion list */
} Tab;

static Display *dpy; static Window win; static GC gc; static Pixmap back; static XFontStruct *font;
static Atom wm_delete, a_clip, a_primary, a_utf8, a_targets, a_prop;
static char *sel_text;                      /* what we hand out when somebody asks for our selection */
static int scr, W = 960, H = 600, charw, lineh, ascent, barh = 28;
static unsigned long c_bg, c_fg, c_bar, c_act, c_cur, c_dim;
static Tab *tabs[MAX_TABS]; static int ntabs, cur, use_process, dirty = 1, quit_flag;

/* ---------------------------------------------------------------- buffer */
static void ln_add(Ln *l, char c)
{
    if (l->len + 2 > l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->s = realloc(l->s, (size_t)l->cap); }
    l->s[l->len++] = c; l->s[l->len] = 0;
}

static void tab_newline(Tab *t)
{
    if (t->nlines >= MAXLINES) {
        free(t->lines[0].s);
        memmove(t->lines, t->lines + 1, (size_t)(t->nlines - 1) * sizeof(Ln));
        t->nlines--;
    }
    if (t->nlines >= t->lcap) { t->lcap = t->lcap ? t->lcap * 2 : 256; t->lines = realloc(t->lines, (size_t)t->lcap * sizeof(Ln)); }
    memset(&t->lines[t->nlines++], 0, sizeof(Ln));
}

static void tab_putc(Tab *t, unsigned char c)
{
    Ln *l = &t->lines[t->nlines - 1];
    if (t->esc == 1) { t->esc = (c == '[') ? 2 : 0; return; }
    if (t->esc == 2) { if (c >= 0x40 && c <= 0x7e) t->esc = 0; return; }
    if (c == 27) { t->esc = 1; return; }
    if (c == '\n') { tab_newline(t); return; }
    if (c == '\r') return;
    if (c == '\b') { if (l->len > 0) l->s[--l->len] = 0; return; }
    if (c == '\t') { do ln_add(l, ' '); while (l->len % 8); return; }
    if (c < 32) return;
    if (c >= 127) c = '?';                       /* core X fonts: ASCII only */
    ln_add(l, (char)c);
}

static void tab_append(Tab *t, const char *d, size_t n) { for (size_t i = 0; i < n; i++) tab_putc(t, (unsigned char)d[i]); dirty = 1; }
static void tab_puts(Tab *t, const char *s) { tab_append(t, s, strlen(s)); }
static void tab_set_partial(Tab *t, const char *s)
{
    Ln *l = &t->lines[t->nlines - 1];
    l->len = 0; if (l->s) l->s[0] = 0;
    for (; *s; s++) ln_add(l, *s);
}
static char *tab_partial_copy(Tab *t) { Ln *l = &t->lines[t->nlines - 1]; return strndup(l->s ? l->s : "", (size_t)l->len); }

static void tab_title(Tab *t, int idx, char *out, size_t sz)
{
    const char *cwd = G->tabs[t->slot].cwd, *home = getenv("HOME");
    char p[PATH_MAX + 2];
    if (home && *home && !strncmp(cwd, home, strlen(home))) snprintf(p, sizeof p, "~%s", cwd + strlen(home));
    else snprintf(p, sizeof p, "%s", cwd);
    size_t n = strlen(p);
    if (n > 20) snprintf(out, sz, "%d: ..%s", idx + 1, p + n - 18);
    else snprintf(out, sz, "%d: %s", idx + 1, p);
}

/* ---------------------------------------------------------------- tabs */
typedef struct { int in_fd, out_fd, slot; } TabArgs;
static void *tab_thread(void *arg)
{
    TabArgs a = *(TabArgs *)arg; free(arg);
    shell_run(a.in_fd, a.out_fd, a.slot);
    close(a.in_fd); close(a.out_fd);
    return NULL;
}

static Tab *spawn_tab(void)
{
    if (ntabs >= MAX_TABS) return NULL;
    int slot = -1;
    for (int i = 0; i < MAX_TABS; i++) if (!G->tabs[i].used) { slot = i; break; }
    if (slot < 0) return NULL;
    int inp[2], outp[2];
    if (pipe2(inp, O_CLOEXEC) < 0) return NULL;
    if (pipe2(outp, O_CLOEXEC) < 0) { close(inp[0]); close(inp[1]); return NULL; }
    G->tabs[slot].used = 1; G->tabs[slot].fg_pgid = 0;
    Tab *t = calloc(1, sizeof *t);
    t->slot = slot; t->in_fd = inp[1]; t->out_fd = outp[0];
    tab_newline(t);

    if (use_process) {
        pid_t pid = fork();
        if (pid == 0) {
            for (int j = 0; j < ntabs; j++) { close(tabs[j]->in_fd); close(tabs[j]->out_fd); }
            close(inp[1]); close(outp[0]); close(ConnectionNumber(dpy));
            shell_run(inp[0], outp[1], slot);
            _exit(0);
        }
        t->pid = pid;
        close(inp[0]); close(outp[1]);
    } else {
        TabArgs *a = malloc(sizeof *a);
        *a = (TabArgs){ inp[0], outp[1], slot };
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_t th;
        if (pthread_create(&th, &at, tab_thread, a) != 0) { free(a); free(t); return NULL; }
    }
    tabs[ntabs++] = t; cur = ntabs - 1; dirty = 1;
    return t;
}

static void close_tab(int i)
{
    Tab *t = tabs[i];
    close(t->in_fd); close(t->out_fd);
    if (t->pid > 0) { int st; waitpid(t->pid, &st, 0); }
    for (int k = 0; k < t->nlines; k++) free(t->lines[k].s);
    free(t->lines);
    for (int k = 0; k < t->ncand; k++) { free(t->cand[k].full); free(t->cand[k].disp); }
    free(t->cand);
    G->tabs[t->slot].used = 0; G->tabs[t->slot].fg_pgid = 0;
    free(t);
    for (int k = i; k < ntabs - 1; k++) tabs[k] = tabs[k + 1];
    ntabs--;
    if (cur >= ntabs) cur = ntabs - 1;
    if (cur < 0) cur = 0;
    if (ntabs == 0) quit_flag = 1;
    dirty = 1;
}

static void shutdown_tabs(void)
{
    for (int i = 0; i < ntabs; i++) { close(tabs[i]->in_fd); tabs[i]->in_fd = -1; }   /* EOF => shells exit, HUP their jobs */
    for (int round = 0; round < 25; round++) {                                         /* wait <= 500ms */
        int alive = 0;
        for (int i = 0; i < ntabs; i++) {
            if (tabs[i]->out_fd < 0) continue;
            struct pollfd p = { tabs[i]->out_fd, POLLIN, 0 };
            if (poll(&p, 1, 0) > 0) {
                char b[1024]; ssize_t r = read(tabs[i]->out_fd, b, sizeof b);
                if (r <= 0) { close(tabs[i]->out_fd); tabs[i]->out_fd = -1; continue; }
            }
            alive++;
        }
        if (!alive) break;
        usleep(20000);
    }
    for (int i = 0; i < ntabs; i++) if (tabs[i]->pid > 0) { kill(tabs[i]->pid, SIGKILL); waitpid(tabs[i]->pid, NULL, 0); }
}

/* ------------------------------------------------------------- drawing */
typedef struct { const char *s; int len; int bottom; int off; } Row;

static unsigned long color(const char *name)
{
    XColor c, e;
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, scr), name, &c, &e)) return c.pixel;
    return BlackPixel(dpy, scr);
}

static char *bottom_text(Tab *t, int *cursor)
{
    Ln *l = &t->lines[t->nlines - 1];
    size_t cap = (size_t)l->len + (size_t)t->ilen + 300;
    char *b = malloc(cap);
    if (t->searching) { snprintf(b, cap, "Enter search term: %s", t->sbuf); *cursor = (int)strlen(b); }
    else {
        memcpy(b, l->s ? l->s : "", (size_t)l->len);
        memcpy(b + l->len, t->input, (size_t)t->ilen);
        b[l->len + t->ilen] = 0;
        *cursor = l->len + t->icur;
    }
    return b;
}

static void redraw(void)
{
    XSetForeground(dpy, gc, c_bg); XFillRectangle(dpy, back, gc, 0, 0, (unsigned)W, (unsigned)H);
    /* tab bar */
    XSetForeground(dpy, gc, c_bar); XFillRectangle(dpy, back, gc, 0, 0, (unsigned)W, (unsigned)barh);
    int tw = ntabs > 0 ? (W - 34) / ntabs : 160;
    if (tw > 190) tw = 190;
    for (int i = 0; i < ntabs; i++) {
        char title[96]; tab_title(tabs[i], i, title, sizeof title);
        XSetForeground(dpy, gc, i == cur ? c_act : c_bar);
        XFillRectangle(dpy, back, gc, i * tw, 0, (unsigned)tw - 2, (unsigned)barh);
        XSetForeground(dpy, gc, i == cur ? c_fg : c_dim);
        int maxc = (tw - 12) / charw; if (maxc < 1) maxc = 1;
        int tl = (int)strlen(title); if (tl > maxc) tl = maxc;
        XDrawString(dpy, back, gc, i * tw + 6, (barh + ascent) / 2 + 1, title, tl);
    }
    XSetForeground(dpy, gc, c_dim);
    XDrawString(dpy, back, gc, ntabs * tw + 8, (barh + ascent) / 2 + 1, "+", 1);

    if (ntabs > 0) {
        Tab *t = tabs[cur];
        int cols = (W - 8) / charw; if (cols < 1) cols = 1;
        int vrows = (H - barh) / lineh; if (vrows < 1) vrows = 1;
        int curs; char *bt = bottom_text(t, &curs); int blen = (int)strlen(bt);
        int need = vrows + t->scroll + 1, cnt = 0;
        Row *rev = malloc((size_t)need * sizeof(Row));
        for (int li = t->nlines - 1; li >= 0 && cnt < need; li--) {
            int bottom = li == t->nlines - 1;
            const char *s = bottom ? bt : (t->lines[li].s ? t->lines[li].s : "");
            int len = bottom ? blen : t->lines[li].len;
            int nr = bottom ? len / cols + 1 : (len ? (len + cols - 1) / cols : 1);
            for (int r = nr - 1; r >= 0 && cnt < need; r--) {
                int rl = len - r * cols; if (rl > cols) rl = cols; if (rl < 0) rl = 0;
                rev[cnt++] = (Row){ s + r * cols, rl, bottom, r };
            }
        }
        if (cnt < need && t->scroll > cnt - vrows) t->scroll = cnt - vrows > 0 ? cnt - vrows : 0;
        int hi = t->scroll + vrows - 1; if (hi > cnt - 1) hi = cnt - 1;
        for (int k = hi; k >= t->scroll; k--) {
            int idx = hi - k, y = barh + idx * lineh;
            XSetForeground(dpy, gc, c_fg); XSetBackground(dpy, gc, c_bg);
            if (rev[k].len) XDrawImageString(dpy, back, gc, 4, y + ascent + 1, rev[k].s, rev[k].len);
            if (rev[k].bottom && rev[k].off == curs / cols) {
                int col = curs % cols;
                XSetForeground(dpy, gc, c_cur);
                XFillRectangle(dpy, back, gc, 4 + col * charw, y, (unsigned)charw, (unsigned)lineh);
                if (col < rev[k].len) {
                    XSetForeground(dpy, gc, c_bg); XSetBackground(dpy, gc, c_cur);
                    XDrawImageString(dpy, back, gc, 4 + col * charw, y + ascent + 1, rev[k].s + col, 1);
                }
            }
        }
        free(rev); free(bt);
    }
    XCopyArea(dpy, back, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
    XFlush(dpy);
}

static void make_backbuffer(void)
{
    if (back) XFreePixmap(dpy, back);
    back = XCreatePixmap(dpy, win, (unsigned)W, (unsigned)H, (unsigned)DefaultDepth(dpy, scr));
}

/* ---------------------------------------------------------- line editor */
static void input_set(Tab *t, const char *s)
{
    snprintf(t->input, INPUT_MAX, "%s", s);
    t->ilen = (int)strlen(t->input); t->icur = t->ilen;
}
static void input_insert(Tab *t, const char *s, int n)
{
    if (t->ilen + n > INPUT_MAX) return;
    memmove(t->input + t->icur + n, t->input + t->icur, (size_t)(t->ilen - t->icur + 1));
    memcpy(t->input + t->icur, s, (size_t)n);
    t->ilen += n; t->icur += n;
}
static void input_delete(Tab *t, int from, int to)       /* [from,to) */
{
    if (from < 0) from = 0;
    if (to > t->ilen) to = t->ilen;
    if (to <= from) return;
    memmove(t->input + from, t->input + to, (size_t)(t->ilen - to + 1));
    t->ilen -= to - from;
    if (t->icur >= to) t->icur -= to - from;
    else if (t->icur > from) t->icur = from;
}

static void send_line(Tab *t, const char *s)
{
    char *b = malloc(strlen(s) + 2);
    sprintf(b, "%s\n", s);
    size_t n = strlen(b), off = 0;
    while (off < n) { ssize_t w = write(t->in_fd, b + off, n - off); if (w <= 0) break; off += (size_t)w; }
    free(b);
}

static void submit(Tab *t)
{
    tab_append(t, t->input, (size_t)t->ilen); tab_append(t, "\n", 1);
    send_line(t, t->input);
    t->ilen = t->icur = 0; t->input[0] = 0; t->hpos = 0; t->scroll = 0;
}

/* ---- completion ---- */
static void unescape(const char *s, int n, char *out)
{
    int k = 0;
    for (int i = 0; i < n; i++) { if (s[i] == '\\' && i + 1 < n) i++; out[k++] = s[i]; }
    out[k] = 0;
}
static void escape_into(const char *s, char *out, size_t cap)
{
    size_t k = 0;
    for (; *s && k + 3 < cap; s++) {
        if (strchr(" \t\\'\"$&;|<>()*?[]#`", *s)) out[k++] = '\\';
        out[k++] = *s;
    }
    out[k] = 0;
}
static void replace_word(Tab *t, int s, int e, const char *full)
{
    char esc[PATH_MAX * 2]; escape_into(full, esc, sizeof esc);
    input_delete(t, s, e);
    t->icur = s;
    input_insert(t, esc, (int)strlen(esc));
}
static void comp_end(Tab *t)
{
    for (int k = 0; k < t->ncand; k++) { free(t->cand[k].full); free(t->cand[k].disp); }
    free(t->cand); t->cand = NULL; t->ncand = 0; t->comp = 0; t->numbuf[0] = 0;
}
static int cand_cmp(const void *a, const void *b) { return strcmp(((const Cand *)a)->disp, ((const Cand *)b)->disp); }

static void do_complete(Tab *t)
{
    int e = t->icur, s = e;
    while (s > 0 && !(isspace((unsigned char)t->input[s - 1]) && !(s > 1 && t->input[s - 2] == '\\'))) s--;
    char raw[PATH_MAX], dirpart[PATH_MAX] = "", base[PATH_MAX], dirpath[PATH_MAX * 2];
    unescape(t->input + s, e - s, raw);
    char *slash = strrchr(raw, '/');
    if (slash) { size_t dl = (size_t)(slash - raw) + 1; memcpy(dirpart, raw, dl); dirpart[dl] = 0; snprintf(base, sizeof base, "%s", slash + 1); }
    else snprintf(base, sizeof base, "%s", raw);
    const char *cwd = G->tabs[t->slot].cwd[0] ? G->tabs[t->slot].cwd : ".";
    const char *home = getenv("HOME");
    if (!dirpart[0]) snprintf(dirpath, sizeof dirpath, "%s", cwd);
    else if (dirpart[0] == '/') snprintf(dirpath, sizeof dirpath, "%s", dirpart);
    else if (dirpart[0] == '~' && dirpart[1] == '/') snprintf(dirpath, sizeof dirpath, "%s/%s", home ? home : "", dirpart + 2);
    else snprintf(dirpath, sizeof dirpath, "%s/%s", cwd, dirpart);

    DIR *d = opendir(dirpath);
    if (!d) return;
    Cand *c = NULL; int n = 0, cap = 0; size_t bl = strlen(base);
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (nm[0] == '.' && base[0] != '.') continue;
        if (strncmp(nm, base, bl)) continue;
        char full[PATH_MAX * 2]; struct stat st;
        snprintf(full, sizeof full, "%s/%s", dirpath, nm);
        int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (n >= cap) { cap = cap ? cap * 2 : 16; c = realloc(c, (size_t)cap * sizeof(Cand)); }
        char f2[PATH_MAX * 2]; snprintf(f2, sizeof f2, "%s%s%s", dirpart, nm, isdir ? "/" : "");
        c[n].full = strdup(f2); c[n].disp = strdup(nm); n++;
    }
    closedir(d);
    if (n == 0) { free(c); return; }
    qsort(c, (size_t)n, sizeof(Cand), cand_cmp);
    if (n == 1) { replace_word(t, s, e, c[0].full); free(c[0].full); free(c[0].disp); free(c); return; }

    /* several matches: commit the line, print the numbered list, extend to the common prefix */
    size_t lcp = strlen(c[0].disp);
    for (int i = 1; i < n; i++) { size_t k = 0; while (k < lcp && c[i].disp[k] == c[0].disp[k]) k++; lcp = k; }
    char *prompt = tab_partial_copy(t);
    tab_append(t, t->input, (size_t)t->ilen); tab_append(t, "\n", 1);
    for (int i = 0; i < n; i++) { char it[PATH_MAX + 16]; snprintf(it, sizeof it, "%s%d. %s", i ? "   " : "", i + 1, c[i].disp); tab_puts(t, it); }
    tab_puts(t, "\n");
    tab_puts(t, prompt); free(prompt);
    if (lcp > bl) { char w[PATH_MAX * 2]; snprintf(w, sizeof w, "%s%.*s", dirpart, (int)lcp, c[0].disp); replace_word(t, s, e, w); }
    comp_end(t);
    t->cand = c; t->ncand = n; t->comp = 1; t->cstart = s; t->numbuf[0] = 0;
}

static void comp_select(Tab *t, int v)
{
    if (v >= 1 && v <= t->ncand) replace_word(t, t->cstart, t->icur, t->cand[v - 1].full);
    comp_end(t);
}

/* ---- Ctrl+R ---- */
static void search_finish(Tab *t)
{
    char out[20][HIST_LINE]; int exact = 0;
    char *prompt = tab_partial_copy(t);
    t->searching = 0;
    char line[300]; snprintf(line, sizeof line, "Enter search term: %s", t->sbuf);
    tab_set_partial(t, line); tab_puts(t, "\n");
    int n = t->slen ? hist_search(t->sbuf, out, 20, &exact) : 0;
    if (n == 0) tab_puts(t, "No match for search term in history\n");
    for (int i = 0; i < n; i++) { tab_puts(t, out[i]); tab_puts(t, "\n"); }
    tab_set_partial(t, prompt); free(prompt);
    if (n == 1) input_set(t, out[0]);
    dirty = 1;
}

/* ---- actions ---- */
static void new_tab_action(void) { spawn_tab(); }

static void do_action(Tab *t, int act)
{
    switch (act) {
    case A_LEFT: if (t->icur > 0) t->icur--; break;
    case A_RIGHT: if (t->icur < t->ilen) t->icur++; break;
    case A_HOME: t->icur = 0; break;
    case A_END: t->icur = t->ilen; break;
    case A_HIST_PREV: {
        char b[HIST_LINE];
        if (hist_from_end(t->hpos, b, sizeof b)) {
            if (t->hpos == 0) snprintf(t->hsave, sizeof t->hsave, "%s", t->input);
            t->hpos++; input_set(t, b);
        }
        break; }
    case A_HIST_NEXT: {
        char b[HIST_LINE];
        if (t->hpos > 0) {
            t->hpos--;
            if (t->hpos == 0) input_set(t, t->hsave);
            else if (hist_from_end(t->hpos - 1, b, sizeof b)) input_set(t, b);
        }
        break; }
    case A_KILL_START: input_delete(t, 0, t->icur); t->icur = 0; break;
    case A_KILL_END: input_delete(t, t->icur, t->ilen); break;
    case A_DEL_WORD: {
        int e = t->icur, s = e;
        while (s > 0 && t->input[s - 1] == ' ') s--;
        while (s > 0 && t->input[s - 1] != ' ') s--;
        input_delete(t, s, e); t->icur = s;
        break; }
    case A_CLEAR: {
        char *p = tab_partial_copy(t);
        for (int k = 0; k < t->nlines; k++) free(t->lines[k].s);
        t->nlines = 0; tab_newline(t); tab_set_partial(t, p); free(p);
        t->scroll = 0;
        break; }
    case A_CLOSE:
        if (t->ilen > 0) { input_delete(t, t->icur, t->icur + 1); break; }
        if (G->tabs[t->slot].fg_pgid > 0) break;          /* a job is running: stdin is not wired to it */
        if (t->in_fd >= 0) { close(t->in_fd); t->in_fd = -1; }   /* EOF => shell exits => tab closes */
        break;
    case A_COPY:                                   /* own CLIPBOARD (+PRIMARY): other apps and tabs can paste it */
        free(sel_text); sel_text = strdup(t->input);
        XSetSelectionOwner(dpy, a_clip, win, CurrentTime);
        XSetSelectionOwner(dpy, a_primary, win, CurrentTime);
        break;
    case A_PASTE:                                  /* ask the owner; the answer arrives as SelectionNotify */
        XConvertSelection(dpy, a_clip, XA_STRING, a_prop, win, CurrentTime);
        break;
    case A_SEARCH: t->searching = 1; t->slen = 0; t->sbuf[0] = 0; break;
    case A_COMPLETE: do_complete(t); break;
    case A_NEWTAB: new_tab_action(); break;
    case A_NEXTTAB: if (ntabs) cur = (cur + 1) % ntabs; break;
    case A_PREVTAB: if (ntabs) cur = (cur + ntabs - 1) % ntabs; break;
    case A_INTERRUPT: {
        pid_t pg = G->tabs[t->slot].fg_pgid;
        if (pg > 0) { kill(-pg, SIGINT); tab_puts(t, "^C"); }
        else {
            tab_append(t, t->input, (size_t)t->ilen); tab_puts(t, "^C\n");
            t->ilen = t->icur = 0; t->input[0] = 0; t->hpos = 0;
            if (write(t->in_fd, "\x03\n", 2) < 0) { /* shell gone */ }
        }
        break; }
    case A_SUSPEND: {
        pid_t pg = G->tabs[t->slot].fg_pgid;
        if (pg > 0) { kill(-pg, SIGTSTP); tab_puts(t, "^Z"); }
        break; }
    }
    dirty = 1;
}

static void on_key(XKeyEvent *ke)
{
    if (!ntabs) return;
    Tab *t = tabs[cur];
    char buf[32]; KeySym ks;
    int n = XLookupString(ke, buf, sizeof buf, &ks, NULL);
    if (ks >= XK_Shift_L && ks <= XK_Hyper_R) return;
    KeySym base = XLookupKeysym(ke, 0), lo, up;
    XConvertCase(base, &lo, &up); base = lo;
    unsigned mods = ((ke->state & ControlMask) ? MOD_CTRL : 0) | ((ke->state & ShiftMask) ? MOD_SHIFT : 0) |
                    ((ke->state & Mod1Mask) ? MOD_ALT : 0);
    int vrows = (H - barh) / lineh;
    dirty = 1;

    if ((base == XK_Prior || base == XK_Next) && (mods & MOD_SHIFT)) {
        t->scroll += base == XK_Prior ? vrows / 2 : -vrows / 2;
        if (t->scroll < 0) t->scroll = 0;
        return;
    }
    t->scroll = 0;

    if (t->comp) {                                         /* choosing from the numbered list */
        if (n == 1 && isdigit((unsigned char)buf[0]) && !(mods & (MOD_CTRL | MOD_ALT))) {
            size_t l = strlen(t->numbuf);
            if (l < sizeof t->numbuf - 1) { t->numbuf[l] = buf[0]; t->numbuf[l + 1] = 0; }
            int v = atoi(t->numbuf);
            if (t->ncand <= 9 || v * 10 > t->ncand) comp_select(t, v);
            return;
        }
        if ((ks == XK_Return || ks == XK_KP_Enter) && t->numbuf[0]) { comp_select(t, atoi(t->numbuf)); return; }
        comp_end(t);
    }
    if (t->searching) {
        if (ks == XK_Return || ks == XK_KP_Enter) { search_finish(t); return; }
        if (ks == XK_Escape || (mods == MOD_CTRL && base == 'c')) { t->searching = 0; return; }
        if (ks == XK_BackSpace) { if (t->slen > 0) t->sbuf[--t->slen] = 0; return; }
        if (n == 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127 && !(mods & (MOD_CTRL | MOD_ALT)) && t->slen < 250) {
            t->sbuf[t->slen++] = buf[0]; t->sbuf[t->slen] = 0;
        }
        return;
    }
    int act = keymap_lookup(mods, (unsigned long)base);
    if (act) { do_action(t, act); return; }
    if (ks == XK_Return || ks == XK_KP_Enter) { submit(t); return; }
    if (ks == XK_BackSpace) { if (t->icur > 0) input_delete(t, t->icur - 1, t->icur); return; }
    if (ks == XK_Delete) { input_delete(t, t->icur, t->icur + 1); return; }
    if (n >= 1 && !(mods & (MOD_CTRL | MOD_ALT)))
        for (int i = 0; i < n; i++)
            if ((unsigned char)buf[i] >= 32 && buf[i] != 127 && (unsigned char)buf[i] < 128) input_insert(t, &buf[i], 1);
}

/* somebody (another app, or our own paste) asks for the selection we own */
static void on_selection_request(XSelectionRequestEvent *rq)
{
    XSelectionEvent r;
    memset(&r, 0, sizeof r);
    r.type = SelectionNotify; r.display = rq->display; r.requestor = rq->requestor;
    r.selection = rq->selection; r.target = rq->target; r.time = rq->time;
    r.property = rq->property ? rq->property : rq->target;
    if (rq->target == a_targets) {
        Atom list[3] = { a_targets, XA_STRING, a_utf8 };
        XChangeProperty(dpy, rq->requestor, r.property, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, 3);
    } else if ((rq->target == XA_STRING || rq->target == a_utf8) && sel_text) {
        XChangeProperty(dpy, rq->requestor, r.property, rq->target, 8, PropModeReplace,
                        (unsigned char *)sel_text, (int)strlen(sel_text));
    } else r.property = None;
    XSendEvent(dpy, rq->requestor, False, 0, (XEvent *)&r);
}

/* the answer to our XConvertSelection(): read the text and insert it into the current tab */
static void on_selection_notify(XSelectionEvent *se)
{
    if (se->property == None || !ntabs) return;
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, se->property, 0, 4096, True, AnyPropertyType, &type, &fmt, &n, &after, &data) != Success || !data) return;
    Tab *t = tabs[cur];
    for (unsigned long i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) input_insert(t, &c, 1);
    }
    XFree(data);
    dirty = 1;
}

static void on_button(XButtonEvent *be)
{
    if (be->button == Button4 || be->button == Button5) {
        if (ntabs) { Tab *t = tabs[cur]; t->scroll += be->button == Button4 ? 3 : -3; if (t->scroll < 0) t->scroll = 0; dirty = 1; }
        return;
    }
    if (be->button != Button1 || be->y >= barh) return;
    int tw = ntabs > 0 ? (W - 34) / ntabs : 160;
    if (tw > 190) tw = 190;
    int i = be->x / tw;
    if (i < ntabs) { cur = i; dirty = 1; }
    else if (be->x < ntabs * tw + 30) new_tab_action();
}

/* ------------------------------------------------------------ main loop */
int gui_main(int up)
{
    use_process = up;
    XInitThreads();
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "terminator: cannot open X display (try --cli for a headless shell)\n"); return 1; }
    scr = DefaultScreen(dpy);
    font = XLoadQueryFont(dpy, "9x15");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) { fprintf(stderr, "terminator: no usable font\n"); return 1; }
    charw = font->max_bounds.width > 0 ? font->max_bounds.width : 8;
    ascent = font->ascent; lineh = font->ascent + font->descent + 2;
    c_bg = color("#1b1d23"); c_fg = color("#d7dae0"); c_bar = color("#2a2d35");
    c_act = color("#3b4252"); c_cur = color("#98c379"); c_dim = color("#8b93a3");

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)W, (unsigned)H, 0, c_fg, c_bg);
    XStoreName(dpy, win, up ? "Terminator (process mode)" : "Terminator");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    a_clip = XInternAtom(dpy, "CLIPBOARD", False); a_primary = XA_PRIMARY;
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False); a_targets = XInternAtom(dpy, "TARGETS", False);
    a_prop = XInternAtom(dpy, "TERMINATOR_SEL", False);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFont(dpy, gc, font->fid);
    make_backbuffer();
    XMapWindow(dpy, win);

    Tab *first = spawn_tab();
    if (!first) { fprintf(stderr, "terminator: could not start a shell tab\n"); return 1; }
    for (int i = 0; i < rc_nwarn; i++) { tab_puts(first, rc_warn[i]); tab_puts(first, "\n"); }

    while (!quit_flag) {
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose: dirty = 1; break;
            case ConfigureNotify:
                if (ev.xconfigure.width != W || ev.xconfigure.height != H) { W = ev.xconfigure.width; H = ev.xconfigure.height; make_backbuffer(); dirty = 1; }
                break;
            case KeyPress: on_key(&ev.xkey); break;
            case ButtonPress: on_button(&ev.xbutton); break;
            case SelectionRequest: on_selection_request(&ev.xselectionrequest); break;
            case SelectionNotify: on_selection_notify(&ev.xselection); break;
            case ClientMessage: if ((Atom)ev.xclient.data.l[0] == wm_delete) quit_flag = 1; break;
            }
        }
        if (quit_flag) break;
        if (dirty) { dirty = 0; redraw(); }

        struct pollfd pf[MAX_TABS + 1];
        pf[0] = (struct pollfd){ ConnectionNumber(dpy), POLLIN, 0 };
        for (int i = 0; i < ntabs; i++) pf[i + 1] = (struct pollfd){ tabs[i]->out_fd, POLLIN, 0 };
        if (poll(pf, (nfds_t)ntabs + 1, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int i = ntabs - 1; i >= 0; i--) {
            if (!(pf[i + 1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char b[4096]; ssize_t r = read(tabs[i]->out_fd, b, sizeof b);
            if (r > 0) tab_append(tabs[i], b, (size_t)r);
            else if (r == 0 || (errno != EINTR && errno != EAGAIN)) close_tab(i);
        }
    }
    shutdown_tabs();
    return 0;
}
