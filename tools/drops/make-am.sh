#!/bin/sh
# usage: make-am.sh BUNDLE.zip -> payload-am.bin (payload-al + kernel with 0169 tear-free copy, 0170 render thread on CPU 6)
set -e
B=$1; IN=/home/claude/in/am; OUT=/home/claude/out
rm -rf $IN && mkdir -p $IN && unzip -q -o "$B" -d $IN
(cd $IN && sha256sum -c SHA256SUMS >/dev/null && echo "bundle sums ok")
cat $IN/RUNS.txt
K=$IN/kernel/kernel.development.sweet
cd /home/claude/ios-sweet/tools && python3 repayload.py $OUT/payload-al.bin $K $OUT/payload-am.bin --expect "0169 disp_ovl0" --expect "SWEET BIG" > /tmp/am.txt; cut -c1-160 /tmp/am.txt
ls -l $OUT/payload-am.bin
