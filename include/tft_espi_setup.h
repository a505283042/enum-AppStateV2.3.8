#pragma once

// -------- Driver --------
#define GC9A01_DRIVER

// -------- Screen size --------
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// -------- Pins (跟你 board_pins.h 一致) --------
#define TFT_MOSI 35
#define TFT_MISO -1
#define TFT_SCLK 36
#define TFT_CS   42
#define TFT_DC   41
#define TFT_RST  40

// 如果你有背光引脚就填，没有就不管（你这里是 -1）
// #define TFT_BL   1

// -------- SPI frequency --------
#define SPI_FREQUENCY  40000000   // 40MHz（稳的话你也可以后面加到 60/80）
#define SPI_READ_FREQUENCY  20000000
