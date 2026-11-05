#!/bin/sh
# CG721 wake hook: publish newest event thumbnail to MQTT (HA last-view) and
# push every not-yet-sent event clip (+ thumbnail) to the LAN receiver -> HA media.
# Log lives on the SD card and is truncated each wake (no NOR flash wear).

SD=/tmp/sdcard
MQTT=192.168.X.X        # Mosquitto broker (HA) - thumbnail last-view
RECV=192.168.X.X        # clip receiver host (Docker container on the SER8 host)
LOG=$SD/app.log
PUSHED=$SD/.pushed

# wait for the SD to mount (it comes up ~1s into the wake)
i=0
while [ ! -d "$SD/DCIM" ] && [ "$i" -lt 15 ]; do sleep 1; i=$((i+1)); done
[ -d "$SD/DCIM" ] || exit 0          # SD never came up this wake; nothing to do

# fresh log each wake (SD-backed, bounded size)
say(){ u=$(cat /proc/uptime 2>/dev/null); echo "[${u%% *}] $*" >> "$LOG" 2>/dev/null; }
: > "$LOG" 2>/dev/null
say "=== wake === (SD up after ${i}s)"

D=""; for d in $(ls -1t "$SD/DCIM" 2>/dev/null); do D="$SD/DCIM/$d"; break; done
[ -z "$D" ] && { say "DCIM empty"; exit 0; }
say "dir=$D"

# newest thumbnail -> MQTT last-view (retained)
T=""; for f in $(ls -1t "$D"/*.thumbnail.jpg 2>/dev/null); do T="$f"; break; done
if [ -n "$T" ]; then
    export MQTT_HOST="$MQTT" MQTT_PORT=1883 MQTT_USER=mosquitto MQTT_PASS='CHANGE_ME'
    export MQTT_TOPIC=cg721/snapshot MQTT_FILE="$T"
    LD_PRELOAD=/config/mqttpub.so busybox true; say "mqtt ${T##*/} rc=$?"
fi

# push every clip not yet confirmed-sent
[ -f "$PUSHED" ] || : > "$PUSHED"
for f in $(ls -1 "$D"/*.mp4 2>/dev/null); do
    b="${f##*/}"
    grep -q "$b" "$PUSHED" 2>/dev/null && continue
    export UP_HOST="$RECV" UP_PORT=8099 UP_FILE="$f" UP_NAME="$b"
    LD_PRELOAD=/config/clipup.so busybox true; r=$?
    say "clip $b rc=$r"
    if [ "$r" = "0" ]; then
        tj="${f%.mp4}.thumbnail.jpg"
        [ -f "$tj" ] && { export UP_FILE="$tj" UP_NAME="${tj##*/}"; LD_PRELOAD=/config/clipup.so busybox true; say "thumb ${tj##*/} rc=$?"; }
        echo "$b" >> "$PUSHED"
    fi
done
say "done"
exit 0
