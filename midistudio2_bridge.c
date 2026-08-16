/*
 * midistudio2_bridge.c
 *
 * User-space міст: libusb (читання сирих даних з MidiTech MidiStudio-2,
 * VID_7104 / PID_2202) -> CoreMIDI (віртуальне джерело в системі).
 *
 * Працює на macOS Apple Silicon (arm64) БЕЗ kernel extension:
 * увесь код виконується в user-space, як звичайна утиліта командного рядка.
 * Оригінальний вендорський Mac-драйвер (kext, PowerPC/раній Intel, 2004-2006)
 * не запускається на Apple Silicon в принципі — застаріла архітектура
 * kernel extension, відсутня нотаризація і arm64-зріз.
 *
 * Протокол пристрою (структура 4-байтових MIDI-подій, номер pipe=2,
 * заголовки коліщаток pitch-bend/modulation) відтворений на основі
 * аналізу відкритого Windows WDM-драйвера для цього ж пристрою:
 *   https://github.com/MaximShershavikov/MidiStudio-2-USB-MIDI-Keyboard-Driver
 *   (GPLv3). Цей .c-файл написаний з нуля під зовсім іншу платформу й
 *   архітектуру (libusb + CoreMIDI замість WDM/kernel-mode), але
 *   протокольні факти (заголовки байтів, формула інверсії коліщатка
 *   127-value) звідти. Підтверджено живим захопленням трафіку з реального
 *   пристрою — за фактом виявлено, що назви констант PITCH_BEND_ROLLE_HEAD
 *   / MOD_ROLLE_HEAD в оригінальному репозиторії переплутані місцями
 *   (0x0b/CC = modulation, 0x0e/pitch-bend = pitch bend), у цьому файлі
 *   назви виправлені відповідно до реальних MIDI-статус-байтів.
 *
 * Ліцензія: GPLv3, див. LICENSE в цьому репозиторії.
 *
 * Збірка (Apple Silicon, Homebrew arm64):
 *   brew install libusb
 *   clang -O2 -o midistudio2_bridge midistudio2_bridge.c \
 *       -I/opt/homebrew/include/libusb-1.0 -L/opt/homebrew/lib -lusb-1.0 \
 *       -framework CoreMIDI -framework CoreFoundation
 *
 * Запуск:
 *   ./midistudio2_bridge --describe   # дерево USB-дескрипторів
 *   ./midistudio2_bridge --dump       # сирі байти в консоль (діагностика)
 *   ./midistudio2_bridge              # реальний міст у CoreMIDI
 *
 * Права: звичайний user-space USB-доступ через libusb/IOKit на macOS
 * не потребує root для vendor-specific пристроїв. Якщо
 * libusb_claim_interface() падає з LIBUSB_ERROR_ACCESS — спробуй
 * System Settings -> Privacy & Security, або тимчасово sudo для тесту.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <libusb.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#define VENDOR_ID   0x7104
#define PRODUCT_ID  0x2202
#define READ_TIMEOUT_MS 1000
#define READ_BUF_SIZE   64   /* типовий wMaxPacketSize для old-school int/bulk EP */
#define RECONNECT_WAIT_US 500000

static volatile sig_atomic_t g_running = 1;
static void on_sigint(int sig) { (void)sig; g_running = 0; }

typedef struct {
    libusb_device_handle *handle;
    int iface_num;
    unsigned char ep_in_addr;
    unsigned char ep_in_type;   /* LIBUSB_TRANSFER_TYPE_BULK / _INTERRUPT */
    int ep_in_maxpacket;
} usb_ctx_t;

/* Друкує повне дерево дескрипторів: усі інтерфейси/альтернативи/ендпоінти.
 * Потрібно, якщо перший знайдений IN-ендпоінт "мовчить" — можливо, є ще
 * інтерфейси, які ми не бачимо в звичайному режимі. */
static void describe_device(libusb_device *dev)
{
    struct libusb_device_descriptor dd;
    libusb_get_device_descriptor(dev, &dd);
    printf("Device: bNumConfigurations=%d bDeviceClass=0x%02x\n",
           dd.bNumConfigurations, dd.bDeviceClass);

    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) {
        printf("Не вдалось прочитати активний config descriptor\n");
        return;
    }
    printf("Config: bNumInterfaces=%d bConfigurationValue=%d\n",
           cfg->bNumInterfaces, cfg->bConfigurationValue);

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *idesc = &iface->altsetting[a];
            printf("  Interface %d alt %d: class=0x%02x subclass=0x%02x proto=0x%02x endpoints=%d\n",
                   idesc->bInterfaceNumber, idesc->bAlternateSetting,
                   idesc->bInterfaceClass, idesc->bInterfaceSubClass,
                   idesc->bInterfaceProtocol, idesc->bNumEndpoints);
            for (int e = 0; e < idesc->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &idesc->endpoint[e];
                int is_in = (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;
                int type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                const char *type_s =
                    type == LIBUSB_TRANSFER_TYPE_BULK ? "bulk" :
                    type == LIBUSB_TRANSFER_TYPE_INTERRUPT ? "interrupt" :
                    type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS ? "iso" : "control";
                printf("    EP 0x%02x %s %s maxpacket=%d interval=%d\n",
                       ep->bEndpointAddress, is_in ? "IN " : "OUT",
                       type_s, ep->wMaxPacketSize, ep->bInterval);
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

/* Знаходимо ендпоінт за індексом пайпа в порядку дескриптора —
 * саме так нумерує пайпи класичний WDK-семпл "Bulkusb", на якому
 * побудований оригінальний Windows-драйвер (\PIPE00, \PIPE01, \PIPE02...).
 * MidiManager.cpp відкриває САМЕ \PIPE02, тому за замовчуванням шукаємо
 * ендпоінт з індексом 2 (0-based) серед УСІХ ендпоінтів інтерфейсу
 * (IN і OUT разом, у порядку, як вони йдуть у дескрипторі). */
static int find_endpoint_by_pipe_index(libusb_device *dev, usb_ctx_t *ctx, int want_index)
{
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) {
        return -1;
    }

    int idx = 0;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *idesc = &iface->altsetting[a];
            for (int e = 0; e < idesc->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &idesc->endpoint[e];
                if (idx == want_index) {
                    int is_in = (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;
                    int type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                    if (!is_in) {
                        fprintf(stderr, "Pipe %d = 0x%02x, але це OUT-ендпоінт, не IN!\n",
                                want_index, ep->bEndpointAddress);
                        libusb_free_config_descriptor(cfg);
                        return -1;
                    }
                    ctx->iface_num = idesc->bInterfaceNumber;
                    ctx->ep_in_addr = ep->bEndpointAddress;
                    ctx->ep_in_type = type;
                    ctx->ep_in_maxpacket = ep->wMaxPacketSize;
                    libusb_free_config_descriptor(cfg);
                    return 0;
                }
                idx++;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    fprintf(stderr, "Pipe index %d не знайдено (усього ендпоінтів: %d)\n", want_index, idx);
    return -1;
}

static void usb_close(usb_ctx_t *ctx)
{
    if (!ctx || !ctx->handle) {
        return;
    }
    libusb_release_interface(ctx->handle, ctx->iface_num);
    libusb_close(ctx->handle);
    ctx->handle = NULL;
}

/* libusb_init має бути викликаний один раз у main. Відсутній пристрій —
 * не помилка: caller чекає і пробує знову. Інші збої логуються. */
static int usb_open(usb_ctx_t *ctx, int pipe_index, int force_ep_addr)
{
    usb_close(ctx);
    memset(ctx, 0, sizeof(*ctx));

    libusb_device_handle *h = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, PRODUCT_ID);
    if (!h) {
        return -1;
    }
    ctx->handle = h;

    libusb_device *dev = libusb_get_device(h);

    if (force_ep_addr >= 0) {
        /* ручний override: --ep=0x82 */
        struct libusb_config_descriptor *cfg = NULL;
        if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
            fprintf(stderr, "Не вдалось прочитати config descriptor.\n");
            usb_close(ctx);
            return -1;
        }
        int found = 0;
        for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
            const struct libusb_interface *iface = &cfg->interface[i];
            for (int a = 0; a < iface->num_altsetting && !found; a++) {
                const struct libusb_interface_descriptor *idesc = &iface->altsetting[a];
                for (int e = 0; e < idesc->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ep = &idesc->endpoint[e];
                    if (ep->bEndpointAddress == force_ep_addr) {
                        ctx->iface_num = idesc->bInterfaceNumber;
                        ctx->ep_in_addr = ep->bEndpointAddress;
                        ctx->ep_in_type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                        ctx->ep_in_maxpacket = ep->wMaxPacketSize;
                        found = 1;
                        break;
                    }
                }
            }
        }
        libusb_free_config_descriptor(cfg);
        if (!found) {
            fprintf(stderr, "Ендпоінт 0x%02x не знайдено в дескрипторі.\n", force_ep_addr);
            usb_close(ctx);
            return -1;
        }
    } else if (find_endpoint_by_pipe_index(dev, ctx, pipe_index) != 0) {
        fprintf(stderr, "Не вдалось знайти пайп з індексом %d.\n", pipe_index);
        usb_close(ctx);
        return -1;
    }

    if (libusb_kernel_driver_active(h, ctx->iface_num) == 1) {
        libusb_detach_kernel_driver(h, ctx->iface_num);
    }
    if (libusb_claim_interface(h, ctx->iface_num) != 0) {
        fprintf(stderr, "Не вдалось claim_interface(%d). Перевір права доступу.\n", ctx->iface_num);
        usb_close(ctx);
        return -1;
    }

    /* Еквівалент IOCTL_INTUSB_RESET_PIPE з Windows-драйвера:
     * скидає data-toggle і зняти можливий stall на ендпоінті. */
    int ch = libusb_clear_halt(h, ctx->ep_in_addr);
    fprintf(stderr, "clear_halt(0x%02x) -> %s\n", ctx->ep_in_addr, libusb_error_name(ch));

    fprintf(stderr, "OK: interface=%d ep_in=0x%02x type=%s maxpacket=%d\n",
            ctx->iface_num, ctx->ep_in_addr,
            ctx->ep_in_type == LIBUSB_TRANSFER_TYPE_BULK ? "bulk" : "interrupt",
            ctx->ep_in_maxpacket);
    return 0;
}

static int usb_read(usb_ctx_t *ctx, unsigned char *buf, int buflen, int *actual)
{
    if (ctx->ep_in_type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
        return libusb_interrupt_transfer(ctx->handle, ctx->ep_in_addr, buf, buflen,
                                          actual, READ_TIMEOUT_MS);
    } else {
        return libusb_bulk_transfer(ctx->handle, ctx->ep_in_addr, buf, buflen,
                                     actual, READ_TIMEOUT_MS);
    }
}

/*
 * Формат підтверджено живим захопленням трафіку (--dump) з реального
 * пристрою: дані читаються по 4 байти на "подію" з Pipe2 (endpoint 0x82,
 * bulk, maxpacket=64):
 *   byte0 = CIN-нібл (стандартна USB-MIDI CIN-таблиця: 0x09=Note On,
 *           0x08=Note Off, 0x0B=Control Change, 0x0E=Pitch Bend Change)
 *   byte1 = MIDI status byte
 *   byte2 = data1
 *   byte3 = data2
 * Далі це просто пакується у стандартний 3-байтовий MIDI short message.
 *
 * Обидва коліщатка (mod і pitch-bend) дублюють кожну подію одразу на
 * двох MIDI-каналах в одному 8-байтовому USB-пакеті (дві 4-байтові події
 * підряд) — апаратна особливість прошивки, не помилка парсингу. Ми
 * пересилаємо обидві копії в CoreMIDI, це нешкідливо.
 */
static void handle_event_bytes(const unsigned char *ev, MIDIEndpointRef src)
{
    unsigned char status = ev[1];
    unsigned char data1  = ev[2];
    unsigned char data2  = ev[3];

    if (status == 0x00 && data1 == 0x00 && data2 == 0x00) {
        return; /* порожній / padding-чанк */
    }

    Byte midiBytes[3] = { status, data1, data2 };
    Byte packetBuf[128];
    MIDIPacketList *pktlist = (MIDIPacketList *)packetBuf;
    MIDIPacket *pkt = MIDIPacketListInit(pktlist);
    pkt = MIDIPacketListAdd(pktlist, sizeof(packetBuf), pkt, 0, 3, midiBytes);
    if (pkt) {
        MIDIReceived(src, pktlist);
    }
}

/* Після unplug нота може лишитись затиснутою в DAW. */
static void panic_hanging_notes(MIDIEndpointRef src)
{
    if (!src) {
        return;
    }
    for (int ch = 0; ch < 16; ch++) {
        unsigned char msgs[2][3] = {
            { (unsigned char)(0xB0 | ch), 123, 0 }, /* All Notes Off */
            { (unsigned char)(0xB0 | ch), 120, 0 }, /* All Sound Off */
        };
        for (int i = 0; i < 2; i++) {
            Byte packetBuf[128];
            MIDIPacketList *pktlist = (MIDIPacketList *)packetBuf;
            MIDIPacket *pkt = MIDIPacketListInit(pktlist);
            pkt = MIDIPacketListAdd(pktlist, sizeof(packetBuf), pkt, 0, 3, msgs[i]);
            if (pkt) {
                MIDIReceived(src, pktlist);
            }
        }
    }
}

int main(int argc, char **argv)
{
    int dump_only  = 0;
    int describe   = 0;
    int force_ep   = -1;   /* -1 = автовибір за pipe-індексом */
    int pipe_index = 2;    /* PIPE02, як в оригінальному Windows-драйвері */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0) dump_only = 1;
        else if (strcmp(argv[i], "--describe") == 0) describe = 1;
        else if (strncmp(argv[i], "--ep=", 5) == 0) force_ep = (int)strtol(argv[i] + 5, NULL, 0);
        else if (strncmp(argv[i], "--pipe=", 7) == 0) pipe_index = atoi(argv[i] + 7);
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (describe) {
        if (libusb_init(NULL) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
        libusb_device_handle *h = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, PRODUCT_ID);
        if (!h) { fprintf(stderr, "Пристрій не знайдено\n"); return 1; }
        describe_device(libusb_get_device(h));
        libusb_close(h);
        libusb_exit(NULL);
        return 0;
    }

    if (libusb_init(NULL) != 0) {
        fprintf(stderr, "libusb_init failed\n");
        return 1;
    }

    usb_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    MIDIClientRef client = 0;
    MIDIEndpointRef src = 0;

    if (!dump_only) {
        MIDIClientCreate(CFSTR("MidiStudio-2 Bridge"), NULL, NULL, &client);
        MIDISourceCreate(client, CFSTR("MidiStudio-2"), &src);
        fprintf(stderr, "CoreMIDI virtual source \"MidiStudio-2\" створено.\n");
    } else {
        fprintf(stderr, "Режим --dump: тільки друк сирих байтів, CoreMIDI не використовується.\n");
    }

    unsigned char buf[READ_BUF_SIZE];
    int timeout_count = 0;
    int ever_connected = 0;
    int waiting_logged = 0;

    while (g_running) {
        if (!ctx.handle) {
            if (usb_open(&ctx, pipe_index, force_ep) != 0) {
                if (!waiting_logged) {
                    fprintf(stderr,
                            "Чекаю MidiStudio-2 (VID_%04x&PID_%04x). Підключи клавіатуру.\n",
                            VENDOR_ID, PRODUCT_ID);
                    waiting_logged = 1;
                }
                usleep(RECONNECT_WAIT_US);
                continue;
            }
            if (ever_connected) {
                fprintf(stderr, "USB reconnect успішний.\n");
            }
            ever_connected = 1;
            waiting_logged = 0;
            timeout_count = 0;
        }

        int actual = 0;
        int r = usb_read(&ctx, buf, sizeof(buf), &actual);

        if (r == LIBUSB_ERROR_TIMEOUT) {
            timeout_count++;
            if (dump_only && (timeout_count % 3 == 0)) {
                fprintf(stderr, "... чекаю дані (%d timeout-ів, ep=0x%02x) ...\n",
                        timeout_count, ctx.ep_in_addr);
            }
            continue;
        }
        if (r == LIBUSB_ERROR_INTERRUPTED) {
            continue;
        }
        timeout_count = 0;
        if (r != 0) {
            fprintf(stderr, "USB %s — чекаю reconnect.\n", libusb_error_name(r));
            panic_hanging_notes(src);
            usb_close(&ctx);
            waiting_logged = 0;
            usleep(RECONNECT_WAIT_US);
            continue;
        }
        if (actual <= 0) {
            continue;
        }

        if (dump_only) {
            printf("[%2d bytes] ", actual);
            for (int i = 0; i < actual; i++) printf("%02x ", buf[i]);
            printf("\n");
            fflush(stdout);
            continue;
        }

        for (int off = 0; off + 4 <= actual; off += 4) {
            handle_event_bytes(&buf[off], src);
        }
    }

    fprintf(stderr, "Завершення...\n");
    panic_hanging_notes(src);
    usb_close(&ctx);
    if (client) MIDIClientDispose(client);
    libusb_exit(NULL);
    return 0;
}
