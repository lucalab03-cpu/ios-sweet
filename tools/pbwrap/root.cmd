# sweetroot command file: gives PosterBoard the tracing dylib, once.
#
# Delivered by the dropbox (/var/sweet/root.cmd) and applied at boot by /usr/local/bin/sweetroot. The first
# operation must be "apply": without it sweetroot only prints what it would do. Log: /var/tmp/sweetroot.log
apply

# the untouched binary, kept once (cp -n never overwrites what an earlier boot made)
cp -n /Applications/PosterBoard.app/PosterBoard /Applications/PosterBoard.app/PosterBoard.orig

# the copy that install-wrap adds LC_LOAD_DYLIB to; it also fixes the first page's code signature hash
cp -n /Applications/PosterBoard.app/PosterBoard.orig /Applications/PosterBoard.app/PosterBoard.inj
run /usr/local/bin/install-wrap --patch /Applications/PosterBoard.app/PosterBoard.inj /usr/lib/sweetposter.dylib

# the wrapper takes the place of the real binary and starts .inj (after two quick crashes: .orig)
cp /usr/local/libexec/pbwrap /Applications/PosterBoard.app/PosterBoard
chmod 755 /Applications/PosterBoard.app/PosterBoard
ls /Applications/PosterBoard.app

# one relaunch now, through the wrapper, instead of after this launch's crash
run /usr/bin/killall PosterBoard

# To go back (a later root.cmd, or the same file with these two lines uncommented):
# cp /Applications/PosterBoard.app/PosterBoard.orig /Applications/PosterBoard.app/PosterBoard
# run /usr/bin/killall PosterBoard
