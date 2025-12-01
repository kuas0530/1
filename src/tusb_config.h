#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// 使用 PIO-USB 軟體模擬 Host，原生 USB 做 Device
#define CFG_TUSB_MCU                OPT_MCU_RP2040
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_DEBUG              0

// --- Device 設定 (接電腦) ---
#define BOARD_TUD_RHPORT            0    // 原生 Port 0
#define CFG_TUD_ENABLED             1    // 啟用 Device
#define CFG_TUD_HID                 1    // 啟用 HID
#define CFG_TUD_CDC                 0    // 不用 CDC
#define CFG_TUD_MSC                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_ENDPOINT0_SIZE      64
#define CFG_TUD_HID_EP_BUFSIZE      64

// --- Host 設定 (接滑鼠) ---
#define BOARD_TUH_RHPORT            1    // PIO Port 1
#define CFG_TUH_ENABLED             1    // 啟用 Host
#define CFG_TUH_RPI_PIO_USB         1    // 💥 關鍵：啟用 PIO USB 支援
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_DEVICE_MAX          1
#define CFG_TUH_HID                 1    // 啟用 HID Host
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

#ifdef __cplusplus
}
#endif

#endif