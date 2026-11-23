# Which processes get the Metal device (tools/metal2 driver, /var/sweet/metal.allow)

Two one-file dropbox packages, each a directory to pass to `pack-dropbox.py`:

- `apps/` (point 3 of the speed plan): backboardd and PosterBoard as now, plus SpringBoard, Settings
  (`Preferences`), Safari and WebKit's processes. For after the job queue (kernel patch 0135) has been seen to work:
  more programs drawing with the GPU means more load on it.
- `base/`: the list the pbwrap-metal2 drop put on the phone (backboardd, backboardd.orig, PosterBoard, metaltest,
  mtltest), to go back.

The driver reads the file when a process first asks for a device, so a change counts from the next start of each
process (the next boot, for SpringBoard and backboardd).
