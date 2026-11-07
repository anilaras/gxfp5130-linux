# Kalibrasyon dosyası / Calibration file

Sürücü, sensörün Windows'taki üretim kalibrasyonunu kullanır. Bu dosya her cihaza
özeldir (sensör kimliği içerir), bu yüzden depoda yoktur.

1. Windows'ta parmak izi en az bir kez kullanılmış olmalı.
2. `C:\ProgramData\Goodix\goodix_calib.dat` dosyasını (140480 bayt) bu klasöre
   `goodix_calib.dat` adıyla kopyalayın. Linux'tan Windows bölümünü bağlayıp da
   alabilirsiniz.
3. `sh kur.sh` dosyayı `/var/lib/fprintd/gxfp/goodix_calib.dat` olarak kurar.

Kayıtlı parmak izleri bu dosyaya bağlıdır; değiştirirseniz yeniden kayıt gerekir.

---

The driver uses the sensor's Windows production calibration, which is unique to
each device. Copy `C:\ProgramData\Goodix\goodix_calib.dat` (140480 bytes) here
as `goodix_calib.dat`; `kur.sh` installs it to `/var/lib/fprintd/gxfp/`.
Enrolled prints are bound to this file.
