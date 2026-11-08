# Zrzut oryginalnego firmware z nietkniętego egzemplarza (tylko odczyt)

Cel: z drugiej, nieruszonej płytki wyciągamy oryginalny obraz flash — z niego odczytamy
mapowanie GPIO, konfigurację PHY, zegary i semantykę CRC. **Nic nie zapisujemy, nic nie
kasujemy** — wyłącznie `savemem`.

## 1. Podłącz probe

ST-Link V2/V3, J-Link albo dowolny DAPLink do padów SWD: `SWDIO`, `SWCLK`, `GND`, `3V3`.
Płytkę zasil normalnie (PoE albo DC 7–23 V), żeby PHY i przekaźniki miały zasilanie —
ale do samego odczytu flash wystarczy 3V3 z probe, o ile płytka nie potrzebuje więcej.

Sprawdź, czy probe jest widoczny:

```bash
cd <katalog repozytorium>
PYTHONPATH=work/toolchain/py python3 -m pyocd list
```

## 2. Zidentyfikuj target (zanim cokolwiek odczytasz)

```bash
PYTHONPATH=work/toolchain/py python3 -m pyocd list --targets | grep -i -E "stm32f1|stm32f4"
```

Jeśli nie wiesz, który to — nie zgaduj, tylko odczytaj `CPUID` i `DBGMCU_IDCODE`:

```bash
PYTHONPATH=work/toolchain/py python3 -m pyocd cmd -t cortex_m \
  -c "read32 0xE000ED00" -c "read32 0xE0042000"
```

`0xE0042000` (DBGMCU_IDCODE): dolne 12 bitów = DEV_ID. Mapowanie:
`0x410` = STM32F10x medium density, `0x414` = F10x high density, `0x418` = F107/F105
(Connectivity Line — **ten ma MAC Ethernet**), `0x413` = F405/407, `0x419` = F42x/43x.

## 3. Zrzut flash

Dla STM32F107 (256 KB flash):

```bash
PYTHONPATH=work/toolchain/py python3 -m pyocd cmd -t stm32f107rc \
  -c "savemem 0x08000000 0x40000 work/orig-fw/f107-flash.bin"
```

Dla F4 (1 MB):

```bash
PYTHONPATH=work/toolchain/py python3 -m pyocd cmd -t stm32f407vg \
  -c "savemem 0x08000000 0x100000 work/orig-fw/f4-flash.bin"
```

Dodatkowo warto zrzucić:

```bash
-c "savemem 0x1FFFF000 0x800 work/orig-fw/system-mem.bin"   # bootloader w ROM (F1)
-c "savemem 0x1FFFF800 0x10  work/orig-fw/option-bytes.bin" # opcje: RDP, BOOT
```

## 4. Co z tym zrobimy

1. `arm-none-eabi-objdump -D -b binary -marmv7-m` po obrazie → wektory, init, pętla główna.
2. Odczytywanie zapisów do `RCC->APB2ENR`, `GPIOx->CRL/CRH/ODR`, `ETH_*` → **pinout i PHY**
   bez zgadywania i bez rozkręcania driverów przekaźników.
3. Referencja semantyki `55 AA`/CRC do nowego firmware.

## 5. Bezpieczeństwo

- Nie używaj `pyocd erase`, `load` ani `--mass-erase` na tej płytce.
- Jeśli `savemem` zwróci błąd odczytu — najprawdopodobniej RDP (readout protection) jest
  włączony; wtedy zostaje tylko druga droga: identyfikacja z oznaczeń układów + zdjęć PCB.
- Zrzut trzymaj poza repo/workspace, jeśli zawiera dane producenta — u nas ląduje
  w `work/orig-fw/`, czyli w katalogu roboczym.
