#!/system/bin/sh
/data/local/Droidspaces/bin/droidspaces --name=arch-kde-mgl run bash -lc 'nohup bash /root/anland-build-client.sh > /root/mobilegl-unified-client-build.log 2>&1 < /dev/null & echo $!'
