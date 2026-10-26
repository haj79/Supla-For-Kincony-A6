/*
  Supla For Kincony KC868-A6 -- wersja "oficjalna"
  ==================================================
  WerOpr: 2026-08-25 @lukasz06
  EKSPERYMENTALNA wersja bazująca na oficjalnej architekturze biblioteki
  SuplaDevice (przykład WebInterface.ino z github.com/SUPLA/supla-device),
  zamiast własnego, ręcznie pisanego WebServera + Preferences.

  Co to daje w porównaniu do wersji "custom":
  - Storage::Config (LittleFsConfig) -> GUID/AuthKey generowane i zapisywane
    automatycznie przez bibliotekę.
  - Wbudowany EspWebServer -> panel konfiguracyjny (WiFi, serwer Supla,
    email) dostępny pod http://192.168.4.1 w trybie AP. Jest GENERYCZNY
    (głównie po angielsku), nie jest to spolszczony panel z poprzedniej
    wersji.
  - configureAsConfigButton(&SuplaDevice) na przycisku BOOT -> oficjalna,
    w pełni wspierana obsługa:
      * lokalne przytrzymanie przycisku -> wejście w tryb konfiguracji
        (bez utraty ustawień),
      * zdalne kliknięcie "Enter config mode" w Supla Cloud przy tym
        urządzeniu -> to samo, zainicjowane z chmury.
    Dokładny czas przytrzymania (domyślnie ok. 5s, dłuższe przytrzymanie
    może odblokować reset fabryczny) jest zdefiniowany WEWNĄTRZ biblioteki,
    nie w tym pliku -- do zweryfikowania w dokumentacji Supla::Control::Button
    po skompilowaniu.
  - Supla::Html::UpdateParameter + własna obsługa Update.h -> przycisk
    "Aktualizacja Firmware" prowadzący na stronę /update z wyborem pliku
    .bin, przyciskiem "Powrót", i stylizowaną stroną wyniku z odliczaniem
    (własna implementacja upload handlera, NIE biblioteka HTTPUpdateServer
    -- ta dawała nieedytowalną stronę "Update Success! Rebooting..." bez
    możliwości dopasowania wyglądu). NIE wbudowane bloki
    Supla::Html::SwUpdate/CustomSwUpdate, które robią coś innego --
    automatyczną aktualizację z iot.updates.supla.org.
  - Modbus: dwie NIEZALEŻNE warstwy:
    1) Supla::Modbus::Configurator + Supla::Html::ModbusParameters
       (wbudowane w bibliotekę) -- wybór roli Master/Slave/Disabled, tryb
       RTU, wspólny baudrate/stop bity dla całej magistrali RS485, adres
       WŁASNY urządzenia (widoczny tylko przy roli Slave).
    2) Własna sekcja "/modbus-devices" (5 slotów) -- działa TYLKO gdy rola
       = Master i tryb = RTU. Tu użytkownik wpisuje adresy urządzeń, które
       KC868 ma ODPYTYWAĆ (nie własny adres!), rejestr, kod funkcji
       (0x03/0x04), scan rate, timeout, i może każdy slot zarejestrować
       jako osobny kanał "General Purpose Measurement" w Supla Cloud.
       Oparte o bibliotekę ModbusMaster (ta sama, sprawdzona, co w wersji
       custom).

  Czego świadomie NIE MA w tej wersji (do dopisania później, jeśli się
  sprawdzi):
  - automatyka wiatr/deszcz/woda/światło,
  - rolety (RollerShutter) -- w tej wersji wszystkie 6 kanałów to zwykłe
    przekaźniki (POWERSWITCH), tak jak w konfiguracji domyślnej.
  - spolszczony, autorski wygląd panelu WWW (poza stronami /update i
    /modbus-devices, które są w 100% własne).

  WAŻNE: Ten plik NIE został skompilowany (brak dostępu do internetu/
  menadżera płytek ESP32 w środowisku, w którym go przygotowałem).
  Sprawdź kompilację w Arduino IDE / PlatformIO przed wgraniem na
  urządzenie. Będziesz potrzebować biblioteki "SuplaDevice" (najnowszej
  z github.com/SUPLA/supla-device lub Library Managera) oraz biblioteki
  "PCF8574".
*/

#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PCF8574.h>

#include <SuplaDevice.h>
#include <supla/network/esp32_wifi.h>
#include <supla/control/relay.h>
#include <supla/control/relay_roller_shutter_pair.h>
#include <supla/control/action_trigger.h>
#include <supla/control/button.h>
#include <supla/control/virtual_relay.h>
#include <supla/network/network.h>
#include <supla/control/action_trigger.h>
#include <supla/io.h>
#include <supla/storage/littlefs_config.h>
#include <supla/network/esp_web_server.h>
#include <supla/network/html/device_info.h>
#include <supla/network/html/protocol_parameters.h>
#include <supla/modbus/modbus_configurator.h>
#include <supla/network/html/modbus_parameters.h>
#include <ModbusMaster.h>
#include <supla/sensor/general_purpose_measurement.h>
#include <supla/sensor/binary_base.h>
#include <supla/sensor/container.h>
#include <supla/sensor/virtual_thermometer.h>
#include <supla/sensor/DS18B20.h>
#include <supla/network/html/wifi_parameters.h>
#include <Update.h>

// Własny bloczek HTML: przycisk "Aktualizacja Firmware", który przenosi na
// osobną stronę /update obsługiwaną przez standardową (nie-Supla) bibliotekę
// HTTPUpdateServer -- to daje dokładnie klasyczny widok "wybierz plik +
// UPDATE + BACK TO GUI", zamiast wbudowanych bloków Supla::Html::SwUpdate/
// CustomSwUpdate, które robią coś innego (patrz historia w komentarzach
// poniżej / rozmowa, która doprowadziła do tej wersji).
namespace Supla {
namespace Html {
class UpdateParameter : public Supla::HtmlElement {
 public:
  UpdateParameter() : HtmlElement(HTML_SECTION_FORM) {}
  void send(Supla::WebSender* sender) override {
    sender->send("<div>");
    sender->send(
        "<button type=\"button\" onclick=\"wlaczUpdateServer();\">"
        "Aktualizacja Firmware"
        "</button>");
    sender->send(
        "<script>"
        "function wlaczUpdateServer(){"
        "window.location.href='/update';"
        "}"
        "</script>");
  }
  bool handleResponse(const char* key, const char* value) override {
    return false;
  }
};
};  // namespace Html
};  // namespace Supla
#include <supla/device/supla_ca_cert.h>
#include <supla/device/channel_conflict_resolver.h>
#include <supla/events.h>

// Nazwa urządzenia -- jedno miejsce, użyte zarówno w SuplaDevice.setName(),
// jak i na nagłówkach naszych własnych stron (/update, /sensors-control).
const char* const DEVICE_DISPLAY_NAME = "Supla For Kincony A6";

// Logo Supli (SVG) -- skopiowane 1:1 ze źródła głównej strony panelu.
const char* const SUPLA_LOGO_SVG =
    "<svg id=logo version=1.1 viewBox=\"0 0 200 200\" x=0 xml:space=preserve "
    "y=0 style=\"height:100px;\"><path d=\"M59.3,2.5c18.1,0.6,31.8,8,40.2,23.5"
    "c3.1,5.7,4.3,11.9,4.1,18.3c-0.1,3.6-0.7,7.1-1.9,10.6c-0.2,0.7-0.1,1.1,0.6"
    ",1.5c12.8,7.7,25.5,15.4,38.3,23c2.9,1.7,5.8,3.4,8.7,5.3c1,0.6,1.6,0.6,2.5"
    "-0.1c4.5-3.6,9.8-5.3,15.7-5.4c12.5-0.1,22.9,7.9,25.2,19c1.9,9.2-2.9,19.2"
    "-11.8,23.9c-8.4,4.5-16.9,4.5-25.5,0.2c-0.7-0.3-1-0.2-1.5,0.3c-4.8,4.9-9.7"
    ",9.8-14.5,14.6c-5.3,5.3-10.6,10.7-15.9,16c-1.8,1.8-3.6,3.7-5.4,5.4c-0.7,"
    "0.6-0.6,1,0,1.6c3.6,3.4,5.8,7.5,6.2,12.2c0.7,7.7-2.2,14-8.8,18.5c-12.3,8"
    ".6-30.3,3.5-35-10.4c-2.8-8.4,0.6-17.7,8.6-22.8c0.9-0.6,1.1-1,0.8-2c-2-6."
    "2-4.4-12.4-6.6-18.6c-6.3-17.6-12.7-35.1-19-52.7c-0.2-0.7-0.5-1-1.4-0.9c"
    "-12.5,0.7-23.6-2.6-33-10.4c-8-6.6-12.9-15-14.2-25c-1.5-11.5,1.7-21.9,9."
    "6-30.7C32.5,8.9,42.2,4.2,53.7,2.7c0.7-0.1,1.5-0.2,2.2-0.2C57,2.4,58.2,2"
    ".5,59.3,2.5z M76.5,81c0,0.1,0.1,0.3,0.1,0.6c1.6,6.3,3.2,12.6,4.7,18.9c4"
    ".5,17.7,8.9,35.5,13.3,53.2c0.2,0.9,0.6,1.1,1.6,0.9c5.4-1.2,10.7-0.8,15."
    "7,1.6c0.8,0.4,1.2,0.3,1.7-0.4c11.2-12.9,22.5-25.7,33.4-38.7c0.5-0.6,0.4"
    "-1,0-1.6c-5.6-7.9-6.1-16.1-1.3-24.5c0.5-0.8,0.3-1.1-0.5-1.6c-9.1-4.7-18"
    ".1-9.3-27.2-14c-6.8-3.5-13.5-7-20.3-10.5c-0.7-0.4-1.1-0.3-1.6,0.4c-1.3,"
    "1.8-2.7,3.5-4.3,5.1c-4.2,4.2-9.1,7.4-14.7,9.7C76.9,80.3,76.4,80.3,76.5,"
    "81z M89,42.6c0.1-2.5-0.4-5.4-1.5-8.1C83,23.1,74.2,16.9,61.7,15.8c-10-0."
    "9-18.6,2.4-25.3,9.7c-8.4,9-9.3,22.4-2.2,32.4c6.8,9.6,19.1,14.2,31.4,11."
    "9C79.2,67.1,89,55.9,89,42.6z M102.1,188.6c0.6,0.1,1.5-0.1,2.4-0.2c9.5-1"
    ".4,15.3-10.9,11.6-19.2c-2.6-5.9-9.4-9.6-16.8-8.6c-8.3,1.2-14.1,8.9-12.4"
    ",16.6C88.2,183.9,94.4,188.6,102.1,188.6z M167.7,88.5c-1,0-2.1,0.1-3.1,0"
    ".3c-9,1.7-14.2,10.6-10.8,18.6c2.9,6.8,11.4,10.3,19,7.8c7.1-2.3,11.1-9.1"
    ",9.6-15.9C180.9,93,174.8,88.5,167.7,88.5z\"/></svg>";

// Struktury dla "Kontrola Czujników" (progi/warunkowanie/automatyka)
// zdefiniowane TU, na samej górze pliku -- Arduino IDE automatycznie
// generuje prototypy wszystkich funkcji na początku pliku, WCZEŚNIEJ niż
// widzi definicje własnych struktur zapisanych w środku pliku. Jeśli
// jakaś funkcja przyjmuje taki typ jako parametr, wygenerowany prototyp
// odwołuje się do niego, zanim jeszcze istnieje -- stąd błąd kompilacji
// "'AutomationCfg' does not name a type". Rozwiązanie: struktury używane
// jako parametry funkcji muszą być zdefiniowane jak najwcześniej w pliku.
struct AutomationCfg {
  bool enabled;
  float threshold;   // nieużywane dla Opadów (czujnik binarny -- liczy się czas, nie próg)
  uint32_t condSec;  // czas warunkowania w sekundach (zawsze przechowywany w sekundach; dla Opadów UI pokazuje minuty)
  uint8_t relayMask; // bit i = kanał przekaźnika i (0-5) ma reagować
};

struct AutomationState {
  bool conditionSinceValid = false;
  unsigned long conditionSince = 0;
  bool actionActive = false;
};

// Opcjonalne wysyłanie stanu istniejącej reguły alarmowej do lokalnego
// MQTT tabletu. Każda reguła ma osobny checkbox i osobny temat.
struct MqttAlarmCfg {
  bool enabled;
  String topic;
};

// Osobna struktura dla Temperatury -- DWIE niezależne reguły:
// "powyżej progu" i "poniżej progu", każda z własnym progiem, histerezą
// i zestawem przekaźników. Np. temp > 28 -> włącz wentylator (CH3),
// temp < 5 -> włącz grzałkę (CH5), każda reguła z osobną histerezą,
// żeby przekaźniki nie migotały przy wartości granicznej.
struct TempAutomationCfg {
  bool enabled;
  float highThreshold;
  float highHysteresis;
  uint8_t highRelayMask;
  float lowThreshold;
  float lowHysteresis;
  uint8_t lowRelayMask;
};

// Zbiornik -- prawdziwa histereza Max/Min (nie próg+czas warunkowania jak
// Wiatr/Opady/Światło): włącz przy Max, zostaw włączone aż spadnie do Min.
struct WaterAutomationCfg {
  bool enabled;
  float maxLevel;
  float minLevel;
  uint8_t relayMask;
};

// Przycisk "Odczyt Czujników" -> /sensors-control
namespace Supla {
namespace Html {
class SensorControlButton : public Supla::HtmlElement {
 public:
  SensorControlButton() : HtmlElement(HTML_SECTION_FORM) {}
  void send(Supla::WebSender* sender) override {
    sender->send(
        "<div><button type=\"button\" onclick=\"window.location.href='/sensors-control';\">"
        "Ustawienia Czujnik\u00f3w"
        "</button></div>");
  }
  bool handleResponse(const char* key, const char* value) override {
    return false;
  }
};
};  // namespace Html
};  // namespace Supla

// Przycisk "Przekaźniki i Przyciski" -> /relay-config
namespace Supla {
namespace Html {
class RelayConfigButton : public Supla::HtmlElement {
 public:
  RelayConfigButton() : HtmlElement(HTML_SECTION_FORM) {}
  void send(Supla::WebSender* sender) override {
    sender->send(
        "<div><button type=\"button\" onclick=\"window.location.href='/relay-config';\">"
        "Przeka\u017aniki i Przyciski"
        "</button></div>");
  }
  bool handleResponse(const char* key, const char* value) override {
    return false;
  }
};
};  // namespace Html
};  // namespace Supla

// --- Obsługa konfliktu kanałów po usunięciu w Supla Cloud ---
// Gdy klikniesz "Usuń" przy kanale/subdevice w Cloud, serwer kasuje go u
// siebie i restartuje połączenie. Urządzenie (jeszcze nic nie wiedząc)
// próbuje zarejestrować się ze WSZYSTKIMI swoimi kanałami -- w tym tym
// usuniętym -- i dostaje w odpowiedzi "konflikt": serwer informuje, które
// kanały u niego już nie istnieją. Ten resolver obsługuje te powiadomienia.
//
// UWAGA -- zaimplementowałem tylko BEZPIECZNĄ, potwierdzoną część
// (przypadki "nieodwracalne": zły typ kanału / kanał brakujący na
// urządzeniu -- loguję błąd i przerywam, zgodnie z przykładem). NIE
// zaimplementowałem automatycznego samo-kasowania obiektów kanałów po
// stronie urządzenia przy "kanał brakuje na serwerze" -- oficjalny
// przykład używa do tego API (Supla::RegisterDevice::getMaxChannelNumberUsed,
// AutoLock, Storage::IsStateStorageValid/WriteStateStorage,
// getDeviceByChannelNumber), którego nie mam możliwości zweryfikować bez
// zgadywania, i które zakłada inną, bardziej modularną architekturę niż
// nasz pojedynczy plik .ino. Zamiast zgadywać: logujemy, które kanały
// zniknęły na serwerze, a Ty ręcznie dopasowujesz odpowiedni slot
// w panelu WWW (odznaczasz "Cloud") i restartujesz.
class ConflictResolver : public Supla::Device::ChannelConflictResolver {
 public:
  bool onChannelConflictReport(
      uint8_t *channelReport,
      uint8_t channelReportSize,
      bool hasConflictInvalidType,
      bool hasConflictChannelMissingOnServer,
      bool hasConflictChannelMissingOnDevice) override {
    if (hasConflictChannelMissingOnDevice) {
      SUPLA_LOG_ERROR(
          "ConflictResolver: kana\u0142 brakuje na urz\u0105dzeniu -- "
          "nieodwracalne, przerywam.");
      return false;
    }
    if (hasConflictInvalidType) {
      SUPLA_LOG_ERROR(
          "ConflictResolver: niezgodny typ kana\u0142u -- nieodwracalne, "
          "przerywam.");
      return false;
    }
    if (hasConflictChannelMissingOnServer) {
      SUPLA_LOG_INFO(
          "ConflictResolver: serwer zg\u0142asza usuni\u0119te kana\u0142y "
          "(rozmiar raportu: %d).", channelReportSize);
      for (int i = 0; i < channelReportSize; i++) {
        if (channelReport[i] == 0) {
          SUPLA_LOG_INFO(
              " - kana\u0142 %d zosta\u0142 usuni\u0119ty po stronie Cloud "
              "-- dopasuj odpowiedni slot w panelu WWW (odznacz \"Cloud\") "
              "i zrestartuj.", i);
        }
      }
    }
    return false;
  }
};

ConflictResolver channelConflictResolver;

// --- Piny KC868-A6 (I2C do PCF8574, tak jak w wersji custom) ---
constexpr uint8_t SDA_ = 4;
constexpr uint8_t SCL_ = 15;
#define BOOT_BUTTON_PIN 0
#define RS485_RXD_PIN 14
#define RS485_TXD_PIN 27

PCF8574 outPcf_(0x24);  // 6 wyjść przekaźnikowych
PCF8574 inPcf_(0x22);   // 6 wejść (przyciski ścienne)

// --- Warstwa Io: most między Supla::Control::Relay/Button a PCF8574 ---
class Kc868A6RelayIo : public Supla::Io::Base {
 public:
  explicit Kc868A6RelayIo(PCF8574 &pcfRef) : Supla::Io::Base(), pcf(pcfRef) {}
  void customPinMode(int channelNumber, uint8_t pin, uint8_t mode) override {}
  void customDigitalWrite(int channelNumber, uint8_t pin, uint8_t val) override {
    pcf.digitalWrite(pin, val);
  }
  int customDigitalRead(int channelNumber, uint8_t pin) override {
    return pcf.digitalRead(pin);
  }
 private:
  PCF8574 &pcf;
};

class Kc868A6ButtonIo : public Supla::Io::Base {
 public:
  explicit Kc868A6ButtonIo(PCF8574 &pcfRef) : Supla::Io::Base(), pcf(pcfRef) {}
  void customPinMode(int channelNumber, uint8_t pin, uint8_t mode) override {}
  void customDigitalWrite(int channelNumber, uint8_t pin, uint8_t val) override {}
  int customDigitalRead(int channelNumber, uint8_t pin) override {
    return pcf.digitalRead(pin);
  }
 private:
  PCF8574 &pcf;
};

Kc868A6RelayIo relayIo(outPcf_);
Kc868A6ButtonIo buttonIo(inPcf_);

inline uint8_t channelToInputPin(uint8_t channel) {
  return 5 - channel;
}

String urlEncode(const String &s) {
  String out = "";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// --- Warstwa sieciowa i konfiguracja (oficjalna architektura) ---
Supla::ESP32Wifi suplaWifi;
Supla::LittleFsConfig configSupla;     // przechowywanie konfiguracji (LittleFS)
Supla::EspWebServer suplaServer;       // wbudowany serwer WWW trybu konfiguracji

// Bloki HTML panelu konfiguracyjnego (kolejność tworzenia = kolejność na stronie)
Supla::Html::DeviceInfo htmlDeviceInfo(&SuplaDevice);
Supla::Html::WifiParameters htmlWifi;
Supla::Html::ProtocolParameters htmlProto;
Supla::Modbus::Configurator modbusConfigurator;
// Supla::Html::ModbusParameters USUNIĘTE z głównej strony -- rola/baudrate/
// adres własny przeniesione na /modbus-devices razem z tabelą urządzeń,
// żeby wszystko dotyczące Modbus było w jednym miejscu.
Supla::Html::RelayConfigButton htmlRelayConfigBtn;  // przycisk -> /relay-config (Przekaźniki i Przyciski)
Supla::Html::SensorControlButton htmlSensorControlBtn;  // przycisk -> /sensors-control (Kontrola i Odczyt Czujników)
Supla::Html::UpdateParameter htmlUpdateButton;  // przycisk "Aktualizacja Firmware" -> /update (HTTPUpdateServer)

Supla::Control::Relay* relays_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
Supla::Control::RelayRollerShutterPair* pairs_[3] = {nullptr, nullptr, nullptr};

// Roleta vs Markiza (kierunek) + Docelowa pozycja (%) dla każdej z 3 par --
// zadeklarowane TU (wcześnie), bo używa ich applyRelayMask() poniżej.
// W przeciwieństwie do funkcji, zwykłe zmienne globalne NIE mają
// automatycznych prototypów w Arduino -- muszą być zadeklarowane przed
// pierwszym użyciem, dosłownie jak w zwykłym C++.
bool pairIsMarkiza[3] = {false, false, false};
uint8_t pairTargetPosition[3] = {100, 100, 100};  // 0=otwarta/schowana, 100=zamknięta/wysunięta
// Zapamiętana pozycja sprzed reakcji automatyki -- do przywrócenia, gdy
// warunek ustąpi. valid=false oznacza "nic teraz nie zapamiętane" (para
// nie jest akurat w trakcie reakcji).
uint8_t pairSavedPosition[3] = {0, 0, 0};
bool pairSavedPositionValid[3] = {false, false, false};
Supla::Control::Button* physButtons[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
// Typ przycisku (0=Monostabilny, 1=Bistabilny) dla ka\u017cdego z 6 fizycznych
// kana\u0142\u00f3w -- niezale\u017cnie, bo w trybie Przeka\u017anik oba przyciski pary
// dzia\u0142aj\u0105 osobno. W trybie Roleta biblioteka prawdopodobnie i tak
// zarz\u0105dza przyciskami po swojemu (do zweryfikowania testem).
uint8_t buttonType[6] = {0, 0, 0, 0, 0, 0};
Supla::Control::Button* cfgButton = nullptr;
Supla::Control::VirtualRelay* webServerToggle = nullptr;  // wirtualny przełącznik: wł/wył lokalnego panelu WWW z poziomu aplikacji

// --- Modbus Master: odczyt do 5 zewnętrznych urządzeń (czujników/liczników)
// po RS485. Aktywne TYLKO gdy w panelu WWW (blok ModbusParameters) wybrana
// jest rola Master + tryb RTU -- to jest niezależne od roli Slave, którą
// obsługuje wbudowany Configurator (tam ustawia się TYLKO adres WŁASNY
// urządzenia jako Slave; tu ustawiamy adresy urządzeń, które MY odpytujemy).
// Wspólny baudrate/stop-bity dla całej magistrali RS485 pochodzą z tego
// samego Configuratora (w wersji custom każde urządzenie miało swój
// "baudRate" w configu, ale i tak używany był tylko ten z urządzenia [0] --
// tu to naprawiamy, ustawiając jeden, wspólny baudrate dla całego portu,
// zgodnie z fizyką magistrali RS485).
ModbusMaster node;

// Wirtualny czujnik binarny (0/1) sterowany z kodu -- BinaryBase wymaga
// zaimplementowania czystej metody wirtualnej getValue(); zamiast czytać
// fizyczny pin (jak w standardowych przykładach), zwracamy naszą własną
// zmienną "state", ustawianą w readSensorsControl() na podstawie odczytu
// z rejestru Modbus lub pinu ADC (próg ustawiany per-slot w panelu WWW).
namespace Supla {
namespace Sensor {
class ModbusBinarySensor : public BinaryBase {
 public:
  ModbusBinarySensor() {
    // Potwierdzone wprost w proto.h (ver. >= 27) -- dzięki temu kanał
    // rejestruje się w Cloud od razu jako "Czujnik binarny", bez żółtego
    // ostrzeżenia o nieskonfigurowanej funkcji.
    getChannel()->setDefault(SUPLA_CHANNELFNC_BINARY_SENSOR);
  }
  bool state = false;
  bool getValue() override {
    return state;
  }
};

// Kanał "Zbiornik" (Water Tank) -- wzorowane 1:1 na sprawdzonym przykładzie
// (klasa AnalogContainer od veeroos). Daje w Cloud natywny, wizualny
// wskaźnik poziomu zbiornika zamiast zwykłego wykresu pomiaru.
class TankLevelSensor : public Container {
 public:
  TankLevelSensor() {
    setInternalLevelReporting(true);
    getChannel()->setDefault(SUPLA_CHANNELFNC_WATER_TANK);
  }
  void setLevel(uint8_t value) {
    poziom = value;
  }
  int readNewValue() override {
    return poziom;
  }
 protected:
  uint8_t poziom = 0;
};
}  // namespace Sensor
}  // namespace Supla


// --- Kontrola Czujników: 5 stałych, nazwanych slotów (Wiatr/Opady/Woda/
// Nasłonecznienie/Dodatkowy), z których KAŻDY niezależnie może być
// odczytywany albo przez Modbus (RS485, tak jak w "Konfiguracja Modbus"),
// albo z czujnika analogowego (ADC) podłączonego bezpośrednio do ESP32.
// To osobna, dodatkowa pula kanałów Supla -- tworzona PO kanałach
// przekaźników i PO kanałach z "Konfiguracja Modbus", żeby nie zaburzyć
// już przydzielonej numeracji.
#define MAX_SENSOR_SLOTS 5
const char* const kSensorDefaultNames[MAX_SENSOR_SLOTS] = {
  "Czujnik Wiatru", "Czujnik Opad\u00f3w", "Czujnik Temperatury",
  "Czujnik Nat\u0119\u017cenia \u015awiat\u0142a", "Poziom Wody w Zbiorniku"
};

struct SensorSlot {
  bool enabled;
  bool cloudRegistered;
  char name[48];
  uint8_t source;      // 0 = Modbus (RS485), 1 = Analogowy (ADC)
  uint8_t slaveId;      // tylko dla źródła Modbus
  uint16_t reg;         // tylko dla źródła Modbus
  uint8_t funcCode;     // tylko dla źródła Modbus: 3 = Holding, 4 = Input
  uint8_t analogPin;    // tylko dla źródła Analogowy: numer GPIO (ADC)
  uint8_t oneWirePin;    // tylko dla źródła DS18B20: numer GPIO (1-Wire)
  int calibMin;    // tylko dla źródła Analogowy: surowy odczyt ADC odpowiadający 0%
  int calibMax;    // tylko dla źródła Analogowy: surowy odczyt ADC odpowiadający 100%
  float waterMinCm; // slot 5 RS485: 0% w cm, niezależnie od kalibracji ADC
  float waterMaxCm; // slot 5 RS485: 100% w cm
  int lastRaw;    // ostatni surowy odczyt ADC -- tylko do podglądu przy kalibracji, nie zapisywany
  float divider;
  uint8_t channelType;  // 0 = Pomiar, 1 = Binarny (0/1)
  float threshold;
  uint32_t intervalMs;
  float lastValue;
  bool error;
  uint8_t consecutiveErrors;  // do wygaszania (backoff) częstotliwości prób przy uporczywym braku odpowiedzi
  unsigned long lastReadTime;
  Supla::Sensor::GeneralPurposeMeasurement *sensor;
  Supla::Sensor::ModbusBinarySensor *binSensor;
  Supla::Sensor::TankLevelSensor *tankSensor;
  Supla::Sensor::VirtualThermometer *thermoSensor;
  Supla::Sensor::DS18B20 *ds18b20;
};

SensorSlot sensorSlots[MAX_SENSOR_SLOTS];
uint32_t g_modbusBaudrate = 9600;
uint8_t g_modbusStopBits = 1;  // 1 albo 2
bool g_typeChangeAutoDisabled[MAX_SENSOR_SLOTS] = {false, false, false, false, false};
Preferences sensorPrefs;

void loadSensorSlots() {
  sensorPrefs.begin("sensorsctl", true);
  g_modbusBaudrate = sensorPrefs.getUInt("mb_baud", 9600);
  g_modbusStopBits = sensorPrefs.getUChar("mb_stop", 1);
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    String pfx = "s" + String(i) + "_";
    sensorSlots[i].enabled = sensorPrefs.getBool((pfx + "en").c_str(), false);
    sensorSlots[i].cloudRegistered = sensorPrefs.getBool((pfx + "cl").c_str(), false);
    String savedName = sensorPrefs.getString((pfx + "nm").c_str(), kSensorDefaultNames[i]);
    if (savedName.length() == 0) savedName = kSensorDefaultNames[i];  // zabezpieczenie przed jawnie pustą, zapisaną nazwą
    strncpy(sensorSlots[i].name, savedName.c_str(), sizeof(sensorSlots[i].name));
    // Domyślne źródło: slot 2 (Czujnik Temperatury) = DS18B20, slot 4
    // (Poziom Wody w Zbiorniku) = Analogowy.
    uint8_t defaultSource = (i == 2) ? 2 : 0;  // slot 4: woda Modbus RS485 (ADC nadal dostępne w WWW)
    sensorSlots[i].source = sensorPrefs.getUChar((pfx + "sc").c_str(), defaultSource);
    // Domyślne adresy Modbus: Wiatr(0)=3, Opady(1)=1, pozostałe kolejno.
    uint8_t defaultSlaveId;
    if (i == 0) defaultSlaveId = 3;
    else if (i == 1) defaultSlaveId = 1;
    else if (i == 2) defaultSlaveId = 2;
    else defaultSlaveId = i + 2;
    sensorSlots[i].slaveId = sensorPrefs.getUChar((pfx + "id").c_str(), i == 4 ? 2 : defaultSlaveId);
    sensorSlots[i].reg = sensorPrefs.getUShort((pfx + "rg").c_str(), i == 4 ? 4 : 0);
    sensorSlots[i].funcCode = sensorPrefs.getUChar((pfx + "fc").c_str(), i == 4 ? 4 : 3);
    sensorSlots[i].analogPin = sensorPrefs.getUChar((pfx + "ap").c_str(), 36);
    uint8_t defaultOneWirePin = (i == 2) ? 33 : 0;
    sensorSlots[i].oneWirePin = sensorPrefs.getUChar((pfx + "ow").c_str(), defaultOneWirePin);
    sensorSlots[i].calibMin = sensorPrefs.getInt((pfx + "cmn").c_str(), i == 4 ? 0 : 1);
    sensorSlots[i].calibMax = sensorPrefs.getInt((pfx + "cmx").c_str(), i == 4 ? 5000 : 4095);
    // Osobne ustawienia poziomu wody w cm; NIE nadpisują kalibracji ADC.
    sensorSlots[i].waterMinCm = sensorPrefs.getFloat((pfx + "wmin").c_str(), 0.0f);
    sensorSlots[i].waterMaxCm = sensorPrefs.getFloat((pfx + "wmax").c_str(), 85.0f);
    // Domyślny dzielnik dla slotu 0 (Czujnik Wiatru): 10.
    float defaultDivider = (i == 0 || i == 4) ? 10.0f : 1.0f;
    sensorSlots[i].divider = sensorPrefs.getFloat((pfx + "dv").c_str(), defaultDivider);
    // Domyślne wartości dla slotu 1 (Czujnik Opadów): typ Binarny, próg
    // 10.5 -- dla pozostałych slotów zostaje ogólny domyślny Pomiar/0.5.
    uint8_t defaultChannelType = (i == 1) ? 1 : (i == 4) ? 2 : (i == 2) ? 3 : 0;
    float defaultThreshold = (i == 1) ? 10.5f : 0.5f;
    sensorSlots[i].channelType = sensorPrefs.getUChar((pfx + "ct").c_str(), defaultChannelType);
    sensorSlots[i].threshold = sensorPrefs.getFloat((pfx + "th").c_str(), defaultThreshold);
    sensorSlots[i].intervalMs = sensorPrefs.getUInt((pfx + "iv").c_str(), 5000);
    sensorSlots[i].lastValue = 0.0f;
    sensorSlots[i].lastRaw = 0;
    sensorSlots[i].error = true;
    sensorSlots[i].consecutiveErrors = 0;
    sensorSlots[i].lastReadTime = 0;
    sensorSlots[i].sensor = nullptr;
    sensorSlots[i].binSensor = nullptr;
    sensorSlots[i].tankSensor = nullptr;
    sensorSlots[i].thermoSensor = nullptr;
    sensorSlots[i].ds18b20 = nullptr;
  }
  sensorPrefs.end();
}

bool doSaveSensorSlots() {
  auto srv = suplaServer.getServerPtr();
  sensorPrefs.begin("sensorsctl", false);
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) g_typeChangeAutoDisabled[i] = false;

  if (srv->hasArg("mb_baud")) {
    g_modbusBaudrate = srv->arg("mb_baud").toInt();
    g_modbusStopBits = srv->arg("mb_stop").toInt();
    sensorPrefs.putUInt("mb_baud", g_modbusBaudrate);
    sensorPrefs.putUChar("mb_stop", g_modbusStopBits);
  }

  // Przebieg 1: wczytaj wszystko do zmiennych tymczasowych i zwaliduj,
  // ZANIM cokolwiek zapiszemy -- w szczególności sprawdzamy, czy dwa sloty
  // źródła Analogowego nie wskazują na ten sam pin ADC (0 = "nieustawiony",
  // wiele slotów może mieć 0 jednocześnie bez konfliktu).
  bool cl[MAX_SENSOR_SLOTS], en[MAX_SENSOR_SLOTS];
  bool submitted[MAX_SENSOR_SLOTS];  // czy TEN slot faktycznie przyszedł w tym żądaniu
  String nm[MAX_SENSOR_SLOTS];
  uint8_t sc[MAX_SENSOR_SLOTS], id[MAX_SENSOR_SLOTS], fc[MAX_SENSOR_SLOTS];
  uint16_t rg[MAX_SENSOR_SLOTS];
  uint8_t ap[MAX_SENSOR_SLOTS], ow[MAX_SENSOR_SLOTS], ct[MAX_SENSOR_SLOTS];
  int cmn[MAX_SENSOR_SLOTS], cmx[MAX_SENSOR_SLOTS];
  float dv[MAX_SENSOR_SLOTS], th[MAX_SENSOR_SLOTS];
  uint32_t iv[MAX_SENSOR_SLOTS];

  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    String pfx = "s" + String(i) + "_";
    submitted[i] = srv->hasArg(pfx + "sc");
    if (!submitted[i]) {
      // Ten slot NIE by\u0142 cz\u0119\u015bci\u0105 tego \u017c\u0105dania -- wysy\u0142amy teraz tylko
      // JEDEN wybrany czujnik naraz (biblioteka WebServer ma sztywny limit
      // 32 argument\u00f3w POST). Nic dla niego NIE ZAPISUJEMY w og\u00f3le --
      // zostaje dok\u0142adnie to, co ju\u017c by\u0142o w pami\u0119ci trwa\u0142ej.
      continue;
    }
    cl[i] = srv->hasArg(pfx + "cl");
    en[i] = srv->hasArg(pfx + "en");  // niezależne od Cloud -- odczyt można testować przed rejestracją
    nm[i] = srv->arg(pfx + "nm");
    sc[i] = srv->arg(pfx + "sc").toInt();
    if (sc[i] == 0) {
      id[i] = srv->arg(pfx + "id").toInt();
      rg[i] = srv->arg(pfx + "rg").toInt();
    } else {
      // Źródło ≠ Modbus -- pole i tak pokazuje "0" w panelu, więc ZAMIAST
      // zapisać to 0 i zgubić adres, zachowujemy to, co już było
      // zapisane wcześniej (przyda się, gdy kiedyś wrócisz do Modbus).
      id[i] = sensorPrefs.getUChar((pfx + "id").c_str(), 1);
      rg[i] = sensorPrefs.getUShort((pfx + "rg").c_str(), 0);
    }
    fc[i] = srv->arg(pfx + "fc").toInt();
    ap[i] = srv->arg(pfx + "ap").toInt();
    ow[i] = srv->arg(pfx + "ow").toInt();
    // Nie niszcz istniejącej kalibracji analogowej, gdy jej pola są ukryte/wyłączone.
    cmn[i] = srv->hasArg(pfx + "cmn") ? srv->arg(pfx + "cmn").toInt()
                                     : sensorPrefs.getInt((pfx + "cmn").c_str(), sensorSlots[i].calibMin);
    cmx[i] = srv->hasArg(pfx + "cmx") ? srv->arg(pfx + "cmx").toInt()
                                     : sensorPrefs.getInt((pfx + "cmx").c_str(), sensorSlots[i].calibMax);
    if (cmx[i] == cmn[i]) cmx[i] = cmn[i] + 1;  // zabezpieczenie przed dzieleniem przez 0
    dv[i] = srv->arg(pfx + "dv").toFloat();
    ct[i] = srv->arg(pfx + "ct").toInt();
    th[i] = srv->arg(pfx + "th").toFloat();
    iv[i] = srv->arg(pfx + "iv").toInt();
    if (dv[i] == 0.0f) dv[i] = 1.0f;
    if (iv[i] == 0) iv[i] = 5000;

    // Auto-wyłączenie rejestracji przy zmianie Typu kanału -- Supla Cloud
    // nie pozwala zmienić typu istniejącego kanału (potwierdzone na forum:
    // "Supla nie pozwala na usuwanie lub zmianę typu kanału. Można jedynie
    // dodawać."). Jeśli kanał był już zarejestrowany i zmieniasz mu Typ,
    // automatycznie wyłączamy "Zarejestruj w Cloud" przy TYM zapisie --
    // musisz i tak zrobić restart, poczekać aż się połączy, i dopiero
    // wtedy ręcznie włączyć z powrotem (drugi restart) -- ale przynajmniej
    // pierwszy krok dzieje się sam, bez pamiętania o nim.
    bool wasCloudRegistered = sensorPrefs.getBool((pfx + "cl").c_str(), false);
    uint8_t oldChannelType = sensorPrefs.getUChar((pfx + "ct").c_str(), 0);
    if (wasCloudRegistered && cl[i] && ct[i] != oldChannelType) {
      cl[i] = false;
      g_typeChangeAutoDisabled[i] = true;
    }
  }

  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (!submitted[i] || sc[i] != 1 || ap[i] == 0) continue;  // tylko źródło Analogowy, tylko pin faktycznie ustawiony
    for (int j = 0; j < MAX_SENSOR_SLOTS; j++) {
      if (j == i || !submitted[j]) continue;
      if (sc[j] == 1 && ap[j] == ap[i]) {
        sensorPrefs.end();
        return false;  // konflikt -- ten sam pin ADC w dwóch slotach, nic nie zapisujemy
      }
    }
  }
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (!submitted[i] || sc[i] != 2 || ow[i] == 0) continue;  // tylko źródło DS18B20, tylko pin faktycznie ustawiony
    for (int j = 0; j < MAX_SENSOR_SLOTS; j++) {
      if (j == i || !submitted[j]) continue;
      if (sc[j] == 2 && ow[j] == ow[i]) {
        sensorPrefs.end();
        return false;  // konflikt -- ten sam pin 1-Wire w dwóch slotach, nic nie zapisujemy
      }
    }
  }

  // Przebieg 2: walidacja przeszła, zapisujemy TYLKO to, co faktycznie
  // przyszło w tym żądaniu -- reszta slotów zostaje całkowicie nietknięta.
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (!submitted[i]) continue;
    String pfx = "s" + String(i) + "_";
    sensorPrefs.putBool((pfx + "en").c_str(), en[i]);
    sensorPrefs.putBool((pfx + "cl").c_str(), cl[i]);
    sensorPrefs.putString((pfx + "nm").c_str(), nm[i]);
    sensorPrefs.putUChar((pfx + "sc").c_str(), sc[i]);
    sensorPrefs.putUChar((pfx + "id").c_str(), id[i]);
    sensorPrefs.putUShort((pfx + "rg").c_str(), rg[i]);
    sensorPrefs.putUChar((pfx + "fc").c_str(), fc[i]);
    sensorPrefs.putUChar((pfx + "ap").c_str(), ap[i]);
    sensorPrefs.putUChar((pfx + "ow").c_str(), ow[i]);
    sensorPrefs.putInt((pfx + "cmn").c_str(), cmn[i]);
    sensorPrefs.putInt((pfx + "cmx").c_str(), cmx[i]);
    if (i == 4 && sc[i] == 0 && srv->hasArg(pfx + "wmin") && srv->hasArg(pfx + "wmax")) {
      float lowCm = srv->arg(pfx + "wmin").toFloat();
      float highCm = srv->arg(pfx + "wmax").toFloat();
      // Bezpieczne ograniczenia: brak ujemnej wysokości i dzielenia przez zero.
      lowCm = constrain(lowCm, 0.0f, 500.0f);
      highCm = constrain(highCm, 0.0f, 500.0f);
      if (highCm > lowCm) {
        sensorPrefs.putFloat((pfx + "wmin").c_str(), lowCm);
        sensorPrefs.putFloat((pfx + "wmax").c_str(), highCm);
      }
    }
    sensorPrefs.putFloat((pfx + "dv").c_str(), dv[i]);
    sensorPrefs.putUChar((pfx + "ct").c_str(), ct[i]);
    sensorPrefs.putFloat((pfx + "th").c_str(), th[i]);
    sensorPrefs.putUInt((pfx + "iv").c_str(), iv[i]);
  }
  sensorPrefs.end();
  return true;
}

void saveSensorSlotsFromServer() {
  auto srv = suplaServer.getServerPtr();
  if (!doSaveSensorSlots()) {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
    html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
    html += ".btn{display:block;width:100%;max-width:420px;margin:12px auto;padding:14px;";
    html += "background:#000;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;";
    html += "text-align:center;text-decoration:none;cursor:pointer;box-sizing:border-box;}</style></head>";
    html += "<body><h1 style='color:#fff;'>SUPLA</h1>";
    html += "<div class='card'><h2>B\u0142\u0105d zapisu</h2>";
    html += "<p>Dwa czujniki ze \u017ar\u00f3d\u0142em Analogowym maj\u0105 ustawiony ten sam pin ADC -- "
            "NIC nie zosta\u0142o zapisane. Popraw numery pin\u00f3w (ka\u017cdy pin mo\u017ce by\u0107 "
            "u\u017cyty tylko w jednym slocie) i spr\u00f3buj ponownie.</p></div>";
    html += "<a class='btn' href='/sensors-control'>POWR\u00d3T</a>";
    html += "</body></html>";
    srv->send(200, "text/html", html);
    return;
  }
  String html2 = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html2 += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html2 += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
  html2 += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
  html2 += ".btn{display:block;width:100%;max-width:420px;margin:12px auto;padding:14px;";
  html2 += "background:#000;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;";
  html2 += "text-align:center;text-decoration:none;cursor:pointer;box-sizing:border-box;}</style></head>";
  html2 += "<body><h1 style='color:#fff;'>SUPLA</h1>";
  html2 += "<div class='card'><h2>Zapisano</h2>";
  html2 += "<p>Wszystkie zmiany na tej stronie (w tym dzielnik, progi, "
          "rejestracja kana\u0142\u00f3w Cloud) wymagaj\u0105 restartu, \u017ceby zacz\u0119\u0142y dzia\u0142a\u0107 -- "
          "mo\u017cesz zapisa\u0107 kilka czujnik\u00f3w po kolei i zrobi\u0107 jeden restart na ko\u0144cu. "
          "Po klikni\u0119ciu POWR\u00d3T ni\u017cej, na dole strony \"Ustawienia Czujnik\u00f3w\" "
          "znajdziesz przycisk \"Restart\" (osobny od \"Zapisz\").</p></div>";
  String autoDisabledMsg = "";
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (g_typeChangeAutoDisabled[i]) {
      autoDisabledMsg += String(sensorSlots[i].name) + "<br>";
    }
  }
  if (autoDisabledMsg.length() > 0) {
    html2 += "<div class='card' style='background:#fff3e0;'><h3>Uwaga</h3>"
             "<p>Wykryto zmian\u0119 Typu kana\u0142u dla ju\u017c zarejestrowanego czujnika -- "
             "Supla Cloud nie pozwala zmieni\u0107 typu istniej\u0105cego kana\u0142u, wi\u0119c "
             "automatycznie WY\u0141\u0104CZYLI\u015aMY \"Zarejestruj w Cloud\" dla:<br><b>" +
             autoDisabledMsg + "</b>Zrestartuj teraz, poczekaj a\u017c urz\u0105dzenie po\u0142\u0105czy "
             "si\u0119 z Cloud, a DOPIERO POTEM wr\u00f3\u0107 tutaj i w\u0142\u0105cz to z powrotem "
             "(to spowoduje kolejny, osobny restart).</p></div>";
  }
  html2 += "<a class='btn' href='/sensors-control'>POWR\u00d3T</a>";
  html2 += "</body></html>";
  srv->send(200, "text/html", html2);
}

void saveSensorSlotsAndRestart() {
  auto srv = suplaServer.getServerPtr();
  if (!doSaveSensorSlots()) {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
    html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
    html += ".btn{display:block;width:100%;max-width:420px;margin:12px auto;padding:14px;";
    html += "background:#000;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;";
    html += "text-align:center;text-decoration:none;cursor:pointer;box-sizing:border-box;}</style></head>";
    html += "<body><h1 style='color:#fff;'>SUPLA</h1>";
    html += "<div class='card'><h2>B\u0142\u0105d zapisu</h2>";
    html += "<p>Dwa czujniki ze \u017ar\u00f3d\u0142em Analogowym maj\u0105 ustawiony ten sam pin ADC -- "
            "NIC nie zosta\u0142o zapisane, urz\u0105dzenie NIE zosta\u0142o zrestartowane. Popraw numery "
            "pin\u00f3w i spr\u00f3buj ponownie.</p></div>";
    html += "<a class='btn' href='/sensors-control'>POWR\u00d3T</a>";
    html += "</body></html>";
    srv->send(200, "text/html", html);
    return;
  }
  // Wysyłamy treść z licznikiem BEZPOŚREDNIO jako odpowiedź na POST
  // (zamiast przekierowywać) -- przekierowanie próbowało pobrać stronę
  // Z URZĄDZENIA, które w tym momencie się restartuje, więc ta pierwsza
  // próba się nie udawała, a przeglądarka po cichu ponawiała ją w tle --
  // gdy w końcu się udało (po faktycznym restarcie), licznik pokazywał
  // się myląco "za późno", sugerując drugi restart, którego naprawdę
  // nie było.
  String autoDisabledNames = "";
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (g_typeChangeAutoDisabled[i]) {
      if (autoDisabledNames.length() > 0) autoDisabledNames += ", ";
      autoDisabledNames += String(sensorSlots[i].name);
    }
  }
  srv->send(200, "text/html", buildRestartCountdownHtml(autoDisabledNames));
  Serial.println("[diag] saveSensorSlotsAndRestart: wywolano ESP.restart() za chwile...");
  delay(300);
  ESP.restart();
}

String buildRestartCountdownHtml(const String &typeChangeWarning) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
  html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
  html += "#cnt{font-size:32px;font-weight:bold;margin:10px 0;}</style></head>";
  html += "<body><h1 style='color:#fff;'>SUPLA</h1><div class='card'>";
  html += "<h3 id='ttl'>Restartowanie urz\u0105dzenia...</h3>";
  html += "<div id='cnt'>15</div>";
  html += "<p id='msg'>Urz\u0105dzenie za chwil\u0119 si\u0119 zrestartuje, mo\u017ce to potrwa\u0107 do minuty. "
          "To okno mo\u017ce straci\u0107 po\u0142\u0105czenie -- je\u015bli tak si\u0119 stanie, znajdziesz "
          "urz\u0105dzenie pod jego zwyk\u0142ym adresem IP (np. w panelu routera) albo w aplikacji "
          "Supla. To bezpiecznie mo\u017cesz odswie\u017cy\u0107 (to zwyk\u0142a strona, nie wywo\u0142a "
          "kolejnego restartu).</p>";
  html += "<a id='backlink' href='/sensors-control' style='display:none;margin-top:10px;"
          "background:#000;color:#fff;padding:12px;border-radius:6px;text-decoration:none;"
          "font-weight:bold;text-align:center;'>POWR\u00d3T</a></div>";
  if (typeChangeWarning.length() > 0) {
    html += "<div class='card' style='background:#fff3e0;'><h3>Zmieniono typ kana\u0142u!</h3>"
            "<p>Supla Cloud nie pozwala zmieni\u0107 typu istniej\u0105cego kana\u0142u, nale\u017cy "
            "najpierw usun\u0105\u0107 stary kana\u0142, p\u00f3\u017aniej zarejestrowa\u0107 nowy.<br>"
            "Automatycznie wy\u0142\u0105czono \"Zarejestruj w Cloud\" dla: <b>" +
            typeChangeWarning +
            "</b>.<br>Najpierw usu\u0144 konflikt kana\u0142\u00f3w w Cloud, potem wr\u00f3\u0107 do "
            "\"Ustawienia Czujnik\u00f3w\" i w\u0142\u0105cz to z powrotem (spowoduje to kolejny, "
            "osobny restart).</p></div>";
  }
  html += "<script>";
  html += "var n=15;";
  html += "var t=setInterval(function(){";
  html += "n--; document.getElementById('cnt').textContent=n;";
  html += "if(n<=0){clearInterval(t);";
  html += "document.getElementById('cnt').style.display='none';";
  html += "document.getElementById('ttl').textContent='Gotowe';";
  html += "document.getElementById('msg').textContent='Urz\\u0105dzenie powinno by\\u0107 ju\\u017c z powrotem online w domowej sieci WiFi. Sprawd\\u017a w routerze lub w aplikacji Supla.';";
  html += "document.getElementById('backlink').style.display='block';";
  html += "}";
  html += "},1000);";
  html += "</script>";
  html += "</body></html>";
  return html;
}

void pageRestartCountdown() {
  auto srv = suplaServer.getServerPtr();
  srv->send(200, "text/html", buildRestartCountdownHtml(srv->arg("typeChangeWarning")));
}

void pageRestartDevice() {
  // Wysyłamy treść z licznikiem BEZPOŚREDNIO jako odpowiedź na POST
  // (zamiast przekierowywać na osobną stronę) -- przekierowanie próbowało
  // pobrać stronę Z URZĄDZENIA, które w tym momencie się restartuje, więc
  // ta pierwsza próba się nie udawała, a przeglądarka po cichu ponawiała
  // ją w tle -- gdy w końcu się udało (po faktycznym restarcie), licznik
  // pokazywał się myląco "za późno", sugerując drugi restart, którego
  // naprawdę nie było.
  auto srv = suplaServer.getServerPtr();
  srv->send(200, "text/html", buildRestartCountdownHtml(""));
  delay(300);
  ESP.restart();
}

// Odczyt 5 slotów -- Modbus wymaga roli Master+RTU (współdzieli magistralę
// RS485 z "Konfiguracja Modbus"); Analogowy działa zawsze, niezależnie od
// ustawień Modbus.
// --- Diagnostyka "zawieszeń" ---
// Mierzymy bezpośrednio, ile czasu zajmuje wywołanie odczytu Modbus (funkcje
// biblioteki ModbusMaster są BLOKUJĄCE -- czekają na odpowiedź lub timeout).
// Zapamiętujemy najdłuższy zaobserwowany czas i nazwę czujnika, którego
// dotyczył -- widoczne na stronie "Kontrola i Odczyt Czujników".
unsigned long maxLoopGapMs = 0;
String maxLoopGapContext = "";

void readSensorsControl() {
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (!sensorSlots[i].enabled) continue;
    if (sensorSlots[i].source == 2) {
      // DS18B20 czyta się sam (w swoim iterateAlways()) -- my tylko
      // podglądamy jego aktualną wartość na potrzeby naszego
      // podsumowania/statusu na stronie. Brak czujnika / błąd odczytu
      // sygnalizowany jest przez TEMPERATURE_NOT_AVAILABLE.
      if (sensorSlots[i].ds18b20) {
        float v = sensorSlots[i].ds18b20->getValue();
        sensorSlots[i].error = (v == TEMPERATURE_NOT_AVAILABLE);
        if (!sensorSlots[i].error) sensorSlots[i].lastValue = v;
      } else {
        sensorSlots[i].error = true;
      }
      continue;
    }
    // Backoff: przy kolejnych błędach z rzędu wydłużamy efektywny odstęp
    // między próbami (x2 za każdy błąd, maks. x16), żeby nie płacić co
    // chwilę 2-sekundowej kary czasu oczekiwania (timeout ModbusMaster)
    // za uporczywie niedostępne urządzenie. Po pierwszym sukcesie wraca
    // do normalnego tempa.
    uint32_t backoffMultiplier = 1UL << min(sensorSlots[i].consecutiveErrors, (uint8_t)4);
    uint32_t effectiveInterval = sensorSlots[i].intervalMs * backoffMultiplier;
    if (millis() - sensorSlots[i].lastReadTime >= effectiveInterval) {
      sensorSlots[i].lastReadTime = millis();
      float div = (sensorSlots[i].divider != 0.0f) ? sensorSlots[i].divider : 1.0f;
      if (sensorSlots[i].source == 0) {
        unsigned long t0 = millis();
        node.begin(sensorSlots[i].slaveId, Serial2);
        uint8_t res = (sensorSlots[i].funcCode == 4)
            ? node.readInputRegisters(sensorSlots[i].reg, 1)
            : node.readHoldingRegisters(sensorSlots[i].reg, 1);
        unsigned long elapsed = millis() - t0;
        if (elapsed > maxLoopGapMs) {
          maxLoopGapMs = elapsed;
          maxLoopGapContext = sensorSlots[i].name;
        }
        if (res == node.ku8MBSuccess) {
          // Zachowaj rzeczywisty surowy rejestr dla podglądu WWW.
          uint16_t rawModbus = node.getResponseBuffer(0);
          sensorSlots[i].lastRaw = (int)rawModbus;
          if (i == 4 && sensorSlots[i].channelType == 2) {
            // Przeliczanie cm według konfigurowalnego dzielnika w WWW.
            // RAW pozostaje nietknięty w WWW; do SUPLA trafia procent wysokości.
            float waterCm = (float)rawModbus / div;
            float span = sensorSlots[i].waterMaxCm - sensorSlots[i].waterMinCm;
            float percent = (span > 0.0f)
                ? 100.0f * (waterCm - sensorSlots[i].waterMinCm) / span
                : 0.0f;
            sensorSlots[i].lastValue = constrain(percent, 0.0f, 100.0f);
          } else {
            sensorSlots[i].lastValue = (float)rawModbus / div;
          }
          sensorSlots[i].error = false;
          sensorSlots[i].consecutiveErrors = 0;
        } else {
          sensorSlots[i].error = true;
          if (sensorSlots[i].consecutiveErrors < 250) sensorSlots[i].consecutiveErrors++;
        }
      } else {
        int raw = analogRead(sensorSlots[i].analogPin);
        sensorSlots[i].lastRaw = raw;
        float pct = 100.0f * (raw - sensorSlots[i].calibMin) /
                    (float)(sensorSlots[i].calibMax - sensorSlots[i].calibMin);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        sensorSlots[i].lastValue = pct;
        sensorSlots[i].error = false;
        sensorSlots[i].consecutiveErrors = 0;
      }
    }
    if (sensorSlots[i].sensor) {
      // Przy błędzie odczytu wysyłamy NAN zamiast zostawiać ostatnią/zerową
      // wartość -- Supla Cloud pokazuje wtedy "---" zamiast mylącego "0".
      sensorSlots[i].sensor->setValue(
          sensorSlots[i].error ? NAN : sensorSlots[i].lastValue);
    }
    if (sensorSlots[i].binSensor && !sensorSlots[i].error) {
      sensorSlots[i].binSensor->state = (sensorSlots[i].lastValue >= sensorSlots[i].threshold);
    }
    if (sensorSlots[i].tankSensor && !sensorSlots[i].error) {
      float lvl = sensorSlots[i].lastValue;
      if (lvl < 0) lvl = 0;
      if (lvl > 100) lvl = 100;
      sensorSlots[i].tankSensor->setLevel((uint8_t)lvl);
    }
    if (sensorSlots[i].thermoSensor) {
      sensorSlots[i].thermoSensor->setValue(
          sensorSlots[i].error ? TEMPERATURE_NOT_AVAILABLE : sensorSlots[i].lastValue);
    }
  }
}

// --- Kontrola Czujników: warunkowanie/automatyka dla przekaźników i rolet
// na podstawie progów czujników (Wiatr/Opady/Woda/Nasłonecznienie). To sam
// PANEL USTAWIEŃ -- w oryginalnej wersji custom taki panel istniał, ale
// (sprawdziłem cały plik) faktyczna logika porównania i wyzwalania
// przekaźników NIGDY nie została tam dopisana, tylko UI ustawień. Tutaj
// robimy to samo: ustawienia teraz, logikę uruchamiania (porównanie z
// wartością z "Odczyt Czujników" i sterowanie przekaźnikami) -- w kolejnym
// kroku, jeśli chcesz.
AutomationCfg windCfg = {false, 7.0f, 30, 0};
AutomationCfg rainCfg = {false, 0.0f, 60, 0};    // 60s = 1 min domyślnie
WaterAutomationCfg waterCfg = {false, 80.0f, 50.0f, 0};
AutomationCfg lightCfg = {false, 500.0f, 60, 0};
TempAutomationCfg tempCfg = {false, 30.0f, 1.0f, 0, 5.0f, 1.0f, 0};

String alarmMqttHost = "";
uint16_t alarmMqttPort = 1884;
MqttAlarmCfg windMqtt = {false, "Alarm/Wiatr"};
MqttAlarmCfg rainMqtt = {false, "Alarm/Opady"};
MqttAlarmCfg waterMqtt = {false, "Alarm/PoziomWody"};
// Dodatkowy alarm niskiego poziomu; progi wspólne z automatyką zbiornika.
MqttAlarmCfg waterLowMqtt = {false, "Alarm/PoziomWody_Niski"};
bool waterHighAlarmActive = false;
bool waterLowAlarmActive = false;
MqttAlarmCfg lightMqtt = {false, "Alarm/Swiatlo"};
MqttAlarmCfg tempHighMqtt = {false, "Alarm/Temperatura_Wysoka"};
MqttAlarmCfg tempLowMqtt = {false, "Alarm/Temperatura_Niska"};

Preferences autoPrefs;

void loadAutomationCfg() {
  autoPrefs.begin("automation", true);
  windCfg.enabled = autoPrefs.getBool("w_en", false);
  windCfg.threshold = autoPrefs.getFloat("w_th", 7.0f);
  windCfg.condSec = autoPrefs.getUInt("w_cs", 30);
  windCfg.relayMask = autoPrefs.getUChar("w_rm", 0);

  rainCfg.enabled = autoPrefs.getBool("r_en", false);
  rainCfg.condSec = autoPrefs.getUInt("r_cs", 60);
  rainCfg.relayMask = autoPrefs.getUChar("r_rm", 0);

  waterCfg.enabled = autoPrefs.getBool("wa_en", false);
  waterCfg.maxLevel = autoPrefs.getFloat("wa_max", 80.0f);
  waterCfg.minLevel = autoPrefs.getFloat("wa_min", 50.0f);
  waterCfg.relayMask = autoPrefs.getUChar("wa_rm", 0);

  lightCfg.enabled = autoPrefs.getBool("l_en", false);
  lightCfg.threshold = autoPrefs.getFloat("l_th", 500.0f);
  lightCfg.condSec = autoPrefs.getUInt("l_cs", 60);
  lightCfg.relayMask = autoPrefs.getUChar("l_rm", 0);

  tempCfg.enabled = autoPrefs.getBool("t_en", false);
  tempCfg.highThreshold = autoPrefs.getFloat("t_ht", 30.0f);
  tempCfg.highHysteresis = autoPrefs.getFloat("t_hh", 1.0f);
  tempCfg.highRelayMask = autoPrefs.getUChar("t_hrm", 0);
  tempCfg.lowThreshold = autoPrefs.getFloat("t_lt", 5.0f);
  tempCfg.lowHysteresis = autoPrefs.getFloat("t_lh", 1.0f);
  tempCfg.lowRelayMask = autoPrefs.getUChar("t_lrm", 0);

  alarmMqttHost = autoPrefs.getString("mq_host", "");
  alarmMqttPort = (uint16_t)autoPrefs.getUInt("mq_port", 1884);

  windMqtt.enabled = autoPrefs.getBool("w_mq", false);
  windMqtt.topic = autoPrefs.getString("w_mt", "Alarm/Wiatr");
  rainMqtt.enabled = autoPrefs.getBool("r_mq", false);
  rainMqtt.topic = autoPrefs.getString("r_mt", "Alarm/Opady");
  waterMqtt.enabled = autoPrefs.getBool("wa_mq", false);
  waterMqtt.topic = autoPrefs.getString("wa_mt", "Alarm/PoziomWody");
  waterLowMqtt.enabled = autoPrefs.getBool("wal_mq", false);
  waterLowMqtt.topic = autoPrefs.getString("wal_mt", "Alarm/PoziomWody_Niski");
  lightMqtt.enabled = autoPrefs.getBool("l_mq", false);
  lightMqtt.topic = autoPrefs.getString("l_mt", "Alarm/Swiatlo");
  tempHighMqtt.enabled = autoPrefs.getBool("th_mq", false);
  tempHighMqtt.topic = autoPrefs.getString("th_mt", "Alarm/Temperatura_Wysoka");
  tempLowMqtt.enabled = autoPrefs.getBool("tl_mq", false);
  tempLowMqtt.topic = autoPrefs.getString("tl_mt", "Alarm/Temperatura_Niska");

  autoPrefs.end();
}


// --- Lokalny MQTT alarmów do tabletu (port 1884) ---
// Nie wymaga dodatkowej biblioteki MQTT. Używamy WiFiClient i minimalnego
// MQTT 3.1.1: CONNECT -> PUBLISH QoS0 -> DISCONNECT. Alarmy występują rzadko,
// więc krótkie połączenie przy zmianie stanu jest prostsze i niezależne od
// połączenia SUPLA Cloud.
void mqttWriteRemainingLength(WiFiClient &client, size_t value) {
  do {
    uint8_t encoded = value % 128;
    value /= 128;
    if (value > 0) encoded |= 0x80;
    client.write(encoded);
  } while (value > 0);
}

bool openAlarmMqtt(WiFiClient &client) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (alarmMqttHost.length() == 0 || alarmMqttPort == 0) return false;

  client.setTimeout(1000);
  if (!client.connect(alarmMqttHost.c_str(), alarmMqttPort)) {
    Serial.printf("[alarm-mqtt] Brak połączenia z %s:%u\n",
                  alarmMqttHost.c_str(), alarmMqttPort);
    return false;
  }

  String clientId = "KinconyA6-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFULL), HEX);
  size_t remaining = 10 + 2 + clientId.length();

  client.write((uint8_t)0x10);  // CONNECT
  mqttWriteRemainingLength(client, remaining);

  const uint8_t vh[] = {
    0x00, 0x04, 'M', 'Q', 'T', 'T',
    0x04,       // MQTT 3.1.1
    0x02,       // Clean Session
    0x00, 0x0F  // KeepAlive 15s
  };
  client.write(vh, sizeof(vh));
  client.write((uint8_t)(clientId.length() >> 8));
  client.write((uint8_t)(clientId.length() & 0xFF));
  client.write((const uint8_t*)clientId.c_str(), clientId.length());

  unsigned long start = millis();
  while (client.available() < 4 && client.connected() && millis() - start < 1000) {
    delay(1);
  }
  if (client.available() < 4) {
    client.stop();
    return false;
  }

  uint8_t connack[4];
  if (client.readBytes(connack, 4) != 4 ||
      connack[0] != 0x20 || connack[1] != 0x02 || connack[3] != 0x00) {
    client.stop();
    return false;
  }
  return true;
}

bool publishAlarmMqttRaw(const String &topic, bool active) {
  if (topic.length() == 0) return false;

  WiFiClient client;
  if (!openAlarmMqtt(client)) return false;

  const char *payload = active ? "ON" : "OFF";
  size_t topicLen = topic.length();
  size_t payloadLen = strlen(payload);
  size_t remaining = 2 + topicLen + payloadLen;

  client.write((uint8_t)0x30);  // PUBLISH QoS0, retain=0
  mqttWriteRemainingLength(client, remaining);
  client.write((uint8_t)(topicLen >> 8));
  client.write((uint8_t)(topicLen & 0xFF));
  client.write((const uint8_t*)topic.c_str(), topicLen);
  client.write((const uint8_t*)payload, payloadLen);
  client.flush();

  // Grzeczne rozłączenie MQTT.
  const uint8_t disconnectPacket[2] = {0xE0, 0x00};
  client.write(disconnectPacket, sizeof(disconnectPacket));
  client.flush();
  delay(2);
  client.stop();

  Serial.printf("[alarm-mqtt] %s = %s\n", topic.c_str(), payload);
  return true;
}

void publishAlarmMqttState(const MqttAlarmCfg &cfg, bool active) {
  if (!cfg.enabled) return;
  publishAlarmMqttRaw(cfg.topic, active);
}

// --- Rzeczywista logika warunkowania i sterowania przekaźnikami ---
// Dla każdego czujnika: jeśli warunek (próg przekroczony / deszcz wykryty)
// utrzymuje się NIEPRZERWANIE przez skonfigurowany "Czas Warunkowania",
// włączamy wybrane przekaźniki. Gdy warunek ustąpi -- wyłączamy je z
// powrotem (automatyczny powrót do normalnego stanu). Jeśli w trakcie
// odliczania warunek zniknie, zegar się resetuje (musi być NIEPRZERWANY).
AutomationState windState, rainState, waterState, lightState;

void applyRelayMask(uint8_t mask, bool on) {
  bool pairHandled[3] = {false, false, false};
  for (int r = 0; r < 6; r++) {
    if (!(mask & (1 << r))) continue;
    int p = r / 2;
    if (pairModeLabel(r) == "Roleta") {
      if (pairHandled[p]) continue;  // ta sama para już obsłużona (drugi CH tej samej pary)
      pairHandled[p] = true;
      if (pairs_[p] == nullptr) continue;
      auto rs = pairs_[p]->getRollerShutter();
      if (rs == nullptr) continue;
      if (on) {
        // Zapamiętaj pozycję TYLKO jeśli jeszcze nic nie zapamiętano dla tej
        // pary -- chroni przed nadpisaniem oryginalnej pozycji, gdyby druga
        // reguła automatyki aktywowała się na tej samej parze w trakcie,
        // gdy pierwsza już trwa (znany, uproszczony przypadek: przywrócenie
        // nast\u0105pi dopiero, gdy TA reguła, kt\u00f3ra pierwsza zareagowała, ustąpi
        // -- nie ma tu liczenia, ile reguł jednocześnie trzyma tę parę).
        if (!pairSavedPositionValid[p]) {
          pairSavedPosition[p] = rs->getCurrentPosition();
          pairSavedPositionValid[p] = true;
        }
        int target = pairTargetPosition[p];
        if (pairIsMarkiza[p]) target = 100 - target;
        rs->setTargetPosition(target);
      } else {
        if (pairSavedPositionValid[p]) {
          rs->setTargetPosition(pairSavedPosition[p]);
          pairSavedPositionValid[p] = false;
        } else {
          rs->setTargetPosition(0);  // brak zapamiętanej wartości -- bezpieczny domyślny powrót
        }
      }
    } else if (relays_[r] != nullptr) {
      if (on) {
        relays_[r]->turnOn();
      } else {
        relays_[r]->turnOff();
      }
    }
  }
}

void evaluateAutomation(const AutomationCfg &cfg, AutomationState &state,
                        bool conditionNow, const MqttAlarmCfg &mqttCfg) {
  if (!cfg.enabled) {
    if (state.actionActive) {
      applyRelayMask(cfg.relayMask, false);
      state.actionActive = false;
      publishAlarmMqttState(mqttCfg, false);
    }
    state.conditionSinceValid = false;
    return;
  }
  if (conditionNow) {
    if (!state.conditionSinceValid) {
      state.conditionSinceValid = true;
      state.conditionSince = millis();
    }
    if (!state.actionActive &&
        millis() - state.conditionSince >= (unsigned long)cfg.condSec * 1000UL) {
      applyRelayMask(cfg.relayMask, true);
      state.actionActive = true;
      publishAlarmMqttState(mqttCfg, true);
    }
  } else {
    state.conditionSinceValid = false;
    if (state.actionActive) {
      applyRelayMask(cfg.relayMask, false);
      state.actionActive = false;
      publishAlarmMqttState(mqttCfg, false);
    }
  }
}

// Sloty "Odczyt Czujników" w stałej kolejności: 0=Wiatr, 1=Opady, 2=Woda,
// 3=Nasłonecznienie (zgodnie z kSensorDefaultNames). Sensor w błędzie
// (error==true) traktujemy jako "warunek niespełniony" -- bezpieczny
// domyślny wybór, żeby nie działać na nieaktualnych/martwych danych.
void runAutomation() {
  bool windCond = !sensorSlots[0].error &&
                  sensorSlots[0].lastValue >= windCfg.threshold;
  evaluateAutomation(windCfg, windState, windCond, windMqtt);

  bool rainCond = false;
  if (!sensorSlots[1].error) {
    if (sensorSlots[1].binSensor) {
      rainCond = sensorSlots[1].binSensor->state;
    } else if (sensorSlots[1].sensor) {
      rainCond = sensorSlots[1].lastValue >= sensorSlots[1].threshold;
    }
  }
  evaluateAutomation(rainCfg, rainState, rainCond, rainMqtt);

  // Zbiornik -- prawdziwa histereza: włącz gdy poziom osiągnie Max, zostaw
  // włączone (nawet jeśli poziom trochę spadnie poniżej Max) aż spadnie
  // do Min -- bez sztucznego opóźniania czasem, sam poziom decyduje.
  if (!waterCfg.enabled) {
    if (waterState.actionActive) {
      applyRelayMask(waterCfg.relayMask, false);
      waterState.actionActive = false;
    }
    if (waterHighAlarmActive) {
      waterHighAlarmActive = false;
      publishAlarmMqttState(waterMqtt, false);
    }
    if (waterLowAlarmActive) {
      waterLowAlarmActive = false;
      publishAlarmMqttState(waterLowMqtt, false);
    }
  } else if (!sensorSlots[4].error) {
    float lvl = sensorSlots[4].lastValue;
    if (!waterState.actionActive && lvl >= waterCfg.maxLevel) {
      applyRelayMask(waterCfg.relayMask, true);
      waterState.actionActive = true;
    } else if (waterState.actionActive && lvl <= waterCfg.minLevel) {
      applyRelayMask(waterCfg.relayMask, false);
      waterState.actionActive = false;
    }

    // Alarmy MQTT niezależne od histerezy przekaźnika, ale używają
    // dokładnie tych samych progów Max/Min z webserwera.
    // W zakresie między Min i Max oba alarmy są wyłączone.
    bool highNow = lvl >= waterCfg.maxLevel;
    bool lowNow = lvl <= waterCfg.minLevel;
    if (highNow != waterHighAlarmActive) {
      waterHighAlarmActive = highNow;
      publishAlarmMqttState(waterMqtt, highNow);
    }
    if (lowNow != waterLowAlarmActive) {
      waterLowAlarmActive = lowNow;
      publishAlarmMqttState(waterLowMqtt, lowNow);
    }
  }

  bool lightCond = !sensorSlots[3].error &&
                   sensorSlots[3].lastValue >= lightCfg.threshold;
  evaluateAutomation(lightCfg, lightState, lightCond, lightMqtt);

  evaluateTempAutomation();
}

// Osobna logika dla Temperatury -- DWIE niezależne reguły ("powyżej progu"
// i "poniżej progu"), każda z własną histerezą i przekaźnikami. Sensor
// w błędzie -- nic nie zmieniamy (bezpieczne zachowanie na nieaktualnych
// danych, stan zostaje taki jak był).
bool tempHighActive = false;
bool tempLowActive = false;

void evaluateTempAutomation() {
  if (!tempCfg.enabled) {
    if (tempHighActive) {
      applyRelayMask(tempCfg.highRelayMask, false);
      tempHighActive = false;
      publishAlarmMqttState(tempHighMqtt, false);
    }
    if (tempLowActive) {
      applyRelayMask(tempCfg.lowRelayMask, false);
      tempLowActive = false;
      publishAlarmMqttState(tempLowMqtt, false);
    }
    return;
  }
  if (sensorSlots[2].error) return;
  float t = sensorSlots[2].lastValue;

  // Reguła "powyżej progu"
  if (!tempHighActive) {
    if (t >= tempCfg.highThreshold) {
      applyRelayMask(tempCfg.highRelayMask, true);
      tempHighActive = true;
      publishAlarmMqttState(tempHighMqtt, true);
    }
  } else {
    if (t <= tempCfg.highThreshold - tempCfg.highHysteresis) {
      applyRelayMask(tempCfg.highRelayMask, false);
      tempHighActive = false;
    }
  }

  // Reguła "poniżej progu"
  if (!tempLowActive) {
    if (t <= tempCfg.lowThreshold) {
      applyRelayMask(tempCfg.lowRelayMask, true);
      tempLowActive = true;
      publishAlarmMqttState(tempLowMqtt, true);
    }
  } else {
    if (t >= tempCfg.lowThreshold + tempCfg.lowHysteresis) {
      applyRelayMask(tempCfg.lowRelayMask, false);
      tempLowActive = false;
    }
  }
}

// Co 60 s ponownie wysyłamy bieżące stany skonfigurowanych alarmów.
// Dzięki temu po restarcie tabletu aktywny alarm wróci na listę bez
// konieczności czekania na kolejne przekroczenie progu.
void syncAlarmMqttStates() {
  static unsigned long lastSync = 0;
  if (millis() - lastSync < 60000UL) return;
  lastSync = millis();

  publishAlarmMqttState(windMqtt, windState.actionActive);
  publishAlarmMqttState(rainMqtt, rainState.actionActive);
  publishAlarmMqttState(waterMqtt, waterHighAlarmActive);
  publishAlarmMqttState(waterLowMqtt, waterLowAlarmActive);
  publishAlarmMqttState(lightMqtt, lightState.actionActive);
  publishAlarmMqttState(tempHighMqtt, tempHighActive);
  publishAlarmMqttState(tempLowMqtt, tempLowActive);
}

// Checkboxy wyboru przekaźników (CH1-CH6) dla jednego bloku czujnika
// Odczytuje aktualny tryb pary (Przekaźnik/Roleta) -- to ustawia się
// WYŁĄCZNIE w Supla Cloud (konfiguracja funkcji kanału), nie u nas lokalnie.
String pairModeLabel(int channel) {
  int p = channel / 2;
  if (pairs_[p] == nullptr) return "?";
  if (pairs_[p]->isInRollerShutterMode()) return "Roleta";
  if (pairs_[p]->isInRelayMode()) return "Przeka\u017anik";
  return "?";
}

String relayCheckboxesHtml(const String &namePrefix, uint8_t mask) {
  // Owijamy WSZYSTKO (siatkę + ewentualną notatkę) w jeden kontener
  // display:block, o pełnej szerokości -- .form-field (rodzic) jest
  // flex-row, więc bez tego siatka i notatka by\u0142yby dwoma OSOBNYMI
  // elementami flex, rywalizuj\u0105cymi o miejsce w jednym wierszu
  // (st\u0105d nak\u0142adanie/\u015bciskanie), zamiast normalnie uk\u0142ada\u0107 si\u0119
  // jedno pod drugim.
  String h = "<div style='width:100%;flex:1;min-width:0;'>";
  // Siatka 2 kolumny -- przy 6 elementach daje naturalnie 3 rzędy po 2,
  // czyli każdy rząd = jedna fizyczna para (CH1+CH2, CH3+CH4, CH5+CH6).
  h += "<div class='relgrid' style='grid-template-columns:repeat(2,1fr);'>";
  for (int r = 0; r < 6; r++) {
    String mode = pairModeLabel(r);
    bool isRoller = (mode == "Roleta");
    String label = isRoller ? (pairIsMarkiza[r / 2] ? "Markiza" : "Roleta") : ("P" + String(r + 1));
    int partner = (r % 2 == 0) ? r + 1 : r - 1;
    String partnerName = namePrefix + String(partner);
    h += "<span class='relitem'" + String(isRoller ? " style='color:#c62828;'" : "") + ">";
    h += "<label class='switch'><input type='checkbox' name='" + namePrefix + String(r) + "'";
    if (mask & (1 << r)) h += " checked";
    if (isRoller) {
      // Ta sama para (Roleta/Markiza) = jeden logiczny kanał -- zaznaczenie
      // jednego suwaka automatycznie zaznacza drugi, żeby nie wyglądało to
      // na dwa niezależne wybory, skoro i tak sterują tym samym.
      h += " onchange=\"document.getElementsByName('" + partnerName + "')[0].checked=this.checked;\"";
    }
    h += "><span class='slider'></span></label>";
    h += label + "</span>";
  }
  h += "</div>";
  if (mask != 0) {
    bool hasRollerSelected = false;
    for (int r = 0; r < 6; r++) {
      if ((mask & (1 << r)) && pairModeLabel(r) == "Roleta") hasRollerSelected = true;
    }
    if (hasRollerSelected) {
      h += "<div style='color:#666;font-size:12px;margin-top:10px;padding-top:8px;"
           "border-top:1px solid #e0e0e0;text-align:left;'>"
           "Wybrany kana\u0142 w trybie Roleta -- reakcja jedzie na docelow\u0105 pozycj\u0119 "
           "ustawion\u0105 w \"Przeka\u017aniki i Przyciski\" (nie w\u0142\u0105cza/wy\u0142\u0105cza "
           "jak zwyk\u0142y przeka\u017anik).</div>";
    }
  }
  h += "</div>";
  return h;
}

// Etykieta NA GÓRZE (pełna szerokość), a pod nią siatka suwaków (i ewentualna
// notatka o trybie Roleta) -- w przeciwieństwie do zwykłego .form-field
// (wiersz obok siebie), to całkowicie eliminuje jakiekolwiek dzielenie
// miejsca między etykietą a siatką, więc nic już nie może się nakładać.
String relayFieldHtml(const String &labelText, const String &namePrefix, uint8_t mask) {
  String h = "<div style='padding:10px 10px;border-top:1px solid #00d150;margin:0 -10px;'>";
  h += "<label style='color:#00d150;display:block;margin-bottom:8px;'>" + labelText + "</label>";
  h += relayCheckboxesHtml(namePrefix, mask);
  h += "</div>";
  return h;
}

void saveAlarmMqttTargetFromServer() {
  auto srv = suplaServer.getServerPtr();

  alarmMqttHost = srv->arg("mq_host");
  alarmMqttHost.trim();

  long mqttPortArg = srv->arg("mq_port").toInt();
  if (mqttPortArg < 1 || mqttPortArg > 65535) mqttPortArg = 1884;
  alarmMqttPort = (uint16_t)mqttPortArg;

  autoPrefs.begin("automation", false);
  autoPrefs.putString("mq_host", alarmMqttHost);
  autoPrefs.putUInt("mq_port", alarmMqttPort);
  autoPrefs.end();

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
  html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
  html += ".btn{display:block;width:100%;max-width:420px;margin:12px auto;padding:14px;";
  html += "background:#000;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;";
  html += "text-align:center;text-decoration:none;cursor:pointer;box-sizing:border-box;}</style></head>";
  html += "<body><h1 style='color:#fff;'>SUPLA</h1>";
  html += "<div class='card'><h2>Zapisano</h2>";
  html += "<p>Ustawienia połączenia dla alarmów MQTT zostały zapisane.</p></div>";
  html += "<a class='btn' href='/sensors-control'>POWRÓT</a>";
  html += "</body></html>";
  srv->send(200, "text/html", html);
}

void testAlarmMqttFromServer(bool active) {
  auto srv = suplaServer.getServerPtr();

  String testHost = srv->arg("mq_host");
  testHost.trim();
  long testPortArg = srv->arg("mq_port").toInt();
  if (testPortArg < 1 || testPortArg > 65535) testPortArg = 1884;
  uint16_t testPort = (uint16_t)testPortArg;

  String oldHost = alarmMqttHost;
  uint16_t oldPort = alarmMqttPort;

  alarmMqttHost = testHost;
  alarmMqttPort = testPort;
  bool ok = publishAlarmMqttRaw("Alarm/Test", active);

  alarmMqttHost = oldHost;
  alarmMqttPort = oldPort;

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
  html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
  html += ".ok{color:#2e7d32}.err{color:#c62828}";
  html += ".btn{display:block;width:100%;max-width:420px;margin:12px auto;padding:14px;";
  html += "background:#000;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;";
  html += "text-align:center;text-decoration:none;cursor:pointer;box-sizing:border-box;}</style></head>";
  html += "<body><h1 style='color:#fff;'>SUPLA</h1><div class='card'>";
  html += ok ? "<h2 class='ok'>Test wysłany</h2>" : "<h2 class='err'>Błąd połączenia</h2>";
  html += "<p>Temat: <b>Alarm/Test</b><br>Stan: <b>";
  html += active ? "ON" : "OFF";
  html += "</b></p>";
  if (ok) {
    html += "<p>Sprawdź aplikację Supla(t) na tablecie.</p>";
  } else {
    html += "<p>Nie udało się połączyć z podanym adresem IP i portem MQTT.</p>";
  }
  html += "</div><a class='btn' href='/sensors-control'>POWRÓT</a></body></html>";
  srv->send(200, "text/html", html);
}

void testAlarmMqttOnFromServer() {
  testAlarmMqttFromServer(true);
}

void testAlarmMqttOffFromServer() {
  testAlarmMqttFromServer(false);
}

void saveAutomationCfgFromServer() {
  auto srv = suplaServer.getServerPtr();

  windCfg.enabled = srv->hasArg("w_en");
  windCfg.threshold = max(0.0f, srv->arg("w_th").toFloat());
  windCfg.condSec = max(0L, srv->arg("w_cs").toInt());
  windCfg.relayMask = 0;
  for (int r = 0; r < 6; r++) if (srv->hasArg("w_r" + String(r))) windCfg.relayMask |= (1 << r);
  windMqtt.enabled = srv->hasArg("w_mq");
  windMqtt.topic = srv->arg("w_mt"); windMqtt.topic.trim();

  rainCfg.enabled = srv->hasArg("r_en");
  {
    float mins = max(0.5f, srv->arg("r_cm").toFloat());
    mins = round(mins * 2.0f) / 2.0f;  // zaokrąglenie do najbliższej wielokrotności 0,5
    rainCfg.condSec = (uint32_t)(mins * 60.0f);  // minuty -> sekundy
  }
  rainCfg.relayMask = 0;
  for (int r = 0; r < 6; r++) if (srv->hasArg("r_r" + String(r))) rainCfg.relayMask |= (1 << r);
  rainMqtt.enabled = srv->hasArg("r_mq");
  rainMqtt.topic = srv->arg("r_mt"); rainMqtt.topic.trim();

  waterCfg.enabled = srv->hasArg("wa_en");
  waterCfg.maxLevel = constrain(srv->arg("wa_max").toFloat(), 0.0f, 100.0f);
  waterCfg.minLevel = constrain(srv->arg("wa_min").toFloat(), 0.0f, 100.0f);
  if (waterCfg.minLevel >= waterCfg.maxLevel) waterCfg.minLevel = waterCfg.maxLevel - 1.0f;  // zabezpieczenie -- Min musi być mniejsze od Max
  waterCfg.relayMask = 0;
  for (int r = 0; r < 6; r++) if (srv->hasArg("wa_r" + String(r))) waterCfg.relayMask |= (1 << r);
  waterMqtt.enabled = srv->hasArg("wa_mq");
  waterMqtt.topic = srv->arg("wa_mt"); waterMqtt.topic.trim();
  waterLowMqtt.enabled = srv->hasArg("wal_mq");
  waterLowMqtt.topic = srv->arg("wal_mt"); waterLowMqtt.topic.trim();

  lightCfg.enabled = srv->hasArg("l_en");
  lightCfg.threshold = max(0.0f, srv->arg("l_th").toFloat());
  lightCfg.condSec = max(0L, srv->arg("l_cs").toInt());
  lightCfg.relayMask = 0;
  for (int r = 0; r < 6; r++) if (srv->hasArg("l_r" + String(r))) lightCfg.relayMask |= (1 << r);
  lightMqtt.enabled = srv->hasArg("l_mq");
  lightMqtt.topic = srv->arg("l_mt"); lightMqtt.topic.trim();

  tempCfg.enabled = srv->hasArg("t_en");
  tempCfg.highThreshold = srv->arg("t_ht").toFloat();
  tempCfg.highHysteresis = max(0.0f, srv->arg("t_hh").toFloat());
  tempCfg.highRelayMask = 0;
  for (int r = 0; r < 6; r++) if (srv->hasArg("t_hr" + String(r))) tempCfg.highRelayMask |= (1 << r);
  tempHighMqtt.enabled = srv->hasArg("th_mq");
  tempHighMqtt.topic = srv->arg("th_mt"); tempHighMqtt.topic.trim();

  tempCfg.lowThreshold = srv->arg("t_lt").toFloat();
  tempCfg.lowHysteresis = max(0.0f, srv->arg("t_lh").toFloat());
  tempCfg.lowRelayMask = 0;
  for (int r = 0; r < 6; r++) if (srv->hasArg("t_lr" + String(r))) tempCfg.lowRelayMask |= (1 << r);
  tempLowMqtt.enabled = srv->hasArg("tl_mq");
  tempLowMqtt.topic = srv->arg("tl_mt"); tempLowMqtt.topic.trim();

  autoPrefs.begin("automation", false);
  autoPrefs.putBool("w_en", windCfg.enabled);
  autoPrefs.putFloat("w_th", windCfg.threshold);
  autoPrefs.putUInt("w_cs", windCfg.condSec);
  autoPrefs.putUChar("w_rm", windCfg.relayMask);

  autoPrefs.putBool("r_en", rainCfg.enabled);
  autoPrefs.putUInt("r_cs", rainCfg.condSec);
  autoPrefs.putUChar("r_rm", rainCfg.relayMask);

  autoPrefs.putBool("wa_en", waterCfg.enabled);
  autoPrefs.putFloat("wa_max", waterCfg.maxLevel);
  autoPrefs.putFloat("wa_min", waterCfg.minLevel);
  autoPrefs.putUChar("wa_rm", waterCfg.relayMask);

  autoPrefs.putBool("l_en", lightCfg.enabled);
  autoPrefs.putFloat("l_th", lightCfg.threshold);
  autoPrefs.putUInt("l_cs", lightCfg.condSec);
  autoPrefs.putUChar("l_rm", lightCfg.relayMask);

  autoPrefs.putBool("t_en", tempCfg.enabled);
  autoPrefs.putFloat("t_ht", tempCfg.highThreshold);
  autoPrefs.putFloat("t_hh", tempCfg.highHysteresis);
  autoPrefs.putUChar("t_hrm", tempCfg.highRelayMask);
  autoPrefs.putFloat("t_lt", tempCfg.lowThreshold);
  autoPrefs.putFloat("t_lh", tempCfg.lowHysteresis);
  autoPrefs.putUChar("t_lrm", tempCfg.lowRelayMask);

  autoPrefs.putBool("w_mq", windMqtt.enabled);
  autoPrefs.putString("w_mt", windMqtt.topic);
  autoPrefs.putBool("r_mq", rainMqtt.enabled);
  autoPrefs.putString("r_mt", rainMqtt.topic);
  autoPrefs.putBool("wa_mq", waterMqtt.enabled);
  autoPrefs.putString("wa_mt", waterMqtt.topic);
  autoPrefs.putBool("wal_mq", waterLowMqtt.enabled);
  autoPrefs.putString("wal_mt", waterLowMqtt.topic);
  autoPrefs.putBool("l_mq", lightMqtt.enabled);
  autoPrefs.putString("l_mt", lightMqtt.topic);
  autoPrefs.putBool("th_mq", tempHighMqtt.enabled);
  autoPrefs.putString("th_mt", tempHighMqtt.topic);
  autoPrefs.putBool("tl_mq", tempLowMqtt.enabled);
  autoPrefs.putString("tl_mt", tempLowMqtt.topic);

  autoPrefs.end();

  // Restart NIE jest tu wymagany -- w przeciwieństwie do Modbus/Odczytu
  // Czujników, te ustawienia nie tworzą/usuwają żadnych kanałów Supla, tylko
  // sterują logiką w loop(), więc wystarczy je wczytać na bieżąco (jak
  // dopiszemy faktyczną logikę warunkowania, będzie czytać te zmienne
  // globalne bezpośrednio, bez potrzeby restartu).
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
  html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
  html += ".btn{display:block;width:100%;max-width:420px;margin:12px auto;padding:14px;";
  html += "background:#000;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;";
  html += "text-align:center;text-decoration:none;cursor:pointer;box-sizing:border-box;}</style></head>";
  html += "<body><h1 style='color:#fff;'>SUPLA</h1><div class='card'><h2>Zapisano</h2></div>";
  html += "<a class='btn' href='/sensors-control'>POWR\u00d3T</a>";
  html += "</body></html>";
  srv->send(200, "text/html", html);
}

// Zwraca aktualny stan czujników jako JSON -- używane przez skrypt na
// stronie "Kontrola i Odczyt Czujników" do cichego odświeżania samych
// wartości/statusów, bez przeładowania całej strony (i bez gubienia tego,
// co akurat edytujesz w formularzu).
void pageSensorsStatusJson() {
  String json = "{\"sensors\":[";
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (i > 0) json += ",";
    json += "{\"i\":" + String(i) + ",\"enabled\":" + String(sensorSlots[i].enabled ? "true" : "false") +
            ",\"error\":" + String(sensorSlots[i].error ? "true" : "false") +
            ",\"value\":" + String(sensorSlots[i].lastValue, 1) +
            ",\"raw\":" + String(sensorSlots[i].lastRaw) +
            ",\"divider\":" + String(sensorSlots[i].divider, 4) + "}";
  }
  json += "]}";
  suplaServer.getServerPtr()->send(200, "application/json", json);
}

// --- Przekaźniki i Przyciski: Roleta vs Markiza (kierunek reakcji
// automatyki) + Docelowa pozycja (%) dla każdej z 3 par. Nie tworzy/usuwa
// żadnych kanałów Supla -- to czyste flagi logiczne wykorzystywane w
// automatyce (Kontrola Czujników), więc NIE wymaga restartu.
Preferences relayPrefs;

void loadRelayConfig() {
  relayPrefs.begin("relaycfg", true);
  for (int p = 0; p < 3; p++) {
    pairIsMarkiza[p] = relayPrefs.getBool(("mk" + String(p)).c_str(), false);
    pairTargetPosition[p] = relayPrefs.getUChar(("tp" + String(p)).c_str(), 100);
  }
  for (int ch = 0; ch < 6; ch++) {
    uint8_t defaultBtnType = (ch >= 4) ? 1 : 0;  // CH5/CH6 domyślnie Bistabilny
    buttonType[ch] = relayPrefs.getUChar(("bt" + String(ch)).c_str(), defaultBtnType);
  }
  relayPrefs.end();
}

void doSaveRelayConfig() {
  auto srv = suplaServer.getServerPtr();
  relayPrefs.begin("relaycfg", false);
  for (int p = 0; p < 3; p++) {
    bool v = srv->hasArg("mk" + String(p));
    pairIsMarkiza[p] = v;
    relayPrefs.putBool(("mk" + String(p)).c_str(), v);
    int tp = srv->arg("tp" + String(p)).toInt();
    if (tp < 0) tp = 0;
    if (tp > 100) tp = 100;
    pairTargetPosition[p] = (uint8_t)tp;
    relayPrefs.putUChar(("tp" + String(p)).c_str(), pairTargetPosition[p]);
  }
  for (int ch = 0; ch < 6; ch++) {
    int bt = srv->arg("bt" + String(ch)).toInt();
    buttonType[ch] = (bt == 1) ? 1 : 0;
    relayPrefs.putUChar(("bt" + String(ch)).c_str(), buttonType[ch]);
  }
  relayPrefs.end();
}

void saveRelayConfigFromServer() {
  doSaveRelayConfig();
  auto srv = suplaServer.getServerPtr();

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
  html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
  html += ".btn{display:block;width:100%;max-width:420px;margin:12px auto;padding:14px;";
  html += "background:#000;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;";
  html += "text-align:center;text-decoration:none;cursor:pointer;box-sizing:border-box;}</style></head>";
  html += "<body><h1 style='color:#fff;'>SUPLA</h1><div class='card'><h2>Zapisano</h2>";
  html += "<p>Markiza i Docelowa pozycja dzia\u0142aj\u0105 od razu.</p></div>";
  html += "<a class='btn' href='/relay-config'>POWR\u00d3T</a>";
  html += "</body></html>";
  srv->send(200, "text/html", html);
}

void saveRelayConfigAndRestart() {
  doSaveRelayConfig();
  auto srv = suplaServer.getServerPtr();

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
  html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
  html += "#cnt{font-size:32px;font-weight:bold;margin:10px 0;}</style></head>";
  html += "<body><h1 style='color:#fff;'>SUPLA</h1><div class='card'>";
  html += "<h3>Zapisano. Restartowanie urz\u0105dzenia...</h3>";
  html += "<div id='cnt'>15</div>";
  html += "<p>Urz\u0105dzenie za chwil\u0119 si\u0119 zrestartuje. Je\u015bli to okno straci "
          "po\u0142\u0105czenie, znajdziesz urz\u0105dzenie pod jego zwyk\u0142ym adresem IP "
          "(np. w panelu routera) albo w aplikacji Supla.</p></div>";
  html += "<script>";
  html += "var n=15;";
  html += "var t=setInterval(function(){";
  html += "n--; document.getElementById('cnt').textContent=n;";
  html += "if(n<=0){clearInterval(t);window.location.href='/';}";
  html += "}, 1000);";
  html += "</script>";
  html += "</body></html>";
  srv->send(200, "text/html", html);
  delay(300);
  ESP.restart();
}

void pageRelayConfig() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>*,*::before,*::after{box-sizing:border-box}";
  html += "body{font-family:\"Helvetica Neue\",Helvetica,Arial,sans-serif;line-height:1.5;";
  html += "font-size:16px;background:#00d151;color:#fff;margin:0;padding:20px 10px;}";
  html += "h1,h3{margin:10px 0;font-weight:300;}";
  html += ".box{background:#fff;color:#333;border-radius:10px;padding:10px;margin:10px 0;";
  html += "box-shadow:0 5px 6px rgba(0,0,0,.3);text-align:left;max-width:500px;margin-left:auto;margin-right:auto;}";
  html += ".box h3{margin:0 0 5px;font-size:17px;}";
  html += ".form-field{display:flex;align-items:center;padding:10px 10px;border-top:1px solid #00d150;";
  html += "margin:0 -10px;font-size:15px;}";
  html += ".box>.form-field:first-of-type{border-top:0;}";
  html += ".form-field>label:not(.switch){text-align:left;flex:1;}";
  html += "button{display:block;color:#fff;background:#000;text-align:center;width:100%;border:0;";
  html += "border-radius:10px;padding:12px;text-transform:uppercase;text-decoration:none;cursor:pointer;";
  html += "font-size:1.1em;margin-top:15px;box-shadow:0 5px 6px rgba(0,0,0,.3);}";
  html += "a.wide-link{display:block;color:#fff;background:#000;text-align:center;width:100%;";
  html += "border-radius:10px;padding:12px;text-transform:uppercase;text-decoration:none;";
  html += "font-size:1.1em;margin-top:15px;box-shadow:0 5px 6px rgba(0,0,0,.3);max-width:500px;";
  html += "margin-left:auto;margin-right:auto;box-sizing:border-box;}";
  html += ".switch{position:relative;display:inline-block;width:40px;height:22px;flex-shrink:0;";
  html += "align-self:center;margin:0 0 0 auto;}";
  html += ".switch input{opacity:0;width:0;height:0;}";
  html += ".switch .slider{position:absolute;cursor:pointer;inset:0;background-color:#ccc;";
  html += "transition:.3s;border-radius:22px;}";
  html += ".switch .slider:before{position:absolute;content:'';height:16px;width:16px;left:3px;bottom:3px;";
  html += "background-color:#fff;transition:.3s;border-radius:50%;}";
  html += ".switch input:checked+.slider{background-color:#00d151;}";
  html += ".switch input:checked+.slider:before{transform:translateX(18px);}";
  html += ".switch.disabled{opacity:.35;pointer-events:none;}";
  html += ".pair-card{border:1px solid #eee;border-radius:8px;padding:10px;margin:10px 0;}";
  html += ".pair-title{font-weight:bold;font-size:15px;margin-bottom:6px;}";
  html += ".badge{display:inline-block;padding:2px 10px;border-radius:12px;font-size:12px;";
  html += "font-weight:bold;margin-left:8px;}";
  html += ".badge.relay{background:#e3f2fd;color:#1565c0;}";
  html += ".badge.roller{background:#e8f5e9;color:#00d151;}";
  html += "</style></head><body>";
  html += "<div style='text-align:center;'>";
  html += SUPLA_LOGO_SVG;
  html += "</div><h1 style='text-align:center;color:#fff;'>" + String(DEVICE_DISPLAY_NAME) + "</h1>";

  html += "<div class='box'><h3 style='font-weight:bold;'>Roleta vs Markiza</h3>";
  html += "<p style='font-size:12px;color:#666;'>Tryb ka\u017cdej pary (Przeka\u017anik / Roleta) ustawia si\u0119 "
          "w <b>Supla Cloud</b> (funkcja kana\u0142u) -- tutaj tylko go widzisz. Prze\u0142\u0105cznik "
          "\"Odwr\u00f3cone sterowanie\" ma znaczenie WY\u0141\u0104CZNIE, gdy dana para jest akurat w trybie "
          "Roleta -- w trybie Przeka\u017anik jest wyszarzony i nieaktywny. Dla markizy \"reakcja\" "
          "automatyki oznacza schowanie, nie zamkni\u0119cie -- st\u0105d odwr\u00f3cony kierunek. Zmiany "
          "tutaj NIE wymagaj\u0105 restartu.</p>";
  html += "<form method='POST' action='/relay-config-save'>";
  for (int p = 0; p < 3; p++) {
    String mode = pairModeLabel(2 * p);
    bool isRoller = (mode == "Roleta");
    html += "<div class='pair-card'>";
    String badgeText = isRoller ? "Roleta/Markiza/Inne" : mode;
    html += "<div class='pair-title'>CH" + String(2 * p + 1) + " + CH" + String(2 * p + 2) +
            "<span class='badge " + String(isRoller ? "roller" : "relay") + "'>" + badgeText + "</span></div>";
    html += "<div class='form-field' style='border-top:0;margin:0;padding:6px 0;'>";
    html += "<label>Odwr\u00f3cone sterowanie (dla markizy)</label>";
    html += "<label class='switch" + String(isRoller ? "" : " disabled") + "'>"
            "<input type='checkbox' name='mk" + String(p) + "'" +
            (pairIsMarkiza[p] ? " checked" : "") + "><span class='slider'></span></label>";
    html += "</div>";
    html += "<div class='form-field' style='border-top:0;margin:0;padding:6px 0;" +
            String(isRoller ? "" : "opacity:.35;") + "'>";
    html += "<label>Docelowa pozycja przy reakcji (%)</label>";
    html += "<input type='number' min='0' max='100' name='tp" + String(p) + "' value='" +
            String(pairTargetPosition[p]) + "' style='width:70px;flex:none;'>";
    html += "</div>";
    html += "<p style='font-size:11px;color:#999;margin:4px 0 0;'>100% = pe\u0142na reakcja "
            "(roleta: zamkni\u0119cie, markiza z zaznaczonym \"Odwr\u00f3conym sterowaniem\": schowanie). "
            "0% = powr\u00f3t do otwartej/wysuni\u0119tej. Sprawd\u017a testem, czy zgadza si\u0119 to z "
            "fizycznym podpi\u0119ciem Twojego urz\u0105dzenia.</p></div>";
  }
  html += "<button type='submit'>Zapisz</button>";
  for (int ch = 0; ch < 6; ch++) {
    html += "<input type='hidden' name='bt" + String(ch) + "' value='" + String(buttonType[ch]) + "'>";
  }
  html += "</form></div>";

  html += "<div class='box'><h3 style='font-weight:bold;'>Typ Przycisku</h3>";
  html += "<p style='font-size:12px;color:#666;'>Monostabilny = zwyk\u0142y przycisk chwilowy.<br>"
          "Bistabilny = w\u0142\u0105cznik sta\u0142y.<br>"
          "Zmiany tutaj WYMAGAJ\u0104 restartu.</p>";
  html += "<form method='POST' action='/relay-config-save'>";
  for (int ch = 0; ch < 6; ch++) {
    html += "<div class='form-field'><label>Przycisk CH" + String(ch + 1) + "</label>";
    html += "<select name='bt" + String(ch) + "' style='font-size:18px;padding:10px 8px;border-radius:10px;'>";
    html += "<option value='0'" + String(buttonType[ch] == 0 ? " selected" : "") + ">Monostabilny</option>";
    html += "<option value='1'" + String(buttonType[ch] == 1 ? " selected" : "") + ">Bistabilny</option>";
    html += "</select></div>";
  }
  // Ukryte pola -- wysyłamy razem bieżące wartości Markizy/Pozycji, żeby
  // ten sam handler zapisu (który obsługuje oba formularze) ich nie wyzerował.
  for (int p = 0; p < 3; p++) {
    if (pairIsMarkiza[p]) {
      html += "<input type='hidden' name='mk" + String(p) + "' value='on'>";
    }
    html += "<input type='hidden' name='tp" + String(p) + "' value='" + String(pairTargetPosition[p]) + "'>";
  }
  html += "<button type='submit' formaction='/relay-config-save-restart' "
          "onclick=\"return confirm('Zapisa\u0107 i zrestartowa\u0107 urz\u0105dzenie teraz?');\">"
          "Zapisz i Restart</button></form></div>";
  html += "<a class='wide-link' href='/'>Powr\u00f3t</a>";
  html += "</body></html>";
  suplaServer.getServerPtr()->send(200, "text/html", html);
}

void pageSensorsControl() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  // Bez meta-refresh -- pełne przeładowanie strony co 30s zgarniało to, co
  // akurat edytowałeś. Zamiast tego: cichy odczyt wartości/statusu w tle
  // (AJAX, patrz skrypt na końcu strony), bez ruszania reszty formularza.
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  // Styl 1:1 z natywnego panelu SuplaDevice (skopiowany z rzeczywistego
  // źródła strony /) -- te same klasy, kolory, próg 530px, wygląd
  // przycisków, żeby nasze własne strony wyglądały identycznie jak reszta
  // panelu, a nie jak coś doklejonego z zewnątrz.
  html += "<style>*,*::before,*::after{box-sizing:border-box}";
  html += "body{font-family:\"Helvetica Neue\",Helvetica,Arial,sans-serif;line-height:1.5;";
  html += "font-size:16px;background:#00d151;color:#fff;margin:0;padding:20px 10px;}";
  html += "h1,h3{margin:10px 0;font-weight:300;}";
  html += ".box{background:#fff;color:#333;border-radius:10px;padding:10px;margin:10px 0;";
  html += "box-shadow:0 5px 6px rgba(0,0,0,.3);text-align:left;max-width:500px;margin-left:auto;margin-right:auto;}";
  html += ".box h3{margin:0 0 5px;font-size:17px;}";
  html += ".form-field{display:flex;align-items:center;padding:10px 10px;border-top:1px solid #00d150;";
  html += "margin:0 -10px;font-size:15px;}";
  html += ".box>.form-field:first-of-type{border-top:0;}";
  html += ".form-field>label:not(.switch){width:190px;text-align:left;white-space:nowrap;overflow:hidden;";
  html += "text-overflow:ellipsis;margin-right:10px;color:#00d150;flex-shrink:0;align-self:center;}";
  html += "@media (max-width:530px){.form-field{font-size:11px;padding:3px 3px;}";
  html += ".form-field>label:not(.switch){width:128px;font-size:10px;margin-right:2px;}";
  html += ".form-field input,.form-field select{font-size:10px;padding:2px 1px;}}";
  html += ".form-field>div{width:100%;}";
  html += ".form-field input:not([type=checkbox]),.form-field select{width:100%;flex:1;min-width:0;border:1px solid #ccc;";
  html += "border-radius:6px;padding:6px 4px;font-size:15px;background:#fff;}";
  html += ".form-field.checkbox label{width:100%;color:#333;display:flex;align-items:center;gap:8px;}";
  html += ".hint{font-size:.8em;opacity:.8;margin-top:2px;}";
  html += ".hint.warn{opacity:1;border-radius:6px;padding:4px 6px;background:#fff3cd;color:#5c4300;}";
  html += ".hint.error{opacity:1;border-radius:6px;padding:4px 6px;background:#f8d7da;color:#5c0008;}";
  html += "button{display:block;color:#fff;background:#000;text-align:center;width:100%;border:0;";
  html += "border-radius:10px;padding:12px;text-transform:uppercase;text-decoration:none;cursor:pointer;";
  html += "font-size:1.1em;margin-top:15px;box-shadow:0 5px 6px rgba(0,0,0,.3);}";
  html += "a.wide-link{display:block;color:#fff;background:#000;text-align:center;width:100%;";
  html += "border-radius:10px;padding:12px;text-transform:uppercase;text-decoration:none;";
  html += "font-size:1.1em;margin-top:15px;box-shadow:0 5px 6px rgba(0,0,0,.3);max-width:500px;";
  html += "margin-left:auto;margin-right:auto;box-sizing:border-box;}";
  html += "select{width:100%;padding:6px 8px;border:1px solid #ccc;border-radius:6px;background:#fff;font-size:15px;}";
  html += ".ok{color:#2e7d32;font-weight:bold;} .err{color:#c62828;font-weight:bold;}";
  // Suwaki (toggle switch) zamiast kwadracików -- ten sam mechanizm co
  // w natywnym panelu Supli. Wszędzie ten sam rozmiar. Kropka: po lewej
  // (start) gdy wyłączony, po prawej (koniec, na zielonym tle) gdy włączony.
  html += ".switch{position:relative;display:inline-block;width:40px;height:22px;flex-shrink:0;align-self:center;margin:0 0 0 auto;}";
  html += ".switch input{opacity:0;width:0;height:0;}";
  html += ".switch .slider{position:absolute;cursor:pointer;inset:0;background-color:#ccc;";
  html += "transition:.3s;border-radius:22px;}";
  html += ".switch .slider:before{position:absolute;content:'';height:16px;width:16px;left:3px;bottom:3px;";
  html += "background-color:#fff;transition:.3s;border-radius:50%;}";
  html += ".switch input:checked+.slider{background-color:#00d151;}";
  html += ".switch input:checked+.slider:before{transform:translateX(18px);}";
  // Kalibracja zbiornika: węższe pola liczbowe, więcej miejsca na pełne etykiety.
  // Zasięg wyłącznie na grupę kalibracji, nie zmienia wielkości przełączników.
  html += "#waterModbusCalibration .form-field>label:not(.switch){width:auto;flex:1;min-width:0;";
  html += "white-space:nowrap;overflow:visible;text-overflow:clip;margin-right:8px;font-size:12px;}";
  html += "#waterModbusCalibration .form-field>input[type=number]{width:105px;max-width:105px;flex:0 0 105px;}";
  html += "@media(max-width:530px){#waterModbusCalibration .form-field>label:not(.switch){font-size:10px;margin-right:3px;}";
  html += "#waterModbusCalibration .form-field>input[type=number]{width:74px;max-width:74px;flex:0 0 74px;}}";
  // Progi i Warunki: pelne etykiety bez obcinania, takze na malych ekranach.
  html += "#sensorAutomationForm .form-field>label:not(.switch){white-space:normal;overflow:visible;";
  html += "text-overflow:clip;overflow-wrap:break-word;min-width:0;line-height:1.35;}";
  // Przekaźniki reagujące -- 3 w wierszu na komputerze, 2 na wąskim ekranie
  // (żeby "CH1 (Roleta)" miało dość miejsca), etykieta może się zawinąć
  // zamiast wychodzić poza kolumnę.
  html += ".relgrid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px 6px;width:100%;flex:1;min-width:0;justify-items:center;}";
  html += ".relitem{display:flex;flex-direction:column;align-items:center;gap:2px;font-size:11px;line-height:1.2;}";
  html += ".relitem .switch{margin:0;}";
  html += "@media (max-width:530px){.relgrid{grid-template-columns:repeat(2,1fr);}}";
  html += "</style></head><body>";
  html += "<div style='text-align:center;'>";
  html += SUPLA_LOGO_SVG;
  html += "</div><h1 style='text-align:center;color:#fff;margin-bottom:10px;'>" + String(DEVICE_DISPLAY_NAME) + "</h1>";
  html += "<div style='text-align:center;color:#fff;font-size:13px;margin-bottom:20px;'>";
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    html += "<div>" + String(sensorSlots[i].name) + ": <b id='topstat" + String(i) + "'>";
    if (!sensorSlots[i].enabled) {
      html += "wy\u0142\u0105czony";
    } else if (sensorSlots[i].error) {
      html += "B\u0141\u0104D";
    } else {
      if (i == 4 && sensorSlots[i].source == 0) html += String(sensorSlots[i].lastRaw);
      else html += String(sensorSlots[i].lastValue, 1);
    }
    html += "</b></div>";
  }
  html += "</div>";

  // --- Sekcja 1: Ustawienia Czujników (źródło, adresy, dzielnik, typ kanału, Cloud) ---
  // Zamiast jednej szerokiej tabeli (13 kolumn) -- rozwijana lista wyboru
  // czujnika, a pod nią zwykłe, pojedyncze pola tylko DLA WYBRANEGO
  // czujnika (JS pokazuje/ukrywa odpowiedni blok). Wszystkie pola --
  // również te aktualnie ukryte -- i tak wysyłają się razem w jednym
  // formularzu przy zapisie (ukrycie przez CSS nie wyklucza pola z wysyłki).
  html += "<div class='box'><h3 style='font-size:20px;font-weight:bold;'>Ustawienia Czujnik\u00f3w</h3>";
  html += "<p style='font-size:12px;color:#666;'>"
          "Piny ADC na KC868-A6: <b>GPIO36, GPIO39, GPIO34, GPIO35</b>.<br>"
          "Piny 1-Wire na KC868-A6 (dla DS18B20): <b>GPIO32, GPIO33</b>.<br>"
          "(pozosta\u0142e piny zaj\u0119te przez I2C/RS485/RS232/SPI/LoRa).<br>"
          "Zmiany WYMAGAJ\u0104 Restartu!!!</p>";
  html += "<form method='POST' action='/sensors-control-save'>";
  html += "<div class='form-field'><label for='sensorPicker'>Wybierz Czujnik</label>";
  html += "<select id='sensorPicker' onchange=\"selectSensorPanel(this.value);\">";
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    html += "<option value='" + String(i) + "'>" + String(sensorSlots[i].name) + "</option>";
  }
  html += "</select></div>";

  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    String pfx = "s" + String(i) + "_";
    html += "<div class='sp' id='sp" + String(i) + "'" + String(i == 0 ? "" : " style='display:none;'") + ">";

    html += "<div class='form-field'><label>W\u0142\u0105cz Odczyt</label><label class='switch'><input type='checkbox' name='" + pfx + "en'" + (sensorSlots[i].enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
    html += "<div class='form-field'><label>Zarejestruj w Cloud</label><label class='switch'><input type='checkbox' name='" + pfx + "cl'" + (sensorSlots[i].cloudRegistered ? " checked" : "") + "><span class='slider'></span></label></div>";
    html += "<div class='form-field'><label>Nazwa Kana\u0142u</label><input type='text' name='" + pfx + "nm' value='" + String(sensorSlots[i].name) + "'></div>";
    html += "<div id='typeGroup" + String(i) + "' style='display:" + String(sensorSlots[i].source == 2 ? "none" : "block") + ";'>";
    html += "<div class='form-field'><label>Typ kana\u0142u</label><select name='" + pfx + "ct' id='ct" + String(i) + "' onchange=\"document.getElementById('binGroup" + String(i) + "').style.display=(this.value=='1')?'block':'none';\">";
    html += "<option value='0'" + String(sensorSlots[i].channelType == 0 ? " selected" : "") + ">KPOP</option>";
    html += "<option value='1'" + String(sensorSlots[i].channelType == 1 ? " selected" : "") + ">Binarny (0/1)</option>";
    html += "<option value='2'" + String(sensorSlots[i].channelType == 2 ? " selected" : "") + ">Zbiornik (poziom 0-100%)</option>";
    html += "<option value='3'" + String(sensorSlots[i].channelType == 3 ? " selected" : "") + ">Termometr</option>";
    html += "</select></div>";
    html += "<div id='binGroup" + String(i) + "' style='display:" + String(sensorSlots[i].channelType == 1 ? "block" : "none") + ";'>";
    html += "<div class='form-field'><label>Zmiana stanu (k. binarny)</label><input type='number' step='0.1' name='" + pfx + "th' value='" + String(sensorSlots[i].threshold, 1) + "'></div>";
    html += "</div>";
    html += "</div>";
    html += "<div class='form-field'><label>\u0179r\u00f3d\u0142o</label><select name='" + pfx + "sc' id='src" + String(i) + "' onchange='updateSrcFields(" + String(i) + ")'>";
    html += "<option value='0'" + String(sensorSlots[i].source == 0 ? " selected" : "") + ">Modbus (RS485)</option>";
    html += "<option value='1'" + String(sensorSlots[i].source == 1 ? " selected" : "") + ">Analogowy (ADC)</option>";
    html += "<option value='2'" + String(sensorSlots[i].source == 2 ? " selected" : "") + ">DS18B20 (1-Wire, GPIO32/33)</option>";
    html += "</select></div>";
    html += "<div id='modbusGroup" + String(i) + "' style='display:" + String(sensorSlots[i].source == 0 ? "block" : "none") + ";'>";
    html += "<div class='form-field'><label>Adres Slave (Modbus)</label><input type='number' min='0' max='247' name='" + pfx + "id' value='" + String(sensorSlots[i].source == 0 ? sensorSlots[i].slaveId : 0) + "'></div>";
    html += "<div class='form-field'><label>Funkcja (Modbus)</label><select name='" + pfx + "fc'>";
    html += "<option value='3'" + String(sensorSlots[i].funcCode == 3 ? " selected" : "") + ">Read Holding Registers (0x03)</option>";
    html += "<option value='4'" + String(sensorSlots[i].funcCode == 4 ? " selected" : "") + ">Read Input Registers (0x04)</option>";
    html += "</select></div>";
    html += "<div class='form-field'><label>Rejestr (Modbus)</label><input type='number' min='0' name='" + pfx + "rg' value='" + String(sensorSlots[i].source == 0 ? sensorSlots[i].reg : 0) + "'></div>";
    html += "<div class='form-field'><label>Baudrate (Modbus)</label><select name='mb_baud'>";
    {
      uint32_t bauds[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
      for (uint32_t b : bauds) {
        html += "<option value='" + String(b) + "'" + String(g_modbusBaudrate == b ? " selected" : "") + ">" + String(b) + "</option>";
      }
    }
    html += "</select></div>";
    html += "<div class='form-field'><label>Stop Bits (Modbus)</label><select name='mb_stop'>";
    html += "<option value='1'" + String(g_modbusStopBits == 1 ? " selected" : "") + ">1</option>";
    html += "<option value='2'" + String(g_modbusStopBits == 2 ? " selected" : "") + ">2</option>";
    html += "</select></div>";
    html += "</div>";
    html += "<div id='adcGroup" + String(i) + "' style='display:" + String(sensorSlots[i].source == 1 ? "block" : "none") + ";'>";
    html += "<div class='form-field'><label>Pin ADC</label><input type='number' min='0' max='39' name='" + pfx + "ap' value='" + String(sensorSlots[i].analogPin) + "'></div>";
    html += "<div class='form-field'><label>Kalibracja Min</label><input type='number' name='" + pfx + "cmn' value='" + String(sensorSlots[i].calibMin) + "'></div>";
    html += "<div class='form-field'><label>Kalibracja Max</label><input type='number' name='" + pfx + "cmx' value='" + String(sensorSlots[i].calibMax) + "'></div>";
    html += "</div>";
    html += "<div id='owGroup" + String(i) + "' style='display:" + String(sensorSlots[i].source == 2 ? "block" : "none") + ";'>";
    html += "<div class='form-field'><label>Pin 1-Wire</label><input type='number' min='0' max='39' name='" + pfx + "ow' value='" + String(sensorSlots[i].oneWirePin) + "'></div>";
    html += "</div>";
    html += "<div id='modbusOnlyGroup" + String(i) + "' style='display:" + String(sensorSlots[i].source == 0 ? "block" : "none") + ";'>";
    html += "<div class='form-field'><label>Scan Rate [ms]</label><input type='number' min='0' name='" + pfx + "iv' value='" + String(sensorSlots[i].intervalMs) + "'></div>";
    html += "<div class='form-field'><label>Dzielnik</label><input type='number' step='1' name='" + pfx + "dv' value='" + String((long)sensorSlots[i].divider) + "'></div>";
    html += "</div>";
    if (i == 4) {
      // Kalibracja umieszczona za Dzielnikiem, bez zmian w innych ustawieniach.
      // Nowe pola wyglądają jak pozostałe form-field. Kalibracja ADC nadal osobno.
      html += "<div id='waterModbusCalibration' style='display:" + String(sensorSlots[i].source == 0 ? "block" : "none") + ";'>";
      html += "<h3 style='border-top:2px solid #00d150;padding-top:10px;margin-top:0;'>Kalibracja Zbiornika</h3>";
      html += "<div class='form-field'><label>Max Wysokość Poziomu Wody (cm)</label><input type='number' min='0' max='500' step='0.1' name='" + pfx + "wmax' value='" + String(sensorSlots[i].waterMaxCm, 1) + "'></div>";
      html += "<div class='form-field'><label>Min Wysokość Poziomu Wody (cm)</label><input type='number' min='0' max='500' step='0.1' name='" + pfx + "wmin' value='" + String(sensorSlots[i].waterMinCm, 1) + "'></div>";
      html += "<div class='form-field'><label>Aktualny poziom wody (cm)</label><b id='watercm4'>--</b></div>";
      html += "</div>";
    }
    html += "</div>";
  }

  html += "<button type='submit' formaction='/sensors-control-save-restart' "
          "onclick=\"return confirm('Zapisa\u0107 i zrestartowa\u0107 urz\u0105dzenie teraz?');\">"
          "Zapisz i Restartuj</button>";
  html += "</form>";
  html += "<a class='wide-link' href='/'>Powr\u00f3t</a>";
  html += "</div>";
  // --- Sekcja 2: Progi i Warunki (progi, czas warunkowania, przekaźniki reagujące) ---
  // Ten sam wzorzec co "Odczyt" poniżej: rozwijana lista wyboru czujnika +
  // zwykłe, podpisane pola tylko dla wybranego.
  html += "<div class='box'><h3 style='font-size:20px;font-weight:bold;'>Progi i Warunki</h3>";
  html += "<p style='font-size:12px;color:#666;'>Zmiany Nie Wymagaj\u0105 Restartu.</p>";
  html += "<form id='sensorAutomationForm' method='POST' action='/sensor-automation-save'>";
  html += "<div class='form-field'><label for='ctrlPicker'>Wybierz Czujnik</label>";
  html += "<select id='ctrlPicker' onchange=\"document.querySelectorAll('.cp').forEach(function(e){e.style.display='none';});"
          "document.getElementById('cp'+this.value).style.display='block';\">";
  html += "<option value='0'>Czujnik Wiatru</option>";
  html += "<option value='1'>Czujnik Opad\u00f3w</option>";
  html += "<option value='4'>Czujnik Temperatury</option>";
  html += "<option value='3'>Czujnik Nat\u0119\u017cenia \u015awiat\u0142a</option>";
  html += "<option value='2'>Poziom Wody w Zbiorniku</option>";
  html += "</select></div>";

  html += "<div class='cp' id='cp0'>";
  html += "<div class='form-field'><label>W\u0142\u0105cz Kontrol\u0119</label><label class='switch'><input type='checkbox' name='w_en'" + String(windCfg.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Pr\u00f3g Pr\u0119dko\u015bci Wiatru (m/s)</label><input type='number' min='0' step='0.1' name='w_th' value='" + String(windCfg.threshold, 1) + "'></div>";
  html += "<div class='form-field'><label>Czas Warunkowania (sek)</label><input type='number' min='0' name='w_cs' value='" + String(windCfg.condSec) + "'></div>";
  html += "<div class='form-field'><label>Wyślij przez MQTT</label><label class='switch'><input type='checkbox' name='w_mq'" + String(windMqtt.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Temat MQTT</label><input type='text' name='w_mt' value='" + windMqtt.topic + "'></div>";
  html += relayFieldHtml("Przeka\u017aniki reaguj\u0105ce", "w_r", windCfg.relayMask);
  html += "</div>";

  html += "<div class='cp' id='cp1' style='display:none;'>";
  html += "<div class='form-field'><label>W\u0142\u0105cz Kontrol\u0119</label><label class='switch'><input type='checkbox' name='r_en'" + String(rainCfg.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Czas Warunkowania (min)</label><input type='number' min='0.5' step='0.5' name='r_cm' value='" + String(rainCfg.condSec / 60.0f, 1) + "'></div>";
  html += "<div class='form-field'><label>Wyślij przez MQTT</label><label class='switch'><input type='checkbox' name='r_mq'" + String(rainMqtt.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Temat MQTT</label><input type='text' name='r_mt' value='" + rainMqtt.topic + "'></div>";
  html += relayFieldHtml("Przeka\u017aniki reaguj\u0105ce", "r_r", rainCfg.relayMask);
  html += "</div>";

  html += "<div class='cp' id='cp2' style='display:none;'>";
  html += "<p style='font-size:12px;color:#666;'>Przeka\u017anik w\u0142\u0105czy si\u0119, gdy poziom wody "
          "osi\u0105gnie warto\u015b\u0107 ustawion\u0105 w Max Poziom Wody i pozostanie w\u0142\u0105czony, "
          "dop\u00f3ki poziom wody nie spadnie do warto\u015bci ustalonej w Min Poziom Wody.</p>";
  html += "<div class='form-field'><label>W\u0142\u0105cz Kontrol\u0119</label><label class='switch'><input type='checkbox' name='wa_en'" + String(waterCfg.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Max Poziom Wody (%)</label><input type='number' min='0' max='100' step='0.1' name='wa_max' value='" + String(waterCfg.maxLevel, 1) + "'></div>";
  html += "<div class='form-field'><label>Min Poziom Wody (%)</label><input type='number' min='0' max='100' step='0.1' name='wa_min' value='" + String(waterCfg.minLevel, 1) + "'></div>";
  html += "<div class='form-field'><label>Alarm wysoki poziom - MQTT</label><label class='switch'><input type='checkbox' name='wa_mq'" + String(waterMqtt.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Temat MQTT (wysoki)</label><input type='text' name='wa_mt' value='" + waterMqtt.topic + "'></div>";
  html += "<div class='form-field'><label>Alarm niski poziom - MQTT</label><label class='switch'><input type='checkbox' name='wal_mq'" + String(waterLowMqtt.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Temat MQTT (niski)</label><input type='text' name='wal_mt' value='" + waterLowMqtt.topic + "'></div>";
  html += relayFieldHtml("Przeka\u017aniki reaguj\u0105ce", "wa_r", waterCfg.relayMask);
  html += "</div>";

  html += "<div class='cp' id='cp3' style='display:none;'>";
  html += "<div class='form-field'><label>W\u0142\u0105cz Kontrol\u0119</label><label class='switch'><input type='checkbox' name='l_en'" + String(lightCfg.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Pr\u00f3g Nat\u0119\u017cenia \u015awiat\u0142a (lux)</label><input type='number' min='0' step='1' name='l_th' value='" + String(lightCfg.threshold, 0) + "'></div>";
  html += "<div class='form-field'><label>Czas Warunkowania (sek)</label><input type='number' min='0' name='l_cs' value='" + String(lightCfg.condSec) + "'></div>";
  html += "<div class='form-field'><label>Wyślij przez MQTT</label><label class='switch'><input type='checkbox' name='l_mq'" + String(lightMqtt.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Temat MQTT</label><input type='text' name='l_mt' value='" + lightMqtt.topic + "'></div>";
  html += relayFieldHtml("Przeka\u017aniki reaguj\u0105ce", "l_r", lightCfg.relayMask);
  html += "</div>";

  html += "<div class='cp' id='cp4' style='display:none;'>";
  html += "<div class='form-field'><label>W\u0142\u0105cz Kontrol\u0119</label><label class='switch'><input type='checkbox' name='t_en'" + String(tempCfg.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<p style='font-size:12px;color:#666;margin:10px 0 4px;font-weight:bold;'>Gdy temperatura POWY\u017bEJ progu:</p>";
  html += "<div class='form-field'><label>Pr\u00f3g \u2265 (\u00b0C)</label><input type='number' step='0.1' name='t_ht' value='" + String(tempCfg.highThreshold, 1) + "'></div>";
  html += "<div class='form-field'><label>Histereza \u2265 (\u00b0C)</label><input type='number' min='0' step='0.1' name='t_hh' value='" + String(tempCfg.highHysteresis, 1) + "'></div>";
  html += "<div class='form-field'><label>Wyślij przez MQTT</label><label class='switch'><input type='checkbox' name='th_mq'" + String(tempHighMqtt.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Temat MQTT</label><input type='text' name='th_mt' value='" + tempHighMqtt.topic + "'></div>";
  html += relayFieldHtml("Przeka\u017aniki \u2265", "t_hr", tempCfg.highRelayMask);
  html += "<p style='font-size:12px;color:#666;margin:14px 0 4px;font-weight:bold;'>Gdy temperatura PONI\u017bEJ progu:</p>";
  html += "<div class='form-field'><label>Pr\u00f3g \u2264 (\u00b0C)</label><input type='number' step='0.1' name='t_lt' value='" + String(tempCfg.lowThreshold, 1) + "'></div>";
  html += "<div class='form-field'><label>Histereza \u2264 (\u00b0C)</label><input type='number' min='0' step='0.1' name='t_lh' value='" + String(tempCfg.lowHysteresis, 1) + "'></div>";
  html += "<div class='form-field'><label>Wyślij przez MQTT</label><label class='switch'><input type='checkbox' name='tl_mq'" + String(tempLowMqtt.enabled ? " checked" : "") + "><span class='slider'></span></label></div>";
  html += "<div class='form-field'><label>Temat MQTT</label><input type='text' name='tl_mt' value='" + tempLowMqtt.topic + "'></div>";
  html += relayFieldHtml("Przeka\u017aniki \u2264", "t_lr", tempCfg.lowRelayMask);
  html += "</div>";

  html += "<button type='submit'>Zapisz</button></form></div>";

  // --- Sekcja 3: Alarmy Dla Innego Urządzenia ---
  html += "<div class='box'><h3 style='font-size:20px;font-weight:bold;'>Alarmy Dla Innego Urządzenia</h3>";
  html += "<p style='font-size:12px;color:#666;'>Opcjonalne wysyłanie stanów alarmowych przez MQTT do innego urządzenia w sieci lokalnej.</p>";
  html += "<form method='POST' action='/alarm-mqtt-target-save'>";
  html += "<div class='form-field'><label>Adres IP Urządzenia</label><input type='text' name='mq_host' value='" + alarmMqttHost + "' placeholder='np. 192.168.1.100'></div>";
  html += "<div class='form-field'><label>Port MQTT</label><input type='number' min='1' max='65535' name='mq_port' value='" + String(alarmMqttPort) + "'></div>";
  html += "<div style='font-size:12px;color:#666;text-align:left;'>Dla Supla(t) użyj portu 1884. Włączenie wysyłania i temat MQTT ustawiasz osobno przy każdej regule alarmowej poniżej.</div>";
  html += "<button type='submit'>Zapisz</button>";
  html += "<div style='display:flex;gap:8px;margin-top:10px;'>";
  html += "<button type='submit' formaction='/alarm-mqtt-test-on' style='width:50%;margin-top:0;background:#c62828;'>Test Alarm ON</button>";
  html += "<button type='submit' formaction='/alarm-mqtt-test-off' style='width:50%;margin-top:0;background:#555;'>Test Alarm OFF</button>";
  html += "</div>";
  html += "<div style='font-size:12px;color:#666;text-align:left;margin-top:8px;'>Test wysyła temat <b>Alarm/Test</b> z wartością ON albo OFF. Możesz testować także przed zapisaniem adresu IP.</div>";
  html += "</form></div>";

  html += "<script>";
  html += "function selectSensorPanel(v){";
  html += "document.querySelectorAll('.sp').forEach(function(e){";
  html += "var active=(e.id=='sp'+v);";
  html += "e.style.display=active?'block':'none';";
  html += "e.querySelectorAll('input,select').forEach(function(el){el.disabled=!active;});";
  html += "});";
  html += "if(v==4)updateSrcFields(4);";
  html += "}";
  html += "selectSensorPanel(document.getElementById('sensorPicker').value);";
  html += "function updateSrcFields(i){";
  html += "var v=document.getElementById('src'+i).value;";
  html += "document.getElementById('modbusGroup'+i).style.display=(v=='0')?'block':'none';";
  html += "document.getElementById('adcGroup'+i).style.display=(v=='1')?'block':'none';";
  html += "document.getElementById('owGroup'+i).style.display=(v=='2')?'block':'none';";
  html += "document.getElementById('typeGroup'+i).style.display=(v=='2')?'none':'block';";
  html += "document.getElementById('modbusOnlyGroup'+i).style.display=(v=='0')?'block':'none';";
  html += "if(i==4){var wc=document.getElementById('waterModbusCalibration');if(wc)wc.style.display=(v=='0')?'block':'none';}";
  html += "if(i==4){var wc=document.getElementById('waterModbusCalibration');if(wc)wc.querySelectorAll('input').forEach(function(e){e.disabled=(v!='0');});var adc=document.getElementById('adcGroup4');if(adc)adc.querySelectorAll('input').forEach(function(e){e.disabled=(v!='1');});}";
  html += "}";
  html += "function refreshStatus(){";
  html += "fetch('/sensors-status').then(function(r){return r.json();}).then(function(d){";
  html += "d.sensors.forEach(function(s){";
  html += "var top=document.getElementById('topstat'+s.i);";
  html += "if(s.i==4){var wc=document.getElementById('watercm4');if(wc)wc.textContent=(!s.enabled||s.error)?'--':(s.raw/(s.divider||1)).toFixed(1);}";
  html += "if(!top)return;";
  html += "if(!s.enabled){top.textContent='wy\u0142\u0105czony';}";
  html += "else if(s.error){top.textContent='B\u0141\u0104D';}";
  html += "else{top.textContent=(s.i==4&&document.getElementById('src4').value=='0')?s.raw:s.value;}";
  html += "});";
  html += "}).catch(function(){});";
  html += "}";
  html += "setInterval(refreshStatus,5000);";
  html += "</script>";
  html += "</body></html>";
  suplaServer.getServerPtr()->send(200, "text/html", html);
}

// Własna strona GET /update -- rejestrowana PRZED httpUpdater.setup(), żeby
// mieć pierwszeństwo nad domyślną stroną z biblioteki HTTPUpdateServer
// (ESPWebServer/WebServer dopasowuje handlery w kolejności rejestracji,
// pierwszy pasujący wygrywa). Formularz (name="update", POST na /update)
// jest zgodny z tym, czego oczekuje standardowy handler POST z
// HTTPUpdateServer -- ten sam, dobrze znany wzorzec używany od lat w
// przykładach ESP32 Update.h, więc wysyłka pliku nadal trafia do
// wbudowanej obsługi flashowania. Dopisany jest tylko przycisk "Powrót".
void pageUpdateCustom() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
  html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;text-align:left;}";
  html += "h2{margin-top:0;}";
  html += ".filebox{border:1px solid #ccc;border-radius:6px;padding:14px;background:#fafafa;margin-bottom:18px;text-align:center;}";
  html += "#fileInput{display:none;}";
  html += ".choosebtn{display:inline-block;padding:8px 16px;background:#eee;border:1px solid #bbb;";
  html += "border-radius:6px;cursor:pointer;font-size:14px;}";
  html += "#fname{display:block;margin-top:10px;font-size:13px;color:#333;word-break:break-all;white-space:normal;}";
  html += ".btn{display:block;width:100%;margin:12px 0 0 0;padding:14px;";
  html += "background:#000;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;";
  html += "text-align:center;text-decoration:none;cursor:pointer;box-sizing:border-box;}";
  html += "#progWrap{display:none;margin-top:14px;}";
  html += "#progBar{width:100%;height:22px;background:#eee;border-radius:6px;overflow:hidden;}";
  html += "#progFill{width:0%;height:100%;background:#00c853;transition:width .15s;}";
  html += "#progPct{text-align:center;margin-top:6px;font-weight:bold;}</style></head>";
  html += "<body><div style='text-align:center;'>";
  html += SUPLA_LOGO_SVG;
  html += "</div><h1 style='color:#fff;text-align:center;'>" + String(DEVICE_DISPLAY_NAME) + "</h1>";
  html += "<div class='card'><h2>Firmware Update</h2>";
  html += "<div class='filebox'>";
  html += "<input type='file' id='fileInput' "
          "onchange=\"document.getElementById('fname').textContent = "
          "this.files.length ? this.files[0].name : 'Nie wybrano pliku';\">";
  html += "<label for='fileInput' class='choosebtn'>Wybierz plik</label>";
  html += "<span id='fname'>Nie wybrano pliku</span></div>";
  html += "<div id='progWrap'><div id='progBar'><div id='progFill'></div></div>";
  html += "<div id='progPct'>0%</div></div>";
  html += "<button class='btn' type='button' onclick='doUpload()'>AKTUALIZACJA</button>";
  html += "<a class='btn' href='/'>POWR\u00d3T</a>";
  html += "</div>";
  html += "<script>";
  html += "function doUpload(){";
  html += "var input=document.getElementById('fileInput');";
  html += "if(!input.files.length){alert('Najpierw wybierz plik.');return;}";
  html += "var fd=new FormData();fd.append('update',input.files[0]);";
  html += "document.getElementById('progWrap').style.display='block';";
  html += "var xhr=new XMLHttpRequest();";
  html += "xhr.open('POST','/update',true);";
  html += "xhr.upload.onprogress=function(e){";
  html += "if(e.lengthComputable){";
  html += "var p=Math.round(e.loaded/e.total*100);";
  html += "document.getElementById('progFill').style.width=p+'%';";
  html += "document.getElementById('progPct').textContent=p+'%';";
  html += "}};";
  html += "xhr.onload=function(){document.open();document.write(xhr.responseText);document.close();};";
  html += "xhr.onerror=function(){alert('B\u0142\u0105d po\u0142\u0105czenia podczas wysy\u0142ania pliku.');};";
  html += "xhr.send(fd);";
  html += "}";
  html += "</script>";
  html += "</body></html>";
  suplaServer.getServerPtr()->send(200, "text/html", html);
}

// Obsługa samego przesyłania pliku (wywoływana wielokrotnie w trakcie
// uploadu -- START/WRITE/END) -- zastępuje to, co wcześniej robiła
// biblioteka HTTPUpdateServer za kulisami, żeby mieć pełną kontrolę nad
// stroną wynikową (poniżej).
void handleUpdateUpload() {
  HTTPUpload& upload = suplaServer.getServerPtr()->upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Aktualizacja: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("Aktualizacja OK: %u bajt\u00f3w\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

// Wywoływana PO zakończeniu uploadu -- wysyła stronę wyniku (z tym samym
// odliczaniem, co strona restartu) i restartuje urządzenie, jeśli
// aktualizacja się powiodła.
void handleUpdateResult() {
  auto srv = suplaServer.getServerPtr();
  bool ok = !Update.hasError();
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#00c853;font-family:sans-serif;text-align:center;padding:20px;}";
  html += ".card{background:#fff;max-width:420px;margin:20px auto;padding:20px;border-radius:8px;}";
  html += "#cnt{font-size:32px;font-weight:bold;margin:10px 0;}</style></head>";
  html += "<body><h1 style='color:#fff;'>SUPLA</h1><div class='card'>";
  if (ok) {
    html += "<h3 id='ttl'>Aktualizacja uda\u0142a si\u0119! Restart...</h3>";
    html += "<div id='cnt'>15</div>";
    html += "<p id='msg'>Urz\u0105dzenie restartuje si\u0119. Za chwil\u0119 spr\u00f3bujemy wr\u00f3ci\u0107 "
            "automatycznie do g\u0142\u00f3wnej strony -- je\u015bli si\u0119 nie uda (urz\u0105dzenie zmieni\u0142o "
            "sie\u0107), znajdziesz je pod jego zwyk\u0142ym adresem IP (np. w panelu routera) albo w aplikacji "
            "Supla.</p></div>";
    html += "<script>";
    html += "var n=15;";
    html += "var t=setInterval(function(){";
    html += "n--; document.getElementById('cnt').textContent=n;";
    html += "if(n<=0){clearInterval(t);";
    html += "window.location.href='/';";
    html += "}}, 1000);";
    html += "</script>";
  } else {
    html += "<h3>B\u0142\u0105d aktualizacji</h3>";
    html += "<p>Nie uda\u0142o si\u0119 wgra\u0107 nowego firmware. Urz\u0105dzenie NIE zosta\u0142o "
            "zrestartowane -- dzia\u0142a nadal na starej wersji. Spr\u00f3buj ponownie.</p>";
    html += "<a href='/update' style='display:inline-block;margin-top:10px;padding:10px 20px;";
    html += "background:#000;color:#fff;border-radius:6px;text-decoration:none;'>POWR\u00d3T</a></div>";
  }
  html += "</body></html>";
  srv->sendHeader("Connection", "close");
  srv->send(200, "text/html", html);
  if (ok) {
    delay(300);
    ESP.restart();
  }
}

// Zamienia wbudowany makro-znacznik kompilatora __DATE__ (format "Mmm dd yyyy",
// np. "Aug 25 2026") na nasz format "dd.mm.yyyy" -- dzięki temu data w
// WerOpr aktualizuje się SAMA przy każdej kompilacji, zamiast wymagać
// ręcznej edycji przed każdym wgraniem.
String buildDateDMY() {
  const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int mon = 1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(__DATE__, months[i], 3) == 0) {
      mon = i + 1;
      break;
    }
  }
  int day = atoi(&__DATE__[4]);
  char yearStr[5] = {__DATE__[7], __DATE__[8], __DATE__[9], __DATE__[10], '\0'};
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d.%02d.%s", day, mon, yearStr);
  return String(buf);
}

void setup() {
  Serial.begin(115200);

  // migrateSlot2Slot4Swap(); -- USUNIĘTE: spełniła jednorazowe zadanie
  // (zamiana slotów 2/4 po zmianie kolejności). Zostawiona WŁĄCZONA
  // szkodziła przy pełnym erase -- uruchamiała się na pustych danych
  // i zapisywała jawnie puste nazwy zamiast pozwolić zadziałać wartości
  // domyślnej.
  loadSensorSlots();
  loadAutomationCfg();
  loadRelayConfig();

  Wire.begin(SDA_, SCL_, 400000);
  Wire.setTimeOut(50);

  outPcf_.begin();
  for (int y = 0; y < 6; y++) {
    outPcf_.pinMode(y, OUTPUT);
    outPcf_.digitalWrite(y, HIGH);  // przekaźniki KC868 są aktywne stanem LOW
  }

  inPcf_.begin();
  for (int y = 0; y < 6; y++) {
    inPcf_.pinMode(5 - y, INPUT);
  }

  // 3 pary wyjść (CH1+CH2, CH3+CH4, CH5+CH6). Każda para domyślnie startuje
  // jako 2x zwykły przekaźnik (POWERSWITCH) -- identyczne zachowanie jak
  // wcześniej. Przełączenie dowolnej pary na 1x roletę odbywa się CAŁKOWICIE
  // z poziomu Supla Cloud (konfiguracja funkcji kanału) -- NIE mamy własnego
  // przełącznika w panelu WWW, biblioteka obsługuje to sama.
  //
  // relayFunctions -- ŚWIADOMIE ograniczone (zamiast domyślnego "wszystko
  // oprócz rolety") do trzech bezpiecznych funkcji: Włącznik zasilania,
  // Włącznik światła, Automat schodowy. Domyślna lista dopuszczała 15
  // opcji w Cloud, w tym funkcje IMPULSOWE (Brama, Furtka, Drzwi) --
  // nasza automatyka w "Kontrola Czujników" woła zwykłe turnOn()/
  // turnOff() zakładając trwały stan włącz/wyłącz, więc dla funkcji
  // impulsowych zachowywałaby się nieprzewidywalnie. Strona "roleta"
  // (tiltFunctionsEnabled=true) zostaje w pełni dostępna -- to
  // ograniczenie dotyczy tylko strony "zwykły przekaźnik".
  const _supla_int_t kSafeRelayFunctions =
      SUPLA_BIT_FUNC_POWERSWITCH | SUPLA_BIT_FUNC_LIGHTSWITCH |
      SUPLA_BIT_FUNC_STAIRCASETIMER;
  for (int p = 0; p < 3; p++) {
    int out0 = 2 * p;
    int out1 = 2 * p + 1;
    pairs_[p] = new Supla::Control::RelayRollerShutterPair(
        &relayIo, out0, out1, false /* highIsOn -- przekaźniki KC868 aktywne stanem LOW */,
        true /* tiltFunctionsEnabled */, kSafeRelayFunctions);
    // Jawne, stałe numery kanałów -- para p zawsze dostaje kanały (2p, 2p+1),
    // niezależnie od kolejności rejestracji/ponownej rejestracji w Cloud.
    // Wcześniej polegaliśmy na automatycznej numeracji, co przy pełnym
    // usunięciu i ponownej rejestracji urządzenia w Cloud dawało
    // nieprzewidywalny wynik. To NIE wymaga żadnej zmiany okablowania --
    // mapowanie fizycznych wyjść (CH1+2/3+4/5+6 -> para 0/1/2) zostaje
    // dokładnie takie samo jak było, tylko teraz jawnie ustalone.
    // UWAGA/TEST: dla kanału 0 (para 0, kanał główny) celowo NIE wołamy
    // setChannelNumber() -- potwierdzone w logu (nawet PO usunięciu
    // niezwiązanego konfliktu Opadów, kanały 0 i 1 nadal całkowicie
    // brakowały na liście rejestracji), że jawne ustawienie na ZERO
    // powoduje, że kanał w ogóle nie trafia na listę -- podczas gdy jawne
    // 2,3,4,5 działają normalnie. Zostawiamy ten JEDEN kanał na
    // automatycznej numeracji (i tak powinien dostać 0, będąc tworzony
    // jako pierwszy).
    if (out0 != 0) {
      pairs_[p]->getChannel()->setChannelNumber(out0);
    }
    pairs_[p]->getSecondaryChannel()->setChannelNumber(out1);
    // Domyślna funkcja dla WSZYSTKICH par -- świadomie zawsze bezpieczny
    // zwykły przełącznik, nigdy automatycznie Roleta/Markiza. Auto-domyślne
    // ustawienie na roletę/markizę byłoby ryzykowne -- świeżo zarejestrowany,
    // jeszcze niesprawdzony kanał mógłby dostać komendę "zamknij/otwórz"
    // zanim ktokolwiek zdąży to zweryfikować. Po pełnej ponownej rejestracji
    // w Cloud (usunięcie + dodanie od nowa) trzeba więc ręcznie przypisać
    // Markiza Tarasowa (para 0) i Roleta (para 1) -- to celowy kompromis
    // na rzecz bezpieczeństwa, nie przeoczenie.
    pairs_[p]->setDefaultFunctions(SUPLA_CHANNELFNC_POWERSWITCH,
                                   SUPLA_CHANNELFNC_POWERSWITCH);
    // UWAGA: świadomie setDefaultStateOff() zamiast setDefaultStateRestore().
    // Przywracanie stanu sprawiało, że przy starcie z zasilania 12V (gdzie
    // cewki przekaźników FAKTYCZNIE mają prąd, w przeciwieństwie do zasilania
    // z USB) kilka przekaźników próbowało się załączyć jednocześnie w
    // momencie, gdy napięcie 12V dopiero się stabilizuje -- skok poboru
    // prądu prawdopodobnie powodował chwilowy spadek napięcia i zawieszanie/
    // resetowanie się ESP32 (brownout). Teraz wszystkie pary startują
    // zawsze jako WYŁĄCZONE -- bezpieczne, przewidywalne, bez skoku prądu.
    // Kosztem jest utrata "pamiętania" stanu sprzed zaniku zasilania.
    pairs_[p]->setDefaultStateOff();

    physButtons[out0] = new Supla::Control::Button(
        &buttonIo, channelToInputPin(out0), true, true);
    physButtons[out1] = new Supla::Control::Button(
        &buttonIo, channelToInputPin(out1), true, true);
    // Typ przycisku (mono/bistabilny) -- konfigurowalny per kanał w
    // zakładce "Przekaźniki i Przyciski". W trybie rolety biblioteka
    // prawdopodobnie i tak zarządza przyciskami po swojemu (do
    // zweryfikowania testem) -- to ustawienie ma znaczenie głównie
    // w trybie przekaźnika.
    physButtons[out0]->setButtonType(
        buttonType[out0] == 1 ? Supla::Control::Button::ButtonType::BISTABLE
                              : Supla::Control::Button::ButtonType::MONOSTABLE);
    physButtons[out1]->setButtonType(
        buttonType[out1] == 1 ? Supla::Control::Button::ButtonType::BISTABLE
                              : Supla::Control::Button::ButtonType::MONOSTABLE);

    // Action Trigger dla obu przycisków tej pary -- pozwala w Supla Cloud
    // skonfigurować dowolne akcje na wielokrotne kliknięcie (np. 2x = 25%,
    // 3x = 50%, 4x = 75% otwarcia/zamknięcia), bez pisania tej logiki u nas
    // w kodzie. setRelatedChannel() sprawia, że AT pojawia się w Cloud POD
    // kanałem tej pary, a nie jako osobny, samodzielny kanał.
    auto at0 = new Supla::Control::ActionTrigger();
    auto at1 = new Supla::Control::ActionTrigger();
    at0->setRelatedChannel(pairs_[p]);
    at1->setRelatedChannel(pairs_[p]);
    // Jawne numery kanałów (20+) -- unika kolizji z auto-numeracją, która
    // spowodowała wcześniej, że "Webserwer" i sensory próbowały zająć ten
    // sam numer co inny kanał.
    at0->getChannel()->setChannelNumber(20 + 2 * p);
    at1->getChannel()->setChannelNumber(21 + 2 * p);

    pairs_[p]->attach(physButtons[out0], physButtons[out1], at0, at1);

    // Zachowujemy relays_[] wskazujące na faktyczne obiekty Relay z pary --
    // dzięki temu istniejąca automatyka (turnOn/turnOff przez relays_[r])
    // działa bez zmian, DOPÓKI dana para jest w trybie przekaźnika. Gdy para
    // przejdzie w tryb rolety, te wskaźniki nadal istnieją, ale sterowanie
    // przez nie nie będzie already sensowne dla rolety -- to ograniczenie
    // do rozwiązania, gdy dopiszemy właściwą logikę sterowania roletami
    // w automatyce.
    relays_[out0] = pairs_[p]->getPrimaryRelay();
    relays_[out1] = pairs_[p]->getSecondaryRelay();
  }

  // Przycisk BOOT jako oficjalny "config button": przytrzymanie lokalne
  // ORAZ komenda "Enter config mode" z poziomu Supla Cloud będą działać
  // automatycznie dzięki obecności Storage::Config (LittleFsConfig) powyżej.
  cfgButton = new Supla::Control::Button(BOOT_BUTTON_PIN, true, true);
  cfgButton->configureAsConfigButton(&SuplaDevice);

  // Wirtualny przełącznik "Panel WWW" -- tworzony TERAZ, zaraz po
  // przekaźnikach, PRZED pętlą czujników poniżej. Dzięki temu jego numer
  // kanału jest raz na zawsze ustalony i nie przesuwa się, gdy w przyszłości
  // dodasz/usuniesz rejestrację kolejnego czujnika w Cloud (liczba kanałów
  // z pętli czujników zmienia się w czasie, więc cokolwiek miałoby być PO
  // niej, przesuwałoby się przy każdej takiej zmianie -- to właśnie
  // spowodowało konflikt, na który trafiłeś). Włączenie/wyłączenie z
  // aplikacji Supla uruchamia lub zatrzymuje lokalny panel WWW urządzenia
  // (Supla::START_LOCAL_WEB_SERVER / STOP_LOCAL_WEB_SERVER).
  webServerToggle = new Supla::Control::VirtualRelay();
  webServerToggle->getChannel()->setChannelNumber(6);  // jawny numer -- unika kolizji z Action Triggerami/sensorami
  webServerToggle->setInitialCaption("Webserwer");
  webServerToggle->setDefaultFunction(SUPLA_CHANNELFNC_POWERSWITCH);
  webServerToggle->addAction(Supla::START_LOCAL_WEB_SERVER, SuplaDevice, Supla::ON_TURN_ON);
  webServerToggle->addAction(Supla::STOP_LOCAL_WEB_SERVER, SuplaDevice, Supla::ON_TURN_OFF);
  // UWAGA: usunięto setDefaultStateRestore() -- przywracanie zapisanego
  // stanu dzieje się dopiero W TRAKCIE SuplaDevice.begin() (poniżej),
  // więc nadpisywało nasze wcześniejsze wymuszenie "włączony" ustawione
  // tutaj. Faktyczne wymuszenie robimy teraz PO begin() (patrz niżej),
  // żeby na pewno było ostatnim słowem.

  // Kanały "Odczyt Czujników" -- TWORZONE PO przekaźnikach (CH0-5) I PO
  // "Webserwer", żeby numeracja kanałów w Supla Cloud się nie przesuwała.
  // Kolejność tworzenia kanałów w kodzie = kolejność numeracji w Cloud;
  // zmiana tej kolejności po tym, jak urządzenie już się raz zarejestrowało,
  // powoduje dokładnie ten błąd "konflikt typu kanału", na który trafiłeś.
  //
  // POPRAWKA: każdy slot dostaje teraz NA STAŁE zarezerwowany numer kanału
  // (7-11), niezależnie od tego, które inne sloty są akurat zarejestrowane
  // w Cloud. Potwierdzone przez klew (główny developer SuplaDevice, forum
  // Supla, maj 2024): Supla::Channel::setChannelNumber() pozwala na
  // dowolną, nieciągłą numerację, ustawioną PRZED SuplaDevice.begin() --
  // to eliminuje "przesuwanie się" numerów przy włączaniu/wyłączaniu
  // rejestracji poszczególnych czujników, które powodowało konflikty na
  // WIELU kanałach naraz.
  //
  // Pełny schemat numeracji kanałów:
  //   0-5  -- 3 pary przekaźnik/roleta
  //   6    -- Webserwer
  //   7    -- Czujnik Wiatru
  //   8    -- Czujnik Opadów
  //   9    -- Czujnik Temperatury
  //   10   -- Czujnik Natężenia Światła
  //   11   -- Poziom Wody w Zbiorniku
  //   20-25 -- Action Triggery (2 na parę × 3 pary)
  const int kSensorChannelNum[MAX_SENSOR_SLOTS] = {7, 8, 9, 10, 11};
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (sensorSlots[i].cloudRegistered) {
      int reservedChannelNum = kSensorChannelNum[i];
      if (sensorSlots[i].source == 2) {
        // DS18B20 -- klasa całkowicie samodzielna: sama odczytuje magistralę
        // 1-Wire I sama tworzy swój kanał termometru w Cloud. Nie przechodzi
        // przez naszą zwykłą ścieżkę "Typ kanału" (Pomiar/Binarny/Zbiornik/
        // Temperatura) -- ten wybór jest dla niej ignorowany.
        sensorSlots[i].ds18b20 = new Supla::Sensor::DS18B20(sensorSlots[i].oneWirePin);
        sensorSlots[i].ds18b20->setInitialCaption(sensorSlots[i].name);
        sensorSlots[i].ds18b20->getChannel()->setChannelNumber(reservedChannelNum);
      } else if (sensorSlots[i].channelType == 1) {
        sensorSlots[i].binSensor = new Supla::Sensor::ModbusBinarySensor();
        sensorSlots[i].binSensor->setInitialCaption(sensorSlots[i].name);
        sensorSlots[i].binSensor->getChannel()->setChannelNumber(reservedChannelNum);
        if (i == 1) {
          // Czujnik Opadów -- domyślna ikona nr 4. Działa TYLKO przy
          // pierwszej rejestracji tego kanału (ten sam mechanizm co
          // ikona Wiatru wcześniej). Numeracja ikon liczy się od 0.
          sensorSlots[i].binSensor->getChannel()->setDefaultIcon(4);
        }
      } else if (sensorSlots[i].channelType == 2) {
        sensorSlots[i].tankSensor = new Supla::Sensor::TankLevelSensor();
        sensorSlots[i].tankSensor->setInitialCaption(sensorSlots[i].name);
        sensorSlots[i].tankSensor->getChannel()->setChannelNumber(reservedChannelNum);
      } else if (sensorSlots[i].channelType == 3) {
        sensorSlots[i].thermoSensor = new Supla::Sensor::VirtualThermometer();
        sensorSlots[i].thermoSensor->setInitialCaption(sensorSlots[i].name);
        sensorSlots[i].thermoSensor->getChannel()->setChannelNumber(reservedChannelNum);
      } else {
        sensorSlots[i].sensor = new Supla::Sensor::GeneralPurposeMeasurement();
        sensorSlots[i].sensor->setInitialCaption(sensorSlots[i].name);
        sensorSlots[i].sensor->setValue(NAN);  // brak odczytu na starcie -> "---" zamiast "0"
        sensorSlots[i].sensor->getChannel()->setChannelNumber(reservedChannelNum);
        if (i == 0) {
          // Czujnik Wiatru -- domyślna ikona nr 6. Działa TYLKO przy
          // pierwszej rejestracji tego kanału (potwierdzone przez klew,
          // głównego developera SuplaDevice, na forum Supla). Numeracja
          // ikon w bibliotece liczy się od 0 -- jeśli po sprawdzeniu w
          // Cloud okaże się, że to nie ta ikona, o którą Ci chodziło,
          // spróbuj 5 (jedno przesunięcie) zamiast 6.
          sensorSlots[i].sensor->getChannel()->setDefaultIcon(6);
        } else if (i == 3) {
          // Czujnik Natężenia Światła -- domyślna ikona nr 14, ten sam
          // mechanizm i to samo zastrzeżenie co dla Wiatru powyżej.
          sensorSlots[i].sensor->getChannel()->setDefaultIcon(14);
        }
      }
    }
  }

  SuplaDevice.setName(DEVICE_DISPLAY_NAME);
  // Widoczne w Supla Cloud przy urządzeniu jako "wersja oprogramowania".
  SuplaDevice.setSwVersion((buildDateDMY() + " @lukasz06").c_str());

  // Pozwala Supla Cloud kasować dowolny kanał/urządzenie od strony serwera
  // (nie mamy "systemowych" kanałów, które trzeba by chronić). Wraz z
  // ConflictResolver deklarowanym wyżej -- patrz komentarz przy jego
  // definicji co do zakresu tego, co faktycznie obsługujemy automatycznie.
  SuplaDevice.addFlags(SUPLA_DEVICE_FLAG_BLOCK_ADDING_CHANNELS_AFTER_DELETION);
  SuplaDevice.addFlags(SUPLA_DEVICE_FLAG_ALWAYS_ALLOW_CHANNEL_DELETION);
  SuplaDevice.setChannelConflictResolver(&channelConflictResolver);

  // Przywraca zachowanie "od razu rozgłasza sieć po restarcie/braku
  // konfiguracji, bez limitu czasu w trybie config" (zamiast trzeba
  // przytrzymać przycisk + auto-wyłączenie po 5 min). Wcześniej podejrzewane
  // jako przyczyna losowych, niewyjaśnionych wejść w tryb config -- po
  // dalszej diagnozie okazało się, że to był inny, osobny błąd (patrz
  // rozmowa), więc te ustawienia wracają.
  SuplaDevice.allowWorkInOfflineMode(0);
  SuplaDevice.setLeaveCfgModeAfterInactivityMin(0);

  // Właściwości Modbus dostępne do wyboru w panelu WWW. Ustawiamy PRZED
  // SuplaDevice.begin(), bo Configurator::onLoadConfig() (wywoływane w
  // trakcie begin()) odczytuje te właściwości. Zakładam RS485 (RTU) jako
  // Master -- typowe dla KC868-A6 z czujnikami/licznikami podpiętymi po
  // RS485. Jeśli potrzebujesz też trybu Slave/TCP/UDP, dopisujemy analogicznie.
  Supla::Modbus::ConfigProperties modbusProps;
  modbusProps.protocol.master = 1;
  modbusProps.protocol.slave = 1;
  modbusProps.protocol.rtu = 1;
  modbusProps.baudrate.b4800 = 1;
  modbusProps.baudrate.b9600 = 1;
  modbusProps.baudrate.b19200 = 1;
  modbusProps.baudrate.b38400 = 1;
  modbusProps.baudrate.b57600 = 1;
  modbusProps.baudrate.b115200 = 1;
  modbusProps.stopBits.one = 1;  // rezygnujemy z wyboru stop bitów -- zawsze 1
  modbusConfigurator.setProperties(modbusProps);
  SuplaDevice.setSuplaCACert(suplaCACert);
  SuplaDevice.setSupla3rdPartyCACert(supla3rdCACert);

  // Brak argumentów w begin() -- GUID/AuthKey/serwer/email są odczytywane
  // z konfiguracji zapisanej przez panel WWW (Storage::Config), a GUID/
  // AuthKey są automatycznie wygenerowane przy pierwszym uruchomieniu.
  SuplaDevice.begin();

  // Podpięcie /update pod ten sam serwer, którego używa panel konfiguracyjny
  // Supli. getServerPtr() zwraca "surowy" WebServer -- potwierdzone na forum
  // Supla, ale NIE zweryfikowane przeze mnie co do TIMINGU (czy wskaźnik
  // istnieje już na tym etapie, czy tworzy się dopiero przy wejściu w tryb
  // konfiguracji). Stąd zabezpieczenie warunkiem -- jeśli po wgraniu
  // przycisk "Aktualizacja Firmware" nie przenosi do prawidłowej strony
  // /update, prawdopodobnie trzeba to wywołanie przenieść w miejsce, gdzie
  // faktycznie wchodzimy w tryb konfiguracji.
  if (suplaServer.getServerPtr() != nullptr) {
    suplaServer.getServerPtr()->on("/update", HTTP_GET, pageUpdateCustom);
    suplaServer.getServerPtr()->on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
    suplaServer.getServerPtr()->on("/sensors-control", HTTP_GET, pageSensorsControl);
    suplaServer.getServerPtr()->on("/sensors-control-save", HTTP_POST, saveSensorSlotsFromServer);
    suplaServer.getServerPtr()->on("/sensors-control-save-restart", HTTP_POST, saveSensorSlotsAndRestart);
    suplaServer.getServerPtr()->on("/sensors-status", HTTP_GET, pageSensorsStatusJson);
    suplaServer.getServerPtr()->on("/alarm-mqtt-target-save", HTTP_POST, saveAlarmMqttTargetFromServer);
    suplaServer.getServerPtr()->on("/alarm-mqtt-test-on", HTTP_POST, testAlarmMqttOnFromServer);
    suplaServer.getServerPtr()->on("/alarm-mqtt-test-off", HTTP_POST, testAlarmMqttOffFromServer);
    suplaServer.getServerPtr()->on("/sensor-automation-save", HTTP_POST, saveAutomationCfgFromServer);
    suplaServer.getServerPtr()->on("/relay-config", HTTP_GET, pageRelayConfig);
    suplaServer.getServerPtr()->on("/relay-config-save", HTTP_POST, saveRelayConfigFromServer);
    suplaServer.getServerPtr()->on("/relay-config-save-restart", HTTP_POST, saveRelayConfigAndRestart);
    suplaServer.getServerPtr()->on("/restart", HTTP_POST, pageRestartDevice);
    suplaServer.getServerPtr()->on("/restart-countdown", HTTP_GET, pageRestartCountdown);
  }

  // RS485 -- inicjalizowane ZAWSZE, niezależnie od tego, jaką rolę Modbus
  // pokazuje aktualnie Supla Cloud (rola Master ustawiona "na sztywno" po
  // naszej stronie, na życzenie -- rezygnujemy ze zdalnej zależności i
  // ostrzeżenia o nieaktywnej roli). Baudrate/stop bity TEŻ teraz ustawiane
  // lokalnie (patrz "Ustawienia Czujników"), niezależnie od konfiguracji
  // zdalnej Cloud -- g_modbusBaudrate/g_modbusStopBits wczytane wcześniej
  // w loadSensorSlots().
  uint32_t sConf = (g_modbusStopBits == 2) ? SERIAL_8N2 : SERIAL_8N1;
  Serial2.begin(g_modbusBaudrate, sConf, RS485_RXD_PIN, RS485_TXD_PIN);
}

void loop() {
  readSensorsControl();
  runAutomation();
  syncAlarmMqttStates();
  SuplaDevice.iterate();

  // Wymuszenie "zawsze włączony" dla Webserwera -- robimy to TUTAJ (nie
  // zaraz po begin() w setup()), z opóźnieniem, żeby dać połączeniu z
  // Cloud czas się w pełni nawiązać najpierw (iterate() musi się kilka
  // razy wykonać). Wcześniejsza próba (zaraz po begin()) powodowała, że
  // urządzenie w ogóle się nie rejestrowało -- prawdopodobnie próba
  // wysłania zmiany stanu kanału zanim połączenie było gotowe.
  static bool webServerForcedOn = false;
  static unsigned long bootTime = millis();
  if (!webServerForcedOn && millis() - bootTime >= 10000) {
    webServerToggle->turnOn();
    webServerForcedOn = true;
  }

  // Diagnostyka: wolna pamięć sterty co 30s -- sprawdzamy, czy spada z
  // czasem (fragmentacja) na tyle, żeby uniemożliwić start serwera WWW.
  static unsigned long lastHeapLog = 0;
  if (millis() - lastHeapLog >= 30000) {
    lastHeapLog = millis();
    Serial.printf("[diag] Wolna pami\u0119\u0107 sterty: %u bajt\u00f3w (najwi\u0119kszy ci\u0105g\u0142y blok: %u)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
}
