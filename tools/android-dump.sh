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

# Each step says its name and has a time limit: some debugfs/sysfs files and dumpsys services can block forever
# (2026-11-15: the first run sat still for minutes, with no way to tell where).
grab() { echo "dump: $2"; timeout 10 cat "$1" > "$O/$2" 2>/dev/null; }
run() { echo "dump: $2"; timeout 30 sh -c "$1" > "$O/$2" 2>&1; }

# device tree and kernel
echo "dump: fdt.dtb"; cp /sys/firmware/fdt $O/fdt.dtb 2>/dev/null
grab /proc/iomem iomem.txt
grab /proc/interrupts interrupts.txt
grab /proc/cpuinfo cpuinfo.txt
grab /proc/cmdline cmdline.txt
grab /proc/bootconfig bootconfig.txt
grab /proc/modules modules.txt
grab /proc/meminfo meminfo.txt
grab /proc/version version.txt
grab /proc/config.gz config.gz
grab /proc/bus/input/devices input-devices.txt
grab /proc/asound/cards asound-cards.txt
grab /proc/asound/pcm asound-pcm.txt
run "dmesg" dmesg.txt
run "uname -a" uname.txt
run "getprop" getprop.txt
run "ls -l /dev/block/by-name /dev/block/platform/*/by-name" partitions.txt

# kernel debug views: only with "all" as the first argument. On the lamu (2026-11-15) the first full run rebooted
# Android: MediaTek kernels can crash reading debugfs views of clocks, regulators, GPIOs or pins that touch
# powered-down blocks.
if [ "$1" = all ]; then
	grab $D/clk/clk_summary clk_summary.txt
	grab $D/regulator/regulator_summary regulator_summary.txt
	grab $D/gpio gpio.txt
	run "for f in $D/pinctrl/*/pinmux-pins; do echo \"== \$f\"; cat \$f; done" pinctrl.txt
fi
grab $D/memblock/memory memblock-memory.txt
grab $D/memblock/reserved memblock-reserved.txt

# sysfs: one uevent / attribute file per device, by class
for cls in leds backlight power_supply thermal regulator input sound net drm graphics; do
	for d in /sys/class/$cls/*; do
		[ -e "$d" ] || continue
		n=$O/sys/$cls-${d##*/}.txt
		echo "dump: sysfs $cls ${d##*/}"
		{
			echo "== $d -> $(readlink -f $d)"
			# uevent and plain attributes only: reading every attribute can reach hardware that is off (a reboot)
			for a in uevent name type max_brightness brightness actual_brightness bl_power status present online health technology capacity voltage_now current_now charge_full charge_full_design temp mode microvolts state; do
				[ -f "$d/$a" ] || continue
				printf '%s: ' "$a"; timeout 2 head -c 400 "$d/$a" 2>/dev/null | tr '\n' ' '; echo
			done
		} > "$n" 2>/dev/null
	done
done
run "for z in /sys/class/thermal/thermal_zone*; do echo \"\$z \$(cat \$z/type) \$(cat \$z/temp)\"; done" thermal.txt
run "ls -l /sys/bus/platform/devices/" platform-devices.txt
run "for d in /sys/bus/i2c/devices/*; do echo \"\$d \$(cat \$d/name 2>/dev/null) driver=\$(basename \$(readlink \$d/driver) 2>/dev/null)\"; done" i2c-devices.txt
run "for d in /sys/bus/spi/devices/*; do echo \"\$d \$(cat \$d/modalias 2>/dev/null)\"; done" spi-devices.txt
run "for d in /sys/bus/platform/drivers/*; do echo \"== \${d##*/}\"; ls \$d | grep -v -E '^(bind|unbind|uevent|module)\$'; done" platform-drivers.txt

# Android's dumps
run "dumpsys display" dumpsys-display.txt
run "dumpsys SurfaceFlinger" dumpsys-surfaceflinger.txt
run "dumpsys power" dumpsys-power.txt
run "dumpsys battery" dumpsys-battery.txt
run "dumpsys sensorservice" dumpsys-sensors.txt
run "dumpsys media.audio_flinger" dumpsys-audio.txt
run "dumpsys wifi | head -300" dumpsys-wifi.txt
run "dumpsys telephony.registry | head -200" dumpsys-telephony.txt
run "dumpsys input" dumpsys-input.txt
run "ip addr; ip link" net.txt
run "logcat -d -b all -t 20000" logcat.txt

# vendor: modules (the .ko binaries: display, panel, backlight, charger, audio, Wi-Fi drivers to read), firmware, configs
echo "dump: vendor modules, firmware list and configs"
for m in /vendor_dlkm/lib/modules /vendor/lib/modules /system_dlkm/lib/modules; do
	[ -d $m ] && cp $m/*.ko $m/modules.* $O/modules/ 2>/dev/null
done
run "ls -lR /vendor/firmware /vendor/firmware_mnt /vendor/etc/firmware 2>/dev/null" firmware-list.txt
for f in /vendor/etc/*.xml /vendor/etc/*.conf /vendor/etc/*.cfg /vendor/etc/*.ini /vendor/etc/audio/* /vendor/etc/wifi/* /vendor/etc/init/*.rc /vendor/etc/init/hw/*.rc; do
	[ -f "$f" ] && cp "$f" $O/vendor-etc/ 2>/dev/null
done

echo "dump: packing"
cd /data/local/tmp && tar czf sweetdump.tgz sweetdump && rm -rf sweetdump
ls -l /data/local/tmp/sweetdump.tgz
