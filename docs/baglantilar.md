# Bağlantılar

Bu sayfa ESP32 ile sürücü ve MT6701 enkoder arasındaki bağlantıları ve her birinin nedenini anlatır. Güç tarafı (batarya, ön şarj, klemens) için: [tezgah-ozeti.md, bölüm 3](tezgah-ozeti.md#3-güç-tarafı-batarya-ön-şarj-direnci-acil-stop-klemens).

## 1. ESP32 – sürücü

| ESP32 | Ara eleman | Sürücü teli | Boştayken ölçülen | Görev |
|---|---|---|---|---|
| GND | doğrudan | Gaz fişi **siyah** | 0 V (B− ile aynı) | Ortak toprak, tek bağlantı noktası |
| GPIO25 (DAC1) | 1 kΩ seri | Gaz fişi **yeşil** | 0 V | Gaz gerilimi |
| GPIO26 | 1 kΩ seri | Yön fişi **gri** | 4,7 V | 0 V'a çekilince geri |
| GPIO27 | 1 kΩ seri | **Fren** teli | ölçülecek | 0 V'a çekilince fren |
| — | — | Gaz fişi **kırmızı** | 5,05 V | **Bağlanmaz, bantlı** |
| — | — | Yön fişi **bronz** | 5,05 V, iç pull-up ≈ 10,2 kΩ | Kullanılmıyor (bkz. özet 4.2) |

### Neden böyle

- **ESP32 pinleri 5 V'a dayanıklı değil.** Pin en fazla 3,6 V görebilir. Sürücünün gri ve fren hatları kendi içinde yaklaşık 5 V'a çekili.
- **Open-drain.** GPIO26 ve GPIO27 kodda `OUTPUT_OPEN_DRAIN` modunda. Pin ya içerideki anahtarla 0 V'a bağlanır ya da tamamen bırakılır; hiçbir zaman 3,3 V vermez. Sürücünün bu girişleri aktif düşük çalıştığı için 3,3 V zaten bir işe yaramaz.
- **1 kΩ seri direnç zorunlu.** Pin hattı bıraktığında hattın 5 V'u pine doğru gelir. Pin yaklaşık 3,8 V'ta iç koruma diyotuyla kenetlenir, aradaki fark dirençlerin üstünde düşer. Akım ≈ (5 − 3,8) V ÷ (1 kΩ + sürücünün iç pull-up'ı); bronz için ≈ 0,1 mA. Hat yanlışlıkla katı bir 5 V'a (kırmızı tel) değse bile 1 kΩ akımı ≈ 1,25 mA ile sınırlar.
- **1 kΩ çekmeye engel değil.** Bronzda ölçüldü: 1 kΩ ile toprağa çekilince 0,452 V. Gri için de geri yön doğrulandı.
- **Açılış ve reset güvenli.** ESP32 açılırken ya da resetlenirken pinler boşta kalır: yön ileri, fren yok. Gaz girişi sürücü içinde toprağa çekili (boşta 0 V ölçüldü), yani gaz da sıfır.
- **Gazdaki 1 kΩ.** Bir bağlantı hatasında DAC çıkışını korur. Bedeli: DAC 1,40 V'tayken yeşilde ≈ 1,2 V ölçüldü (sürücünün iç yükü ile bölücü oluşuyor). Yazılımdaki gaz değerleri DAC'a yazılan değerdir.
- **Tek toprak noktası.** ESP32'nin GND'si sürücüye yalnızca gaz fişinin siyahından bağlanır. Prize bağlı bir cihaz (şarj aleti, HDMI ekran) ikinci bir toprak yolu açıp toprak döngüsü yaratabilir.

### Fren için kontrol

Kod, fren telinin 0 V'a çekilince freni devreye soktuğunu varsayar (kılavuzdaki "alçak seviye fren" gibi). Bağlamadan önce:

1. Sürücü açık, ESP32 bağlı değilken fren telinin siyaha göre gerilimini ölçün. **5,5 V'un altında** olmalı.
2. Tel gerilim uygulanınca fren yapan bir tipse (kılavuzdaki mor "yüksek seviye fren" gibi) bu bağlantı uygun değildir; devre ve kod değişmelidir.

## 2. ESP32 – MT6701 enkoder (I2C)

ESP32'nin tüm bağlantıları kartın tek tarafında (3V3, GND, 25, 26, 27, 32, 33). Diğer taraf breadboard'a gömülü olduğu için I²C, varsayılan GPIO21/22 yerine GPIO33/32'de.

Kullanılan modülde beş pin var: **VDD, GND, Analog/PWM, SCL, SDA**. SSI için gereken CSN pini dışarı çıkarılmamış, bu yüzden I²C kullanılıyor.

| ESP32 | MT6701 modülü | Not |
|---|---|---|
| 3V3 | VDD | 5 V değil: çıkışlar besleme seviyesinde, 5 V'ta ESP32 pinine 5 V gelir |
| GND | GND | |
| GPIO33 | SDA | Gerekirse 4,7 kΩ ile 3V3'e pull-up |
| GPIO32 | SCL | Gerekirse 4,7 kΩ ile 3V3'e pull-up |
| — | Analog/PWM | Boş |

**Pull-up:** Veri sayfası SDA/SCL'de en fazla 150 ns yükselme süresi istiyor ve örnek devrelerde 4,7 kΩ kullanıyor. ESP32'nin iç pull-up'ları (≈ 45 kΩ) bunun için zayıf. Modülde pull-up olup olmadığını anlamak için: modül enerjisizken SDA ile VDD arasını ölçün; 2–10 kΩ görürseniz pull-up vardır. Yoksa 4,7 kΩ ekleyin. Programda `e` komutundaki "okuma hatası" sürekli artıyorsa ilk bakılacak yer burası.

### I²C okuması (veri sayfası Rev 1.8, bölüm 7.7)

- Adres 0x06 (7 bit).
- Register 0x03 = Açı[13:6], register 0x04 = Açı[5:0] (üst 6 bit). Önce 0x03, sonra 0x04 okunur.
- 14 bit: 16384 adım/tur, adım başına ≈ 0,022°.
- I²C'de alan durumu (manyetik alan zayıf/güçlü) ve CRC yok. Mıknatısın doğru yerde olduğu, mili elle çevirip açının 0–360° düzgün değiştiğine bakılarak kontrol edilir.
- Program sensörü saniyede 1000 kez okur (100 kHz I²C). Pull-up taktıktan sonra kodda `I2C_HIZ` 400000 yapılabilir.

### Mıknatıs ve montaj (veri sayfası Rev 1.8)

| Büyüklük | Değer |
|---|---|
| Önerilen mıknatıs | Ø6 × 2,5 mm, diametrik (kutuplar yan yüzde) |
| Çip yüzeyinde alan | 200–1000 gauss |
| Hava boşluğu (mıknatıs – çip yüzeyi) | en az 0,5 mm, tipik 1,0 mm, en fazla 2,0 mm |
| Eksen kaçıklığı | en fazla 0,3 mm |
| Besleme | 3,0–5,5 V (projede 3,3 V) |

Eski veri sayfası sürümü (Rev 0.1) Ø8 × 2,5 mm mıknatıs ve 300–800 gauss yazar. Montaj hesabı için: [tezgah-ozeti.md, bölüm 9](tezgah-ozeti.md#9-mt6701-enkoder).
