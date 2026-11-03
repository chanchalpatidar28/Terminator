/* main.c - ./terminator [useProcess=True] | --cli | -c "line" */
#include "shell.h"
#include <signal.h>
#include <strings.h>

int main(int argc, char **argv)
{
    int use_process = 0, cli = 0;
    const char *cline = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--", 2)) a += 2;
        if (!strncasecmp(a, "useProcess=", 11)) use_process = !strcasecmp(a + 11, "true") || !strcmp(a + 11, "1");
        else if (!strcmp(a, "cli")) cli = 1;
        else if (!strcmp(a, "-c") && i + 1 < argc) cline = argv[++i];
        else { fprintf(stderr, "usage: %s [useProcess=True] [--cli] [-c \"command line\"]\n", argv[0]); return 2; }
    }

    signal(SIGPIPE, SIG_IGN);                 /* children restore the defaults */
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    shared_init();
    keymap_load_rc();

    if (cline) {                              /* headless: run one line through the engine */
        Shell *sh = shell_new(0, 1, -1);
        sh->job_control = 0;
        run_line(sh, cline);
        return sh->exit_flag ? sh->exit_code : sh->last_status;
    }
    if (cli) {                                /* headless REPL on stdin/stdout (testing) */
        shell_run(0, 1, -1);
        return 0;
    }
    return gui_main(use_process);
}
