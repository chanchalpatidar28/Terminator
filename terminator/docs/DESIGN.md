# Terminator — design document

## 1. Architecture in one picture
```
                      X server
                         ^ events / draw
   +---------------------+----------------------+
   |  GUI  (single X thread, gui.c)             |   poll() on: X socket + one fd per tab
   |  tab buffers, line editor, redraw          |
   +---+--------------------------------+-------+
       | in_fd  (typed lines)           ^ out_fd (all output)
   +---v--------------------------------+-------+
   |  tab shell (engine.c) = thread  OR  forked process
   |  parse -> expand -> fork/exec jobs, builtins, job table
   +--------------------------------------------+
   Shared memory (shared.c): history, clipboard, keymap, per-tab control block (cwd, fg_pgid)
```
**Key idea:** the engine does not know where text comes from. `shell_run(in_fd, out_fd, tab)`
reads lines from `in_fd` and writes every byte (prompt, output, errors, job notices) to `out_fd`.
The GUI, `--cli` (fd 0/1) and scripts all plug into that one engine. Thread mode and process mode
differ only in *how `shell_run` is started* (`pthread_create` vs `fork`), so behaviour is identical.

## 2. GUI and event loop (`gui.c`)
* Only the GUI thread calls Xlib (`XInitThreads()` is still called defensively).
* Loop: drain `XPending()` events → redraw if dirty → `poll()` on the X connection fd plus every
  tab's `out_fd` → read whatever is ready. No busy loop; idle CPU ≈ 0.
* Each tab owns a line buffer (`Ln[]`, max 20,000 lines). The **last line is the partial line**: the
  shell's prompt lands there, and the edited input is drawn after it. Escape sequences, `\r`, `\b`
  and tabs are normalised on ingestion. Hidden tabs keep filling their buffers.
* Drawing is double-buffered (pixmap → `XCopyArea`), text is wrapped at draw time.
* The line editor (cursor, kill/yank, history browsing, `Ctrl+R`, completion list) lives in the GUI
  because it owns the keyboard; only completed lines are sent to the shell. A completed multi-line
  command is detected by the shell (`line_incomplete`), which then prints the `> ` prompt.

## 3. Execution modes and synchronisation
| | Thread mode (default) | Process mode (`useProcess=True`) |
|-|-|-|
| Tab shell | detached `pthread` | `fork()` of the GUI |
| GUI ↔ tab | two pipes | the same two pipes |
| Shared state | the shared mapping | the shared mapping |
| Lock | process-shared semaphore (`G->lock`) | same semaphore |

All shared state sits in **one `MAP_SHARED|MAP_ANONYMOUS` mapping created before any tab exists**
(`struct Shared`): history ring (10,000 × 512 B), clipboard, keymap, and a `TabCtl` per tab
(`cwd`, `fg_pgid`). One `sem_t` (initialised with `pshared=1`, used as a binary semaphore) protects it, so the *same code* is correct
for threads and for processes. Everything else (cwd, input line, output buffer, jobs, variables)
is private to a tab: it lives in the tab's own `Shell` object or `Tab` struct.

Thread-mode hazards and how they are avoided:
* `chdir()`, `dup2()` and signal dispositions are process-wide. The process never calls `chdir`:
  each tab keeps its own `cwd` string, and **only the forked child** calls `chdir(cwd)`, `dup2`, and
  resets signals before `exec`. `cd` just validates with `realpath/stat` and updates the string.
* Never `waitpid(-1)`: a tab only waits for its own children, so tabs cannot steal each other's.
* **fd leak between tabs:** if tab A has a pipe open while tab B forks, B's child would inherit A's
  write end and A's reader would never see EOF. All pipes are `O_CLOEXEC`, and `pipe2()+fork()`
  sequences are serialised with `fork_lock` (re-initialised in every child).
* The `Ctrl+C` path touches no shared signal handler: the GUI `kill(-pgid, SIGINT)`s one job.

## 4. Shell engine (`engine.c`)
Pipeline: **read → split → expand → build → run**.
1. *read*: `line_incomplete()` runs a small quote-stack scanner (`Scan`: command / `"` / `'` / `$(`)
   so an unclosed quote, trailing `\` or unfinished `$(` asks for another line (`join_lines`).
2. *split* into segments at top-level `;` `&&` `||` `&` and newlines (quote/paren aware).
   Splitting happens **before** expansion, so `cd x; echo $(pwd)` sees the new directory.
3. *expand/tokenise* one segment: quotes, `\`, `$VAR`, `${VAR}`, `$?`, `$#`, `$0..$9`, `~`, globs
   (matched against the *tab's* cwd, not the process cwd), `$(...)` and `` `...` `` — which fork a child that runs
   the inner text through the same engine and returns its stdout (so nesting is innermost-first).
4. *build*: tokens → `Cmd[]` (`argv`, `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`), `|` separates commands.
5. *run*: N−1 `pipe2()`s, N `fork()`s, parent closes every pipe end, then waits. Children `dup2` the
   pipe/redirect fds, close all other pipe fds, then `execvp` (exit 127 / 126 on failure).

Built-ins that change the shell (`cd`, `exit`, `export`, `source`, `fg`, `bg`, `jobs`, `history`, `bind`)
run **in the shell** when they are the only command of a pipeline (with `>` honoured); inside a
pipeline they run in the forked child like in bash.

## 5. Jobs and signals
* A job = one process group (`setpgid` called by both parent and child to avoid the race). Table
  `Shell.jobs[]` holds `pgid`, each pid, alive flags, state (running/stopped), command text.
* No TTY, so the kernel never generates `SIGINT`/`SIGTSTP`. The GUI reads `KeyPress` + `ControlMask`,
  looks up `G->tabs[slot].fg_pgid` (published by the shell while a foreground job runs) and does
  `kill(-pgid, SIGINT|SIGTSTP)`. With `fg_pgid == 0` (idle) `Ctrl+C` cancels the current line and any
  pending multi-line input (the GUI sends a `0x03` line, the shell drops its accumulator).
* `wait_job()` uses `waitpid(pid, WUNTRACED)` per process: an exit or kill marks the pid dead; a stop
  marks the job `STOPPED`, prints `[n]+ Stopped cmd` and returns to the prompt. `fg` = `SIGCONT` + wait,
  `bg` = `SIGCONT` + keep going, `cmd &` = don't wait. Finished background jobs are reaped with
  `WNOHANG` before each prompt (`[n]+ Done cmd`).
* Terminator itself ignores `SIGINT/SIGTSTP/SIGTTIN/SIGTTOU/SIGPIPE`; children restore `SIG_DFL`
  before `exec`, so a job never inherits an ignored signal. Closing a tab sends `SIGHUP` to its jobs.

## 6. multiWatch
Parse the bracketed list from the **raw** text (quotes must survive). For each command: `pipe2()`,
`fork()`, child joins one shared process group, runs the command through the engine with its stdout/
stderr on the pipe, then writes a `\0` end-of-round marker and sleeps `MW_INTERVAL` (2) seconds. The parent `poll()`s all
read ends, buffers each command's bytes until the marker and prints one block per round in the
required format. `Ctrl+C` signals the whole group; the pipes hit EOF, the parent reaps the children
and returns to the prompt while the shell keeps running.

## 7. History, Ctrl+R, completion, hotkeys
* History: ring buffer in shared memory + append-only `~/.terminator_history` (loaded at start).
  `Ctrl+R` computes exact match → else longest-common-substring (DP, O(n·m)) → else no match.
* Completion: scans the tab's cwd (or the typed directory), filters by prefix, sorts; one match
  completes, several extend to the common prefix and print `1. a   2. b`; a digit picks one.
* Hotkeys: table of `(action, modifiers, keysym)` in shared memory; `bind` edits it live and persists
  to `~/.terminatorrc`; invalid names or clashing keys are rejected with a warning (default kept).

## 7b. Clipboard (X11 selections)
`Ctrl+Shift+C` makes the GUI window the owner of CLIPBOARD and PRIMARY (`XSetSelectionOwner`) and keeps the
text in memory; when another client sends `SelectionRequest` we answer with `XChangeProperty` +
`XSendEvent(SelectionNotify)` (targets: TARGETS, STRING, UTF8_STRING). `Ctrl+Shift+V` calls
`XConvertSelection`; the text arrives later as `SelectionNotify` and is inserted into the current tab.
Only the GUI touches X11, so this works the same in both modes.

## 8. Scripts (`script.c`)
`./run.sh` (shebang `bash`/`sh`), `bash run.sh`, `bash -c "..."` and `source run.sh` are all
interpreted by our code — `/bin/bash` is never exec'd. A script run as a command executes in a forked
child (so its variables don't leak, `exit n` becomes the exit status); `source` runs in the shell itself
and shares variables. Before anything runs, `script_check()` walks the whole file once (joining continuation lines the same way) and rejects `if` without `then`, `elif/else/fi` without `if`, a second `else`, a missing `fi`, or an unterminated quote, printing `file: line N: syntax error: ...` with status 2. Control flow uses a frame stack `{parent_active, taken, active}` which gives
correct nested `if/elif/else/fi` with simple line-by-line execution; conditions are `[ ]`, `test` or any
command (exit status).

## 9. Data structures at a glance
`Shared` (mmap) · `TabCtl` · `Shell` (per tab: fds, cwd, vars, positional params, `Job[64]`) ·
`Job` · `Cmd` · `Token` · `Scan` (quote stack) · `Tab` (GUI: `Ln[]`, input, completion list) · `Bind`.

## 10. Testing
`tests/run_tests.sh` (27 headless engine tests), `tests/gui_smoke.sh thread|process` (Xvfb + xdotool:
typing, `Ctrl+Z`/`bg`/`fg`/`Ctrl+C`, `multiWatch`, tabs, completion, `Ctrl+R`, scripts, rc warnings).
