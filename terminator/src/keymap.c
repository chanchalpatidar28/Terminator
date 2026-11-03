/* keymap.c - user-configurable hotkeys (~/.terminatorrc and the `bind` builtin) */
#include "common.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>

char rc_warn[16][200];
int  rc_nwarn;

static const char *names[A_COUNT] = {
    "", "move-left", "move-right", "line-start", "line-end", "history-prev",
    "history-next", "kill-to-start", "kill-to-end", "delete-word", "clear-screen",
    "close-tab", "copy", "paste", "search-history", "complete", "new-tab",
    "next-tab", "prev-tab", "interrupt", "suspend"
};
const char *action_name(int a) { return (a > 0 && a < A_COUNT) ? names[a] : "?"; }

static int action_by_name(const char *n)
{
    for (int i = 1; i < A_COUNT; i++) if (!strcmp(names[i], n)) return i;
    return 0;
}

/* "Ctrl+Shift+L", "Alt+Backspace", "Left", "a" ... */
static int key_parse(const char *spec, unsigned *mods, unsigned long *ks)
{
    char buf[64], *tok[8]; int n = 0;
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *t = strtok_r(buf, "+", &save); t && n < 8; t = strtok_r(NULL, "+", &save)) tok[n++] = t;
    if (n == 0) return -1;
    *mods = 0;
    for (int i = 0; i < n - 1; i++) {
        if (!strcasecmp(tok[i], "ctrl") || !strcasecmp(tok[i], "control")) *mods |= MOD_CTRL;
        else if (!strcasecmp(tok[i], "shift")) *mods |= MOD_SHIFT;
        else if (!strcasecmp(tok[i], "alt") || !strcasecmp(tok[i], "meta")) *mods |= MOD_ALT;
        else return -1;
    }
    static const struct { const char *alias, *real; } al[] = {
        { "backspace", "BackSpace" }, { "enter", "Return" }, { "esc", "Escape" },
        { "pageup", "Prior" }, { "pagedown", "Next" }, { "del", "Delete" },
        { "ins", "Insert" }, { "spacebar", "space" },
    };
    const char *kn = tok[n - 1];
    for (size_t i = 0; i < sizeof al / sizeof al[0]; i++)
        if (!strcasecmp(kn, al[i].alias)) kn = al[i].real;
    KeySym k = XStringToKeysym(kn);
    if (k == NoSymbol && strlen(tok[n - 1]) == 1)
    {
        char c[2] = { (char)tolower((unsigned char)tok[n - 1][0]), 0 };
        k = XStringToKeysym(c);
    }
    if (k == NoSymbol) 
    return -1;
    KeySym lo, up; XConvertCase(k, &lo, &up);
    *ks = lo;
    return 0;
}

static void add_raw(int action, unsigned mods, unsigned long ks)
{
    if (G->nbinds < MAX_BINDS) G->binds[G->nbinds++] = (Bind){ action, mods, ks };
}

void keymap_defaults(void)
{
    static const struct { int a; const char *k; } d[] = {
        { A_LEFT, "Left" }, 
        { A_RIGHT, "Right" },
        { A_HOME, "Ctrl+A" }, 
        { A_HOME, "Home" }, 
        { A_END, "Ctrl+E" },
         { A_END, "End" },
        { A_HIST_PREV, "Up" }, 
        { A_HIST_NEXT, "Down" },
        { A_KILL_START, "Ctrl+U" },
         { A_KILL_END, "Ctrl+K" },
          { A_DEL_WORD, "Ctrl+W" },
        { A_CLEAR, "Ctrl+L" }, 
        { A_CLOSE, "Ctrl+D" },
        { A_COPY, "Ctrl+Shift+C" }, 
        { A_PASTE, "Ctrl+Shift+V" },
        { A_SEARCH, "Ctrl+R" }, 
        { A_COMPLETE, "Tab" },
         { A_NEWTAB, "Ctrl+T" },
        { A_NEXTTAB, "Alt+Right" }, 
        { A_PREVTAB, "Alt+Left" },
        { A_INTERRUPT, "Ctrl+C" }, 
        { A_SUSPEND, "Ctrl+Z" },
    };
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) {
        unsigned m; unsigned long k;
        if (key_parse(d[i].k, &m, &k) == 0) add_raw(d[i].a, m, k);
    }
}

int keymap_lookup(unsigned mods, unsigned long ks)
{
    int r = 0;
    shlock();
    for (int i = 0; i < G->nbinds; i++)
        if (G->binds[i].mods == mods && G->binds[i].keysym == ks) 
        {
             r = G->binds[i].action;
              break; 
        }
    shunlock();
    return r;
}

/* Rebind an action. On any problem the old (default) binding is kept. */
int bind_set(const char *action, const char *spec, char *err, size_t errsz)
{
    int a = action_by_name(action);
    unsigned m; 
    unsigned long k;
    if (!a) 
    {
        snprintf(err, errsz, "unknown action '%s'", action);
        return -1; 
    }
    if (key_parse(spec, &m, &k) < 0) 
    {
        snprintf(err, errsz, "bad key '%s'", spec);
        return -1;
    }
    shlock();
    for (int i = 0; i < G->nbinds; i++)
        if (G->binds[i].mods == m && G->binds[i].keysym == k && G->binds[i].action != a) 
        {
            snprintf(err, errsz, "'%s' is already used by '%s'", spec, names[G->binds[i].action]);
            shunlock();
            return -1;
        }
    int w = 0;
    for (int i = 0; i < G->nbinds; i++)
        if (G->binds[i].action != a) G->binds[w++] = G->binds[i];
    G->nbinds = w;
    add_raw(a, m, k);
    shunlock();
    return 0;
}

int bind_persist(const char *action, const char *spec)
{
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.terminatorrc", home ? home : ".");
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    fprintf(f, "bind %s %s\n", action, spec);
    fclose(f);
    return 0;
}

void bind_list(int fd)
{
    shlock();
    for (int i = 0; i < G->nbinds; i++) 
    {
        Bind *b = &G->binds[i];
        dprintf(fd, "%-16s %s%s%s%s\n", names[b->action],
                (b->mods & MOD_CTRL) ? "Ctrl+" : "", (b->mods & MOD_SHIFT) ? "Shift+" : "",
                (b->mods & MOD_ALT) ? "Alt+" : "", XKeysymToString(b->keysym));
    }
    shunlock();
}

void keymap_load_rc(void)
{
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.terminatorrc", home ? home : ".");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256]; int ln = 0;

    while (fgets(line, sizeof line, f)) 
    {
        ln++;
        line[strcspn(line, "\r\n")] = 0;
        char *p = line; while (isspace((unsigned char)*p)) p++;

        if (!*p || *p == '#') continue;
        char cmd[32], act[64], key[64], err[120];

        if (sscanf(p, "%31s %63s %63s", cmd, act, key) != 3 || strcmp(cmd, "bind") ||
            bind_set(act, key, err, sizeof err) < 0) 
            {
            if (rc_nwarn < 16)
                snprintf(rc_warn[rc_nwarn++], sizeof rc_warn[0],
                         "warning: ~/.terminatorrc line %d ignored (%s)", ln,
                         strcmp(cmd, "bind") ? "expected: bind <action> <key>" : err);
            continue;
        }
    }
    fclose(f);
}
