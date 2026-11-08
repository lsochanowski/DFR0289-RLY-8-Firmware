# Relay mapping — COMPLETE

*Polski: [niżej](#polski)*

Goal: determine which GPIO pin drives which relay, without a schematic and without the
original firmware (it had been erased — see `docs/STATUS.md`).

## Final map (confirmed by observation on the hardware)

| Relay | Pin | Polarity | How confirmed |
| --- | --- | --- | --- |
| 1 | **PC8** | active high | click series (9 clicks) |
| 2 | **PC9** | active high | click series (10 clicks) |
| 3 | **PC10** | active high | click series (11 clicks) |
| 4 | **PC11** | active high | click series (12 clicks) |
| 5 | **PC12** | active high | click series (13 clicks) |
| 6 | **PC7** | active high | bisection `0x00C0` → both, `0x0040` (PC6) → only 7, `0x0080` (PC7) → 6 |
| 7 | **PC6** | active high | as above |
| 8 | **PB15** | active high | bisection `0xC000`→yes, `0x4000`→no, `0x8000`→yes |

Two quirks of this board worth remembering:

1. **Relays 6 and 7 are swapped** relative to the natural PC6/PC7 order
   (relay 6 = PC7, relay 7 = PC6).
2. **Relay 8 is not in the port C block** — it sits alone on PB15 (port B).

## The method that worked: pin-mask bisection

The probe `firmware/port/probe/bank_all.c` switches on **selected pins of one bank at once**
and leaves them on, so the observer does not have to race the clock. Each successive round
halves the mask.

```bash
make -C firmware probe-bank EXTRA="-DBANK=2 -DMASK=0x00C0"
PYTHONPATH=work/toolchain/py python3 -m pyocd flash -t stm32f103rc \
  -u $STLINK_UID firmware/build/bank_all.elf
```

`DBANK`: 0=A, 1=B, 2=C, 3=D, 4=E. `MASK` defaults to `0xFFFF`.
**PA13/PA14 are always skipped** (SWDIO/SWCLK — driving them breaks the debugger).
The mask of the pins actually switched on is written to RAM at `0x20008000`, so the run can
be verified over SWD.

### Why the earlier methods failed

- **"N+1 clicks" encoding** — at 13–16 clicks the observer lost count,
- **polarity changing** between runs — the results were not comparable,
- **single pins in random order** — no structure, easy to miss a reaction.

Bisection only needs a "yes/no" answer, so it is immune to all three problems.

## Runs that narrowed the search

| Mask | Bank | What is switched on | Result |
| --- | --- | --- | --- |
| `0x9FFF` | A | PA0–PA12, PA15 | no relay |
| `0xFFFF` | B | PB0–PB15 | relay 8 |
| `0x7FFF` | B | B without PB15 | nothing (relay 8 confirmed as the only one on B) |
| `0x00FF` | B | PB0–PB7 | relay 8 no |
| `0xFF00` | B | PB8–PB15 | relay 8 yes |
| `0x0F00` | B | PB8–PB11 | no |
| `0x3000` | B | PB12–PB13 | no |
| `0xC000` | B | PB14–PB15 | yes |
| `0x4000` | B | PB14 | no |
| `0x8000` | B | PB15 | **yes** |
| `0xFFFF` | C | PC0–PC15 | **1–7** |
| `0x00FF` | C | PC0–PC7 | 6 and 7 |
| `0x000F` | C | PC0–PC3 | nothing |
| `0x00F0` | C | PC4–PC7 | 6 and 7 |
| `0x0030` | C | PC4–PC5 | nothing |
| `0x00C0` | C | PC6–PC7 | 6 and 7 |
| `0x0040` | C | PC6 | **relay 7 only** |
| `0x0080` | C | PC7 | **relay 6 only** |

## Wiring into the firmware

`firmware/port/f107/relay.c`:

```c
static const relay_pin relay_map[BOARD_RELAY_COUNT] = {
    {2u, 8u, 0u},  {2u, 9u, 0u},  {2u, 10u, 0u}, {2u, 11u, 0u},
    {2u, 12u, 0u}, {2u, 7u, 0u},  {2u, 6u, 0u},  {1u, 15u, 0u},
};
```

The map is **guarded by a host test** (`firmware/core/test_relay.c`) — a typo in a pin shows
up in `make test`, not on the relays. `relay_set()` sets the inactive state **before**
switching the pin to output, so no coil clicks at startup.

---

## Polski

### Mapowanie przekaźników — KOMPLETNE

Cel: ustalić, który pin GPIO steruje którym przekaźnikiem, bez schematu i bez oryginalnego
firmware (został wymazany — patrz `docs/STATUS.md`).

### Mapa końcowa (potwierdzona obserwacją na sprzęcie)

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

### Metoda, która zadziałała: bisekcja maską pinów

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

#### Dlaczego wcześniejsze metody zawiodły

- **kodowanie „N+1 kliknięć"** — przy 13–16 kliknięciach obserwator gubił rachubę,
- **zmienna polaryzacja** między przebiegami — wyniki nie były porównywalne,
- **pojedyncze piny w losowej kolejności** — brak struktury, łatwo przeoczyć reakcję.

Bisekcja wymaga tylko odpowiedzi „tak/nie", więc jest odporna na wszystkie trzy problemy.

### Przebiegi, które zawęziły poszukiwania

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

### Wpięcie do firmware

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
