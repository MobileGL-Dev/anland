#!/system/bin/sh
# Restart the experimental display daemon cleanly, keeping the original
# daemon (PID 2315, /data/local/tmp/display_daemon.sock) untouched.
set -e
kill 13993 2>/dev/null || true
sleep 1
rm -f /data/local/tmp/anland-mobilegl/display.sock
nohup /data/adb/modules/anland-daemon/display_daemon /data/local/tmp/anland-mobilegl/display.sock \
    > /data/local/tmp/anland-mobilegl/display-daemon.log 2>&1 &
sleep 1
chmod 777 /data/local/tmp/anland-mobilegl
chmod 666 /data/local/tmp/anland-mobilegl/display.sock
echo "daemon restarted:"
ps -A -o PID,ARGS | grep "anland-mobilegl/display.sock" | grep -v grep
