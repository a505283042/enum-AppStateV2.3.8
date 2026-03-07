#include <Arduino.h>
#include <SPI.h>

// 兼容不同 MFRC522 库头文件大小写
#if __has_include(<MFRC522.h>)
  #include <MFRC522.h>
#elif __has_include(<mfrc522.h>)
  #include <mfrc522.h>
#else
  #error "MFRC522 library header not found. Please add MFRC522 library in platformio.ini"
#endif

#include "board/board_pins.h"
#include "nfc/nfc.h"

static MFRC522 g_mfrc522(PIN_RC522_CS, PIN_RC522_RST);

void nfc_init()
{
    Serial.println("[NFC] init...");

    // 不要 SPI.begin()，全局 SPI 已在 board_spi_init() 配好了
    g_mfrc522.PCD_Init();
    delay(50);

    Serial.println("[NFC] ready");
}

/* 轮询NFC模块 - 检测NFC标签/卡片并处理它们 */
void nfc_poll(void)
{
    // 轮询NFC标签/卡片
    // 目前仅记录轮询已发生
    // 在实际实现中，这里会检测NFC标签并进行处理
    // TODO: 实现NFC标签检测和处理功能
}