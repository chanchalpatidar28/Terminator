#!/bin/sh
# Headless engine tests (no X needed).  Run with:  make test
BIN="$(cd "$(dirname "$0")/.." && pwd)/terminator"
export HOME=$(mktemp -d); W=$HOME/w; mkdir -p "$W"; cd "$W"
touch abc.txt def.txt myprog.c
PASS=0; FAIL=0
check() {  # name expected actual
    if [ "$2" = "$3" ]; then PASS=$((PASS+1)); printf 'ok    %s\n' "$1"
    else FAIL=$((FAIL+1)); printf 'FAIL  %s\n   want: %s\n   got : %s\n' "$1" "$2" "$3"; fi
}
tm() { "$BIN" -c "$1" 2>&1; }

check "glob + pipe"            "2"                     "$(tm 'ls *.txt | wc -l')"
check "redirect < >"           "hello"                 "$(printf hello >in.txt; tm 'cat < in.txt > out.txt; cat out.txt')"
check "append >>"              "2"                     "$(tm 'echo a > ap.txt; echo b >> ap.txt; wc -l < ap.txt')"
check "single quotes literal"  '$HOME'                 "$(tm "echo '\$HOME'")"
check "double quotes expand"   "$HOME"                 "$(tm 'echo "$HOME"')"
check "nested \$(...)"         "w:$(ls | wc -l | tr -d ' ')" "$(tm 'echo "$(basename $(pwd)):$(ls | wc -l)"')"
check "; && ||"                "a c d"                 "$(tm 'echo -n a; false || echo -n " c"; true && echo " d"' | tr -d '\n')"
check "\$? of last command"    "0 1"                   "$(tm 'true; echo -n "$? "; false; echo $?' | tr -d '\n')"
check "unknown command = 127"  "127"                   "$(tm 'nosuchcmd >/dev/null 2>&1; echo $?')"
check "cd is a builtin"        "$(dirname "$W")"       "$(printf 'cd ..\npwd\n' | "$BIN" --cli | sed -n 's/^user@terminator> user@terminator> //p')"
check "2>&1 and 2> work"       "ok 1"                  "$(tm 'nosuch > e.txt 2>&1; test -s e.txt && echo -n "ok "; ls /nonexistent 2>/dev/null; echo $?' | tr -d '\n' | sed 's/ 2$/ 1/')"
check "cd keeps state"         "/tmp"                  "$(tm 'cd /tmp; pwd')"
check "cd into missing dir"    "1"                     "$(tm 'cd /nonexistent 2>/dev/null; echo $?')"
check "variables"              "5 5"                   "$(tm 'X=5; echo $X; echo ${X}' | tr '\n' ' ' | sed 's/ $//')"
check "multiline backslash"    "hello world"           "$(printf 'echo hello \\\nworld\n' | "$BIN" --cli | sed 's/user@terminator> //g; s/^> //' | sed -n 1p)"
check "multiline open quote"   "a b"                   "$(printf 'echo "a\nb"\n' | "$BIN" --cli | tr '\n' ' ' | sed 's/user@terminator> //g; s/> //g; s/ *$//')"

cat > t1.sh <<'EOS'
#!/bin/bash
if [ -f "$1" ]; then
    echo "exists $# $1"
    n=3
    if [ "$n" -gt 5 ]; then echo big
    elif [ "$n" -eq 3 ]; then echo three
    else echo other; fi
else
    echo missing
fi
exit 7
EOS
chmod +x t1.sh
check "script ./t1.sh"         "exists 1 myprog.c three" "$(tm './t1.sh myprog.c' | tr '\n' ' ' | sed 's/ $//')"
check "script exit code"       "7"                     "$(tm './t1.sh myprog.c >/dev/null; echo $?')"
check "script bash t1.sh"      "missing"               "$(tm 'bash t1.sh nope')"
check "script nested if"       "three"                 "$(tm 'source t1.sh myprog.c | tail -1' )"
printf 'Q=1\n' > setq.sh; printf '#!/bin/bash\nQ=2\n' > setq2.sh; chmod +x setq2.sh
check "source shares vars"     "1"                     "$(tm 'source setq.sh; echo $Q')"
check "./script doesn't leak"  ""                      "$(tm './setq2.sh; echo $Q')"

check "history builtin"        "echo one"              "$(printf 'echo one\nhistory\n' | "$BIN" --cli | grep -o 'echo one' | head -1)"
OUT=$(TERMINATOR_MW_ITERS=1 "$BIN" -c 'multiWatch ["echo hi", "echo yo | tr a-z A-Z"]')
check "multiWatch header"      "2"                     "$(printf '%s\n' "$OUT" | grep -c '^"echo')"
check "multiWatch dashes (52)" "4"                     "$(printf '%s\n' "$OUT" | grep -c '^-\{52\}$')"
check "multiWatch parallel out" "YO"                   "$(printf '%s\n' "$OUT" | grep YO)"
check "bg job + Done notice"   "1"                     "$(printf 'sleep 0.2 &\nsleep 0.6\n\n' | "$BIN" --cli | grep -c 'Done sleep 0.2')"

# --- spec fixes ---
check "backticks"              "hi"                    "$(tm 'echo `echo hi`')"
check "nested backticks"       "nested"                "$(tm 'echo `echo \`echo nested\``')"
check "backtick in dquotes"    "a hi b"                "$(tm 'echo "a `echo hi` b"')"
printf '#!/bin/bash\necho START\nif [ 1 -eq 1 ]; then\n  echo inside\n' > nofi.sh
check "missing fi: reported"   "nofi.sh: line 4: syntax error: unexpected end of file: missing 'fi'" "$(tm 'bash nofi.sh')"
check "missing fi: nothing ran" "0"                    "$(tm 'bash nofi.sh' | grep -c START)"
printf '#!/bin/bash\nif [ 1 -eq 1 ]\n  echo x\nfi\n' > nothen.sh
check "missing then: reported" "nothen.sh: line 3: syntax error: expected 'then' after the condition" "$(tm 'bash nothen.sh')"
check "spec multiline example" "Isn't process creation exciting? Fork, exec, repeat!" "$(printf 'echo "Isn'"'"'t process creation exciting?\\\nFork, exec, repeat!"\n' | "$BIN" --cli | sed 's/user@terminator> //g; s/^> //' | tr '\n' ' ' | sed 's/ *$//')"

printf '\n%d passed, %d failed\n' $PASS $FAIL
[ $FAIL -eq 0 ]
