# sweetroot command file (tools/pbwrap/root.cmd). Delivered by the dropbox (/var/sweet/root.cmd), applied at boot by
# /usr/local/bin/sweetroot; the first operation must be "apply". Log: /var/tmp/sweetroot.log
#
# 2026-11-24 round "m" (payload-m: eMMC DMA with the 64G bit and CMD12 recovery, 0145).
apply

# the VkPipeline log of round k: 7 MB in one boot (now opt-in through /var/sweet/m2v-pipelog)
rm /var/sweet/m2v-cache/backboardd.orig/pipelines.log

# Settings > About waits for IOKit to go quiet: which services stay busy (forks; writes after 120 s and 180 s)
run /usr/local/bin/sweetbusy /var/tmp/sweetbusy.txt 120

newest /var/mobile/Library/Logs/CrashReporter PosterBoard /var/tmp/sweetcrash.PosterBoard.log
newest /var/mobile/Library/Logs/CrashReporter Preferences /var/tmp/sweetcrash.Preferences.log
newest /var/mobile/Library/Logs/CrashReporter configd /var/tmp/sweetcrash.configd.log

ls /var/sweet/m2v-cache/backboardd.orig
ls /var/sweet/m2v-cache/backboardd.orig/mesa
