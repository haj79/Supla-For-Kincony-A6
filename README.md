# SUPLA for KinCony KC868-A6

Firmware dla **KinCony KC868-A6** oparty na bibliotece **SUPLA Device** (ESP32 / Arduino).

## Funkcje

- Integracja z SUPLA Cloud i konfiguracja przez wbudowany interfejs WWW.
- Sterowanie przekaźnikami, w tym obsługa konfiguracji rolet.
- Odczyty czujników przez **RS485 / Modbus RTU** oraz konfiguracja źródeł pomiarów.
- Obsługa czujników i automatyzacji dla wiatru, opadów, temperatury, światła i poziomu wody.
- Kalibracja i monitorowanie poziomu wody w zbiorniku.
- Progi i warunki sterowania przekaźnikami.
- Osobne alarmy MQTT dla wysokiego i niskiego poziomu wody.
- Aktualizacja firmware przez interfejs WWW.

## Kod źródłowy

Główny plik: `Supla_For_Kincony_A6.ino`.

### Wymagania

- Płytka KinCony KC868-A6 z ESP32.
- Arduino IDE i pakiet płytek ESP32.
- Biblioteki: **SuplaDevice**, **PCF8574**, **ModbusMaster** oraz zależności tych bibliotek.
- Dla funkcji DS18B20 odpowiednie biblioteki zależne od używanej wersji SUPLA Device.

> **Uwaga:** zgodność z konkretnymi wersjami bibliotek oraz pełna procedura kompilacji wymagają jeszcze potwierdzenia na działającej konfiguracji Arduino IDE. Nie publikujemy tu deklaracji, że plik został niezależnie skompilowany.

## Pierwsze uruchomienie

1. Otwórz plik `.ino` w Arduino IDE i zainstaluj wymagane biblioteki.
2. Wybierz właściwą płytkę ESP32 oraz port szeregowy.
3. Skompiluj i wgraj firmware.
4. Skonfiguruj Wi-Fi i SUPLA w interfejsie konfiguracyjnym urządzenia.
5. Skonfiguruj wejścia, czujniki i alarmy zgodnie ze swoją instalacją.

Nie podłączaj urządzeń 230 V bez zachowania odpowiednich zasad bezpieczeństwa i parametrów przekaźników.

## Prywatność i bezpieczeństwo

Kod nie zawiera celowo wpisanych danych dostępowych Wi-Fi, kluczy konta SUPLA ani prywatnego adresu brokera MQTT. Ustawienia urządzenia są zapisywane lokalnie podczas konfiguracji. **Nie publikuj zrzutów konfiguracji, kopii pamięci flash ani plików z hasłami.**

## Status

Projekt rozwijany. Przed użyciem w instalacjach krytycznych należy przetestować konfigurację, alarmy i zachowanie po utracie zasilania lub łączności.
