# sweetroot command file (tools/pbwrap/root.cmd). Delivered by the dropbox (/var/sweet/root.cmd), applied at boot by
# /usr/local/bin/sweetroot; the first operation must be "apply". Log: /var/tmp/sweetroot.log
#
# 2026-11-24 round "o" (payload-o: 0148 verified DMA writes, cached panel mapping).
apply

# where the poster extensions are, by name (directory entries only, 8 levels) -> /var/tmp/sweetfind.txt
run /usr/local/bin/sweetfind /var/tmp/sweetfind.txt Poster /System /Applications /private/var/staged_system_apps
run /usr/local/bin/sweetfind /var/tmp/sweetfind.txt ExtensionKit /System

newest /var/mobile/Library/Logs/CrashReporter PosterBoard /var/tmp/sweetcrash.PosterBoard.log
ls /var/sweet/m2v-cache/backboardd.orig/mesa
