/*
 * Arka aks tezgahi - ESP32 (WROOM-32)
 * Gaz (DAC) + yon (gri) + fren + MT6701 manyetik enkoder (I2C). Transistor yok, pinler dogrudan.
 * ---------------------------------------------------------------------------
 * Acik cevrim test programi. Tekerlek acisini ve hizini canli gosterir.
 *
 * CALISMA PENCERESI (12 s, tezgah ozetindeki duzen)
 *   0-2 s  : gaz 0 (yon yalniz burada degisir)
 *   2-5 s  : gaz (0,3 s rampa ile GAZ_V'ye cikar)
 *   5-12 s : gaz 0 (istenirse fren: FREN_BOSTA)
 *   Yon degisimi aciksa pencereler A (ileri) ve B (geri) sirayla gelir; B'de kart LED'i yanar.
 *
 * BAGLANTILAR
 *   GND     -> surucu gaz soketi SIYAH (tek toprak noktasi)
 *   GPIO25  -> 1k -> gaz soketi YESIL                         (DAC, gaz gerilimi)
 *   GPIO26  -> 1k -> yon soketi GRI     (open-drain: 0 V'a ceker = GERI, birakir = ILERI)
 *   GPIO27  -> 1k -> FREN teli          (open-drain: 0 V'a ceker = FREN, birakir = fren yok)
 *              (Fren teli topraga cekilince devreye giren "alcak seviye fren" varsayildi)
 *   1k ZORUNLU: gri ve fren hatlari bosta ~5 V tasir, ESP32 pini en fazla 3,6 V kaldirir.
 *   Pinler hicbir zaman 3,3 V basmaz; reset/acilista bosta kalir = ILERI ve fren yok.
 *   MT6701 modulu (pinler: VDD, GND, Analog/PWM, SCL, SDA):
 *              VDD -> 3V3 (5V DEGIL), GND -> GND, SDA -> GPIO33, SCL -> GPIO32, Analog/PWM bos
 *              (Tum pinler kartin ayni tarafinda: 3V3, GND, 25, 26, 27, 32, 33)
 *              Ilk denemede pull-up yok: Wire, ESP32'nin ic pull-up'larini (~45k) acar.
 *              Okuma hatasi artarsa SDA-3V3 ve SCL-3V3 arasina birer 4,7k ekle.
 *
 * MT6701 I2C (veri sayfasi Rev 1.8, bolum 7.7):
 *   Adres 0x06. Register 0x03 = Aci[13:6], 0x04 = Aci[5:0] (ust 6 bit). Once 0x03, sonra 0x04.
 *   14 bit, 16384 adim / tur. I2C'de alan durumu ve CRC yok.
 *
 * SERI KOMUTLAR (115200 baud, satir sonu "Yeni Satir")
 *   g / BOOT  : baslat / durdur            s / bosluk : acil durdurma (gaz 0, fren, yon ileri)
 *   + / -     : gaz +/- 0,05 V             y : yon degisimini ac/kapa (yalniz dururken)
 *   f         : freni ac/kapa (fren devredeyken gaz hep 0)
 *   m         : enkoder izleme ac/kapa (dururken 200 ms'de bir satir)
 *   e         : enkoder durumu             r : tur sayaclarini sifirla
 *   i         : I2C teshisi (hat seviyeleri, adres taramasi, SDA/SCL ters mi)
 *   c         : ekran: okunur <-> CSV (Excel)
 *   n9  n4.5  : disli orani N'yi ayarla (motor turu / teker turu)
 *   ?         : genel durum
 *
 * CALISIRKEN (ve 'm' izlemede) 200 ms'DE BIR SATIR. Varsayilan okunur ekran:
 *   MOTOR aci 123.4°  tur +1.343 | TEKER aci 53.7°  toplam +53.7° | devir 0.0 d/dk | hata 0
 *   ('c' ile Excel icin CSV: t_ms;pencere;yon;fren;gaz_V;motor_ddk;teker_ddk;teker_derece;motor_tur;okuma_hata)
 */

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

// ---------------- Pinler ----------------
const int PIN_GAZ  = 25;   // DAC1
const int PIN_YON  = 26;   // 1k -> gri.        open-drain: LOW = geri,  birak = ileri
const int PIN_FREN = 27;   // 1k -> fren teli.  open-drain: LOW = fren,  birak = fren yok
int       PIN_SDA  = 33;   // MT6701 SDA  ('i' teshisi ters takildigini bulursa yer degistirir)
int       PIN_SCL  = 32;   // MT6701 SCL
const int PIN_LED  = 2;    // kart LED'i: B (geri) penceresinde yanar
const int PIN_BOOT = 0;    // BOOT dugmesi

// ---------------- Gaz ----------------
float       GAZ_V           = 1.60f;   // gaz penceresindeki DAC gerilimi (+/- ile degisir)
const float GAZ_UST_SINIR_V = 1.70f;   // yazilim siniri. Yerdeki olcumden sonra 0,05 V adimla artirilir
const float GAZ_ADIM_V      = 0.05f;
const float GAZ_BOS_V       = 0.00f;
const float DAC_VREF        = 3.30f;

// ---------------- Pencere (ms) ----------------
const uint32_t PENCERE_MS  = 12000;
const uint32_t GAZ_BAS_MS  = 2000;
const uint32_t GAZ_SON_MS  = 5000;
const uint32_t RAMPA_MS    = 300;

// ---------------- Fren ----------------
const bool FREN_BOSTA = false;   // true: 5-12 s gaz 0 araliginda fren de devrede

// ---------------- Enkoder / disli ----------------
const int32_t  TUR_ADIM     = 16384;   // 14 bit
float          DISLI_ORANI  = 9.0f;    // motor turu / teker turu. Olculunce 'n' ile duzelt
const uint32_t ENK_ARA_US   = 1000;    // enkoder okuma araligi (1 kHz)
const uint32_t LOG_MS       = 200;     // satir basma araligi
uint8_t        MT6701_ADR   = 0x06;    // varsayilan adres. 'i' teshisi baska adres bulursa onu kullanir
const uint32_t I2C_HIZ      = 100000;  // 4,7k pull-up taktiysan 400000 yapabilirsin

// ---------------- Durum ----------------
bool     calisiyor    = false;
bool     yonDegisimi  = true;
bool     geriPencere  = false;
bool     frenManuel   = false;   // 'f' ya da 's' ile
bool     izleme       = false;   // 'm'
bool     csvMod       = false;   // 'c': false = okunur ekran, true = Excel icin CSV
uint32_t pencereBas   = 0;
uint32_t baslangicMs  = 0;
float    sonGaz       = 0.0f;
bool     sonFren      = false;

// Enkoder durumu
uint16_t enkHam       = 0;
bool     enkGecerli   = false;   // en az bir gecerli okuma oldu mu
int64_t  toplamAdim   = 0;       // tur atlamalari acilmis toplam (isaretli)
uint32_t okumaHata    = 0;       // toplam
uint32_t okumaHataPencere = 0;   // son log araligindaki
uint32_t sonEnkUs     = 0;

// Log / hiz
uint32_t sonLogMs     = 0;
int64_t  sonLogAdim   = 0;

// Seri satir tamponu
char     satir[24];
uint8_t  satirUzun    = 0;
uint32_t sonKarakterMs = 0;

// ---------------- Cikis yardimcilari ----------------
void gazYazV(float volt) {
  if (volt > GAZ_UST_SINIR_V) volt = GAZ_UST_SINIR_V;
  if (volt < 0.0f)            volt = 0.0f;
  int d = (int)(volt / DAC_VREF * 255.0f + 0.5f);
  if (d > 255) d = 255;
  dacWrite(PIN_GAZ, (uint8_t)d);
  sonGaz = volt;
}

void yonYaz(bool geri) {
  digitalWrite(PIN_YON, geri ? LOW : HIGH);      // open-drain: LOW = 0 V'a cek
  digitalWrite(PIN_LED, geri ? HIGH : LOW);
}

void frenYaz(bool devrede) {
  if (devrede) gazYazV(0.0f);          // fren ve gaz asla birlikte
  digitalWrite(PIN_FREN, devrede ? LOW : HIGH);  // open-drain: LOW = 0 V'a cek
  sonFren = devrede;
}

// ---------------- I2C teshis ----------------
void i2cBaslat(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl, I2C_HIZ);
  Wire.setTimeOut(5);
  gpio_pullup_en((gpio_num_t)sda);       // ic pull-up'lari (~45k) kesin acik olsun
  gpio_pullup_en((gpio_num_t)scl);
}

int i2cTara(int sda, int scl, uint8_t *bulunan, int maks) {
  i2cBaslat(sda, scl);
  int n = 0;
  for (uint8_t adr = 1; adr < 127; adr++) {
    Wire.beginTransmission(adr);
    if (Wire.endTransmission() == 0 && n < maks) bulunan[n++] = adr;
  }
  return n;
}

// Hat seviyelerini olcer, adres tarar, SDA/SCL ters takilmissa bulur.
void i2cTeshis() {
  Wire.end();
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, INPUT_PULLUP);
  delay(5);
  int sdaSev = digitalRead(PIN_SDA);
  int sclSev = digitalRead(PIN_SCL);
  Serial.printf("[i2c] Bosta hat seviyesi: SDA(GPIO%d)=%d  SCL(GPIO%d)=%d  (ikisi de 1 olmali)\n",
                PIN_SDA, sdaSev, PIN_SCL, sclSev);
  if (!sdaSev || !sclSev)
    Serial.println("[i2c]  -> 0 olan hat GND'ye degiyor ya da modulde enerji yok (enerjisiz cip hatti asagi ceker).");

  uint8_t bul[8];
  int n = i2cTara(PIN_SDA, PIN_SCL, bul, 8);
  if (n == 0) {
    n = i2cTara(PIN_SCL, PIN_SDA, bul, 8);
    if (n > 0) {
      int t = PIN_SDA; PIN_SDA = PIN_SCL; PIN_SCL = t;
      Serial.printf("[i2c] SDA ve SCL TERS takilmis. Kod ters haliyle devam ediyor (SDA=GPIO%d, SCL=GPIO%d).\n",
                    PIN_SDA, PIN_SCL);
      Serial.println("[i2c]  Kalici duzeltme: kablolarin yerini degistirin.");
    }
  }
  if (n == 0) {
    Serial.println("[i2c] Hicbir cihaz cevap vermedi. Kontrol: modulde VDD-GND arasi 3,3 V var mi,");
    Serial.println("[i2c]  kablolar ve breadboard temaslari saglam mi, pull-up gerekli mi (4,7k SDA-3V3, SCL-3V3).");
    i2cBaslat(PIN_SDA, PIN_SCL);
    return;
  }
  Serial.print("[i2c] Cevap veren adres(ler):");
  bool varsayilan = false;
  for (int i = 0; i < n; i++) { Serial.printf(" 0x%02X", bul[i]); if (bul[i] == 0x06) varsayilan = true; }
  Serial.println();
  MT6701_ADR = varsayilan ? 0x06 : bul[0];
  Serial.printf("[i2c] MT6701 adresi olarak 0x%02X kullaniliyor.\n", MT6701_ADR);
}

// ---------------- MT6701 I2C ----------------
// true: okuma basarili, aci guncellendi
bool enkOku(uint16_t &aci) {
  Wire.beginTransmission(MT6701_ADR);
  Wire.write((uint8_t)0x03);
  if (Wire.endTransmission(false) != 0) return false;          // tekrar-baslat
  if (Wire.requestFrom(MT6701_ADR, (uint8_t)2) != 2) return false;
  uint8_t yuksek = Wire.read();                                 // 0x03: Aci[13:6]
  uint8_t dusuk  = Wire.read();                                 // 0x04: Aci[5:0] << 2
  aci = (uint16_t)(((uint16_t)yuksek << 6) | (dusuk >> 2));
  return true;
}

void enkGuncelle() {
  uint32_t simdi = micros();
  if (simdi - sonEnkUs < ENK_ARA_US) return;
  sonEnkUs = simdi;

  uint16_t aci;
  if (!enkOku(aci)) { okumaHata++; okumaHataPencere++; return; }

  if (enkGecerli) {
    int32_t fark = (int32_t)aci - (int32_t)enkHam;
    if (fark >  TUR_ADIM / 2) fark -= TUR_ADIM;
    if (fark < -TUR_ADIM / 2) fark += TUR_ADIM;
    toplamAdim += fark;
  }
  enkHam = aci;
  enkGecerli = true;
}

double motorTur()     { return (double)toplamAdim / (double)TUR_ADIM; }
double tekerDerece()  { return motorTur() * 360.0 / (double)DISLI_ORANI; }

// ---------------- Durum yazdirma ----------------
void durumYaz() {
  Serial.printf("Durum: %s | pencere: %s | yon degisimi: %s | fren: %s | gaz: %.2f V (sinir %.2f V) | N: %.2f\n",
                calisiyor ? "CALISIYOR" : "DURDU",
                geriPencere ? "B / GERI" : "A / ILERI",
                yonDegisimi ? "acik" : "kapali",
                frenManuel ? "DEVREDE" : "yok",
                GAZ_V, GAZ_UST_SINIR_V, DISLI_ORANI);
}

void enkDurumYaz() {
  if (!enkGecerli) {
    Serial.printf("Enkoder: gecerli okuma yok | okuma hatasi: %lu. VDD=3V3, GND, SDA=33, SCL=32 ve pull-up'lari kontrol et.\n",
                  (unsigned long)okumaHata);
    return;
  }
  Serial.printf("Enkoder: aci=%.2f deg | ham=%u | motor tur=%.3f | teker=%.1f deg | N=%.2f | okuma hatasi=%lu\n",
                enkHam * 360.0f / TUR_ADIM, enkHam,
                motorTur(), tekerDerece(), DISLI_ORANI, (unsigned long)okumaHata);
}

void logBaslik() {
  if (csvMod) Serial.println("t_ms;pencere;yon;fren;gaz_V;motor_ddk;teker_ddk;teker_derece;motor_tur;okuma_hata");
}

float aci360(double derece) {               // 0 ile 360 arasina indir
  double a = fmod(derece, 360.0);
  if (a < 0) a += 360.0;
  return (float)a;
}

void logSatir(uint32_t simdiMs) {
  float sure_s = (simdiMs - sonLogMs) / 1000.0f;
  if (sure_s <= 0.0f) return;
  int64_t degisim = toplamAdim - sonLogAdim;
  sonLogAdim = toplamAdim;
  sonLogMs   = simdiMs;

  float motorDdk = ((float)degisim / (float)TUR_ADIM) / sure_s * 60.0f;
  float tekerDdk = motorDdk / DISLI_ORANI;

  if (csvMod) {
    Serial.printf("%lu;%s;%s;%d;%.2f;%.1f;%.2f;%.1f;%.3f;%lu\n",
                  (unsigned long)(simdiMs - baslangicMs),
                  calisiyor ? (geriPencere ? "B" : "A") : "-",
                  geriPencere ? "geri" : "ileri",
                  sonFren ? 1 : 0,
                  sonGaz, motorDdk, tekerDdk, tekerDerece(), motorTur(),
                  (unsigned long)okumaHataPencere);
  } else {
    float motorAci = enkHam * 360.0f / (float)TUR_ADIM;     // mil acisi, 0-360
    double teker   = tekerDerece();                          // sifirlamadan beri toplam
    if (calisiyor) {
      Serial.printf("[%s %-5s] gaz %.2f V%s | MOTOR aci %6.1f°  devir %7.1f d/dk | TEKER aci %6.1f°  toplam %+9.1f°  devir %6.2f d/dk | hata %lu\n",
                    geriPencere ? "B" : "A", geriPencere ? "geri" : "ileri",
                    sonGaz, sonFren ? " FREN" : "",
                    motorAci, motorDdk, aci360(teker), teker, tekerDdk,
                    (unsigned long)okumaHataPencere);
    } else {
      Serial.printf("MOTOR aci %6.1f°  tur %+8.3f | TEKER aci %6.1f°  toplam %+9.1f° | devir %6.1f d/dk | hata %lu\n",
                    motorAci, motorTur(), aci360(teker), teker, motorDdk,
                    (unsigned long)okumaHataPencere);
    }
  }
  okumaHataPencere = 0;
}

// ---------------- Baslat / durdur ----------------
void baslat() {
  frenManuel = false;
  frenYaz(false);
  gazYazV(GAZ_BOS_V);
  geriPencere = false;
  yonYaz(false);
  pencereBas  = millis();
  baslangicMs = pencereBas;
  sonLogMs    = pencereBas;
  sonLogAdim  = toplamAdim;
  izleme      = false;
  calisiyor   = true;
  Serial.printf("\n[BASLA] gaz=%.2f V, yon degisimi %s, N=%.2f\n",
                GAZ_V, yonDegisimi ? "acik" : "kapali", DISLI_ORANI);
  logBaslik();
}

void durdur(const char *neden) {
  calisiyor = false;
  gazYazV(0.0f);
  Serial.printf("\n[DUR] %s -> gaz 0\n", neden);
}

void acilDur() {
  calisiyor = false;
  frenManuel = true;
  frenYaz(true);                   // gazi da 0 yapar
  yonYaz(false);
  Serial.println("\n[ACIL DUR] gaz 0, FREN DEVREDE, yon ileri. Freni birakmak icin 'f' ya da 'g'.");
}

// ---------------- Komutlar ----------------
void tekKomut(char c) {
  switch (c) {
    case 'g': case 'G':
      if (calisiyor) durdur("kullanici (g)"); else baslat();
      break;
    case 's': case 'S': case ' ':
      acilDur();
      break;
    case '+':
      GAZ_V += GAZ_ADIM_V;
      if (GAZ_V > GAZ_UST_SINIR_V) GAZ_V = GAZ_UST_SINIR_V;
      Serial.printf("[gaz] %.2f V\n", GAZ_V);
      break;
    case '-':
      GAZ_V -= GAZ_ADIM_V;
      if (GAZ_V < 0.0f) GAZ_V = 0.0f;
      Serial.printf("[gaz] %.2f V\n", GAZ_V);
      break;
    case 'y': case 'Y':
      if (calisiyor) { Serial.println("[yon] Once durdur ('g' ya da 's')."); break; }
      yonDegisimi = !yonDegisimi;
      Serial.printf("[yon] Yon degisimi %s\n", yonDegisimi ? "ACIK (A ileri / B geri)" : "KAPALI (hep ileri)");
      break;
    case 'f': case 'F':
      frenManuel = !frenManuel;
      frenYaz(frenManuel);
      Serial.printf("[fren] %s\n", frenManuel ? "DEVREDE (gaz 0)" : "birakildi");
      break;
    case 'm': case 'M':
      if (calisiyor) { Serial.println("[izleme] Calisirken zaten 200 ms'de bir satir basiliyor."); break; }
      izleme = !izleme;
      if (izleme) {
        baslangicMs = millis(); sonLogMs = baslangicMs; sonLogAdim = toplamAdim;
        Serial.println("[izleme] ACIK. Tekeri elle cevirip teker_derece'yi izleyebilirsin. Kapatmak icin 'm'.");
        logBaslik();
      } else {
        Serial.println("[izleme] KAPALI");
      }
      break;
    case 'c': case 'C':
      csvMod = !csvMod;
      Serial.printf("[ekran] %s\n", csvMod ? "CSV (Excel icin, ayrac ';')" : "Okunur ekran");
      if (csvMod && (calisiyor || izleme)) logBaslik();
      break;
    case 'e': case 'E':
      enkDurumYaz();
      break;
    case 'r': case 'R':
      toplamAdim = 0; sonLogAdim = 0; okumaHata = 0;
      Serial.println("[enkoder] Tur sayaclari sifirlandi.");
      break;
    case 'i': case 'I':
      i2cTeshis();
      break;
    case '?':
      durumYaz();
      enkDurumYaz();
      break;
    default:
      break;
  }
}

void satirIsle() {
  satir[satirUzun] = '\0';
  if (satirUzun == 0) return;
  if ((satir[0] == 'n' || satir[0] == 'N') && satirUzun > 1) {
    float n = atof(&satir[1]);
    if (n >= 0.5f && n <= 500.0f) {
      DISLI_ORANI = n;
      Serial.printf("[disli] N = %.2f (motor turu / teker turu)\n", DISLI_ORANI);
    } else {
      Serial.println("[disli] Gecersiz deger. Ornek: n9 ya da n4.5");
    }
  } else {
    for (uint8_t i = 0; i < satirUzun; i++) tekKomut(satir[i]);
  }
  satirUzun = 0;
}

void seriOku() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    sonKarakterMs = millis();
    if (c == '\n' || c == '\r') { satirIsle(); continue; }
    if (satirUzun < sizeof(satir) - 1) satir[satirUzun++] = c;
  }
  // "Satir sonu yok" secilmisse: 100 ms sessizlikten sonra isle
  if (satirUzun > 0 && millis() - sonKarakterMs > 100) satirIsle();
}

void bootDugmesiOku() {
  static bool     oncekiBasili = false;
  static uint32_t sonDegisim   = 0;
  bool basili = (digitalRead(PIN_BOOT) == LOW);
  uint32_t simdi = millis();
  if (basili != oncekiBasili && (simdi - sonDegisim) > 50) {
    sonDegisim   = simdi;
    oncekiBasili = basili;
    if (basili) tekKomut('g');
  }
}

// ---------------- Kurulum ----------------
void setup() {
  // Cikislari once guvenli seviyeye yaz
  digitalWrite(PIN_YON, HIGH);         // once "birak" seviyesi: ileri
  digitalWrite(PIN_FREN, HIGH);        // fren yok
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_YON, OUTPUT_OPEN_DRAIN); // asla 3,3 V basmaz
  pinMode(PIN_FREN, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_YON, HIGH);
  digitalWrite(PIN_FREN, HIGH);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  gazYazV(GAZ_BOS_V);

  Serial.setTxBufferSize(1024);        // yazdirma enkoder okumasini bekletmesin
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== ARKA AKS: GAZ + YON (gri) + FREN + MT6701 I2C (transistorsuz) ===");
  Serial.println("Pencere 12 s: 0-2 s gaz 0 | 2-5 s gaz | 5-12 s gaz 0");
  Serial.println("g/BOOT basla-dur | s acil | +/- gaz | y yon | f fren | m izle | e enkoder | i i2c teshis | c ekran/CSV | r sifirla | n9 disli | ? durum");
  Serial.println("Acilista DURUYOR, yon ileri, fren yok.");

  i2cTeshis();                         // hat seviyeleri + adres taramasi
  delay(5);
  for (int i = 0; i < 5; i++) { sonEnkUs = 0; enkGuncelle(); delay(2); }
  durumYaz();
  enkDurumYaz();
}

// ---------------- Ana dongu ----------------
void loop() {
  enkGuncelle();
  seriOku();
  bootDugmesiOku();

  uint32_t simdi = millis();

  if (calisiyor) {
    uint32_t t = simdi - pencereBas;

    if (t >= PENCERE_MS) {                         // yeni pencere: gaz zaten 0
      pencereBas = simdi;
      t = 0;
      if (yonDegisimi) geriPencere = !geriPencere; else geriPencere = false;
      yonYaz(geriPencere);
      Serial.printf("---- PENCERE %s ----\n", geriPencere ? "B: GERI" : "A: ILERI");
    }

    float gaz  = GAZ_BOS_V;
    bool  fren = frenManuel;
    if (t >= GAZ_BAS_MS && t < GAZ_SON_MS) {
      uint32_t tg = t - GAZ_BAS_MS;
      gaz = (RAMPA_MS > 0 && tg < RAMPA_MS)
              ? GAZ_BOS_V + (GAZ_V - GAZ_BOS_V) * (float)tg / (float)RAMPA_MS
              : GAZ_V;
    } else if (t >= GAZ_SON_MS && FREN_BOSTA) {
      fren = true;
    }

    if (fren) { frenYaz(true); }               // gaz 0
    else      { frenYaz(false); gazYazV(gaz); }
  } else {
    gazYazV(0.0f);
    frenYaz(frenManuel);
  }

  if ((calisiyor || izleme) && simdi - sonLogMs >= LOG_MS) logSatir(simdi);
}
