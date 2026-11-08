# RLY-8 (DFRobot DFR0289) — original firmware protocol

*Polski: [niżej](#polski)*

Sources: `work/docs/datasheet.txt` + OCR of the tables from the PDF (`work/docs/ocr/page-*.txt`,
tesseract), and analysis of `Enternet_Com_Control.exe`. The format tables in the PDF are
images — that is why `pdftotext` alone does not see them; OCR recovered the column headers.

## 1. Binary frame — USB CDC and RS485

```
55 AA | addr | len | cmd | data[len] | crc
 2B      1B    1B    1B                1B
```

- `addr` — device address (RS485, default `0x01`)
- `len` — length of the `data` field (number of bytes), **not** the length of the whole frame
- `crc` — **8-bit sum of all preceding bytes** (no weighting/XOR of any kind)

The sum was checked against all five examples from the datasheet — it matches to the byte.

| cmd | Function | `len` | `data` |
| --- | --- | --- | --- |
| `0x00` | Relay control | 2 | `relay` (1–8), `value` (`0x00` = ON/closed, `0x01` = OFF) |
| `0x01` | Read relay states | 0 | — (response: 1 bitmap byte, bit0=relay1 … bit7=relay8, `1` = closed) |
| `0x02` | Set network | 14 | ip(4) + gateway(4) + netmask(4) + port(2, big-endian) |
| `0x08` | Set baud rate | 4 | big-endian value, e.g. `00 01 C2 00` = 115200 |
| `0x09` | Set RS485 address | 1 | new address |

**Unknown (to be recovered from a firmware dump):** the `cmd` codes for setting/reading the
name, reading the version, reading the network settings, DHCP on/off + read, restart, and the
error code in the response. The datasheet describes these operations in words, but gives hex
examples only for relays, network, baud rate and RS485 address.

Examples from the datasheet (verified by `make -C firmware test`):

```
relay 1 OFF : 55 AA 01 02 00 01 01 04
relay 1 ON  : 55 AA 01 02 00 01 00 03
get state   : 55 AA 01 00 01 01
set network : 55 AA 01 0E 02 C0 A8 01 0A C0 A8 01 01 FF FF FF 00 07 D0 C1
set 115200  : 55 AA 01 04 08 00 01 C2 00 CF
set RS485=2 : 55 AA 01 01 09 02 0C
```

Beware of an ambiguity in the datasheet: "Relay Values: 0x01 is closed, 0x00 is open", while
the example labelled "relay 1 off" uses `0x01`. We treat this as the **relay (coil) state**,
not the contact state — to be confirmed with a multimeter on the board.

Baud rates: 1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200, 128000, 256000.

## 2. JSON — network port only (default TCP 2000)

Frames terminated with `\0` (NUL). Values: `on` = closed, `off` = open.

| Request | Response |
| --- | --- |
| `{"relay1":"on","relay2":"off",…}` | `{"resp":"ok"}` / `{"resp":"error"}` |
| `{"get":"relayStatus"}` | `{"relay1":"off",…,"relay8":"off"}` |
| `{"name":"DFRobot"}` / `{"get":"name"}` | `{"name":"DFRobot"}` |
| `{"ipaddr":"192.168.1.10","gateway":"192.168.1.1","netmask":"255.255.255.0","port":"2000"}` | `{"resp":"ok"}` |
| `{"get":"netconfig"}` | `{"ipaddr":…,"gateway":…,"netmask":…,"port":…}` |
| `{"get":"version"}` | `{"version":"V1.0"}` |
| `{"RS485addr":"22"}` / `{"get":"RS485addr"}` | `{"resp":"ok"}` / `{"RS485addr":"22"}` |
| `{"baudrate":"9600"}` / `{"get":"baudrate"}` | `{"resp":"ok"}` (takes effect after restart) |
| `{"dhcp":"on"}` / `{"get":"dhcp"}` | `{"dhcp":"on"|"off"}` |
| `{"reboot":"1"}` | — |
| anything unknown | `{"resp":"unsupported"}` |

## 3. What we take from this into the new firmware

- **MQTT** — the main interface (HA discovery, 8 switches).
- **JSON/TCP:2000** — backward compatibility: existing scripts (`Fescron/DFRobot-RLY8-python3`)
  and the DFRobot application keep working.
- **`55 AA` frames on RS485** — because that is the only way to reach other devices on the bus.
- Network: DHCP with fallback to static `192.168.1.88/24`, port 2000, MQTT 1883.

## 4. What we do not know without a firmware dump

- the `cmd` codes for name/version/network/DHCP/restart (see above),
- behaviour after a power loss (are relay states restored?),
- timing/keep-alive and the maximum number of TCP clients on port 2000,
- the exact meaning of `value` (`0x00`/`0x01`) relative to the contact state.

---

## Polski

### RLY-8 (DFRobot DFR0289) — protokół oryginalnego firmware

Źródła: `work/docs/datasheet.txt` + OCR tabel z PDF-a (`work/docs/ocr/page-*.txt`, tesseract),
oraz analiza `Enternet_Com_Control.exe`. Tabele formatu w PDF-ie są obrazkami — dlatego
same `pdftotext` ich nie widzi; OCR odzyskał nagłówki kolumn.

### 1. Ramka binarna — USB CDC i RS485

```
55 AA | addr | len | cmd | data[len] | crc
 2B      1B    1B    1B                1B
```

- `addr` — adres urządzenia (RS485, domyślnie `0x01`)
- `len` — długość pola `data` (liczba bajtów), **nie** długość całej ramki
- `crc` — **suma 8-bitowa wszystkich poprzedzających bajtów** (bez żadnej wagi/XOR-a)

Suma sprawdzona na wszystkich pięciu przykładach z datasheetu — zgadza się co do bajtu.

| cmd | Funkcja | `len` | `data` |
| --- | --- | --- | --- |
| `0x00` | Sterowanie przekaźnikiem | 2 | `relay` (1–8), `value` (`0x00` = ON/zwarty, `0x01` = OFF) |
| `0x01` | Odczyt stanu przekaźników | 0 | — (odpowiedź: 1 bajt bitowy, bit0=relay1 … bit7=relay8, `1` = zwarty) |
| `0x02` | Ustaw sieć | 14 | ip(4) + gateway(4) + netmask(4) + port(2, big-endian) |
| `0x08` | Ustaw baudrate | 4 | wartość big-endian, np. `00 01 C2 00` = 115200 |
| `0x09` | Ustaw adres RS485 | 1 | nowy adres |

**Nieznane (do odzyskania ze zrzutu firmware):** kody `cmd` dla ustawienia/odczytu nazwy,
odczytu wersji, odczytu sieci, DHCP on/off + odczyt, restart, oraz kod błędu w odpowiedzi.
Datasheet opisuje te operacje słownie, ale przykłady heksadecymalne podaje tylko dla
przekaźników, sieci, baudrate i adresu RS485.

Przykłady z datasheetu (zweryfikowane przez `make -C firmware test`):

```
relay 1 OFF : 55 AA 01 02 00 01 01 04
relay 1 ON  : 55 AA 01 02 00 01 00 03
get state   : 55 AA 01 00 01 01
set network : 55 AA 01 0E 02 C0 A8 01 0A C0 A8 01 01 FF FF FF 00 07 D0 C1
set 115200  : 55 AA 01 04 08 00 01 C2 00 CF
set RS485=2 : 55 AA 01 01 09 02 0C
```

Uwaga na niejednoznaczność datasheetu: „Relay Values: 0x01 is closed, 0x00 is open”, a
przykład podpisany „relay 1 off” używa `0x01`. Traktujemy to jako **stan przekaźnika
(cewki)**, a nie stan styku — do potwierdzenia multimetrem na płytce.

Baudrate: 1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200, 128000, 256000.

### 2. JSON — tylko port sieciowy (domyślnie TCP 2000)

Ramki kończone `\0` (NUL). Wartości: `on` = zwarty, `off` = rozwarty.

| Żądanie | Odpowiedź |
| --- | --- |
| `{"relay1":"on","relay2":"off",…}` | `{"resp":"ok"}` / `{"resp":"error"}` |
| `{"get":"relayStatus"}` | `{"relay1":"off",…,"relay8":"off"}` |
| `{"name":"DFRobot"}` / `{"get":"name"}` | `{"name":"DFRobot"}` |
| `{"ipaddr":"192.168.1.10","gateway":"192.168.1.1","netmask":"255.255.255.0","port":"2000"}` | `{"resp":"ok"}` |
| `{"get":"netconfig"}` | `{"ipaddr":…,"gateway":…,"netmask":…,"port":…}` |
| `{"get":"version"}` | `{"version":"V1.0"}` |
| `{"RS485addr":"22"}` / `{"get":"RS485addr"}` | `{"resp":"ok"}` / `{"RS485addr":"22"}` |
| `{"baudrate":"9600"}` / `{"get":"baudrate"}` | `{"resp":"ok"}` (efekt po restarcie) |
| `{"dhcp":"on"}` / `{"get":"dhcp"}` | `{"dhcp":"on"|"off"}` |
| `{"reboot":"1"}` | — |
| cokolwiek nieznanego | `{"resp":"unsupported"}` |

### 3. Co z tego bierzemy do nowego firmware

- **MQTT** — główny interfejs (HA discovery, 8 switchy).
- **JSON/TCP:2000** — zgodność wsteczna: istniejące skrypty (`Fescron/DFRobot-RLY8-python3`)
  i aplikacja DFRobot dalej działają.
- **Ramki `55 AA` na RS485** — bo to jedyny sposób na inne urządzenia na magistrali.
- Sieć: DHCP z fallbackiem na statyczne `192.168.1.88/24`, port 2000, MQTT 1883.

### 4. Czego nie wiemy bez zrzutu firmware

- kody `cmd` dla nazwy/wersji/sieci/DHCP/restartu (patrz wyżej),
- zachowanie po zaniku zasilania (przywracanie stanów przekaźników?),
- timing/keep-alive i maks. liczba klientów TCP na porcie 2000,
- dokładne znaczenie `value` (`0x00`/`0x01`) względem stanu styku.
