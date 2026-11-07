#!/bin/sh
# kur.sh ile kurulanları kaldırır. PSK, kalibrasyon ve kayıtlı parmak izleri korunur.
# Normal kullanıcı olarak çalıştırın; sudo ister.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
VERSION=0.1.0

sudo -v || exit 1
sudo systemctl stop fprintd 2>/dev/null || true

if [ -f "$ROOT/build/libfprint/build.ninja" ]; then
  sudo ninja -C "$ROOT/build/libfprint" uninstall >/dev/null
else
  sudo rm -f /usr/local/lib/x86_64-linux-gnu/libfprint-2.so*
fi
sudo ldconfig

sudo modprobe -r gxfp 2>/dev/null || true
sudo dkms remove -m gxfp -v "$VERSION" --all >/dev/null 2>&1 || true
sudo rm -rf "/usr/src/gxfp-$VERSION"
sudo rm -f /usr/local/bin/gxfp_capture /usr/local/bin/gxfp_psk_tool /usr/local/bin/gxfp_recovery
sudo rm -f /etc/udev/rules.d/60-gxfp.rules
sudo rm -f /etc/systemd/system/fprintd.service.d/gxfp.conf
sudo rm -f /etc/systemd/system/fprintd.service.d/gxfp-fprintd.conf /usr/local/libexec/fprintd
sudo udevadm control --reload-rules
sudo systemctl daemon-reload

# pam-ayar.sh ile eklenen PAM profilini geri al
if [ -f /usr/share/pam-configs/fprintd-gxfp ]; then
  sudo pam-auth-update --disable fprintd-gxfp
  sudo rm -f /usr/share/pam-configs/fprintd-gxfp
fi

echo "Kaldırıldı. Sistem Ubuntu'nun kendi libfprint paketine döndü."
echo "Korunanlar: /var/lib/fprintd/gxfp (PSK, kalibrasyon), /var/lib/fprint (parmak izleri)."
echo "Parmak izi girişini kapatmak için: sudo pam-auth-update --disable fprintd"
