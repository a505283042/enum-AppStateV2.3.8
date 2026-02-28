#pragma once
#include <Arduino.h>
#include <vector>

// 单行歌词结构
struct LyricLine {
    uint32_t time_ms;      // 时间戳（毫秒）
    String text;           // 歌词文本
    
    LyricLine(uint32_t t = 0, const String& txt = "") 
        : time_ms(t), text(txt) {}
};

// 歌词解析器类
class LyricsParser {
public:
    LyricsParser();
    ~LyricsParser();
    
    // 从文件加载歌词（支持 .lrc 和 .txt 格式）
    bool loadFromFile(const char* path);
    
    // 从字符串解析歌词
    bool parse(const String& content);
    
    // 获取当前时间对应的歌词索引
    int getCurrentIndex(uint32_t time_ms) const;
    
    // 获取指定索引的歌词
    const LyricLine* getLine(int index) const;
    
    // 获取歌词总行数
    int getLineCount() const { return m_lines.size(); }
    
    // 清空歌词
    void clear();
    
    // 检查是否已加载
    bool isLoaded() const { return !m_lines.empty(); }
    
    // 根据歌曲路径自动查找歌词文件
    static String findLyricsFile(const char* audio_path);
    
private:
    std::vector<LyricLine> m_lines;
    
    // 解析时间标签 [mm:ss.xx] 或 [mm:ss:xx]
    uint32_t parseTimeTag(const String& tag);
    
    // 解析一行歌词
    void parseLine(const String& line);
    
    // 按时间排序
    void sortByTime();
};

// 歌词显示管理器
class LyricsDisplay {
public:
    LyricsDisplay();
    ~LyricsDisplay();
    
    // 加载歌曲对应的歌词（自动查找）
    bool loadForTrack(const char* audio_path);
    
    // 直接加载歌词文件（跳过查找，更快）
    bool loadFromPath(const char* lrc_path);
    
    // 更新当前播放时间
    void updateTime(uint32_t time_ms);
    
    // 获取当前应显示的歌词文本
    String getCurrentLyric() const;
    
    // 获取下一句歌词文本
    String getNextLyric() const;
    
    // 检查是否有歌词
    bool hasLyrics() const;
    
    // 清空歌词
    void clear();
    
    // 获取滚动显示的歌词（3行：上一句、当前、下一句）
    struct ScrollLyrics {
        String prev;      // 上一句（可能为空）
        String current;   // 当前句
        String next;      // 下一句（可能为空）
        float progress;   // 当前句进度 0.0-1.0（用于平滑滚动）
    };
    ScrollLyrics getScrollLyrics() const;
    
private:
    LyricsParser m_parser;
    int m_currentIndex;
    uint32_t m_currentTime;
    
    // 获取下一句歌词的时间（用于计算进度）
    uint32_t getNextLyricTime() const;
};

// 全局歌词显示实例
extern LyricsDisplay g_lyricsDisplay;
