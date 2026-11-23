# The Mapple home-screen boot (tools/mapple/README.md). Log: /var/tmp/sweetroot.log
apply
# which crash reports there are (SweetMapple copies the newest backboardd* and JetsamEvent* into /var/tmp/sweetcrash.*)
ls /var/mobile/Library/Logs/CrashReporter
# sweetroot runs after the dropbox is installed, backboardd before: restarting it now makes it load the driver just
# installed, so a new SweetMapple needs one boot, not two (it costs one restart of backboardd per boot)
run /usr/bin/killall backboardd.orig
