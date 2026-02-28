#include "storage/storage.h"
#include "board/board_pins.h"
#include "board/board_spi.h"

#include <Arduino.h>
#include <FS.h>
#include <SdFat.h>

// 全局 SD 文件系统对象（SdFat = FAT16/32，足够用）
SdFat sd;
static bool storage_ready = false;


bool storage_init(void)
{
    Serial.println("[STORAGE] init (SdFat)");

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    
    // 用独立 SPI_SD 初始化 SdFat
    // DEDICATED_SPI：SdFat 会更积极地管理 SPI（更稳）
    SdSpiConfig cfg(PIN_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(16), &SPI_SD);

    if (!sd.begin(cfg)) {
        Serial.println("[STORAGE] SdFat mount FAILED");
        storage_ready = false;
        return false;
    }

    Serial.println("[STORAGE] SdFat mount OK");
    storage_ready = true;

    storage_list_root();
    return true;
}



bool storage_is_ready(void)
{
    return storage_ready;
}

void storage_list_root(void)
{
    if (!storage_ready) return;

    SdFile root;
    if (!root.open("/")) {
        Serial.println("[STORAGE] open / FAILED");
        return;
    }

    Serial.println("[STORAGE] list /");
    SdFile f;
    while (f.openNext(&root, O_RDONLY)) {
        char name[128];
        f.getName(name, sizeof(name));

        if (f.isDir()) {
            Serial.printf("  %s <DIR>\n", name);
        } else {
            Serial.printf("  %s  %lu bytes\n", name, (unsigned long)f.fileSize());
        }
        f.close();
    }
    root.close();
}
