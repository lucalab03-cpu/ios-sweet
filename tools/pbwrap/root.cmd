# sweetroot command file (tools/pbwrap/root.cmd). Delivered by the dropbox (/var/sweet/root.cmd), applied at boot by
# /usr/local/bin/sweetroot; the first operation must be "apply". Log: /var/tmp/sweetroot.log
#
# 2026-11-25 round "p" (payload-p: 0149 row buffer, 0150 per-target kernel pcs; sweet-boot with a streamed outbox).
apply

# names for backboardd's hottest shared-cache pcs of the 00:16 boot (slide 0x1b248000) -> /var/tmp/sweetsym.txt
run /usr/local/bin/sweetsym /var/sweet/sym-o.in /var/tmp/sweetsym.txt

# where the poster extensions are (round o's run was not fetched: sweet-boot was killed before the outbox)
run /usr/local/bin/sweetfind /var/tmp/sweetfind.txt Poster /System /Applications /private/var/staged_system_apps
run /usr/local/bin/sweetfind /var/tmp/sweetfind.txt ExtensionKit /System

newest /var/mobile/Library/Logs/CrashReporter PosterBoard /var/tmp/sweetcrash.PosterBoard.log
