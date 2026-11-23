# sweetroot command file for the Mapple test (tools/mapple/README.md). Log: /var/tmp/sweetroot.log
# Runs at every boot while this file is on the phone; the "back" drop replaces it with one that does nothing.
apply

# which driver Metal loads: Info.plist's CFBundleExecutable (SweetMapple = Mapple, SweetMTLDriver = metal2)
ls /System/Library/Extensions/SweetMTLDriver.bundle
cat /var/sweet/metal.allow

# loads the bundle by hand, makes Mapple's device directly, then asks Metal for the system device
run /usr/local/bin/vmprobe /System/Library/Extensions/SweetMTLDriver.bundle
