#!/bin/sh
# sudo, su ve yetki pencereleri (polkit) için parmak izi deneme sayısını ayarlar.
# Ubuntu'nun fprintd profili tek deneme verir; bu betik aynı profilin
# değiştirilmiş bir kopyasını ekleyip onun yerine etkinleştirir.
#   sh pam-ayar.sh       3 deneme (varsayılan)
#   sh pam-ayar.sh 2     2 deneme
# Giriş/kilit ekranı (gdm-fingerprint) bundan etkilenmez; orada zaten 3 deneme var.
set -eu

TRIES=${1:-3}
PROFILE=/usr/share/pam-configs/fprintd-gxfp

case "$TRIES" in
  [1-5]) ;;
  *) echo "Deneme sayısı 1-5 arası olmalı" >&2; exit 2 ;;
esac

sudo -v
sudo tee "$PROFILE" >/dev/null <<EOF
Name: Fingerprint authentication (GXFP5130, $TRIES tries)
Default: no
Priority: 260
Conflicts: fprint, fprintd
Auth-Type: Primary
Auth:
	[success=end default=ignore]	pam_fprintd.so max-tries=$TRIES timeout=10
EOF
sudo pam-auth-update --disable fprintd
sudo pam-auth-update --enable fprintd-gxfp

echo "common-auth içindeki parmak izi satırı:"
grep pam_fprintd /etc/pam.d/common-auth
