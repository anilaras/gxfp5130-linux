#!/bin/sh
# GXFP5130 sürücüsünü (DKMS çekirdek modülü + Chicago eşleştiricili libfprint) kurar.
# Normal kullanıcı olarak çalıştırın; root gereken adımlarda sudo ister.
#   sh kur.sh               derle ve kur
#   sh kur.sh --calib-yenile  calib/goodix_calib.dat dosyasını sistemdekinin üzerine yaz
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
VERSION=0.1.0
STATE=/var/lib/fprintd/gxfp
REPLACE_CALIB=0
[ "${1:-}" = "--calib-yenile" ] && REPLACE_CALIB=1

missing=""
for c in make cmake meson ninja dkms pkg-config msgfmt git; do
  command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
done
for m in mbedtls glib-2.0 gusb pixman-1 nss gudev-1.0 cairo opencv4 polkit-gobject-1 libsystemd dbus-1; do
  pkg-config --exists "$m" 2>/dev/null || missing="$missing $m"
done
if [ -n "$missing" ]; then
  echo "Eksik bağımlılıklar:$missing" >&2
  echo "Ubuntu: sudo apt install build-essential dkms cmake meson ninja-build pkg-config \\" >&2
  echo "  libmbedtls-dev libglib2.0-dev libgusb-dev libpixman-1-dev libnss3-dev \\" >&2
  echo "  libgudev-1.0-dev libcairo2-dev libopencv-dev libpolkit-gobject-1-dev \\" >&2
  echo "  libsystemd-dev libdbus-1-dev gettext linux-headers-\$(uname -r)" >&2
  exit 1
fi
if [ ! -f "$ROOT/calib/goodix_calib.dat" ] && ! sudo test -f "$STATE/goodix_calib.dat"; then
  echo "Kalibrasyon dosyası yok: calib/goodix_calib.dat (bkz. calib/README.md)" >&2
  exit 1
fi
[ -d "/lib/modules/$(uname -r)/build" ] || {
  echo "Çalışan çekirdeğin başlık dosyaları yok: linux-headers-$(uname -r)" >&2
  exit 1
}

mkdir -p "$ROOT/build"
LOG="$ROOT/build/kur.log"
: >"$LOG"
# Komutu sessizce çalıştırır; başarısız olursa logun sonunu gösterip çıkar.
run() {
  if ! "$@" >>"$LOG" 2>&1; then
    echo "HATA: $*" >&2
    tail -n 25 "$LOG" >&2
    echo "Tam log: $LOG" >&2
    exit 1
  fi
}

echo "== Derleniyor =="
run cmake -S "$ROOT/userspace" -B "$ROOT/build/userspace" -DCMAKE_BUILD_TYPE=Release
run cmake --build "$ROOT/build/userspace" -j"$(nproc)"
if [ ! -f "$ROOT/build/libfprint/build.ninja" ]; then
  run meson setup "$ROOT/build/libfprint" "$ROOT/libfprint" --prefix=/usr/local \
    -Ddrivers=gxfp -Ddoc=false -Dintrospection=false -Dgtk-examples=false \
    -Dudev_rules=disabled -Dudev_hwdb=disabled
fi
run meson compile -C "$ROOT/build/libfprint"

# fprintd, öğrenen şablonu diske yazan yamayla (fprintd-1.94.5-print-persist.patch).
# Kaynak yoksa resmi v1.94.5 indirilip yama uygulanır. Ubuntu'nun derlemesiyle aynı
# yollar (/etc, /var/lib/fprint) gömülsün diye /usr önekiyle yapılandırılır;
# yalnızca çalıştırılabilir dosya /usr/local/libexec altına kopyalanır.
FPRINTD_SRC="$ROOT/fprintd"
if [ ! -d "$FPRINTD_SRC" ]; then
  FPRINTD_SRC="$ROOT/build/fprintd-src"
  if [ ! -d "$FPRINTD_SRC" ]; then
    run git clone --depth 1 -b v1.94.5 https://gitlab.freedesktop.org/libfprint/fprintd.git "$FPRINTD_SRC"
    run git -C "$FPRINTD_SRC" apply "$ROOT/fprintd-1.94.5-print-persist.patch"
  fi
fi
if [ ! -f "$ROOT/build/fprintd/build.ninja" ]; then
  run env PKG_CONFIG_PATH="$ROOT/build/libfprint/meson-uninstalled" \
    meson setup "$ROOT/build/fprintd" "$FPRINTD_SRC" --prefix=/usr --sysconfdir=/etc \
    --localstatedir=/var -Dpam=false -Dman=false -Dsystemd=false
fi
run meson compile -C "$ROOT/build/fprintd"
# Kurulum, derleme dizinine işaret eden RPATH'i siler; root daemon ev dizininden
# kütüphane yüklememeli.
rm -rf "$ROOT/build/fprintd-stage"
run meson install -C "$ROOT/build/fprintd" --no-rebuild --destdir "$ROOT/build/fprintd-stage"
FPRINTD_BIN="$ROOT/build/fprintd-stage/usr/libexec/fprintd"
if readelf -d "$FPRINTD_BIN" | grep -qE 'R(UN)?PATH'; then
  echo "HATA: $FPRINTD_BIN hâlâ RPATH içeriyor; kurulmadı." >&2
  exit 1
fi

echo "== Kuruluyor (sudo) =="
sudo -v
sudo systemctl stop fprintd 2>/dev/null || true

# Çekirdek modülü (DKMS, çekirdek güncellemelerinde kendiliğinden yeniden derlenir)
sudo modprobe -r gxfp 2>/dev/null || true
sudo dkms remove -m gxfp -v "$VERSION" --all >/dev/null 2>&1 || true
sudo rm -rf "/usr/src/gxfp-$VERSION"
sudo cp -a "$ROOT/kernel" "/usr/src/gxfp-$VERSION"
sudo make -C "/usr/src/gxfp-$VERSION" clean >/dev/null 2>&1 || true
sudo dkms add -m gxfp -v "$VERSION" >/dev/null
sudo dkms build -m gxfp -v "$VERSION" >/dev/null
sudo dkms install -m gxfp -v "$VERSION" >/dev/null

# Araçlar, udev kuralı, fprintd cihaz izni
for t in gxfp_capture gxfp_psk_tool gxfp_recovery; do
  sudo install -o root -g root -m 0755 "$ROOT/build/userspace/$t" "/usr/local/bin/$t"
done
sudo install -o root -g root -Dm0644 "$ROOT/config/60-gxfp.rules" /etc/udev/rules.d/60-gxfp.rules
sudo install -o root -g root -Dm0644 "$ROOT/config/fprintd-gxfp.conf" \
  /etc/systemd/system/fprintd.service.d/gxfp.conf

# libfprint /usr/local altına; Ubuntu'nun /usr altındaki paketine dokunulmaz
sudo meson install -C "$ROOT/build/libfprint" --no-rebuild >/dev/null
sudo ldconfig

# Yamalı fprintd; systemd'ye onu çalıştırmasını söyleyen drop-in
sudo install -o root -g root -Dm0755 "$FPRINTD_BIN" /usr/local/libexec/fprintd
sudo install -o root -g root -Dm0644 /dev/stdin /etc/systemd/system/fprintd.service.d/gxfp-fprintd.conf <<'EOF2'
[Service]
ExecStart=
ExecStart=/usr/local/libexec/fprintd
EOF2

# Kalibrasyon (kayıtlı parmak izleri bu dosyaya bağlıdır)
sudo install -d -m 0700 -o root -g root "$STATE"
if [ -f "$ROOT/calib/goodix_calib.dat" ] &&
   { [ "$REPLACE_CALIB" -eq 1 ] || ! sudo test -f "$STATE/goodix_calib.dat"; }; then
  sudo install -o root -g root -m 0600 "$ROOT/calib/goodix_calib.dat" "$STATE/goodix_calib.dat"
  echo "Kalibrasyon kuruldu: $STATE/goodix_calib.dat"
else
  echo "Mevcut kalibrasyon korundu (değiştirmek için: sh kur.sh --calib-yenile)"
fi

sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc
sudo systemctl daemon-reload
sudo depmod -a
sudo modprobe gxfp

echo
if sudo test -f "$STATE/psk_raw32.bin"; then
  echo "PSK mevcut: $STATE/psk_raw32.bin"
else
  echo "UYARI: PSK yok. Windows'tan çıkarıp $STATE/psk_raw32.bin olarak kurun"
  echo "(bkz. userspace/PSK.md)."
fi
if [ -c /dev/gxfp ]; then echo "/dev/gxfp hazır."; else echo "UYARI: /dev/gxfp oluşmadı; 'journalctl -k -g gxfp' bakın."; fi
echo "Kurulum bitti. Yeni kayıt: fprintd-delete \$USER && fprintd-enroll"
