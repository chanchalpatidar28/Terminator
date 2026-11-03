#!/bin/sh
# GUI smoke test under Xvfb + xdotool.  usage: tests/gui_smoke.sh [thread|process] [outdir]
MODE=${1:-thread}; OUT=${2:-/tmp/guitest}; BIN="$(cd "$(dirname "$0")/.." && pwd)/terminator"
mkdir -p "$OUT"; export HOME=${TEST_HOME:-/tmp/th}; mkdir -p $HOME/w; cd $HOME/w; touch abc.txt abcd.txt def.txt myprog.c
rm -f $HOME/.terminator_history
export DISPLAY=:98
Xvfb :98 -screen 0 1280x800x24 >/dev/null 2>&1 & XPID=$!
sleep 1.5
if [ "$MODE" = process ]; then "$BIN" useProcess=True >$OUT/app.log 2>&1 & else "$BIN" >$OUT/app.log 2>&1 & fi
APP=$!; sleep 1.5
WID=$(xdotool search --name "Terminator" | head -1); xdotool windowfocus $WID
t(){ xdotool type --delay 30 "$1"; xdotool key Return; sleep ${2:-0.5}; }
shot(){ import -window root "$OUT/$MODE-$1.png"; }

t 'sleep 100' 0.4; xdotool key ctrl+z; sleep 0.7
t 'jobs'; shot stopped; t 'bg %1'; t 'jobs'; t 'fg %1' 0.4; xdotool key ctrl+c; sleep 0.7
t 'sleep 50 | cat' 0.4; xdotool key ctrl+c; sleep 0.6
t 'echo after-interrupt'
t 'multiWatch ["date +%T", "ls | wc -l"]' 2.3; xdotool key ctrl+c; sleep 0.8
t 'echo still-alive'
shot signals

# second tab, private cwd, shared history
xdotool key ctrl+t; sleep 0.8
t 'cd /tmp' ; t 'pwd'
xdotool key Up; sleep 0.3; xdotool key Up; sleep 0.3          # history from tab 1 should show up
shot tab2
# completion
xdotool key ctrl+u; xdotool key alt+Left; sleep 0.3
xdotool type --delay 30 './myprog def.txt abc'; xdotool key Tab; sleep 0.5
shot complete-list
xdotool key 1; sleep 0.4
shot complete-picked
xdotool key ctrl+u
# Ctrl+R
xdotool key ctrl+r; sleep 0.2; xdotool type --delay 30 'echo still'; xdotool key Return; sleep 0.5
shot ctrl-r
xdotool key ctrl+u; xdotool key ctrl+r; xdotool type --delay 30 'zzzzzz'; xdotool key Return; sleep 0.5
shot ctrl-r-nomatch
# script run
cat > $HOME/w/check.sh <<'EOS'
#!/bin/bash
if [ -f "$1" ]; then
    echo "File exists, compiling..."
else
    echo "Source file missing!"
fi
EOS
chmod +x $HOME/w/check.sh
xdotool key ctrl+u; t './check.sh myprog.c'; t 'bash check.sh nope.c'
t 'echo "Files in $(basename $(pwd)): $(ls | \'; t 'wc -l)"'
shot script
# closing tabs with Ctrl+D
xdotool key ctrl+d; sleep 0.8; shot after-close
kill $APP 2>/dev/null; sleep 0.5; kill $XPID 2>/dev/null
cat $OUT/app.log
