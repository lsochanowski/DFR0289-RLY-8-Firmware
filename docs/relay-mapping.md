# Mapowanie przekaźników — KOMPLETNE

Cel: ustalić, który pin GPIO steruje którym przekaźnikiem, bez schematu i bez oryginalnego
firmware (został wymazany — patrz `docs/STATUS.md`).

## Mapa końcowa (potwierdzona obserwacją na sprzęcie)

| Przekaźnik | Pin | Polaryzacja | Jak potwierdzone |
| --- | --- | --- | --- |
| 1 | **PC8** | aktywny stan wysoki | seria kliknięć (9 kliknięć) |
| 2 | **PC9** | aktywny stan wysoki | seria kliknięć (10 kliknięć) |
| 3 | **PC10** | aktywny stan wysoki | seria kliknięć (11 kliknięć) |
| 4 | **PC11** | aktywny stan wysoki | seria kliknięć (12 kliknięć) |
| 5 | **PC12** | aktywny stan wysoki | seria kliknięć (13 kliknięć) |
| 6 | **PC7** | aktywny stan wysoki | bisekcja `0x00C0` → oba, `0x0040` (PC6) → tylko 7, `0x0080` (PC7) → 6 |
| 7 | **PC6** | aktywny stan wysoki | jak wyżej |
| 8 | **PB15** | aktywny stan wysoki | bisekcja `0xC000`→tak, `0x4000`→nie, `0x8000`→tak |

Dwie osobliwości tej płytki, warte zapamiętania:

1. **Przekaźniki 6 i 7 są zamienione** względem naturalnej kolejności PC6/PC7
   (relay 6 = PC7, relay 7 = PC6).
2. **Relay 8 nie leży w bloku portu C** — siedzi samotnie na PB15 (port B).

## Metoda, która zadziałała: bisekcja maską pinów

Sonda `firmware/port/probe/bank_all.c` włącza **wybrane piny jednego banku naraz** i zostawia
je włączone, więc obserwator nie goni czasu. Każda kolejna runda zawęża maskę o połowę.

```bash
make -C firmware probe-bank EXTRA="-DBANK=2 -DMASK=0x00C0"
PYTHONPATH=work/toolchain/py python3 -m pyocd flash -t stm32f103rc \
  -u $STLINK_UID firmware/build/bank_all.elf
```

`DBANK`: 0=A, 1=B, 2=C, 3=D, 4=E. `MASK` domyślnie `0xFFFF`.
**PA13/PA14 są zawsze pomijane** (SWDIO/SWCLK — sterowanie nimi zrywa debugger).
Maska faktycznie włączonych pinów trafia do RAM pod `0x20008000`, więc przebieg da się
zweryfikować po SWD.

### Dlaczego wcześniejsze metody zawiodły

- **kodowanie „N+1 kliknięć"** — przy 13–16 kliknięciach obserwator gubił rachubę,
- **zmienna polaryzacja** między przebiegami — wyniki nie były porównywalne,
- **pojedyncze piny w losowej kolejności** — brak struktury, łatwo przeoczyć reakcję.

Bisekcja wymaga tylko odpowiedzi „tak/nie", więc jest odporna na wszystkie trzy problemy.

## Przebiegi, które zawęziły poszukiwania

| Maska | Bank | Co włączone | Wynik |
| --- | --- | --- | --- |
| `0x9FFF` | A | PA0–PA12, PA15 | żaden przekaźnik |
| `0xFFFF` | B | PB0–PB15 | relay 8 |
| `0x7FFF` | B | B bez PB15 | nic (relay 8 potwierdzony jako jedyny na B) |
| `0x00FF` | B | PB0–PB7 | relay 8 nie |
| `0xFF00` | B | PB8–PB15 | relay 8 tak |
| `0x0F00` | B | PB8–PB11 | nie |
| `0x3000` | B | PB12–PB13 | nie |
| `0xC000` | B | PB14–PB15 | tak |
| `0x4000` | B | PB14 | nie |
| `0x8000` | B | PB15 | **tak** |
| `0xFFFF` | C | PC0–PC15 | **1–7** |
| `0x00FF` | C | PC0–PC7 | 6 i 7 |
| `0x000F` | C | PC0–PC3 | nic |
| `0x00F0` | C | PC4–PC7 | 6 i 7 |
| `0x0030` | C | PC4–PC5 | nic |
| `0x00C0` | C | PC6–PC7 | 6 i 7 |
| `0x0040` | C | PC6 | **tylko relay 7** |
| `0x0080` | C | PC7 | **tylko relay 6** |

## Wpięcie do firmware

`firmware/port/f107/relay.c`:

```c
static const relay_pin relay_map[BOARD_RELAY_COUNT] = {
    {2u, 8u, 0u},  {2u, 9u, 0u},  {2u, 10u, 0u}, {2u, 11u, 0u},
    {2u, 12u, 0u}, {2u, 7u, 0u},  {2u, 6u, 0u},  {1u, 15u, 0u},
};
```

Mapa jest **pilnowana testem hostowym** (`firmware/core/test_relay.c`) — literówka w pinie
wyjdzie w `make test`, a nie na przekaźnikach. `relay_set()` ustawia stan nieaktywny **przed**
przełączeniem pinu na wyjście, więc przy starcie żadna cewka nie klapnie.
