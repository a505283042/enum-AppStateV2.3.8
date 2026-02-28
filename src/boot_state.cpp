

#include "boot_state.h"
#include "app_state.h"
#include "nfc/nfc.h"
#include "board/board_spi.h"
#include "storage/storage.h"
#include "storage/storage_music.h"
#include "ui/ui.h"
#include "utils/log.h"
#include "audio/audio_service.h"

#include <vector>

static std::vector<TrackInfo> g_tracks;
static std::vector<AlbumInfo> g_albums;

void boot_state_run(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    Serial.begin(115200);
    delay(300);
    Serial.println("[BOOT] start");
    
    Serial.printf("[MEM] psramFound=%d, PsramSize=%u, FreePsram=%u\n",
              (int)psramFound(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.printf("[MEM] FreeHeap=%u\n", (unsigned)ESP.getFreeHeap());

    // 1) 初始化两条 SPI：默认SPI=UI，SPI_SD=SD
    board_spi_init();

    storage_init();

    // 2) 先点亮屏幕  启动 UI（TFT_eSPI 用默认 SPI，不会再打架）
    ui_init();

    // ✅ 启动音频专用任务（双核：音频与UI分离，避免旋转推屏导致卡顿）
    audio_service_start();

    nfc_init(); 
    ui_enter_boot();



    // 3) 加载音乐索引文件，而不是自动扫描
    if (storage_is_ready()) {
        bool load_success = storage_load_index("/System/music_index.bin");
        if (load_success) {
            LOGI("[STORAGE] Loaded index file successfully");
        } else {
            LOGE("[STORAGE] Failed to load index file. Please long-press the button to rescan.");
            // 可选：如果加载失败，可以 fallback 扫描一次
            // storage_scan_music_flat(g_tracks, g_albums, "/Music");
        }
    }



    Serial.println("[BOOT] -> PLAYER");
    g_app_state = STATE_PLAYER;
}
