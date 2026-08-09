# MidiStudio-2 CoreMIDI Bridge (macOS, Apple Silicon)

User-space драйвер-міст для старої USB-MIDI клавіатури **MidiTech
MidiStart-2 / MidiStudio-2 / MidiControl-2** (та OEM-варіантів на кшталт
Terratec M48) на сучасному macOS з чипом Apple Silicon (M1/M2/M3...).

```
USB\VID_7104&PID_2202
```

## Навіщо це потрібно

Пристрій **не class-compliant** (не є стандартним USB Audio/MIDI-streaming
класом) і на всіх платформах історично вимагав окремий вендорський
kernel-mode драйвер:

- Вендорський Mac-драйвер (2004–2006, `CME-UF-MIDI-*.pkg`) — це
  **kernel extension (kext)** під PowerPC/ранній Intel Mac OS X, 32-бітний,
  без нотаризації і без arm64-зрізу. Він **не запускається на Apple Silicon
  в принципі** — не через права доступу чи SIP, а через фундаментальну
  несумісність архітектури й моделі драйверів (Apple Silicon вимагає
  DriverKit/System Extensions замість legacy kext).

Цей проєкт вирішує задачу **повністю в user-space**, без жодного kernel
extension: `libusb` читає сирі USB-дані напряму, `CoreMIDI` створює
віртуальне MIDI-джерело в системі. Працює нативно на arm64, без Rosetta,
без вимкнення SIP.

## Протокол пристрою (реверс-інжиніринг)

Пристрій має 4 bulk-ендпоінти (interface 0, class 0x02/0x01 — вендорський
варіант, не офіційний USB Audio class):

| Pipe | Endpoint | Напрямок | maxpacket |
|------|----------|----------|-----------|
| 0 | 0x81 | IN  | 16 |
| 1 | 0x01 | OUT | 16 |
| **2** | **0x82** | **IN** | **64** |
| 3 | 0x02 | OUT | 64 |

Дані читаються саме з **Pipe2 (endpoint 0x82)** — це відповідає тому, що
оригінальний Windows-драйвер відкриває `\PIPE02` (нумерація пайпів за
порядком у дескрипторі, класична для WDK-семпла "Bulkusb").

Кожна MIDI-подія — 4 байти:

```
byte0 = CIN nibble (USB-MIDI CIN-таблиця: 0x09 Note On, 0x08 Note Off,
        0x0B Control Change, 0x0E Pitch Bend Change)
byte1 = MIDI status byte
byte2 = data1
byte3 = data2
```

Підтверджено живим захопленням трафіку. Приклади:

```
09 91 47 47   -> Note On,  канал 2, нота 71, velocity 71
09 91 47 00   -> Note Off, канал 2, нота 71 (velocity=0)
0b b1 01 22   -> Control Change, канал 2, CC#1 (Modulation), value=0x22
0e e1 65 5d   -> Pitch Bend Change, канал 2, value=(0x65,0x5d)
```

Обидва коліщатка (mod і pitch-bend) дублюють кожну подію одразу на двох
MIDI-каналах в одному 8-байтовому USB-пакеті (дві 4-байтові події
підряд) — апаратна особливість прошивки.

### Виправлена помилка з оригінального Windows-драйвера

В оригінальному відкритому Windows-драйвері
([MaximShershavikov/MidiStudio-2-USB-MIDI-Keyboard-Driver](https://github.com/MaximShershavikov/MidiStudio-2-USB-MIDI-Keyboard-Driver),
GPLv3), у гілці з фіксом реверснутого коліщатка, константи заголовків
переплутані місцями відносно реальних MIDI-статус-байтів:

- `PITCH_BEND_ROLLE_HEAD` (`0x0b,0xb1`) — насправді це заголовок
  **Control Change** (модуляція), не pitch bend.
- `MOD_ROLLE_HEAD` (`0x0e,0xe1`) — насправді це заголовок **Pitch Bend
  Change**.

Підтверджено живим дампом трафіку з реального пристрою. Логіка інверсії
(`127 - value`, для виправлення фізично реверснутого коліщатка) в
оригіналі працює коректно попри переплутані назви — просто найменування
вводить в оману. У коментарях цього проєкту назви виправлені.

## Збірка

```bash
brew install libusb
clang -O2 -o midistudio2_bridge midistudio2_bridge.c \
    -I/opt/homebrew/include/libusb-1.0 -L/opt/homebrew/lib -lusb-1.0 \
    -framework CoreMIDI -framework CoreFoundation
```

## Використання

```bash
# Дерево USB-дескрипторів (діагностика)
./midistudio2_bridge --describe

# Сирі байти в консоль, без CoreMIDI (діагностика)
./midistudio2_bridge --dump

# Ручний вибір ендпоінта, якщо автовизначення підвело
./midistudio2_bridge --dump --ep=0x82

# Реальний міст: створює віртуальне CoreMIDI-джерело "MidiStudio-2"
./midistudio2_bridge
```

Після запуску без прапорців пристрій з'явиться як віртуальне MIDI-джерело
**"MidiStudio-2"** в Audio MIDI Setup / будь-якому DAW.

## Права доступу

Звичайний user-space USB-доступ через libusb/IOKit на macOS не потребує
root для vendor-specific пристроїв. Якщо `libusb_claim_interface()`
падає з `LIBUSB_ERROR_ACCESS` — перевір System Settings → Privacy &
Security, або тимчасово запусти з `sudo` для діагностики.

## Подяка

Протокольні факти (номер pipe, заголовки байтів коліщаток, формула
інверсії) відтворені на основі аналізу відкритого коду Windows-драйвера
для цього ж пристрою:
[MaximShershavikov/MidiStudio-2-USB-MIDI-Keyboard-Driver](https://github.com/MaximShershavikov/MidiStudio-2-USB-MIDI-Keyboard-Driver)
(GPLv3). Код цього проєкту написаний з нуля під іншу платформу й
архітектуру (libusb + CoreMIDI замість WDM/kernel-mode).

## Ліцензія

GPLv3, див. [LICENSE](./LICENSE).
