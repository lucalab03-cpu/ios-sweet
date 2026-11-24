# Which processes get the Metal device (tools/metal2 driver, /var/sweet/metal.allow)

Two one-file dropbox packages, each a directory to pass to `pack-dropbox.py`:

- `apps/`: backboardd and PosterBoard as now, plus the apps (Settings `Preferences`, Messages `MobileSMS`, Safari and
  WebKit's processes, Photos, Notes, Music...). Not SpringBoard. Why (2026-11-24): Messages and Settings crash at
  launch with a call through NULL inside **CoreImage**, called from MobileIcons -- they render icons with CoreImage,
  which wants a Metal device, and the gate gave them none. The sibling OnePlus 9 Pro port gives every app Metal
  (`metal=2`: all but the window server).
- `base/`: the list the pbwrap-metal2 drop put on the phone (backboardd, backboardd.orig, PosterBoard, metaltest,
  mtltest), to go back.

The driver reads the file when a process first asks for a device, so a change counts from the next start of each
process (the next boot, for SpringBoard and backboardd).
