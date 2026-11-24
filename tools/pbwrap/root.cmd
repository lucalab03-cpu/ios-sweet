# sweetroot command file (tools/pbwrap/root.cmd). Delivered by the dropbox (/var/sweet/root.cmd), applied at boot by
# /usr/local/bin/sweetroot; the first operation must be "apply". Log: /var/tmp/sweetroot.log
#
# 2026-11-24 round "k" (payload-k: profiler 0144, PanVK shader cache). Round i's disk-wide entitlement scan is gone:
# it read every Mach-O under /System/Library for up to 300 s at boot and slowed the phone down (boot of 18:56).
apply

# Wallpaper try (from round i): PosterBoard back to its own, signed binary. The pbwrap wrapper carries no
# entitlements; if RunningBoard checks the program launchd started, the prewarm assertion was refused because of it.
# PosterBoard.orig and .inj stay for later rounds.
cp /Applications/PosterBoard.app/PosterBoard.orig /Applications/PosterBoard.app/PosterBoard
chmod 755 /Applications/PosterBoard.app/PosterBoard
ls /Applications/PosterBoard.app

# who carries com.apple.runningboard.posterkit.host: the four programs only (no scan) -> /var/tmp/sweetents.txt
run /usr/local/bin/sweetents -o /var/tmp/sweetents.txt /Applications/PosterBoard.app/PosterBoard.orig /Applications/PosterBoard.app/PosterBoard.inj /usr/local/libexec/pbwrap /System/Library/CoreServices/SpringBoard.app/SpringBoard

# names for Settings > About's main thread (boot of 17:35): /var/tmp/sweetsym.txt
run /usr/local/bin/sweetsym /var/sweet/sym.in /var/tmp/sweetsym.txt

newest /var/mobile/Library/Logs/CrashReporter PosterBoard /var/tmp/sweetcrash.PosterBoard.log
newest /var/mobile/Library/Logs/CrashReporter Preferences /var/tmp/sweetcrash.Preferences.log
newest /var/mobile/Library/Logs/CrashReporter runningboardd /var/tmp/sweetcrash.runningboardd.log
newest /var/mobile/Library/Logs/CrashReporter backboardd /var/tmp/sweetcrash.backboardd.log

# the shader caches: metal2vulkan's, and PanVK's new one (mesa/) after the first boot with it
ls /var/sweet/m2v-cache
ls /var/sweet/m2v-cache/backboardd.orig
ls /var/sweet/m2v-cache/backboardd.orig/mesa
