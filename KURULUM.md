# GXFP5130 parmak izi sürücüsü (Chicago eşleştiricili)

Huawei MateBook dizüstülerindeki Goodix GXFP5130 (ACPI `GXFP5130:00`) parmak izi
sensörü için Linux sürücüsü. Bu laptopta (MDF-XX, firmware `GF_GCC_EC_20055`)
Ubuntu 26.04 / çekirdek 7.3 üzerinde test edildi.

Temel: [rumours86/gxfp5130-linux](https://github.com/rumours86/gxfp5130-linux)
(commit için `UPSTREAM` dosyasına bakın). Üzerine yapılan değişiklikler aşağıda.

## Kurulum

```sh
sudo apt install build-essential dkms cmake meson ninja-build pkg-config \
  libmbedtls-dev libglib2.0-dev libgusb-dev libpixman-1-dev libnss3-dev \
  libgudev-1.0-dev libcairo2-dev libopencv-dev libpolkit-gobject-1-dev \
  libsystemd-dev libdbus-1-dev gettext linux-headers-$(uname -r)
sh kur.sh
fprintd-delete $USER
fprintd-enroll
sh pam-ayar.sh                          # sudo/su/polkit için 3 deneme hakkı
```

Ubuntu'nun hazır PAM profili (`sudo pam-auth-update --enable fprintd`) sudo'da parmak
izine tek deneme hakkı veriyor; tutmazsa hemen şifreye geçiliyor. `pam-ayar.sh` aynı
profilin 3 denemeli bir kopyasını (`/usr/share/pam-configs/fprintd-gxfp`) ekleyip onu
etkinleştiriyor. Deneme sayısı argümanla değiştirilebilir (`sh pam-ayar.sh 2`). Giriş
ve kilit ekranı kendi ayarını (`gdm-fingerprint`, 3 deneme) kullanır. Her ek deneme,
başkasının parmağıyla girme şansını da artırır. `kaldir.sh` bu profili de kaldırır.

`kur.sh` normal kullanıcıyla çalışır, kurulum adımlarında sudo ister. Kurduğu şeyler:

- DKMS çekirdek modülü `gxfp` (`/dev/gxfp`). Çekirdek güncellemelerinde
  kendiliğinden yeniden derlenir; yeni çekirdeğin `linux-headers` paketi kurulu olmalı.
- libfprint (`/usr/local` altına). Ubuntu'nun `/usr` altındaki paketi değişmez.
- Araçlar: `gxfp_capture`, `gxfp_psk_tool`, `gxfp_recovery` (`/usr/local/bin`).
- udev kuralı ve fprintd cihaz izni.
- `calib/goodix_calib.dat` dosyasını `/var/lib/fprintd/gxfp/` altına kopyalar.
  Orada zaten bir dosya varsa onu korur.

Kaldırmak için `sh kaldir.sh` çalıştırın. PSK, kalibrasyon ve kayıtlı parmak izleri silinmez.

## Gerekli dosyalar

| Dosya | Açıklama |
|---|---|
| `/var/lib/fprintd/gxfp/psk_raw32.bin` | Sensörle şifreli iletişim anahtarı. **Bu klasörde yok** (gizli). Windows'tan çıkarma yöntemi `userspace/PSK.md` dosyasında. |
| `/var/lib/fprintd/gxfp/goodix_calib.dat` | Sensör kalibrasyonu. Windows'taki `C:\ProgramData\Goodix\goodix_calib.dat` dosyasının bir kopyası. Cihaza özel olduğu için depoda yok; kendi dosyanızı kurulumdan önce `calib/` altına koyun (bkz. `calib/README.md`). |

**Kayıtlı parmak izleri kalibrasyon dosyasına bağlıdır.** Dosyayı değiştirirseniz
(`sh kur.sh --calib-yenile`) parmak izlerini yeniden kaydetmeniz gerekir. Windows'un
kendi kopyasını güncellemesi buradaki sabit kopyayı etkilemez.

## Upstream'e göre değişiklikler

1. **Eşleştirici: SIGFM yerine Goodix Chicago.** Bu sensör bir ChicagoHS sensörü.
   Windows'un kalibrasyon dosyası, Chicago algoritmasının beklediği biçimle bire bir
   aynı ve CRC'ler tutuyor. Chicago'nun açık kaynak (LGPL) yeniden yazımı
   [berkekbgz/libfprint-goodix-spi](https://github.com/berkekbgz/libfprint-goodix-spi)
   projesinden `libfprint/libfprint/drivers/gxfp-chicago/` altına alındı.
   `libfprint/libfprint/drivers/gxfp.c` baştan yazıldı: kayıt, doğrulama ve tanıma
   ayrı bir iş parçacığında çalışıyor, kalibrasyonla birlikte ham kareler kullanılıyor.
2. **Referans (T0) kare doğrulaması.** Sensör her dokunuştan sonra birkaç saniye
   parmak izinin gölgesini tutuyor. Her oturumda ~0,3 sn arayla iki parmaksız kare
   alınıyor:
   - İki kare birbirine yakınsa sensör kararlı demektir; yeni kare T0 olarak kullanılır.
     Sensördeki kalıcı kir de bu kareye girer ve görüntüden çıkarılır, Windows'taki gibi.
   - Kareler arasında fark varsa gölge hâlâ kayboluyordur; son kararlı kare
     (`/var/lib/fprint/gxfp/t0.bin`) kullanılır.
   - İlk kare kayıtlı kararlı kareyle zaten örtüşüyorsa ikinci kare alınmaz.
   - Hazırlık sırasında parmak zaten sensördeyse (0,5 sn içinde "parmak yok" gelmezse)
     yeni kare alınmaz ve parmak beklemeden hemen okunur.
   - Eşleşme sonucu parmağın kalkması beklenmeden bildirilir. Eşleşmeyen basışta ise
     kalkma beklenir; böylece bir sonraki deneme aynı basışı tekrar okumaz.
3. **Parmak kalkışı algılama.** Sensörün geçersiz referans değerlerini (`0x00`/`0xFF`)
   yok sayan düzeltme, Void755/gxfpmoc `42860c1` commit'inden alındı
   (`drivers/gxfpmoc/src/flow/fdt.c`).
4. **Soluk/kirli kare kontrolü.** Kirli sensör ya da nemli/yağlı parmak, sırtların
   arasındaki boşlukları doldurur ve sırt sinyali 5–15 kat zayıflar. Sırt sinyali
   eşiğin altında kalan kare eşleştirmeye sokulmaz; "sensörü silip tekrar deneyin"
   anlamında bir tekrar isteği döner. fprintd bunu PAM deneme hakkından saymaz.
   Kayıtta da aynı kontrol uygulanır. Windows'taki "aynı basışta tekrar çekme"
   davranışı denendi ama kapatıldı: testlerde hiç işe yaramadı, nemli parmakta kare
   daha da kötüleşti ve sahte kabul riskini artırıyordu.
5. **Öğrenen şablon (templateStudy).** Her başarılı eşleşmeden sonra Chicago'nun
   öğrenme adımı çalışır. Basış "öğrenmeye uygun" sayılırsa şablona eklenir; Windows
   da böyle yapar. Şablon en fazla 50 basış tutar, dolunca en az katkı sağlayanı
   değiştirir. Ubuntu'nun fprintd'si güncellenen şablonu diske yazmadığı için
   `fprintd/` altında yamalı bir fprintd 1.94.5 var (yama:
   `fprintd-1.94.5-print-persist.patch`, berkekbgz/libfprint-goodix-spi'deki yamanın
   yalnızca şablon kaydetme kısmı). `kur.sh` bunu `/usr/local/libexec/fprintd`
   olarak kurar ve systemd'ye bir drop-in ile onu çalıştırmasını söyler. Ubuntu'nun
   fprintd paketi yerinde kalır. fprintd 1.94.5 libfprint ≥ 1.94.9 istiyor; bu fork
   ise 1.94.5 tabanlı. Eksik olan tek API, `FP_DEVICE_RETRY_TOO_FAST` sabiti
   libfprint'e geri taşındı ve fprintd'nin sürüm koşulu 1.94.5'e indirildi. fprintd
   bu libfprint'e karşı uyarısız derleniyor.
6. **Tanı araçları.** `FP_GXFP_LOG=1` sensörün parmak algılama kayıtlarını loglar,
   `FP_GXFP_DUMP=<dizin>` ham kareleri diske yazar. Kalibrasyon dosyasının yolu
   `FP_GXFP_CALIB`, PSK'nın yolu `FP_GXFP_PSK` ile değiştirilebilir.

## Performans ve güvenlik

Bu laptopta ölçülen değerler:

- Doğru parmak, deneme başına ~%75–85 kabul ediliyor. Kayıttan sonraki 6 denemenin 5'i eşleşti.
- Eşik 207'de başka parmakların ~%4'ü kabul ediliyor (128 denemede 5).
- Kirli sensörde 6 denemede 1 eşleşme, silindikten sonra 4 denemede 4 eşleşme
  ölçüldü. Sensör ara sıra silinmeli.

Bu bir **kolaylık kilidi**; şifrenin yerini tam olarak tutmaz. Daha sıkı bir eşik
için `-DGOODIX_CHICAGO_RUNTIME_SELECTOR_THRESHOLD=225` ile derleyin. O zaman sahte
kabul ~%1'e, doğru kabul ~%50'ye iner.

Sensör görüntüleri ana işlemciye gelir ve eşleştirme orada yapılır. Root yetkisi
olan bir saldırgan bu korumayı aşabilir; sahte parmak (canlılık) tespiti de yoktur.
