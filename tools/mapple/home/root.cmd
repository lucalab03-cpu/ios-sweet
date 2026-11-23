# The Mapple home-screen boot (tools/mapple/README.md). Log: /var/tmp/sweetroot.log
apply
# which crash reports there are (SweetMapple copies the newest backboardd*, JetsamEvent* and PosterBoard* into
# /var/tmp/sweetcrash.*); a new driver is picked up by backboardd itself (SweetMapple.m, sm_restart_on_new_files)
ls /var/mobile/Library/Logs/CrashReporter
ls /System/Library/Extensions/SweetMTLDriver.bundle
cat /System/Library/Extensions/SweetMTLDriver.bundle/vmg.env
