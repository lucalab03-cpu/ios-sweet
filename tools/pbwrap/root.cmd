# sweetroot command file (tools/pbwrap/root.cmd). Delivered by the dropbox (/var/sweet/root.cmd), applied at boot by
# /usr/local/bin/sweetroot; the first operation must be "apply". Log: /var/tmp/sweetroot.log
#
# 2026-11-24 round "i": PosterBoard asserts !_stateLock_switcherConfiguration after RunningBoard invalidated its
# prewarm assertion, "originator doesn't have entitlement com.apple.runningboard.posterkit.host". Who has it, and
# what does the running PosterBoard (the pbwrap wrapper, then PosterBoard.inj) carry? -> /var/tmp/sweetents.txt
apply

run /usr/local/bin/sweetents -o /var/tmp/sweetents.txt /Applications/PosterBoard.app/PosterBoard.orig /Applications/PosterBoard.app/PosterBoard.inj /Applications/PosterBoard.app/PosterBoard /System/Library/CoreServices/SpringBoard.app/SpringBoard
run /usr/local/bin/sweetents -o /var/tmp/sweetents.txt -s com.apple.runningboard.posterkit.host /System/Library/CoreServices /Applications /usr/libexec /System/Library/PrivateFrameworks/PosterBoardServices.framework /System/Library/PrivateFrameworks/PosterKit.framework /System/Library/PrivateFrameworks/PosterFoundation.framework

# names for the addresses of the last boot (Settings > About's main thread): /var/tmp/sweetsym.txt
run /usr/local/bin/sweetsym /var/sweet/sym.in /var/tmp/sweetsym.txt

newest /var/mobile/Library/Logs/CrashReporter PosterBoard /var/tmp/sweetcrash.PosterBoard.log
newest /var/mobile/Library/Logs/CrashReporter mediaserverd /var/tmp/sweetcrash.mediaserverd.log

# PosterBoard with the tracing dylib (each step leaves what an earlier boot made)
cp -n /Applications/PosterBoard.app/PosterBoard /Applications/PosterBoard.app/PosterBoard.orig
cp -n /Applications/PosterBoard.app/PosterBoard.orig /Applications/PosterBoard.app/PosterBoard.inj
run /usr/local/bin/install-wrap --patch /Applications/PosterBoard.app/PosterBoard.inj /usr/lib/sweetposter.dylib
run /usr/local/bin/install-wrap --verify /Applications/PosterBoard.app/PosterBoard.inj
cp /usr/local/libexec/pbwrap /Applications/PosterBoard.app/PosterBoard
chmod 755 /Applications/PosterBoard.app/PosterBoard
