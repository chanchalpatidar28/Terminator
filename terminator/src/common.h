#ifndef COMMON_H
#define COMMON_H
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>

#define MAX_TABS    16
#define HIST_MAX    10000      /* commands kept (memory + file)            */
#define HIST_SHOW   1000       /* commands printed by the `history` builtin */
#define HIST_LINE   512
#define MAX_JOBS    64
#define MAX_PROCS   32
#define MAX_VARS    128
#define MAX_BINDS   64
#define MW_MAX      16
#define MW_INTERVAL 2          /* multiWatch re-runs each command every N seconds */
/* The task text shows  echo "...?\<newline>> ..."  printing TWO lines, i.e. a
 * backslash-newline inside double quotes keeps the newline.  Real bash joins them
 * into one line.  1 = match the task text (default), 0 = behave like bash.      */
#define SPEC_DQ_NEWLINE 1
#define PROMPT      "user@terminator> "
#define CONT_PROMPT ">"

enum Action {
    A_NONE = 0, A_LEFT, A_RIGHT, A_HOME, A_END, A_HIST_PREV, A_HIST_NEXT,
    A_KILL_START, A_KILL_END, A_DEL_WORD, A_CLEAR, A_CLOSE, A_COPY, A_PASTE,
    A_SEARCH, A_COMPLETE, A_NEWTAB, A_NEXTTAB, A_PREVTAB, A_INTERRUPT,
    A_SUSPEND, A_COUNT
};
#define MOD_CTRL  1
#define MOD_SHIFT 2
#define MOD_ALT   4

typedef struct 
{
    int action;
    unsigned mods;
    unsigned long keysym; 
}Bind;

/* Per-tab control block, visible to the GUI *and* to the tab's shell
 * (shared memory => works for both thread mode and process mode).        */
typedef struct 
{
    int used;
    volatile pid_t fg_pgid;      /* foreground job's process group, 0 = idle */
    char cwd[PATH_MAX];
} TabCtl;

/* Everything shared between tabs lives in ONE shared mapping
 * (MAP_SHARED|MAP_ANONYMOUS), guarded by a PROCESS_SHARED semaphore.          */
typedef struct 
{
    sem_t lock;                  /* process-shared binary semaphore */
    long  hist_total;        //track total hist. command
    char  hist[HIST_MAX][HIST_LINE];
    char  clip[4096];         //store  clickboard text

    int   nbinds;          //number of active binding
    Bind  binds[MAX_BINDS];    //keyboard binding
    TabCtl tabs[MAX_TABS];   
} Shared;

extern Shared *G;
void shared_init(void);
static inline void shlock(void)   // lock
{
    while (sem_wait(&G->lock) == -1 && errno == EINTR) {} 
}
static inline void shunlock(void)  //free
    {
        sem_post(&G->lock); 
    }

void hist_add(const char *line);
long hist_count(void);
int  hist_from_end(int k, char *out, size_t cap);                 /* k=0 newest */
int  hist_search(const char *term, char out[][HIST_LINE], int max, int *exact);

extern char rc_warn[16][200];  //run time configuration warning
extern int  rc_nwarn;

void keymap_defaults(void);
void keymap_load_rc(void);
int  keymap_lookup(unsigned mods, unsigned long ks);
int  bind_set(const char *action, const char *spec, char *err, size_t errsz);
int  bind_persist(const char *action, const char *spec);
void bind_list(int fd);
const char *action_name(int a);

void shell_run(int in_fd, int out_fd, int tab_idx);
int  gui_main(int use_process);    //start graphical interface
#endif










