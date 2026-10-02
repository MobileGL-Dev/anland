#!/system/bin/sh
# Deploy/start the MobileGL experiment on HA27Q3LQ. Run as: su -c 'sh deploy_mobilegl.sh <phase>'
# Phases: apk (start consumer app), kwin (start compositor session), logs (tail relevant logs)
set -x
PHASE="$1"
case "$PHASE" in
apk)
    am force-stop com.anland.consumer.mobilegl
    am start -n com.anland.consumer.mobilegl/com.anland.consumer.MainActivity \
        --es socket_path /data/local/tmp/anland-mobilegl/display.sock
    ;;
kwin)
    /data/local/Droidspaces/bin/droidspaces --name=arch-kde-mgl run bash -lc '
        systemctl reset-failed desktop-session.service
        systemctl start desktop-session.service
        sleep 5
        systemctl status desktop-session.service --no-pager -l | head -20
        exit 0'
    ;;
logs)
    echo "=== logcat (anland/mobilegl) ==="
    logcat -d -s AnlandMobileGL:* AndroidRuntime:E | tail -40
    echo "=== worker server log ==="
    tail -30 /data/data/com.anland.consumer.mobilegl/files/mobilegl-server.log 2>/dev/null
    echo "=== display daemon log ==="
    tail -10 /data/local/tmp/anland-mobilegl/display-daemon.log
    echo "=== AVC denials (last 40) ==="
    dmesg | grep -i "avc:.*denied" | tail -40
    ;;
esac
