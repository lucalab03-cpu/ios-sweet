# sweetroot command file (tools/pbwrap/root.cmd). Delivered by the dropbox (/var/sweet/root.cmd), applied at boot by
# /usr/local/bin/sweetroot; the first operation must be "apply". Log: /var/tmp/sweetroot.log
#
# 2026-11-25 round "w" (CoreAnimation on OpenGL ES through the untouched backboardd): the Gallium tests on the new Mesa.
apply

run /usr/local/bin/gltest
run /usr/local/bin/drawtest
