#pragma once
#include <LovyanGFX.hpp>
#include "board/board_pins.h"

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX() {
    { // SPI bus with DMA support
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;          // ESP32-S3 通常用 SPI2_HOST
      cfg.spi_mode = 0;
      cfg.freq_write = 20000000;  // 降低频率测试稳定性
      cfg.freq_read  = 20000000;
      cfg.pin_sclk = PIN_SPI_UI_SCK;
      cfg.pin_mosi = PIN_SPI_UI_MOSI;
      cfg.pin_miso = -1;                 // 不读屏设为-1，更干净
      cfg.pin_dc   = PIN_TFT_DC;         // DC 在 bus 里
      
      // ✅ 打开 DMA（常用写法：AUTO 或 1/2）
      cfg.dma_channel = 1;              // 使用DMA通道1
      cfg.use_lock = true;              // 可选：更稳一些
      
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // panel
      auto pcfg = _panel.config();
      pcfg.pin_cs  = PIN_TFT_CS;         // ✅ CS 在 panel 里配
      pcfg.pin_rst = PIN_TFT_RST;
      pcfg.panel_width  = 240;
      pcfg.panel_height = 240;
      pcfg.offset_x = 0;
      pcfg.offset_y = 0;
      pcfg.invert   = true;              // 如果颜色不对再改 false
      _panel.config(pcfg);
    }
    setPanel(&_panel);
  }
};
