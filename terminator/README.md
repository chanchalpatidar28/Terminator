# Terminator

A terminal emulator **and** shell written in C on top of Xlib. It has tabs, pipes,
redirection, job control, `multiWatch`, shared history with `Ctrl+R`, file-name
completion, user-configurable hotkeys, and its own bash-subset script interpreter.
Only the system calls and X11 are used — no ncurses, no readline, no `system()`.

## Dependencies
* gcc, make, pthreads
* Xlib development files: `sudo apt install libx11-dev` (Debian/Ubuntu)
* An X server (`DISPLAY` set). For headless testing: `xvfb xdotool imagemagick`.

## Build and run
```
make                       # builds ./terminator
./terminator               # thread mode (default): every tab is a thread
./terminator useProcess=True   # process mode: every tab is a forked process
./terminator --cli         # headless shell on stdin/stdout (no GUI, handy for testing)
./terminator -c "ls | wc -l"   # run one line through the engine and exit
make test                  # 27 headless engine tests
sh tests/gui_smoke.sh thread|process   # GUI smoke test under Xvfb, screenshots in /tmp/guitest
```

## Features (task by task)
| # | Task | Where |
|---|------|-------|
| 1 | X11 window, tabs (click a tab, `+`, `Ctrl+T`, `Alt+←/→`, `Ctrl+D` closes) | `gui.c` |
| 2 | External commands (fork/execvp/waitpid), built-ins `cd pwd exit history jobs fg bg source export unset bind multiWatch`, `$?` | `engine.c` |
| 3 | Multiline input: open `'` `"`, trailing `\`, unfinished `$(` or `` ` ``; continuation prompt `> `; `$VAR`, `${VAR}`, `$?`, `$#`, `$1..$9`, nested `$(...)` and `` `...` ``, globs (`*.txt`), `~` | `engine.c` |
| 4 | `<`, `>`, `>>`, `2>`, `2>>`, `2>&1` through `dup2` in the child | `engine.c` |
| 5 | `cmd \| cmd \| cmd`, `;`, `&&`, `\|\|`, `&` | `engine.c` |
| 6 | `multiWatch ["cmd1", "cmd2"]` — one pipe+child per command, `poll()`ed | `engine.c` |
| 7 | Hotkeys: bash defaults, copy/paste through real X11 selections (CLIPBOARD + PRIMARY), rebind in `~/.terminatorrc` or with `bind <action> <key>` | `keymap.c`, `gui.c` |
| 8 | `Ctrl+C`, `Ctrl+Z`, `fg`, `bg`, `jobs`, `cmd &` via process groups | `engine.c`, `gui.c` |
| 9 | History: 10,000 commands, `~/.terminator_history`, shared by all tabs, `Ctrl+R` | `shared.c`, `gui.c` |
| 10 | `Tab` completion of file names (one match / common prefix + numbered list) | `gui.c` |
| 11 | Scripts: `./run.sh`, `bash run.sh`, `source run.sh`; nested `if/elif/else`, `[ ]`/`test`, `exit n` — all interpreted by our own code; the whole file is syntax-checked first (missing `fi`/`then`, stray `else`, open quote) and nothing runs if it is broken | `script.c` |

## Hotkeys
Defaults: `←/→`, `Ctrl+A/E` (line start/end), `↑/↓` (history), `Ctrl+U/K` (kill to start/end),
`Ctrl+W` (delete word), `Ctrl+L` (clear), `Ctrl+D` (EOF / close tab), `Ctrl+Shift+C/V` (copy/paste),
`Ctrl+R` (search), `Tab` (complete), `Ctrl+T` (new tab), `Alt+←/→` (switch tab), `Ctrl+C`, `Ctrl+Z`.
Scrolling: mouse wheel or `Shift+PageUp/PageDown`.

`~/.terminatorrc`:
```
bind clear-screen Ctrl+Shift+L
bind delete-word Alt+Backspace
```
Actions: `move-left move-right line-start line-end history-prev history-next kill-to-start
kill-to-end delete-word clear-screen close-tab copy paste search-history complete new-tab
next-tab prev-tab interrupt suspend`. A bad action/key, or a key already used by another
action, prints a warning in the first tab and keeps the default. Typing `bind` lists the
current bindings; `bind <action> <key>` changes one live and saves it to the rc file.

## Behaviour notes / decisions you may want to check against the spec
* **multiWatch** re-runs every command every 2 seconds (`MW_INTERVAL`) until `Ctrl+C` (each run is printed as
  `"cmd", <epoch>:` + dashed line + output + dashed line, 52 dashes). Set `TERMINATOR_MW_ITERS=N`
  to stop after N rounds (used by the tests).
* **Backslash-newline inside double quotes** keeps the newline, because that is what the task text's
  example prints. Real bash would join the two lines. Set `SPEC_DQ_NEWLINE` to 0 in `src/common.h`
  for bash behaviour. Outside quotes a trailing `\` joins lines like bash.
* `history` prints the latest `HIST_SHOW` (1000) commands; the store keeps `HIST_MAX` (10000).
  Both are `#define`s in `src/common.h`.
* `Ctrl+R`: exact match → most recent; else longest common substring (> 2 chars), all commands
  that tie; else `No match for search term in history`. A single result is also put on the input line.
* `Ctrl+C`/`Ctrl+Z` echo `^C`/`^Z` and signal the tab's foreground **process group** only.
* `Ctrl+Shift+C` puts the current input line on the X11 CLIPBOARD and PRIMARY selections (so other
  programs can paste it); `Ctrl+Shift+V` pastes the CLIPBOARD (from us or from any other program).

## Known limitations
* Output is ASCII-only (core X fonts); other bytes show as `?`.
* Programs that need a real TTY (vim, top, `read` from the keyboard) are not supported — jobs get
  `/dev/null` as stdin (the task has no pty).
* `2>&1` is applied after `>` (so `cmd >f 2>&1` works; `cmd 2>&1 >f` behaves the same as it).
* No loops/functions in scripts (not required), no `$((...))`, no here-docs, no aliases.
* Word splitting is not applied to unquoted `$VAR` / `$(...)` results (newlines become spaces).
* No mouse text selection (copy takes the input line).
