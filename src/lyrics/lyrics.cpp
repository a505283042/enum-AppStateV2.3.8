#include "lyrics/lyrics.h"
#include "utils/log.h"
#include <algorithm>
#include <SdFat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// 外部声明 SdFat 对象（定义在 storage.cpp）
extern SdFat sd;

// 外部声明 SD 卡互斥锁（定义在 storage.cpp）
extern SemaphoreHandle_t g_sd_mutex;

// 全局歌词显示实例
LyricsDisplay g_lyricsDisplay;

// =============================================================================
// LyricsParser 实现
// =============================================================================

LyricsParser::LyricsParser() {}

LyricsParser::~LyricsParser() {}

bool LyricsParser::loadFromFile(const char* path) {
    clear();
    
    uint32_t t0 = millis();
    
    // 获取 SD 卡互斥锁，防止与音频解码线程冲突
    if (g_sd_mutex == nullptr) {
        LOGW("[LYRICS] SD 互斥锁未初始化");
        return false;
    }
    
    if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        LOGW("[LYRICS] 获取 SD 锁超时");
        return false;
    }
    
    File32 file = sd.open(path, O_RDONLY);
    if (!file) {
        LOGW("[LYRICS] Failed to open: %s", path);
        xSemaphoreGive(g_sd_mutex);
        return false;
    }
    
    uint32_t t1 = millis();
    
    uint32_t fileSize = file.fileSize();
    if (fileSize == 0 || fileSize > 65536) {
        file.close();
        xSemaphoreGive(g_sd_mutex);
        LOGW("[LYRICS] Invalid file size: %u", fileSize);
        return false;
    }
    
    String content;
    content.reserve(fileSize);
    
    uint8_t buf[512];
    while (file.available()) {
        int bytesRead = file.read(buf, sizeof(buf));
        if (bytesRead > 0) {
            content.concat((char*)buf, bytesRead);
        }
    }
    file.close();
    
    // 释放 SD 卡互斥锁
    xSemaphoreGive(g_sd_mutex);
    
    uint32_t t2 = millis();
    
    bool result = parse(content);
    
    uint32_t t3 = millis();
    LOGI("[LYRICS] Loaded: %s (%d bytes, %lums)", path, content.length(), t3-t0);
    
    return result;
}

bool LyricsParser::parse(const String& content) {
    clear();
    
    int start = 0;
    int end = content.indexOf('\n');
    
    while (end >= 0) {
        String line = content.substring(start, end);
        line.trim();
        if (line.length() > 0) {
            parseLine(line);
        }
        start = end + 1;
        end = content.indexOf('\n', start);
    }
    
    // 处理最后一行
    if (start < content.length()) {
        String line = content.substring(start);
        line.trim();
        if (line.length() > 0) {
            parseLine(line);
        }
    }
    
    sortByTime();
    return !m_lines.empty();
}

void LyricsParser::parseLine(const String& line) {
    // 查找时间标签 [mm:ss.xx] 或 [mm:ss:xx]
    int tagStart = line.indexOf('[');
    while (tagStart >= 0) {
        int tagEnd = line.indexOf(']', tagStart);
        if (tagEnd < 0) break;
        
        String tag = line.substring(tagStart, tagEnd + 1);
        uint32_t time_ms = parseTimeTag(tag);
        
        if (time_ms > 0) {
            // 提取歌词文本（最后一个时间标签后的内容）
            String text = line.substring(tagEnd + 1);
            text.trim();
            
            // 移除可能存在的其他时间标签
            int nextTag = text.indexOf('[');
            if (nextTag >= 0) {
                text = text.substring(0, nextTag);
                text.trim();
            }
            
            if (text.length() > 0) {
                m_lines.emplace_back(time_ms, text);
            }
        }
        
        tagStart = line.indexOf('[', tagEnd + 1);
    }
}

uint32_t LyricsParser::parseTimeTag(const String& tag) {
    // 格式: [mm:ss.xx] 或 [mm:ss:xx] 或 [mm:ss]
    if (tag.length() < 7 || tag[0] != '[' || tag[tag.length()-1] != ']') {
        return 0;
    }
    
    String inner = tag.substring(1, tag.length() - 1);
    
    int colon1 = inner.indexOf(':');
    if (colon1 < 0) return 0;
    
    int colon2 = inner.indexOf(':', colon1 + 1);
    int dot = inner.indexOf('.', colon1 + 1);
    
    int minutes = inner.substring(0, colon1).toInt();
    int seconds = 0;
    int centiseconds = 0;
    
    if (colon2 >= 0) {
        // [mm:ss:xx] 格式
        seconds = inner.substring(colon1 + 1, colon2).toInt();
        centiseconds = inner.substring(colon2 + 1).toInt();
    } else if (dot >= 0) {
        // [mm:ss.xx] 格式
        seconds = inner.substring(colon1 + 1, dot).toInt();
        centiseconds = inner.substring(dot + 1).toInt();
    } else {
        // [mm:ss] 格式
        seconds = inner.substring(colon1 + 1).toInt();
    }
    
    return (minutes * 60 + seconds) * 1000 + centiseconds * 10;
}

void LyricsParser::sortByTime() {
    std::sort(m_lines.begin(), m_lines.end(), 
        [](const LyricLine& a, const LyricLine& b) {
            return a.time_ms < b.time_ms;
        });
}

int LyricsParser::getCurrentIndex(uint32_t time_ms) const {
    if (m_lines.empty()) return -1;
    
    // 如果时间小于第一句，返回 0（显示第一句，等待播放）
    if (time_ms < m_lines[0].time_ms) {
        return 0;
    }
    
    // 使用二分查找优化性能 O(log n)
    auto it = std::upper_bound(m_lines.begin(), m_lines.end(), time_ms, 
         [](uint32_t t, const LyricLine& line) { return t < line.time_ms; });
    
    int idx = std::distance(m_lines.begin(), it) - 1;
    return (idx < 0) ? 0 : idx;
}

const LyricLine* LyricsParser::getLine(int index) const {
    if (index < 0 || index >= (int)m_lines.size()) {
        return nullptr;
    }
    return &m_lines[index];
}

void LyricsParser::clear() {
    m_lines.clear();
}

String LyricsParser::findLyricsFile(const char* audio_path) {
    String path(audio_path);
    
    int dotIndex = path.lastIndexOf('.');
    if (dotIndex > 0) {
        path = path.substring(0, dotIndex);
    }
    
    String lrcPath = path + ".lrc";
    String txtPath = path + ".txt";
    String lrcPathUpper = path + ".LRC";
    
    File32 f;
    if ((f = sd.open(lrcPath.c_str(), O_RDONLY))) { f.close(); return lrcPath; }
    if ((f = sd.open(txtPath.c_str(), O_RDONLY))) { f.close(); return txtPath; }
    if ((f = sd.open(lrcPathUpper.c_str(), O_RDONLY))) { f.close(); return lrcPathUpper; }
    
    return "";
}

// =============================================================================
// LyricsDisplay 实现
// =============================================================================

LyricsDisplay::LyricsDisplay() : m_currentIndex(-1), m_currentTime(0) {}

LyricsDisplay::~LyricsDisplay() {}

bool LyricsDisplay::loadForTrack(const char* audio_path) {
    clear();
    
    String lyricsPath = LyricsParser::findLyricsFile(audio_path);
    
    if (lyricsPath.length() == 0) {
        return false;
    }
    
    return loadFromPath(lyricsPath.c_str());
}

bool LyricsDisplay::loadFromPath(const char* lrc_path) {
    clear();
    
    if (!lrc_path || lrc_path[0] == '\0') {
        return false;
    }
    
    uint32_t t0 = millis();
    
    bool success = m_parser.loadFromFile(lrc_path);
    
    uint32_t t1 = millis();
    
    if (success) {
        m_currentIndex = 0;  // 初始化为第一句，避免第一帧闪烁
    } else {
        LOGW("[LYRICS] Failed: %s", lrc_path);
    }
    return success;
}

void LyricsDisplay::updateTime(uint32_t time_ms) {
    m_currentTime = time_ms;
    int newIndex = m_parser.getCurrentIndex(time_ms);
    
    if (newIndex != m_currentIndex) {
        m_currentIndex = newIndex;
        LOGD("[LYRICS] Index changed to: %d (time: %u ms)", m_currentIndex, time_ms);
    }
}

String LyricsDisplay::getCurrentLyric() const {
    const LyricLine* line = m_parser.getLine(m_currentIndex);
    if (line) {
        return line->text;
    }
    return "";
}

String LyricsDisplay::getNextLyric() const {
    const LyricLine* line = m_parser.getLine(m_currentIndex + 1);
    if (line) {
        return line->text;
    }
    return "";
}

bool LyricsDisplay::hasLyrics() const {
    return m_parser.isLoaded();
}

void LyricsDisplay::clear() {
    m_parser.clear();
    m_currentIndex = -1;
    m_currentTime = 0;
}

uint32_t LyricsDisplay::getNextLyricTime() const {
    const LyricLine* nextLine = m_parser.getLine(m_currentIndex + 1);
    if (nextLine) {
        return nextLine->time_ms;
    }
    // 如果没有下一句，返回当前时间+5秒（默认值）
    return m_currentTime + 5000;
}

LyricsDisplay::ScrollLyrics LyricsDisplay::getScrollLyrics() const {
    ScrollLyrics result;
    result.progress = 0.0f;
    
    if (!m_parser.isLoaded() || m_currentIndex < 0) {
        return result;
    }
    
    // 获取上一句
    const LyricLine* prevLine = m_parser.getLine(m_currentIndex - 1);
    if (prevLine) {
        result.prev = prevLine->text;
    }
    
    // 获取当前句
    const LyricLine* currLine = m_parser.getLine(m_currentIndex);
    if (currLine) {
        result.current = currLine->text;
        
        // 计算当前句的进度
        uint32_t currStart = currLine->time_ms;
        uint32_t nextStart = getNextLyricTime();
        
        // 只有当播放时间超过当前句开始时间时才计算进度
        if (nextStart > currStart && m_currentTime >= currStart) {
            uint32_t duration = nextStart - currStart;
            uint32_t elapsed = m_currentTime - currStart;
            result.progress = (float)elapsed / (float)duration;
            if (result.progress > 1.0f) result.progress = 1.0f;
        }
    }
    
    // 获取下一句
    const LyricLine* nextLine = m_parser.getLine(m_currentIndex + 1);
    if (nextLine) {
        result.next = nextLine->text;
    }
    
    return result;
}
