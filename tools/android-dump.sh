#!/system/bin/sh
# android-dump.sh: everything the iOS port needs to know about the hardware, collected from Lineage on the phone.
#
#   adb root
#   adb push tools/android-dump.sh /data/local/tmp/   (or paste it with: adb shell 'cat > /data/local/tmp/android-dump.sh')
#   adb shell sh /data/local/tmp/android-dump.sh
#   adb pull /data/local/tmp/sweetdump.tgz
#
# The device tree as the kernel booted it (fdt.dtb: every block's registers, interrupts, clocks, GPIOs, regulators,
# reserved memory), the kernel's own view (iomem, interrupts, clocks, regulators, GPIO/pin state, modules, dmesg,
# config), each subsystem's live state in sysfs (display/backlight, battery/charger, thermal, input, audio, net,
# sensors), the vendor modules and firmware lists, and Android's own dumps for display, power, sensors, audio, Wi-Fi.
# Nothing is changed on the phone; the archive lands in /data/local/tmp/sweetdump.tgz.
O=/data/local/tmp/sweetdump
rm -rf $O $O.tgz
mkdir -p $O/sys $O/modules $O/vendor-etc
mount -t debugfs none /sys/kernel/debug 2>/dev/null
D=/sys/kernel/debug

c() { cat "$1" > "$O/$2" 2>/dev/null; }
r() { sh -c "$1" > "$O/$2" 2>&1; }

# device tree and kernel
cp /sys/firmware/fdt $O/fdt.dtb 2>/dev/null
c /proc/iomem iomem.txt
c /proc/interrupts interrupts.txt
c /proc/cpuinfo cpuinfo.txt
c /proc/cmdline cmdline.txt
c /proc/bootconfig bootconfig.txt
c /proc/modules modules.txt
c /proc/meminfo meminfo.txt
c /proc/version version.txt
c /proc/config.gz config.gz
c /proc/bus/input/devices input-devices.txt
c /proc/asound/cards asound-cards.txt
c /proc/asound/pcm asound-pcm.txt
r "dmesg" dmesg.txt
r "uname -a" uname.txt
r "getprop" getprop.txt
r "ls -l /dev/block/by-name /dev/block/platform/*/by-name" partitions.txt

# kernel debug views (clocks, regulators, GPIO and pin muxing, memory, display)
c $D/clk/clk_summary clk_summary.txt
c $D/regulator/regulator_summary regulator_summary.txt
c $D/gpio gpio.txt
r "for f in $D/pinctrl/*/pinmux-pins $D/pinctrl/*/pins; do echo \"== \$f\"; cat \$f; done" pinctrl.txt
c $D/memblock/memory memblock-memory.txt
c $D/memblock/reserved memblock-reserved.txt
r "ls -R $D/dri $D/mtkfb $D/disp* 2>/dev/null" debug-display-list.txt

# sysfs: one uevent / attribute file per device, by class
for cls in leds backlight power_supply thermal regulator input sound net drm graphics misc; do
	for d in /sys/class/$cls/*; do
		[ -e "$d" ] || continue
		n=$O/sys/$cls-${d##*/}.txt
		{
			echo "== $d -> $(readlink -f $d)"
			for f in "$d"/*; do
				[ -f "$f" ] && [ -r "$f" ] || continue
				case "${f##*/}" in descriptors|config|rom|edid|*_bin|data) continue;; esac
				printf '%s: ' "${f##*/}"; head -c 400 "$f" 2>/dev/null | tr '\n' ' '; echo
			done
		} > "$n" 2>/dev/null
	done
done
r "for z in /sys/class/thermal/thermal_zone*; do echo \"\$z \$(cat \$z/type) \$(cat \$z/temp)\"; done" thermal.txt
r "ls -l /sys/bus/platform/devices/" platform-devices.txt
r "for d in /sys/bus/i2c/devices/*; do echo \"\$d \$(cat \$d/name 2>/dev/null) driver=\$(basename \$(readlink \$d/driver) 2>/dev/null)\"; done" i2c-devices.txt
r "for d in /sys/bus/spi/devices/*; do echo \"\$d \$(cat \$d/modalias 2>/dev/null)\"; done" spi-devices.txt
r "for d in /sys/bus/platform/drivers/*; do echo \"== \${d##*/}\"; ls \$d | grep -v -E '^(bind|unbind|uevent|module)\$'; done" platform-drivers.txt

# Android's dumps
r "dumpsys display" dumpsys-display.txt
r "dumpsys SurfaceFlinger" dumpsys-surfaceflinger.txt
r "dumpsys power" dumpsys-power.txt
r "dumpsys battery" dumpsys-battery.txt
r "dumpsys sensorservice" dumpsys-sensors.txt
r "dumpsys media.audio_flinger" dumpsys-audio.txt
r "dumpsys wifi | head -300" dumpsys-wifi.txt
r "dumpsys telephony.registry | head -200" dumpsys-telephony.txt
r "dumpsys input" dumpsys-input.txt
r "ip addr; ip link" net.txt
r "logcat -d -b all -t 20000" logcat.txt

# vendor: modules (the .ko binaries: display, panel, backlight, charger, audio, Wi-Fi drivers to read), firmware, configs
for m in /vendor_dlkm/lib/modules /vendor/lib/modules /system_dlkm/lib/modules; do
	[ -d $m ] && cp $m/*.ko $m/modules.* $O/modules/ 2>/dev/null
done
r "ls -lR /vendor/firmware /vendor/firmware_mnt /vendor/etc/firmware 2>/dev/null" firmware-list.txt
for f in /vendor/etc/*.xml /vendor/etc/*.conf /vendor/etc/*.cfg /vendor/etc/*.ini /vendor/etc/audio/* /vendor/etc/wifi/* /vendor/etc/init/*.rc /vendor/etc/init/hw/*.rc; do
	[ -f "$f" ] && cp "$f" $O/vendor-etc/ 2>/dev/null
done

cd /data/local/tmp && tar czf sweetdump.tgz sweetdump && rm -rf sweetdump
ls -l /data/local/tmp/sweetdump.tgz
