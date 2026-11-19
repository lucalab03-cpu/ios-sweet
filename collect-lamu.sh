#!/bin/bash
# Collects, read-only, what Open Mapple's porting needs from a running Moto G05 (lamu) into lamu-info/ and
# lamu-info.tar.gz. Needs Android with root (Magisk) or LineageOS with "adb root". Nothing on the phone is changed
# except a temporary folder in /data/local/tmp, removed at the end.
set -e
command -v dtc >/dev/null || { echo "install dtc first: sudo apt install device-tree-compiler"; exit 1; }
T=/data/local/tmp/lamu-info
if [ "$(adb shell id -u | tr -d '\r')" = 0 ]; then RUN="sh"; else RUN="su"; fi
echo "root via: $RUN"

adb shell "$RUN" <<EOF2
set +e
rm -rf $T; mkdir -p $T; cd $T
cat /sys/firmware/fdt > fdt.dtb
for f in iomem interrupts cpuinfo cmdline bootconfig partitions meminfo version modules; do cat /proc/\$f > proc-\$f.txt 2>&1; done
for f in /proc/devinfo/*; do echo "== \$f"; cat \$f; done > devinfo.txt 2>&1
getprop > getprop.txt
ls -l /dev/block/by-name/ > by-name.txt 2>&1
ls -l /dev/block/platform/*/by-name/ >> by-name.txt 2>&1
ls /sys/block > sys-block.txt
UD=\$(readlink -f /dev/block/by-name/userdata); echo "userdata -> \$UD" > disk.txt
DISK=\$(echo \$UD | sed -E 's/p?[0-9]+\$//'); echo "disk -> \$DISK" >> disk.txt
dd if=\$DISK of=gpt-head.bin bs=4096 count=16 2>> disk.txt
cat /sys/block/\$(basename \$DISK)/size >> disk.txt 2>&1
cat /sys/block/\$(basename \$DISK)/queue/logical_block_size >> disk.txt 2>&1
dmesg > dmesg.txt 2>&1
getevent -pl > getevent.txt 2>&1 < /dev/null
for d in /sys/bus/i2c/devices/* /sys/bus/spi/devices/* /sys/bus/platform/devices/*; do
  [ -e \$d/name ] && echo "\$d \$(cat \$d/name)"; [ -e \$d/modalias ] && echo "\$d \$(cat \$d/modalias)"
done > buses.txt 2>&1
for d in /sys/class/misc/mali0/device /sys/kernel/ged /proc/gpufreqv2 /proc/gpufreq; do echo "== \$d"; ls \$d; for f in \$d/*; do [ -f \$f ] && echo "-- \$f" && head -c 2000 \$f 2>/dev/null; done; done > gpu.txt 2>&1
wm size > display.txt 2>&1; wm density >> display.txt 2>&1
dumpsys display | grep -iE 'mDisplayInfo|DisplayDeviceInfo|refresh|panel' | head -40 >> display.txt 2>&1
ls -lR /vendor/firmware /odm/firmware /vendor/etc/firmware > firmware-list.txt 2>&1
for f in /sys/class/power_supply/*/uevent; do echo "== \$f"; cat \$f; done > power.txt 2>&1
chmod -R a+r $T
EOF2

rm -rf lamu-info; mkdir lamu-info
adb pull "$T/." lamu-info/ >/dev/null
adb shell "$RUN" -c "rm -rf $T" 2>/dev/null || echo "rm -rf $T" | adb shell "$RUN"
dtc -q -I dtb -O dts -o lamu-info/live.dts lamu-info/fdt.dtb
tar czf lamu-info.tar.gz lamu-info
ls -l lamu-info; echo; echo "ready: lamu-info.tar.gz"
