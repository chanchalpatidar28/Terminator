/* shared.c - the one shared-memory region: history, clipboard, keymap, tab control */
#include "common.h"
#include <sys/mman.h>

Shared *G;
static char histfile[PATH_MAX];

static void hist_store(const char *line)
{
    char *slot = G->hist[G->hist_total % HIST_MAX];
    strncpy(slot, line, HIST_LINE - 1);
    slot[HIST_LINE - 1] = 0;
    for (char *p = slot; *p; p++) 
    if (*p == '\n') *p = ' ';     //replace newline to space
    G->hist_total++;
}

void shared_init(void)
{
    G = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);           //linux system call for add. space


    if (G == MAP_FAILED)
    {
        perror("mmap"); 
        exit(1); 
    }


    if (sem_init(&G->lock, 1 /* shared between processes */, 1) != 0) //semaphores 
    {
        perror("sem_init"); 
        exit(1);
    }


    const char *home = getenv("HOME");

    snprintf(histfile, sizeof histfile, "%s/.terminator_history", home ? home : ".");

    FILE *f = fopen(histfile, "r");

    if (f) //read the command
    {
        char buf[HIST_LINE * 2];
        while (fgets(buf, sizeof buf, f)) 
        {
            buf[strcspn(buf, "\n")] = 0;
            if (*buf) hist_store(buf);
        }
        fclose(f);
    }
    keymap_defaults();
}


void hist_add(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) return;
    shlock();
    hist_store(line);
    FILE *f = fopen(histfile, "a");
    if (f) {
        const char *last = G->hist[(G->hist_total - 1) % HIST_MAX];
        fprintf(f, "%s\n", last);
        fclose(f);
    }
    shunlock();
}

long hist_count(void)
{
    shlock();
    long n = G->hist_total < HIST_MAX ? G->hist_total : HIST_MAX;
    shunlock();
    return n;
}

int hist_from_end(int k, char *out, size_t cap)
{
    int ok = 0;
    shlock();
    long avail = G->hist_total < HIST_MAX ? G->hist_total : HIST_MAX;
    if (k >= 0 && k < avail) {
        snprintf(out, cap, "%s", G->hist[(G->hist_total - 1 - k) % HIST_MAX]);
        ok = 1;
    }
    shunlock();
    return ok;
}

/* length of the longest common substring of a and b */
static int lcs_len(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b), best = 0;
    int row[HIST_LINE + 1];
    if (!la || !lb) return 0;
    memset(row, 0, sizeof row);
    for (int i = 0; i < la; i++)
        for (int j = lb; j >= 1; j--) {
            row[j] = (a[i] == b[j - 1]) ? row[j - 1] + 1 : 0;
            if (row[j] > best) best = row[j];
        }
    return best;
}

/* Ctrl+R semantics:
 *  1. exact match      -> the most recent one
 *  2. else longest-common-substring matches, if the LCS is > 2 chars
 *  3. else nothing (caller prints "No match for search term in history")  */
int hist_search(const char *term, char out[][HIST_LINE], int max, int *exact)
{
    int n = 0;
    char t[HIST_LINE];
    snprintf(t, sizeof t, "%s", term);
    *exact = 0;
    shlock();
    long avail = G->hist_total < HIST_MAX ? G->hist_total : HIST_MAX;
    for (long k = 0; k < avail; k++) {
        const char *l = G->hist[(G->hist_total - 1 - k) % HIST_MAX];
        if (!strcmp(l, t)) {
            snprintf(out[0], HIST_LINE, "%s", l);
            *exact = 1; n = 1; goto done;
        }
    }
    int best = 0;
    for (long k = 0; k < avail; k++) {
        int l = lcs_len(G->hist[(G->hist_total - 1 - k) % HIST_MAX], t);
        if (l > best) best = l;
    }
    if (best <= 2) goto done;
    for (long k = 0; k < avail && n < max; k++) {
        const char *l = G->hist[(G->hist_total - 1 - k) % HIST_MAX];
        if (lcs_len(l, t) != best) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp(out[i], l)) { dup = 1; break; }
        if (!dup) snprintf(out[n++], HIST_LINE, "%s", l);
    }
done:
    shunlock();
    return n;
}
