#include "storage/storage_music.h"
#include <FS.h>
#include <SdFat.h>
#include <Arduino.h>
#include "utils/log.h"
#include <vector>
#include "meta/meta_id3.h"
#include "meta/meta_flac.h"
#include "meta/meta_id3_cover.h"
#include "meta/meta_flac_cover.h"
#include "ui/ui.h"


using namespace std;

// 复用 storage.cpp 里的 sd 对象：简单做法是“再声明一个 extern”
// 为了让它可编译，我们在这里声明一个全局函数去拿 sd 对象
// ——更工程化的是建 storage_fs.h 暴露 get_fs()，这里先用最少改动版本。
static std::vector<TrackInfo> s_tracks;
static std::vector<AlbumInfo> s_albums;

const std::vector<TrackInfo>& storage_get_tracks(void) { return s_tracks; }
const std::vector<AlbumInfo>& storage_get_albums(void) { return s_albums; }

extern SdFat sd;

static bool ends_with_ignore_case(const String& s, const char* ext) {
  String a = s; a.toLowerCase();
  String b(ext); b.toLowerCase();
  return a.endsWith(b);
} 

static String basename_no_ext(const String& filename) {
  int dot = filename.lastIndexOf('.');
  if (dot <= 0) return filename;
  return filename.substring(0, dot);
}

static bool file_exists(const String& path) {
  File32 f = sd.open(path.c_str(), O_RDONLY);
  bool ok = (bool)f;
  if (f) f.close();
  return ok;
}

static int find_album_index(std::vector<AlbumInfo>& albums, const String& artist, const String& album) {
  for (int i = 0; i < (int)albums.size(); ++i) {
    if (albums[i].artist == artist && albums[i].album == album) return i;
  }
  return -1;
}

static void ensure_album(std::vector<AlbumInfo>& albums, const String& artist, const String& album, const String& music_root) {
  if (find_album_index(albums, artist, album) >= 0) return;
  AlbumInfo ai;
  ai.artist = artist;
  ai.album = album;
  ai.folder = String(music_root);    // 单层目录时，专辑不再有 folder，先填根目录
  ai.cover_path = String();          // P2-1 先空
  albums.push_back(ai);
}

static String pick_cover_in_folder(const String& folder) {
  static const char* fixed[] = {
    "cover.jpg", "cover.jpeg", "cover.png",
    "folder.jpg","folder.jpeg","folder.png",
    "front.jpg", "front.png"
  };

  for (auto name : fixed) {
    String p = folder + "/" + name;
    if (file_exists(p)) return p;   // ✅ 你现在的改法就是这个
  }

  // 兜底：找目录里第一张 jpg/png
  SdFile dir;
  if (!dir.open(folder.c_str(), O_RDONLY) || !dir.isDir()) {
    dir.close();
    return String();
  }

  SdFile f;
  while (f.openNext(&dir, O_RDONLY)) {
    if (!f.isDir()) {
      char name[256];
      f.getName(name, sizeof(name));
      String n(name);
      String lower = n; lower.toLowerCase();

      if (lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png")) {
        String cover = folder + "/" + n;
        f.close();
        dir.close();
        return cover;
      }
    }
    f.close();
  }

  dir.close();
  return String();
}

static void scan_album_folder(const String& music_root,
                              const String& artist,
                              const String& album,
                              SdFile* album_dir,
                              std::vector<TrackInfo>& out_tracks,
                              std::vector<AlbumInfo>& out_albums)
{
  LOGI("[SCAN] Scanning album: %s/%s", artist.c_str(), album.c_str());

  // 获取封面图片路径
  String album_path = music_root + "/" + artist + "/" + album; 
  String cover_path = pick_cover_in_folder(album_path);
  
  AlbumInfo ai;
  ai.artist = artist;
  ai.album = album;
  ai.folder = album_path;
  ai.cover_path = cover_path;
  out_albums.push_back(ai);

  LOGI("[SCAN] Album opened successfully, scanning files...");
  int file_count = 0;
  int mp3_count = 0;

  // 重置目录指针到开始位置，以便从头开始遍历
  album_dir->rewind();
  
  SdFile f;
  while (f.openNext(album_dir, O_RDONLY)) {
    file_count++; 
    if (!f.isDir()) {
      char name[128];
      f.getName(name, sizeof(name));
      String fn(name);
      
      LOGI("[SCAN] Found file: %s", name);

    if (ends_with_ignore_case(fn, ".mp3") || ends_with_ignore_case(fn, ".flac")) {
      TrackInfo t;
      t.artist = artist;
      t.album  = album;
      t.title  = basename_no_ext(fn);

      t.audio_path = album_path + "/" + fn;

      int dot = fn.lastIndexOf('.');
      t.ext = (dot >= 0) ? fn.substring(dot) : "";
      t.ext.toLowerCase();

      String lrc = album_path + "/" + t.title + ".lrc";
      t.lrc_path = file_exists(lrc) ? lrc : String();

      // 封面信息处理（P2-3：封面最小策略）
      // 先默认无封面
      t.cover_source = COVER_NONE;
      t.cover_offset = 0;
      t.cover_size = 0;
      t.cover_mime = "";
      t.cover_path = "";

      // fallback
      if (!ai.cover_path.isEmpty()) {
        t.cover_source = COVER_FILE_FALLBACK;
        t.cover_path = ai.cover_path;
      }

      // embedded 优先覆盖 fallback
      if (ends_with_ignore_case(fn, ".mp3")) {
        Mp3CoverLoc loc;
        if (id3_find_apic(sd, t.audio_path.c_str(), loc) && loc.found) {
          t.cover_source = COVER_MP3_APIC;
          t.cover_offset = loc.offset;
          t.cover_size = loc.size;
          t.cover_mime = loc.mime;
          t.cover_path = ""; // 内嵌时不用文件路径
        }
      } else if (ends_with_ignore_case(fn, ".flac")) {
        FlacCoverLoc loc;
        if (flac_find_picture(sd, t.audio_path.c_str(), loc) && loc.found) {
          t.cover_source = COVER_FLAC_PICTURE;
          t.cover_offset = loc.offset;
          t.cover_size = loc.size;
          t.cover_mime = loc.mime;
          t.cover_path = "";
        }
      }

      // 解析MP3的ID3标签
      if (ends_with_ignore_case(fn, ".mp3")) {
        Id3BasicInfo meta;
        if (id3_read_basic(sd, t.audio_path.c_str(), meta)) {
          if (meta.title.length())  t.title  = meta.title;
          if (meta.artist.length()) t.artist = meta.artist;
          if (meta.album.length())  t.album  = meta.album;
        }
      }
      // 解析FLAC的Vorbis Comment
      else if (ends_with_ignore_case(fn, ".flac")) {
        FlacBasicInfo meta;
        if (flac_read_vorbis_basic(sd, t.audio_path.c_str(), meta)) {
          if (meta.title.length())  t.title  = meta.title;
          if (meta.artist.length()) t.artist = meta.artist;
          if (meta.album.length())  t.album  = meta.album;
        }
      }

      out_tracks.push_back(t);
      LOGI("[SCAN] Added AUDIO: %s (total tracks: %d)",
                    t.audio_path.c_str(), (int)out_tracks.size());
    }

    }
    f.close();
  }

  LOGI("[SCAN] Album scan complete: %d files, %d MP3s found", file_count, mp3_count);
}

bool storage_scan_music(std::vector<TrackInfo>& out_tracks,
                        std::vector<AlbumInfo>& out_albums,
                        const char* music_root)
{
  out_tracks.clear();
  out_albums.clear();

  LOGI("[SCAN] Starting music scan from root: %s", music_root);

  SdFile root;
  if (!root.open(music_root, O_RDONLY) || !root.isDir()) {
    LOGE("[SCAN] Failed to open music root: %s", music_root);
    root.close();
    return false;
  }

  LOGI("[SCAN] Music root opened successfully");

  int artist_count = 0;
  int album_count = 0;

  // 艺术家目录
  SdFile artistDir;
  while (artistDir.openNext(&root, O_RDONLY)) {
    if (!artistDir.isDir()) { artistDir.close(); continue; }

    char artistNameC[128];
    artistDir.getName(artistNameC, sizeof(artistNameC));
    String artistName(artistNameC);
    if (artistName.startsWith(".")) { artistDir.close(); continue; }

    artist_count++;
    LOGI("[SCAN] Found artist: %s", artistName.c_str());

    // 专辑目录
    SdFile albumDir;
    while (albumDir.openNext(&artistDir, O_RDONLY)) {
      if (!albumDir.isDir()) { albumDir.close(); continue; }

      char albumNameC[128];
      albumDir.getName(albumNameC, sizeof(albumNameC));
      String albumName(albumNameC);
      if (albumName.startsWith(".")) { albumDir.close(); continue; }

      album_count++;
      // 使用父目录句柄直接传递给 scan_album_folder
      scan_album_folder(music_root, artistName, albumName, &albumDir, out_tracks, out_albums);

      albumDir.close();
    }

    artistDir.close();
  }

  root.close();
  s_tracks = out_tracks;
  s_albums = out_albums;

  LOGI("[SCAN] Scan complete: %d artists, %d albums, %d tracks", 
                artist_count, album_count, (int)out_tracks.size());
  return true;
}


bool storage_scan_music_flat(std::vector<TrackInfo>& out_tracks,
                             std::vector<AlbumInfo>& out_albums,
                             const char* music_root)
{
  ui_scan_begin();
  int scanned = 0;

  out_tracks.clear();
  out_albums.clear();

  SdFile root;
  if (!root.open(music_root, O_RDONLY) || !root.isDir()) {
    LOGE("[SCAN] open root failed: %s", music_root);
    root.close();
    return false;
  }

  // 单层目录：专辑概念先弱化，留空或统计一个“虚拟专辑”
  // 你也可以 later 按 ID3 album/artist 聚合
  AlbumInfo ai;
  ai.artist = "";
  ai.album = "";
  ai.folder = String(music_root);
  ai.cover_path = ""; // flat 模式可以用 /Music/cover.jpg 之类兜底
  out_albums.push_back(ai);

  SdFile f;
  while (f.openNext(&root, O_RDONLY)) {
    if (f.isDir()) { f.close(); continue; }

    char name[256];
    f.getName(name, sizeof(name));
    String fn(name);
    String lower = fn; lower.toLowerCase();

    if (!(lower.endsWith(".mp3") || lower.endsWith(".flac"))) {
      f.close();
      continue;
    }

    TrackInfo t;
    t.audio_path = String(music_root) + "/" + fn;

    int dot = fn.lastIndexOf('.');
    t.ext = (dot >= 0) ? fn.substring(dot) : "";
    t.ext.toLowerCase();

    // 默认：用文件名当 title
    t.title = basename_no_ext(fn);
    t.artist = "";
    t.album  = "";

    // 歌词同名
    String lrc = String(music_root) + "/" + t.title + ".lrc";
    t.lrc_path = file_exists(lrc) ? lrc : String();

    // ====== 封面初始化 ======
    t.cover_source = COVER_NONE;
    t.cover_offset = 0;
    t.cover_size   = 0;
    t.cover_mime   = "";
    t.cover_path   = "";

    // ====== embedded 封面优先 ======
    if (lower.endsWith(".mp3")) {
      Mp3CoverLoc loc;
      if (id3_find_apic(sd, t.audio_path.c_str(), loc) && loc.found) {
        t.cover_source = COVER_MP3_APIC;
        t.cover_offset = loc.offset;
        t.cover_size   = loc.size;
        t.cover_mime   = loc.mime;
      }

      Id3BasicInfo meta;
      if (id3_read_basic(sd, t.audio_path.c_str(), meta)) {
        if (meta.title.length())  t.title  = meta.title;
        if (meta.artist.length()) t.artist = meta.artist;
        if (meta.album.length())  t.album  = meta.album;
      }
    } else { // flac
      FlacCoverLoc loc;
      if (flac_find_picture(sd, t.audio_path.c_str(), loc) && loc.found) {
        t.cover_source = COVER_FLAC_PICTURE;
        t.cover_offset = loc.offset;
        t.cover_size   = loc.size;
        t.cover_mime   = loc.mime;
      }

      FlacBasicInfo meta;
      if (flac_read_vorbis_basic(sd, t.audio_path.c_str(), meta)) {
        if (meta.title.length())  t.title  = meta.title;
        if (meta.artist.length()) t.artist = meta.artist;
        if (meta.album.length())  t.album  = meta.album;
      }
    }

    out_tracks.push_back(t);
    scanned++;
    ui_scan_tick(scanned);

    // 让出 CPU，防止 WDT
    delay(0);

    f.close();
  }

  root.close();

  // 更新全局缓存
  s_tracks = out_tracks;
  s_albums = out_albums;
  
  ui_scan_end();

  LOGI("[SCAN] flat done: tracks=%d", (int)s_tracks.size());
  return true;
}

static bool write_u16(File32& f, uint16_t v){ return f.write(&v, sizeof(v)) == sizeof(v); }
static bool write_u32(File32& f, uint32_t v){ return f.write(&v, sizeof(v)) == sizeof(v); }
static bool read_u16(File32& f, uint16_t& v){ return f.read(&v, sizeof(v)) == sizeof(v); }
static bool read_u32(File32& f, uint32_t& v){ return f.read(&v, sizeof(v)) == sizeof(v); }

static bool write_str(File32& f, const String& s)
{
  uint16_t n = (uint16_t)min((size_t)65535, (size_t)s.length());
  if (!write_u16(f, n)) return false;
  if (n == 0) return true;
  return f.write((const uint8_t*)s.c_str(), n) == n;
}

static bool read_str(File32& f, String& s)
{
  uint16_t n = 0;
  if (!read_u16(f, n)) return false;
  s = "";
  if (n == 0) return true;

  s.reserve(n);  // 减少 String 反复扩容碎片化（可选但强烈建议）

  const int CH = 128;
  char buf[CH];

  uint16_t remain = n;
  while (remain) {
    uint16_t chunk = (remain > CH) ? CH : remain;
    int r = f.read(buf, chunk);
    if (r != (int)chunk) return false;

    // ✅ 按长度拼接，不依赖 '\0'
    s.concat(buf, chunk);

    remain -= chunk;
  }
  return true;
}

bool storage_save_index(const char* index_path)
{
  // 确保目录存在（/System）
  sd.mkdir("/System");

  File32 f = sd.open(index_path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;

  // header
  const uint32_t magic = 0x4D494458; // 'MIDX'
  const uint16_t ver = 1;
  write_u32(f, magic);
  write_u16(f, ver);
  write_u32(f, (uint32_t)s_tracks.size());

  for (auto& t : s_tracks) {
    write_str(f, t.audio_path);
    write_str(f, t.ext);
    write_str(f, t.artist);
    write_str(f, t.album);
    write_str(f, t.title);
    write_str(f, t.lrc_path);

    // cover
    uint8_t cs = (uint8_t)t.cover_source;
    f.write(&cs, 1);
    write_u32(f, t.cover_offset);
    write_u32(f, t.cover_size);
    write_str(f, t.cover_mime);
    write_str(f, t.cover_path);
  }

  f.close();
  LOGI("[INDEX] saved: %s tracks=%d", index_path, (int)s_tracks.size());
  return true;
}

bool storage_load_index(const char* index_path)
{
  File32 f = sd.open(index_path, O_RDONLY);
  if (!f) return false;

  uint32_t magic=0; uint16_t ver=0; uint32_t cnt=0;
  if (!read_u32(f, magic) || !read_u16(f, ver) || !read_u32(f, cnt)) { f.close(); return false; }
  if (magic != 0x4D494458 || ver != 1 || cnt > 5000) { f.close(); return false; }

  std::vector<TrackInfo> tracks;
  tracks.reserve(cnt);

  for (uint32_t i=0;i<cnt;i++) {
    TrackInfo t;
    if (!read_str(f, t.audio_path)) { f.close(); return false; }
    if (!read_str(f, t.ext))       { f.close(); return false; }
    if (!read_str(f, t.artist))    { f.close(); return false; }
    if (!read_str(f, t.album))     { f.close(); return false; }
    if (!read_str(f, t.title))     { f.close(); return false; }
    if (!read_str(f, t.lrc_path))  { f.close(); return false; }

    uint8_t cs=0;
    if (f.read(&cs, 1) != 1) { f.close(); return false; }
    t.cover_source = (CoverSource)cs;

    if (!read_u32(f, t.cover_offset)) { f.close(); return false; }
    if (!read_u32(f, t.cover_size))   { f.close(); return false; }
    if (!read_str(f, t.cover_mime))   { f.close(); return false; }
    if (!read_str(f, t.cover_path))   { f.close(); return false; }

    tracks.push_back(t);
  }

  f.close();

  // albums 先给一个虚拟项
  std::vector<AlbumInfo> albums;
  AlbumInfo ai;
  ai.artist=""; ai.album=""; ai.folder="/Music"; ai.cover_path="";
  albums.push_back(ai);

  s_tracks = std::move(tracks);
  s_albums = std::move(albums);

  LOGI("[INDEX] loaded: %s tracks=%d", index_path, (int)s_tracks.size());
  return true;
}

bool storage_rescan_flat(const char* music_root, const char* index_path)
{
  std::vector<TrackInfo> t;
  std::vector<AlbumInfo> a;
  if (!storage_scan_music_flat(t, a, music_root)) return false;
  return storage_save_index(index_path);
}
