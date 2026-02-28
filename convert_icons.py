from PIL import Image
import os

# 读取合并的PNG
img = Image.open('ui_icons.png')
print(f'图片尺寸: {img.size}')
print(f'图片模式: {img.mode}')

# 转换为RGB565格式（小端序，适合LGFX）
def rgb888_to_rgb565(r, g, b):
    # RGB565: RRRRRGGG GGGBBBBB
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # 转换为小端序（低字节在前）
    return ((rgb565 & 0xFF) << 8) | ((rgb565 >> 8) & 0xFF)

# 三个图标的位置 (左中右: 音符, 歌手, 专辑)
# 从ui_icons.png来看，图标从x=0开始，每个14像素
icons = [
    ('ICON_NOTE_IMG', 0, 0),     # 音符 (左)
    ('ICON_ARTIST_IMG', 14, 0),  # 歌手 (中)
    ('ICON_ALBUM_IMG', 28, 0)    # 专辑 (右)
]

output = []
for name, x, y in icons:
    # 裁剪区域
    icon = img.crop((x, y, x+14, y+14))

    # 转换为RGB数组（黑色转为透明色）
    pixels = []
    for py in range(14):
        for px in range(14):
            r, g, b, a = icon.getpixel((px, py))
            # 如果alpha通道为0或接近黑色，设为透明
            if a < 128 or (r < 10 and g < 10 and b < 10):
                # TFT_TRANSPARENT = 0x0120 (透明色)
                rgb565 = 0x0120
            else:
                rgb565 = rgb888_to_rgb565(r, g, b)
            pixels.append(f'0x{rgb565:04X}')

    # 生成C数组，每行14个
    lines = []
    for i in range(0, len(pixels), 14):
        lines.append('  ' + ', '.join(pixels[i:i+14]))
    array_str = ',\n'.join(lines)

    output.append(f'static const uint16_t {name}[196] = {{\n{array_str}\n}};')

    # 保存单独的图片用于验证
    icon.save(f'{name.lower()}.png')
    print(f'已生成: {name} ({len(pixels)} 像素)')

# 写入头文件
header_content = '''#pragma once
#include <stdint.h>

// 图标尺寸
#define ICON_IMG_SIZE 14

// 图标数据 (RGB565格式, 14x14 = 196 pixels)
'''

for o in output:
    header_content += o + '\n\n'

# 添加绘制函数声明
header_content += '''// 绘制图标函数
void draw_icon_image(LGFX_Sprite* dst, int x, int y, const uint16_t* icon_data);
'''

with open('include/ui/ui_icon_images.h', 'w', encoding='utf-8') as f:
    f.write(header_content)

print('\n已生成: include/ui/ui_icon_images.h')
