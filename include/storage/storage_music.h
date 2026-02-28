/* 存储音乐模块头文件 */
#pragma once
#include <Arduino.h>  /* 包含Arduino核心库 */
#include <vector>       /* 包含向量容器库 */

/* 封面来源枚举 - 表示封面图片的来源 */
enum CoverSource : uint8_t {
  COVER_NONE = 0,
  COVER_MP3_APIC,
  COVER_FLAC_PICTURE,
  COVER_FILE_FALLBACK
};

/* 音轨信息结构体 - 描述单个音乐文件的信息 */
struct TrackInfo {
  String artist;       /* 艺术家名称 */
  String album;        /* 专辑名称 */
  String title;        /* 歌曲标题 */

  String audio_path;   // ✅ 通用：mp3/flac/wav 都放这里
  String ext;          // ✅ ".mp3" ".flac"（可选，但很实用）
  String lrc_path;     /* 歌词文件路径，如 /Music/A/B/xxx.lrc (若不存在则空) */

  // 封面信息（P2-3：封面最小策略）
  CoverSource cover_source = COVER_NONE;
  uint32_t cover_offset = 0;   // 从文件开头算起
  uint32_t cover_size = 0;     // 封面数据字节数
  String cover_mime;           // "image/jpeg" / "image/png"（可选但很有用）
  String cover_path;           // fallback 文件路径（比如 /Music/folder.jpg）
};

/* 专辑信息结构体 - 描述音乐专辑的信息 */
struct AlbumInfo {
  String artist;       /* 艺术家名称 */
  String album;        /* 专辑名称 */
  String folder;       /* 专辑文件夹路径，如 /Music/A/B */
  String cover_path;   /* 专辑封面路径，如 /Music/A/B/cover.jpg (若不存在则空) */
};

/* 扫描音乐文件 - 扫描指定根目录下的音乐文件并填充音轨和专辑列表 */
bool storage_scan_music(std::vector<TrackInfo>& out_tracks,
                        std::vector<AlbumInfo>& out_albums,
                        const char* music_root);

bool storage_scan_music_flat(std::vector<TrackInfo>& out_tracks,
                             std::vector<AlbumInfo>& out_albums,
                             const char* music_root);


                        
const std::vector<TrackInfo>& storage_get_tracks(void);
const std::vector<AlbumInfo>& storage_get_albums(void);

// ====== Index 缓存（开机快启动） ======
bool storage_load_index(const char* index_path = "/System/music_index.bin");
bool storage_save_index(const char* index_path = "/System/music_index.bin");

// 手动重扫（长按触发）
bool storage_rescan_flat(const char* music_root = "/Music",
                         const char* index_path = "/System/music_index.bin");
