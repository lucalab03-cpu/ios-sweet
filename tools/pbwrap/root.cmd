# sweetroot command file (tools/pbwrap/root.cmd). Delivered by the dropbox (/var/sweet/root.cmd), applied at boot by
# /usr/local/bin/sweetroot; the first operation must be "apply". Log: /var/tmp/sweetroot.log
#
# 2026-11-25 round "r" (device answers cached, chains per commit, 0152 dropped).
apply

# names for backboardd's shared-cache pcs of the 10:55 boot (q) not named before -> /var/tmp/sweetsym-q.txt
run /usr/local/bin/sweetsym /var/sweet/sym-q.in /var/tmp/sweetsym-q.txt
