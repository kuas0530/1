#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h> // 關鍵修正：解決 bool 錯誤

#include "bsp/board_api.h"
#include "device/usbd_pvt.h"
#include "tusb.h"

#include "hid_reports.h"
#include "usb_descriptors.h"

#include "pico/multicore.h"
#include "pico/mutex.h"
#include "pico/util/queue.h"

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+

enum
{
    BLINK_INIT_MOUNTED = 50,
    BLINK_NOT_MOUNTED = 250,
    BLINK_MOUNTED = 1000,
    BLINK_SUSPENDED = 2500,
};
static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;
void led_blinking_task(void);

typedef struct
{
    uint8_t dev_addr;
    uint8_t idx;
    uint8_t protocol;
} hid_device_t;
hid_device_t hid_devices[CFG_TUH_HID];

static int mouse_to_gamepade = 0;
static mutex_t mouse_to_gamepade_mutex;

typedef struct
{
    uint8_t modifier;
    uint8_t keycode[6];
} keyboard_report_t;

typedef struct
{
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
} mouse_report_t;

typedef struct
{
    int8_t x; int8_t y; int8_t z; int8_t rz;
    int8_t rx; int8_t ry; uint8_t hat; uint32_t buttons;
} gamepad_report_t;

#define IDLE_TIMEOUT_MS 10
static bool should_center = false;
static uint32_t last_mouse_move_time = 0;
mouse_report_t last_rpt = { 0 };

#define KEYBOARD_QUEUE_SIZE 8
#define MOUSE_QUEUE_SIZE 8
#define GAMEPAD_QUEUE_SIZE 8
static queue_t keyboard_report_queue;
static queue_t mouse_report_queue;
static queue_t gamepad_report_queue;

// cdc helper
#if USE_CDC
static void __attribute__((format(printf, 1, 2))) cdc_debug_print(const char* fmt, ...);
static void cdc_debug_print(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (!tud_cdc_connected()) return;
    uint32_t len = (uint32_t)n;
    const uint8_t* p = (const uint8_t*)buf;
    while (len) {
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            tud_task();
            avail = tud_cdc_write_available();
            if (avail == 0) break;
        }
        uint32_t chunk = (len > avail) ? avail : len;
        tud_cdc_write(p, chunk);
        p += chunk;
        len -= chunk;
    }
    tud_cdc_write_flush();
}
#define CDC_LOG(...) cdc_debug_print(__VA_ARGS__)
#else
#define CDC_LOG(...) ((void)0)
#endif

// 核心1入口函数
void core1_entry(void)
{
    tusb_rhport_init_t host_init = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL };
    tusb_init(BOARD_TUH_RHPORT, &host_init);
    while (1) {
        tuh_task();
        led_blinking_task();
    }
}

/*------------- MAIN -------------*/
int main(void)
{
    board_init();
    mutex_init(&mouse_to_gamepade_mutex);
    queue_init(&keyboard_report_queue, sizeof(keyboard_report_t), KEYBOARD_QUEUE_SIZE);
    queue_init(&mouse_report_queue, sizeof(mouse_report_t), MOUSE_QUEUE_SIZE);
    queue_init(&gamepad_report_queue, sizeof(gamepad_report_t), GAMEPAD_QUEUE_SIZE);

    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    if (board_init_after_tusb) board_init_after_tusb();

    multicore_launch_core1(core1_entry);

    while (1)
    {
        tud_task();

        // 鍵盤處理
        keyboard_report_t kbd_report;
        if (usbd_edpt_ready(0, 0x83) && queue_try_remove(&keyboard_report_queue, &kbd_report)) {
            bool success = tud_hid_n_keyboard_report(REPORT_ID_KEYBOARD, 0, kbd_report.modifier, kbd_report.keycode);
            if (!success) queue_try_add(&keyboard_report_queue, &kbd_report);
        }

        // 滑鼠處理
        mouse_report_t rpt;
        mutex_enter_blocking(&mouse_to_gamepade_mutex);
        int convert_to_gamepad = mouse_to_gamepade;
        mutex_exit(&mouse_to_gamepade_mutex);

        if (usbd_edpt_ready(0, 0x84) && queue_try_remove(&mouse_report_queue, &rpt))
        {
            uint8_t custom_report[64] = { 0 };
            custom_report[0] = REPORT_ID_CUSTOM;
            memcpy(&custom_report[1], &rpt, sizeof(mouse_report_t));
            tud_hid_n_report(REPORT_ID_CUSTOM, 0, custom_report, 64);

            bool success = false;
            if (convert_to_gamepad == 1) {
                tud_hid_n_gamepad_report(REPORT_ID_GAMEPAD, 0, 0, 0, (rpt.x > 0) ? 127 : ((rpt.x == 0) ? 0 : -127), (rpt.y > 0) ? 127 : ((rpt.y == 0) ? 0 : -127), 0, 0, 0, 0);
                rpt.x = 0; rpt.y = 0;
                success = tud_hid_n_mouse_report(REPORT_ID_MOUSE, 0, rpt.buttons, rpt.x, rpt.y, rpt.wheel, 0);
                last_mouse_move_time = board_millis();
            }
            else if (convert_to_gamepad == 2) {
                success = true;
            }
            else {
                success = tud_hid_n_mouse_report(REPORT_ID_MOUSE, 0, rpt.buttons, rpt.x, rpt.y, rpt.wheel, 0);
            }

            if (!success) {
                CDC_LOG("发送失败，尝试重新放回队列\n");
                if (!queue_try_add(&mouse_report_queue, &rpt)) CDC_LOG("警告：队列已满\n");
            }
        }
        else
        {
            uint32_t now = board_millis();
            if (now - last_mouse_move_time > IDLE_TIMEOUT_MS) {
                should_center = true;
                last_mouse_move_time = now;
            }
            if (convert_to_gamepad == 1 && should_center && usbd_edpt_ready(0, 0x85)) {
                tud_hid_n_gamepad_report(REPORT_ID_GAMEPAD, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                should_center = false;
            }
        }

        // 手柄處理
        gamepad_report_t gmp_report;
        if (usbd_edpt_ready(0, 0x85) && queue_try_remove(&gamepad_report_queue, &gmp_report)) {
            bool success = tud_hid_n_gamepad_report(REPORT_ID_GAMEPAD, 0, gmp_report.x, gmp_report.y, gmp_report.z, gmp_report.rz, gmp_report.rx, gmp_report.ry, gmp_report.hat, gmp_report.buttons);
            if (!success) {
                if (!queue_try_add(&gamepad_report_queue, &gmp_report)) CDC_LOG("Warning: Gamepad queue full\n");
            }
        }
    }
    return 0;
}

//--------------------------------------------------------------------+
// USB HID 解析邏輯
//--------------------------------------------------------------------+

void process_hid_report(uint8_t const* report, uint16_t len)
{
    (void)len;
    uint8_t cmd_id = report[0];
    const uint8_t* payload = &report[1];

    switch (cmd_id)
    {
    case HID_ITF_PROTOCOL_MOUSE:
    {
        uint8_t buttons = payload[0];
        int8_t x = (int8_t)payload[1];
        int8_t y = (int8_t)payload[2];
        int8_t wheel = (int8_t)payload[3];

        mouse_report_t rpt;
        if (queue_try_remove(&mouse_report_queue, &rpt)) {
            buttons = rpt.buttons | buttons;
            x = rpt.x + x;
            y = rpt.y + y;
            wheel = rpt.wheel + wheel;
        }
        else {
            buttons = last_rpt.buttons | buttons;
        }

        mouse_report_t rpt_ = { .buttons = buttons, .x = x, .y = y, .wheel = wheel };
        if (!queue_try_add(&mouse_report_queue, &rpt_)) CDC_LOG("WARN: Mouse queue full\n");
        break;
    }
    case HID_ITF_PROTOCOL_KEYBOARD:
    {
        uint8_t modifier = payload[0];
        uint8_t keycode[6] = { 0 };
        memcpy(keycode, &payload[1], 6);
        tud_hid_n_keyboard_report(REPORT_ID_KEYBOARD, 0, modifier, keycode);
        break;
    }
    case HID_ITF_PROTOCOL_NONE: // GAMEPAD
    {
        if (len >= 11) {
            gamepad_report_t gmp_report = {
                .x = (int8_t)report[0],
                .y = (int8_t)report[1],
                .z = (int8_t)report[2],
                .rz = (int8_t)report[3],
                .rx = (int8_t)report[4],
                .ry = (int8_t)report[5],
                .hat = report[6],
                .buttons = (uint32_t)report[7] | ((uint32_t)report[8] << 8) |
                           ((uint32_t)report[9] << 16) | ((uint32_t)report[10] << 24) };
            if (!queue_try_add(&gamepad_report_queue, &gmp_report)) CDC_LOG("WARN: Gamepad queue full\n");
        }
        break;
    }
    case 3:
    {
        mutex_enter_blocking(&mouse_to_gamepade_mutex);
        mouse_to_gamepade = (int)payload[0];
        mutex_exit(&mouse_to_gamepade_mutex);
        break;
    }
    default: break;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) { (void)instance; return 0; }

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    if (itf == REPORT_ID_CUSTOM && bufsize == 64) process_hid_report(buffer, bufsize);
}

void tuh_umount_cb(uint8_t dev_addr) { CDC_LOG("Device %u unmounted\r\n", dev_addr); }

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    blink_interval_ms = BLINK_MOUNTED;
    hid_devices[instance].dev_addr = dev_addr;
    hid_devices[instance].idx = instance;
    hid_devices[instance].protocol = HID_PROTOCOL_REPORT;

    uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
    if (proto == HID_ITF_PROTOCOL_MOUSE) {
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    }
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t instance, uint8_t protocol) { hid_devices[instance].protocol = protocol; }


// =========================================================================================
// 修正區域：tuh_hid_report_received_cb
// =========================================================================================

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
    CDC_LOG("[HID] Len=%u Data=%02X %02X %02X %02X\n", len, report[0], report[1], report[2], report[3]);

    if (len == 0) {
        board_delay(1000);
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);

    if (proto == HID_ITF_PROTOCOL_MOUSE)
    {
        uint8_t buttons = 0;
        int8_t x = 0;
        int8_t y = 0;
        int8_t wheel = 0;

        // 檢查是否為 Logitech G Pro SL2 無線接收器 (VID=0x046D, PID=0xC54D)
        // ⚠️ 這裡使用 hardcode 檢查，因為 tuh_device_get_vid 需要開啟很多依賴
        // 我們假設如果你接的是 G Pro，它就會生效
        bool is_logi_sl2_wireless = (report[0] != 0 || len >= 4); // 簡單判斷有數據

        if (len >= 4)
        {
            // 嘗試修正：針對你的描述 (X軸不動，Y軸亂跳但有反應)
            // 假設 X 軸數據在 report[2]，Y 軸在 report[1]

            buttons = report[0];
            int8_t raw_byte_1 = (int8_t)report[1];
            int8_t raw_byte_2 = (int8_t)report[2];
            wheel = (int8_t)report[3];

            // 軸線互換 + 反轉
            // 你的測試：向左变向上，向右变向下
            // 這代表：原來的 Y 軸數據 (report[2]) 其實是 X 軸的動作
            x = raw_byte_2;
            y = -raw_byte_1;

            CDC_LOG("Fix: X=%d, Y=%d\n", x, y);
        }

        mouse_report_t rpt_ = { .buttons = buttons, .x = x, .y = y, .wheel = wheel };
        if (!queue_try_add(&mouse_report_queue, &rpt_)) {
            CDC_LOG("WARN: Mouse queue full\n");
        }
        else {
            last_rpt = rpt_;
        }
    }
    else if (proto == HID_ITF_PROTOCOL_KEYBOARD && len >= 8)
    {
        keyboard_report_t kbd_report;
        kbd_report.modifier = report[0];
        for (int i = 0; i < 6; i++) kbd_report.keycode[i] = report[2 + i];
        queue_try_add(&keyboard_report_queue, &kbd_report);
    }

    tuh_hid_receive_report(dev_addr, instance);
}

void led_blinking_task(void)
{
    static uint32_t start_ms = 0;
    static bool led_state = false;
    if (board_millis() - start_ms < blink_interval_ms) return;
    start_ms += blink_interval_ms;
    board_led_write(led_state);
    led_state = 1 - led_state;
}