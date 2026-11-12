#!/bin/bash
# Prepara e scrive nella dropbox la prova del guscio di backboardd. Un comando solo, dalla VM,
# con il telefono in Android e `adb root` gia' fatto.
#
#   bash install-bbprobe.sh IMMAGINE CARTELLA_ARTEFATTO dropbox.map.json pack-dropbox.py
#
# IMMAGINE: la copia della root iOS nella VM (es. p56-gl2.img). Serve solo in lettura, per prendere
#           il backboardd originale.
# CARTELLA_ARTEFATTO: lo zip "bbprobe" della build estratto (bbshim, libbbprobe.dylib).
#
# Crea anche ./bbprobe-undo: la cartella per tornare al backboardd originale
#   python3 pack-dropbox.py dropbox.map.json bbprobe-undo
set -e
IMG=${1:?immagine}; ART=${2:?cartella artefatto}; MAP=${3:?dropbox.map.json}; PACK=${4:?pack-dropbox.py}
for f in "$IMG" "$ART/bbshim" "$ART/libbbprobe.dylib" "$MAP" "$PACK"; do
  [ -f "$f" ] || { echo "manca $f"; exit 1; }
done

WORK=$(pwd)
MNT=$(mktemp -d)
sudo mount -t hfsplus -o ro,force,loop "$IMG" "$MNT"
trap 'sudo umount "$MNT" 2>/dev/null; rmdir "$MNT" 2>/dev/null' EXIT
ORIG="$MNT/usr/libexec/backboardd"
[ -f "$ORIG" ] || { echo "backboardd non trovato nell'immagine"; exit 1; }
if grep -q "backboardd.real" "$ORIG"; then
  echo "il backboardd nell'immagine e' gia' il guscio: serve l'originale"; exit 1
fi
cp "$ORIG" "$WORK/backboardd.orig"
echo "permessi di /var/tmp nell'immagine (servono rwxrwxrwt):"
ls -ld "$MNT/private/var/tmp" "$MNT/var/tmp" 2>&1 || true
sudo umount "$MNT"; rmdir "$MNT"; trap - EXIT

rm -rf "$WORK/bbprobe-drop" "$WORK/bbprobe-undo"
mkdir -p "$WORK/bbprobe-drop/usr/libexec" "$WORK/bbprobe-drop/usr/local/lib" "$WORK/bbprobe-undo/usr/libexec"
install -m 755 "$WORK/backboardd.orig" "$WORK/bbprobe-drop/usr/libexec/backboardd.real"
install -m 755 "$ART/bbshim"           "$WORK/bbprobe-drop/usr/libexec/backboardd"
install -m 755 "$ART/libbbprobe.dylib" "$WORK/bbprobe-drop/usr/local/lib/libbbprobe.dylib"
install -m 755 "$WORK/backboardd.orig" "$WORK/bbprobe-undo/usr/libexec/backboardd"
find "$WORK/bbprobe-drop" "$WORK/bbprobe-undo" -type f -exec ls -l {} +

adb shell id | grep -q 'uid=0' || { echo "adb non e' root: fai 'adb root' e riprova"; exit 1; }
python3 "$PACK" "$MAP" "$WORK/bbprobe-drop"
echo
echo "FATTO. Riavvia in iOS DUE volte (il primo avvio installa i file, dal secondo parte il guscio)."
echo "Per annullare: python3 $PACK $MAP $WORK/bbprobe-undo"
