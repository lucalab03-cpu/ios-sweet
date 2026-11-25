# sweetroot command file (tools/pbwrap/root.cmd). Delivered by the dropbox (/var/sweet/root.cmd), applied at boot by
# /usr/local/bin/sweetroot; the first operation must be "apply". Log: /var/tmp/sweetroot.log
#
# 2026-11-25 round "q" (0151 self-coherent Metal client, 0152 branchless blit).
apply

# names for backboardd's hottest shared-cache pcs of the 00:16 (o) and 00:50 (p) boots -> /var/tmp/sweetsym.txt
run /usr/local/bin/sweetsym /var/sweet/sym-o.in /var/tmp/sweetsym.txt
run /usr/local/bin/sweetsym /var/sweet/sym-p.in /var/tmp/sweetsym-p.txt
