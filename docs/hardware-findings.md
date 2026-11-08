# Ustalenia sprzętowe — RLY-8 (DFR0289), egzemplarz użytkownika

Wszystko poniżej zmierzone na żywym układzie przez SWD (ST-Link + pyOCD), nie zgadywane.

## MCU

| Parametr | Wartość | Jak ustalone |
| --- | --- | --- |
| CPUID | `0x411FC231` → Cortex-M3 r1p1 | `read32 0xE000ED00` |
| DBGMCU_IDCODE | `0x10016418` → DEV_ID `0x418` | `read32 0xE0042000` |
| Rodzina | STM32F10x Connectivity Line (F105/F107) | DEV_ID |
| Flash | 256 KB (`0x1FFFF7E0` = `0x0100`) | rejestr rozmiaru |
| RAM | 64 KB (initial SP `0x20010000`) | wektor 0 |
| Unikalny ID | 96 bit, inny w każdym egzemplarzu (z niego liczony jest MAC) | `0x1FFFF7E8..F0` |
| RDP | poziom 0 (`0x1FFFF800` = `0x00FF5AA5`, bajt RDP `0xA5`) | option bytes |
| Kwarc | **HSE 8 MHz obecny i działa** (`HSERDY` ustawia się) | `RCC_CR` |
| PLL | **wstaje z HSE ×9 = 72 MHz** (`RCC_CR=0x03034503`, `SWS=0b10`) | `RCC_CR`/`RCC_CFGR` |

Uwaga: PLL z HSI/2 ×16 (64 MHz) **nie** zablokował się na tym egzemplarzu; działa wyłącznie
ścieżka z HSE. Firmware musi startować z HSE 8 MHz.

## Co było w flash

- `0x08000000`–`0x080014FF`: ~5 KB (bootloader DFRobot, wektory z resetem `0x08001501`)
- `0x08001500`+: firmware testowy użytkownika — w obrazie string
  `STM32F107 Relay Controller` / `Ethernet dziala!` / serwer HTTP
- od `0x08002000` w górę: **czysto `0xFF`** → oryginalny firmware DFRobot został wymazany,
  więc pinoutu i konfiguracji PHY **nie da się odzyskać z pamięci**
- zrzut całości: `work/orig-fw/current-flash.bin` (sha256 `f5bfb3e2…1bc37`)

## Ethernet — gdzie utknęło

| Test | Wynik |
| --- | --- |
| `RCC_AHBENR` bit 14 (ETHMACEN) | ustawia się poprawnie |
| Transakcja MDIO (`MB` bit) | **wykonuje się** (po poprawieniu adresu rejestru: `MACMIIAR` = `0x40028010`, nie `0x40028014`) |
| Odczyt ID PHY, adresy 0–31 | raz **`0x2000:0x5C90`** = TI **DP83848AA**, w pozostałych przebiegach `0xFFFF` |
| Soak: 1000 rund × 32 adresy (64 000 odczytów) | **0 odpowiedzi** — przy USB, przy PoE i przy +12 V DC |
| Czy PHY wstaje późno? (1000 rund × 32 adresy przez ~10 s) | **0 odpowiedzi** — hipoteza „późny start" odrzucona |
| Skan po pinach GPIO (bez SWD/MDIO/MDC) | brak odpowiedzi w obu polaryzacjach |
| PA0 podniesiony (hipoteza „reset PHY") | 0 odpowiedzi — odrzucona |
| `RCC_CSR` po resetach | `0x1C000000` → `SFTRSTF+PORRSTF+PINRSTF`, **bez** flag watchdoga |

Wniosek: strona MCU jest sprawna (MAC taktowany, transakcje MDIO się kończą, zegar z HSE),
a PHY **odpowiedział dokładnie raz** poprawnym ID. To wzorzec typowy dla niestabilnego
połączenia lub zasilania pojedynczego układu, nie dla błędu konfiguracji.

### Co pozostaje zmierzyć (multimetr, tylko użytkownik)

Pinout z datasheetu TI (DP83848C/VYB, LQFP-48 — DP83848AA ma to samo wyprowadzenie):

| Pin | Nazwa | Oczekiwanie | Co oznacza odchyłka |
| --- | --- | --- | --- |
| **29** | `RESET_N` | **~3,3 V** (wysoki = PHY pracuje) | niski → PHY trzymany w resecie, nigdy nie odpowie na MDIO |
| **32, 38** | `IOVDD33` | ~3,3 V | brak → nie ma zasilania I/O (MDIO martwe) |
| **22** | `AVDD33` | ~3,3 V | brak → nie ma zasilania analogowego (PHY martwy, link nie wstanie) |
| **30** | `MDIO` | ~3,3 V w spoczynku (pull-up 1,5 kΩ) | 0 V lub „pływa" → brak pull-upa albo zwarcie; to tłumaczy `0xFFFF`/`0x0000` na przemian |
| **31** | `MDC` | impulsy z MCU (trudne miernikiem) | — |
| **47** | `IOGND` | 0 V | — |

Kolejność pomiarów (od najbardziej diagnostycznego): **29 → 32/38 → 22 → 30**.

Dodatkowo ciągłość (tryb „piszczy”):
- **PA2 (MCU) ↔ pin 30 (MDIO PHY)**
- **PC1 (MCU) ↔ pin 31 (MDC PHY)**

Jeśli `RESET_N` jest niski, sprawdź, czy nie jest do niego podłączony któryś z pinów GPIO —
wtedy można go uwolnić programowo. Jeśli `AVDD33`/`IOVDD33` nie mają 3,3 V, problem jest
w zasilaniu sekcji PHY (przetwornica/PoE), a nie w firmware.

### Plan B, jeśli PHY okaże się uszkodzony

Urządzenie ma **USB CDC** (STM32 VCP), więc cel „Home Assistant przez MQTT" da się osiągnąć
bez Ethernetu: nasz firmware wystawia protokół (JSON lub ramki `55 AA`) po USB, a na hoście
(HA / Raspberry Pi) działa mały mostek USB → MQTT z discovery. To ta sama architektura
`rly_session`, tylko z innym transportem.

## Błędy, które sam popełniłem po drodze (żeby ich nie powtarzać)

1. Złe adresy rejestrów MAC: `MACMIIAR` to `+0x10`, `MACMIIDR` to `+0x14`. Przy złym adresie
   „MB wisi” wygląda jak awaria MAC, a jest odczytem własnego zapisu.
2. Próba PLL z HSI/2 — na tym egzemplarzu nie lockuje; używać HSE.
3. Skan GPIO dotykający PA13/PA14 (SWDIO/SWCLK) zrywa debugger — po tym każdy odczyt RAM
   jest śmieciem. Sondy muszą pomijać `PA2`, `PC1`, `PA13`, `PA14`.

## Czego nadal brakuje

- oznaczenie układu PHY (DP83848 / LAN8720 / KSZ8081…),
- potwierdzenie, czy na szynie PHY jest 3,3 V (dioda zasilania? pomiar?),
- pinout przekaźników i driverów (ULN2803 / 74HC595) — do ustalenia po uruchomieniu PHY
  albo z oznaczeń układów.


## Dioda, przycisk, USB (2026-11-07/08)

| Element | Wynik | Metoda |
| --- | --- | --- |
| Niebieska dioda | **PB7**, aktywna stanem niskim | miganie grupami wolnych pinów (PA4-6, PA15, PB2-7, PB9, PC0, PC13, PD2), bisekcja 14 → 7 → 3 → 1, potwierdzenie stanem stałym |
| Przycisk DEFAULT | **PB14**, wciśnięty = 0 | wolne piny jako wejścia z pull-up, polling IDR przez SWD przy 3 wciśnięciach |
| USB-B | OTG FS na PA11/PA12 | enumeracja `0483:5740`, `/dev/cu.usbmodem*` |
| PA9 (VBUS) | **niepodłączony do VBUS**; ściągnięty do masy | z kablem: PA9=0 przy pull-down i pull-up, `GOTGCTL.BSVLD=0`; PA9 wymuszony w stan 1 → `BSVLD=1`, enumeracja |
| Wyjścia LED PHY (LEDCR 0x18) | **nie sprawdzono** | test się nie wykonał (sonda zajęta), po znalezieniu PB7 nie powtarzano |
