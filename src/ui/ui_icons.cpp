#include "ui/ui_icons.h"
#include "ui/gc9a01_lgfx.h"
#include "ui/ui_icon_images.h"

// 绘制音量图标
void draw_volume_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int width = 11;  // 11像素宽
  const int height = 11; // 11像素高
  
  // 定义每列的像素位置（y坐标），-1表示结束
  const int columns[11][10] = {
    // 第1列：4格
    {4, 5, 6, 7, -1},
    // 第2列：4格
    {4, 5, 6, 7, -1},
    // 第3列：6格
    {3, 4, 5, 6, 7, 8, -1},
    // 第4列：8格
    {2, 3, 4, 5, 6, 7, 8, 9, -1},
    // 第5列：10格
    {1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
    // 第6列：空
    {-1},
    // 第7列：2格
    {3, 8, -1},
    // 第8列：4格
    {1, 4, 7, 10, -1},
    // 第9列：4格
    {2, 5, 6, 9, -1},
    // 第10列：2格
    {3, 8, -1},
    // 第11列：4格
    {4, 5, 6, 7, -1}
  };
  
  dst->setColor(color);
  
  // 遍历每一列
  for (int col = 0; col < width; col++) {
    // 遍历当前列的每个像素
    for (int i = 0; i < 10; i++) {
      int y_pos = columns[col][i];
      if (y_pos == -1) break; // 结束标记
      dst->drawPixel(x + col, y + y_pos, color);
    }
  }
}

// 绘制随机播放图标
void draw_random_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;

  // 定义每列的像素位置（y坐标），-1表示结束
  const int columns[10][7] = {
    // 第1列：3，8
    {2, 7, -1},
    // 第2列：3，8
    {2, 7, -1},
    // 第3列：4，7
    {3, 6, -1},
    // 第4列：6
    {5, -1},
    // 第5列：5
    {4, -1},
    // 第6列：4，7
    {3, 6, -1},
    // 第7列：3，8
    {2, 7, -1},
    // 第8列：1，3，5，6，8，10
    {0, 2, 4, 5, 7, 9, -1},
    // 第9列：2，3，4，7，8，9
    {1, 2, 3, 6, 7, 8, -1},
    // 第10列：3，8
    {2, 7, -1}
  };

  dst->setColor(color);

  // 遍历每一列
  for (int col = 0; col < icon_size; col++) {
    // 遍历当前列的每个像素
    for (int i = 0; i < 7; i++) {
      int y_pos = columns[col][i];
      if (y_pos == -1) break; // 结束标记
      dst->drawPixel(x + col, y + y_pos, color);
    }
  }
}

// 绘制专辑图标
void draw_album_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;

  // 定义每列的像素位置（y坐标），-1表示结束
  const int columns[10][6] = {
    // 第1列：4-7
    {3, 4, 5, 6, -1},
    // 第2列：3，8
    {2, 7, -1},
    // 第3列：2，6，7，9
    {1, 5, 6, 8, -1},
    // 第4列：1，8，10
    {0, 7, 9, -1},
    // 第5列：1，5，6，10
    {0, 4, 5, 9, -1},
    // 第6列：1，5，6，10
    {0, 4, 5, 9, -1},
    // 第7列：1，3，10
    {0, 2, 9, -1},
    // 第8列：2，4，5，9
    {1, 3, 4, 8, -1},
    // 第9列：3，8
    {2, 7, -1},
    // 第10列：4-7
    {3, 4, 5, 6, -1}
  };

  dst->setColor(color);

  // 遍历每一列
  for (int col = 0; col < icon_size; col++) {
    // 遍历当前列的每个像素
    for (int i = 0; i < 6; i++) {
      int y_pos = columns[col][i];
      if (y_pos == -1) break; // 结束标记
      dst->drawPixel(x + col, y + y_pos, color);
    }
  }
}

// 绘制歌手图标
void draw_artist_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;
  
  // 定义每列的像素位置（y坐标），-1表示结束
  const int columns[10][9] = {
    // 第1列：8，9
    {8, 9, -1},
    // 第2列：7，10
    {7, 10, -1},
    // 第3列：3，4，6，10
    {3, 4, 6, 10, -1},
    // 第4列：2，5，10
    {2, 5, 10, -1},
    // 第5列：1，6，10
    {1, 6, 10, -1},
    // 第6列：1，6，10
    {1, 6, 10, -1},
    // 第7列：2，5，10
    {2, 5, 10, -1},
    // 第8列：3，4，6，10
    {3, 4, 6, 10, -1},
    // 第9列：7，10
    {7, 10, -1},
    // 第10列：8，9
    {8, 9, -1}
  };
  
  dst->setColor(color);
  
  // 遍历每一列
  for (int col = 0; col < icon_size; col++) {
    // 遍历当前列的每个像素
    for (int i = 0; i < 9; i++) {
      int y_pos = columns[col][i];
      if (y_pos == -1) break; // 结束标记
      dst->drawPixel(x + col, y + y_pos, color);
    }
  }
}

// 绘制文件夹图标
void draw_folder_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;
  
  // 定义每列的像素位置（y坐标），-1表示结束
  const int columns[10][9] = {
    // 第1列：2-9
    {1, 2, 3, 4, 5, 6, 7, 8, -1},
    // 第2列：1，10
    {0, 9, -1},
    // 第3列：1，6，7，10
    {0, 5, 6, 9, -1},
    // 第4列：2，5，8，10
    {1, 4, 7, 9, -1},
    // 第5列：3，6，8，10
    {2, 5, 7, 9, -1},
    // 第6列：3，6，8，10
    {2, 5, 7, 9, -1},
    // 第7列：3，5，8，10
    {2, 4, 7, 9, -1},
    // 第8列：3，6，7，10
    {2, 5, 6, 9, -1},
    // 第9列：3，10
    {2, 9, -1},
    // 第10列：4-9
    {3, 4, 5, 6, 7, 8, -1}
  };
  
  dst->setColor(color);
  
  // 遍历每一列
  for (int col = 0; col < icon_size; col++) {
    // 遍历当前列的每个像素
    for (int i = 0; i < 9; i++) {
      int y_pos = columns[col][i];
      if (y_pos == -1) break; // 结束标记
      dst->drawPixel(x + col, y + y_pos, color);
    }
  }
}

// 绘制TF卡图标
void draw_tfcard_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;
  
  // 定义每列的像素位置（y坐标），-1表示结束
  const int columns[10][9] = {
    // 第1列：2-9
    {2, 3, 4, 5, 6, 7, 8, 9, -1},
    // 第2列：2，10
    {2, 10, -1},
    // 第3列：1，2，3，8，9，10
    {1, 2, 3, 8, 9, 10, -1},
    // 第4列：1，3，8，10
    {1, 3, 8, 10, -1},
    // 第5列：1, 2, 3, 8, 10  
    {1, 2, 3, 8, 10, -1},
    // 第6列：1, 8, 10,
    {1, 8, 10, -1},
    // 第7列：2，8，9，10
    {2, 8, 9, 10, -1},
    // 第8列：3，10
    {3, 10, -1},
    // 第9列：4-9
    {4, 5, 6, 7, 8, 9, -1},
    // 第10列：空
    {-1}
  };
  
  dst->setColor(color);
  
  // 遍历每一列
  for (int col = 0; col < icon_size; col++) {
    // 遍历当前列的每个像素
    for (int i = 0; i < 9; i++) {
      int y_pos = columns[col][i];
      if (y_pos == -1) break; // 结束标记
      dst->drawPixel(x + col, y + y_pos, color);
    }
  }
}

// 绘制顺序播放图标
void draw_repeat_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;
  
  // 定义每列的像素位置（y坐标），-1表示结束
  const int columns[10][5] = {
    // 第1列：4, 5, 8
    {3, 4, 7, -1},
    // 第2列：3, 7, 9
    {2, 6, 8, -1},
    // 第3列：3, 6, 8, 10
    {2, 5, 7, 9, -1},
    // 第4列：3, 8
    {2, 7, -1},
    // 第5列：3, 8
    {2, 7, -1},
    // 第6列：3, 8
    {2, 7, -1},
    // 第7列：3, 8
    {2, 7, -1},
    // 第8列：1, 3, 5, 8
    {0, 2, 4, 7, -1},
    // 第9列：2, 3, 4, 8
    {1, 2, 3, 7, -1},
    // 第10列：3, 6, 7
    {2, 5, 6, -1}
  };
  
  dst->setColor(color);
  
  // 遍历每一列
  for (int col = 0; col < icon_size; col++) {
    // 遍历当前列的每个像素
    for (int i = 0; i < 5; i++) {
      int y_pos = columns[col][i];
      if (y_pos == -1) break; // 结束标记
      dst->drawPixel(x + col, y + y_pos, color);
    }
  }
}

// 绘制图片图标（14x14 RGB565，支持透明色）
void draw_icon_image(LGFX_Sprite* dst, int x, int y, const uint16_t* icon_data)
{
  // 使用 pushImage 的透明色版本
  // TFT_TRANSPARENT = 0x0120
  dst->pushImage(x, y, ICON_IMG_SIZE, ICON_IMG_SIZE, icon_data, TFT_TRANSPARENT);
}

// 绘制专辑图片图标
void draw_album_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  (void)color; // 颜色参数未使用，图标自带颜色
  draw_icon_image(dst, x, y, ICON_ALBUM_IMG);
}

// 绘制歌手图片图标
void draw_artist_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  (void)color;
  draw_icon_image(dst, x, y, ICON_ARTIST_IMG);
}

// 绘制音符图片图标
void draw_note_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  (void)color;
  draw_icon_image(dst, x, y, ICON_NOTE_IMG);
}