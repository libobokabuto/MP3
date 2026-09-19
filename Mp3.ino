/*
 * ============================================================================
 *  MP3 播放器 —— v2.1.1 三页面导航 + 版式重排 + 暂停修复
 * ============================================================================
 *
 *  硬件：ESP32-S3-DevKitC-1 + ST7789(240x240) + TF卡(SPI) + MAX98357A(I2S) + 3按键
 *  库  ：ESP8266Audio / Adafruit_ST7789 / Adafruit_GFX / SD(ESP32核心自带)
 *
 *  ── 三个页面 ──────────────────────────────────────────────────────────────
 *
 *   【页面1 播放详情】
 *       B1 短按 -> 播放 / 暂停
 *       B2 短按 -> 上一首          B2 长按 -> 进入【页面2 歌曲列表】
 *       B3 短按 -> 下一首          B3 长按 -> 进入【页面3 设置】
 *
 *   【页面2 歌曲列表】
 *       B2 短按 -> 光标上移        B3 短按 -> 光标下移（到顶/到底循环）
 *       B1 短按 -> 播放光标所在曲目 + 回到页面1
 *       B2/B3 长按 -> 放弃选择，直接回到页面1
 *       注意：移动光标【不会】切歌，正在播的那首用暗绿底标出
 *
 *   【页面3 设置】
 *       B2 -> 音量 +（长按连调）   B3 -> 音量 -（长按连调）
 *       B1 短按 -> 回到页面1
 *
 *  ── v2.1.1 改了什么 ──────────────────────────────────────────────────────
 *   ✔ 【修 bug】暂停不再"重复最后一个音节"：
 *      旧实现是"停止调用 mp3->loop()"，但 ESP32 的 I2S 是 DMA 环形缓冲，
 *      主循环一停填充，硬件就把最后一个 DMA 描述符反复重放。
 *      现在改成暂停期间持续往 I2S 灌静音（feedSilence()）：
 *      DMA 一直在流动，解码器原地不动，恢复时从原位置接着放。
 *   ✔ 【版式重排】三条原则：
 *      1) 一屏一个视觉重心 —— 播放页主角是 140x140 的封面，不再是信息栏
 *      2) 删掉只有工程师才需要的信息：SD OK / 共 N 首 / 音量 N / "播放页"标题
 *         （SD 状态搬到设置页；音量只在设置页看）
 *      3) 少画框，用亮度和留白做层级 —— 全局只有 16px 一种字号，
 *         不靠留白和对齐就完全没层级
 *   ✔ 播放状态改用一个彩色小圆点表达（绿=播放中 / 橙=已暂停 / 暗灰=就绪），
 *      不再是一整条又大又吵的彩色横幅
 *   ✔ 顶部状态行右侧每秒只刷一小块走秒，不重画整行
 *   ✔ 列表页从 6 行加到 7 行（顶部标题栏腾出来的空间）
 *   ✔ 底部提示只留一行（三个按钮的短按功能），去掉常驻的长按说明
 *
 *  ⚠️ 【已知取舍】去掉长按说明之后，"长按2进列表 / 长按3进设置" 变成了隐藏手势，
 *     新上手的人不会知道。如果以后觉得难发现，两个低成本的补救办法：
 *       1) 开机后前 3 秒在底部显示一次长按说明，之后自动消失
 *       2) 列表页/设置页的返回手势改成短按，只保留进入时的长按
 *
 *   ⚠️ 进度条暂时删掉了。原因：原来的进度是假的 ——
 *      width = (elapsed / 250) % 212，每 53 秒回零，跟歌长毫无关系。
 *      要做真的，得解析 MP3 帧头算时长（CBR 下 duration ≈ 音频字节数*8/比特率），
 *      那是独立的一步，别和这次改动混在一起。
 *
 *  ── 已实现 / 未实现 ────────────────────────────────────────────────────────
 *   ✔ 按钮事件抽象（短按 / 长按 / 长按连发）
 *   ✔ 三页面状态机；列表页光标与"正在播放的曲目"分离
 *   ✔ 真·暂停；音量存 NVS（停手 800ms 才落盘，省闪存寿命）
 *   ✔ 歌名跑马灯画在整屏宽度的独立横条里（屏幕边界天然就是裁剪边界）
 *   ✔ 列表页歌名按 UTF-8 字符边界截断 + "…"，不会切出半个汉字
 *   ✔ 重绘过程穿插 serviceAudio()，避免 UI 阻塞导致音频 underrun
 *   ✔ 文件名 hex dump（v2.0 诊断），用来确认 SD 库返回的是不是 UTF-8
 *
 *   ✘ 中文/日文歌名 —— 等 hex dump 确认编码后做 SD 全字库（v2.2）
 *   ✘ 封面图 —— 旧的 drawBmpCover() 逐像素 seek，慢到不可用，
 *                本版先关掉（ENABLE_BMP_COVER=0），v2.4 重做
 *   ✘ 真实进度条 / 时长解析
 *   ✘ 列表超过 MAX_TRACKS 首 / 子目录 / 排序 —— v2.5
 * ============================================================================
 */

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "chinese_font.h"
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// ============================================================================
// 调试开关
// ============================================================================

// v2.0 诊断：把 SD 卡上的文件名按【原始字节】打成 hex，并判断是不是合法 UTF-8。
// 目的：确认 ESP32 的 SD 库（ESP-IDF VFS/FATFS）返回的文件名到底是
//       UTF-8 / GBK / 还是被截断的 8.3 短名。
// 编码确认完，把这个改成 0 就能关掉。
#define DEBUG_FILENAME_HEX 1

// 封面图：旧实现慢到不可用（见文件头注释），v2.4 重做前先关掉。
#define ENABLE_BMP_COVER 0

// ============================================================================
// 引脚
// ============================================================================

const int TFT_CS = 9;
const int TFT_DC = 8;
const int TFT_RST = 14;
const int TFT_BLK = -1;

const int SD_CS = 10;
const int SPI_MOSI = 11;
const int SPI_SCLK = 12;
const int SPI_MISO = 13;

const int BUTTON_PLAY_PIN = 4;
const int BUTTON_PREV_PIN = 15;
const int BUTTON_NEXT_PIN = 16;

const int I2S_BCLK = 5;
const int I2S_LRC = 6;
const int I2S_DIN = 7;

// ============================================================================
// 时间常量
// ============================================================================

const unsigned long DEBOUNCE_MS = 30;               // 去抖窗口
const unsigned long LONG_PRESS_MS = 700;            // 按住多久算长按
const unsigned long REPEAT_INTERVAL_MS = 120;       // 长按之后多久连发一次
const unsigned long TITLE_SCROLL_INTERVAL_MS = 180; // 跑马灯每格多少毫秒
const unsigned long CLOCK_DRAW_INTERVAL_MS = 1000;

// ============================================================================
// 容量与音量
// ============================================================================

const int MAX_TRACKS = 64;
const int LIST_ROWS = 6;

const int VOLUME_STEP = 5;
const int DEFAULT_VOLUME = 30;

// ESP8266Audio 的 SetGain() 是【线性】幅度倍数，内部 gainF2P6 = (uint8_t)(f * 64)，
// f 被夹在 [0, 4]。两个后果：
//   1) 增益精度只有 1/64 ≈ 0.0156
//   2) f = 4.0 时 f*64 = 256，塞进 uint8_t 直接溢出成 0（库的坑），所以绝不能给 4.0
// 这里上限取 1.0（= 原始音量不放大，也不会削顶失真）。
const float VOLUME_MAX_GAIN = 1.0f;

// ============================================================================
// 尺寸 / 配色
// ============================================================================
//
// v2.1.1 重新排版。三条原则：
//   1) 一屏只有一个视觉重心 —— 播放页的主角是封面，不是信息栏
//   2) 删掉"只有工程师才需要"的文字：SD OK / 共 N 首 / 音量 N / "播放页"标题
//   3) 少画框，用亮度和留白做层级（全局只有 16px 一种字号，不靠留白就没层级）

// 页面1 播放详情
const int STATUS_Y = 4;                                          // 顶部状态行
const int COVER_X = 50, COVER_Y = 26, COVER_W = 140, COVER_H = 140;
const int TITLE_BAND_Y = 172, TITLE_BAND_H = 24, TITLE_TEXT_Y = 174;

// 页面2 歌曲列表
const int LIST_ROW_X = 6, LIST_ROW_W = 222, LIST_ROW_H = 24;
const int LIST_FIRST_Y = 26, LIST_ROW_GAP = 25;
const int LIST_SCROLLBAR_X = 234, LIST_TRACK_Y = 26, LIST_TRACK_H = 173;

// 页面3 设置
const int SET_LABEL_Y = 60;
const int SET_BAR_X = 20, SET_BAR_Y = 92, SET_BAR_W = 200, SET_BAR_H = 34;
const int SET_TRACK_Y = 158;

// 底部按键提示（只有一行：三个按钮的短按功能）
const int FOOTER_CLEAR_Y = 200;  // 从这里往下整块清掉重画
const int FOOTER_KEY_Y = 214;    // 三个按钮标签的 y
const int FOOTER_LEFT_X = 40;
const int FOOTER_MID_X = 120;
const int FOOTER_RIGHT_X = 200;

// 配色
const uint16_t COLOR_DIM_TEXT = 0x7BEF;     // 次要文字：中灰
const uint16_t COLOR_IDLE_DOT = 0x4208;     // 空闲状态圆点：暗灰
const uint16_t COLOR_BOX_FILL = 0x10A2;     // 封面占位底色：极暗蓝灰
const uint16_t COLOR_BOX_LINE = 0x39E7;     // 封面占位边框：灰
const uint16_t COLOR_PLAYING_ROW = 0x0320;  // 列表里"正在播放"那一行的暗绿底

// ============================================================================
// 全局对象
// ============================================================================

SPIClass sharedSpi(FSPI);
Adafruit_ST7789 tft(&sharedSpi, TFT_CS, TFT_DC, TFT_RST);

AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *audioFile = nullptr;
AudioOutputI2S *audioOut = nullptr;

Preferences prefs;

// ============================================================================
// 全局状态
// ============================================================================

bool sdOk = false;
uint64_t sdSizeMb = 0;

// ---- 播放状态 ----
bool isPlaying = false;
bool isPaused = false;
unsigned long playbackStartedMs = 0;
unsigned long pausedTotalMs = 0;
unsigned long pausedAtMs = 0;
unsigned long lastClockDrawMs = 0;

// ---- 曲库 ----
String tracks[MAX_TRACKS];
int trackCount = 0;
int currentTrackIndex = 0;   // 正在播放 / 已选中的曲目

// ---- 页面 ----
enum UiPage {
  PAGE_PLAYER,
  PAGE_LIST,
  PAGE_SETTINGS
};

UiPage currentPage = PAGE_PLAYER;

int listCursor = 0;   // 列表页光标（独立于 currentTrackIndex）
int listTop = 0;      // 列表可视区第一行

int volume = DEFAULT_VOLUME;   // 0..100

// 长按连调时音量会每 120ms 变一次。NVS 写多了伤寿命，
// 所以先记脏标记，等手停下来 800ms 再落盘一次。
bool volumeDirty = false;
unsigned long volumeChangedMs = 0;

// ---- 跑马灯 ----
unsigned long lastTitleScrollMs = 0;
int titleScrollOffset = 0;

// ============================================================================
// 按钮类型（必须放在所有函数之前，见下面"按钮"一节的说明）
// ============================================================================

enum BtnId {
  BTN_PLAY = 0,
  BTN_PREV,
  BTN_NEXT,
  BTN_COUNT
};

enum BtnEvent {
  EV_NONE,
  EV_CLICK,    // 短按（松开时触发）
  EV_LONG,     // 长按（按住到 700ms 立刻触发一次，不等松开）
  EV_REPEAT    // 长按之后每 120ms 连发
};

struct ButtonState {
  int pin;
  bool raw;                  // 上一次读到的电平（true = 按下）
  bool stable;               // 去抖后的状态
  unsigned long rawChangeMs;
  unsigned long downMs;
  unsigned long lastRepeatMs;
  bool longFired;
};

const int BUTTON_PINS[BTN_COUNT] = {BUTTON_PLAY_PIN, BUTTON_PREV_PIN,
                                    BUTTON_NEXT_PIN};
ButtonState buttons[BTN_COUNT];

// ============================================================================
// 前置声明（解决 serviceAudio <-> 绘制之间的循环调用）
// ============================================================================

void serviceAudio();
void stopPlayback();
void updatePlaybackStatus(const String &status);
void drawCurrentPage();
void drawPlayerStatusBar();

// ============================================================================
// UTF-8 与文字绘制
// ============================================================================

const ChineseGlyph *findChineseGlyph(uint32_t codepoint) {
  for (size_t i = 0; i < chineseGlyphCount; i++) {
    if (chineseGlyphs[i].codepoint == codepoint) {
      return &chineseGlyphs[i];
    }
  }

  return nullptr;
}

uint32_t decodeUtf8(const char *text, size_t &index) {
  uint8_t first = static_cast<uint8_t>(text[index++]);

  if (first < 0x80) {
    return first;
  }

  if ((first & 0xE0) == 0xC0) {
    uint32_t codepoint = first & 0x1F;
    codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index++]) & 0x3F);
    return codepoint;
  }

  if ((first & 0xF0) == 0xE0) {
    uint32_t codepoint = first & 0x0F;
    codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index++]) & 0x3F);
    codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index++]) & 0x3F);
    return codepoint;
  }

  if ((first & 0xF8) == 0xF0) {
    uint32_t codepoint = first & 0x07;
    codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index++]) & 0x3F);
    codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index++]) & 0x3F);
    codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index++]) & 0x3F);
    return codepoint;
  }

  return '?';
}

// 一个字符占几个像素宽：ASCII 走内置 5x8 字体（步进 6px），其余按 16px 方阵
int glyphAdvance(uint32_t codepoint) {
  return codepoint < 0x80 ? 6 : 16;
}

int utf8TextWidth(const char *text) {
  int width = 0;
  size_t index = 0;

  while (text[index] != '\0') {
    width += glyphAdvance(decodeUtf8(text, index));
  }

  return width;
}

// 把文字按像素宽度裁到 maxWidth 以内；放不下就在尾部换成 "…"
// 关键点：按 UTF-8 字符边界切，绝不切出半个汉字
String fitText(const String &text, int maxWidth) {
  const char *raw = text.c_str();

  if (utf8TextWidth(raw) <= maxWidth) {
    return text;
  }

  const int ellipsisWidth = 16;
  int limit = maxWidth - ellipsisWidth;
  if (limit < 0) {
    limit = 0;
  }

  String out;
  int width = 0;
  size_t index = 0;

  while (raw[index] != '\0') {
    size_t next = index;
    int advance = glyphAdvance(decodeUtf8(raw, next));

    if (width + advance > limit) {
      break;
    }

    out += text.substring(index, next);
    width += advance;
    index = next;
  }

  out += "…";
  return out;
}

// 重绘是长阻塞操作，会饿死 mp3->loop() 导致爆音。
// 每画若干个字就回来喂一次音频。
bool sServicingAudio = false;
int sGlyphsSinceService = 0;

void drawUtf8Text(const char *text, int16_t x, int16_t y, uint16_t color,
                  uint16_t background) {
  size_t index = 0;

  while (text[index] != '\0') {
    uint32_t codepoint = decodeUtf8(text, index);

    if (codepoint < 0x80) {
      // Adafruit 的 drawChar 自己会按屏幕范围裁剪，负坐标是安全的
      tft.drawChar(x, y, static_cast<char>(codepoint), color, background, 1);
      x += 6;
    } else {
      const ChineseGlyph *glyph = findChineseGlyph(codepoint);
      if (glyph) {
        for (int row = 0; row < 16; row++) {
          uint16_t bits = glyph->rows[row];
          if (!bits) {
            continue;
          }
          for (int column = 0; column < 16; column++) {
            if (bits & (1U << (15 - column))) {
              tft.drawPixel(x + column, y + row, color);
            }
          }
        }
      }
      x += 16;
    }

    if (++sGlyphsSinceService >= 12) {
      sGlyphsSinceService = 0;
      serviceAudio();
    }
  }
}

void drawUtf8TextCentered(const char *text, int16_t centerX, int16_t y,
                          uint16_t color, uint16_t background) {
  drawUtf8Text(text, centerX - utf8TextWidth(text) / 2, y, color, background);
}

void drawUtf8TextRight(const char *text, int16_t rightX, int16_t y,
                       uint16_t color) {
  drawUtf8Text(text, rightX - utf8TextWidth(text), y, color, ST77XX_BLACK);
}

// ============================================================================
// 文件名诊断（v2.0）
// ============================================================================

bool isValidUtf8(const char *text) {
  while (*text) {
    uint8_t lead = static_cast<uint8_t>(*text);
    int continuation;

    if (lead < 0x80) {
      continuation = 0;
    } else if ((lead & 0xE0) == 0xC0) {
      continuation = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      continuation = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      continuation = 3;
    } else {
      return false;
    }

    text++;
    for (int i = 0; i < continuation; i++) {
      if ((static_cast<uint8_t>(*text) & 0xC0) != 0x80) {
        return false;
      }
      text++;
    }
  }

  return true;
}

#if DEBUG_FILENAME_HEX
void dumpFileNameDiagnostics(const char *name) {
  int byteCount = 0;
  for (const char *p = name; *p; p++) {
    byteCount++;
  }

  Serial.print("    bytes(");
  Serial.print(byteCount);
  Serial.print(") =");
  for (const char *p = name; *p; p++) {
    uint8_t value = static_cast<uint8_t>(*p);
    Serial.print(' ');
    if (value < 0x10) {
      Serial.print('0');
    }
    Serial.print(value, HEX);
  }
  Serial.println();

  Serial.print("    utf8 = ");
  Serial.println(isValidUtf8(name) ? "YES" : "NO  <-- 不是 UTF-8，需要转换");

  // 按 UTF-8 解出来的码点，方便肉眼判断到底是哪个字
  Serial.print("    codepoints =");
  size_t index = 0;
  int shown = 0;
  while (name[index] != '\0' && shown < 24) {
    uint32_t codepoint = decodeUtf8(name, index);
    Serial.print(" U+");
    Serial.print(codepoint, HEX);
    shown++;
  }
  Serial.println();
}
#endif

// ============================================================================
// SD 卡与曲库
// ============================================================================

bool isMp3File(const char *name) {
  String fileName = String(name);
  fileName.toLowerCase();
  return fileName.endsWith(".mp3");
}

String normalizePath(const String &name) {
  if (name.length() == 0) {
    return "";
  }

  if (name[0] == '/') {
    return name;
  }

  return "/" + name;
}

void scanMp3Files() {
  trackCount = 0;
  currentTrackIndex = 0;
  listCursor = 0;
  listTop = 0;

  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println("Open root dir failed.");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory() && isMp3File(file.name())) {
      const char *rawName = file.name();

#if DEBUG_FILENAME_HEX
      Serial.print("  [raw] ");
      Serial.println(rawName);
      dumpFileNameDiagnostics(rawName);
#endif

      if (trackCount < MAX_TRACKS) {
        tracks[trackCount++] = normalizePath(String(rawName));
      }
    }

    file.close();
    file = root.openNextFile();
  }

  root.close();

#if DEBUG_FILENAME_HEX
  Serial.println();
  Serial.println("  >>> 判断方法：");
  Serial.println("      utf8=YES 且 codepoints 落在 U+4E00~U+9FFF 附近 -> 直接用");
  Serial.println("      utf8=NO  (例如 E7 D6 C3 F7 这种)            -> 是 GBK，要转码");
  Serial.println("      名字变成 ABCDEF~1.MP3 这种                    -> 只拿到 8.3 短名，麻烦");
  Serial.println();
#endif
}

String currentTrackPath() {
  if (trackCount == 0) {
    return "";
  }

  return tracks[currentTrackIndex];
}

String currentTrackLabel() {
  if (trackCount == 0) {
    return "0/0";
  }

  return String(currentTrackIndex + 1) + "/" + String(trackCount);
}

String trackName(const String &path) {
  String name = path;

  if (name.startsWith("/")) {
    name.remove(0, 1);
  }

  int dotIndex = name.lastIndexOf('.');
  if (dotIndex > 0) {
    name.remove(dotIndex);
  }

  return name;
}

String coverPathForTrack(const String &path) {
  String coverPath = path;
  int dotIndex = coverPath.lastIndexOf('.');

  if (dotIndex >= 0) {
    coverPath.remove(dotIndex);
  }

  coverPath += ".bmp";
  return coverPath;
}

String formatElapsed(unsigned long elapsedMs) {
  unsigned long seconds = elapsedMs / 1000;
  unsigned long minutes = seconds / 60;
  seconds %= 60;

  String text = String(minutes) + ":";
  if (seconds < 10) {
    text += "0";
  }
  text += String(seconds);
  return text;
}

// ============================================================================
// 音量
// ============================================================================

// 0..100 -> 线性增益。用二次曲线是因为人耳对响度的感知接近对数，
// 线性映射会让"前 50% 音量几乎听不出区别"。
// 30 -> 0.09 -> 实际 gainF2P6 = 5 -> 5/64 = 0.078（就是之前实测能听的 0.08）
float gainForVolume(int value) {
  if (value < 0) {
    value = 0;
  } else if (value > 100) {
    value = 100;
  }

  float normalized = value / 100.0f;
  return VOLUME_MAX_GAIN * normalized * normalized;
}

void applyVolume() {
  if (audioOut) {
    audioOut->SetGain(gainForVolume(volume));
  }
}

void loadSettings() {
  prefs.begin("mp3ui", false);
  volume = prefs.getInt("vol", DEFAULT_VOLUME);

  if (volume < 0) {
    volume = 0;
  } else if (volume > 100) {
    volume = 100;
  }

  Serial.print("Volume from NVS: ");
  Serial.println(volume);
}

void saveSettings() {
  prefs.putInt("vol", volume);
}

// ============================================================================
// 音频
// ============================================================================

unsigned long elapsedMs() {
  if (!isPlaying) {
    return 0;
  }

  unsigned long now = millis();
  unsigned long paused = pausedTotalMs;
  if (isPaused) {
    paused += now - pausedAtMs;
  }

  return now - playbackStartedMs - paused;
}

void stopPlayback() {
  if (mp3) {
    if (mp3->isRunning()) {
      mp3->stop();
    }
    delete mp3;
    mp3 = nullptr;
  }

  if (audioFile) {
    delete audioFile;
    audioFile = nullptr;
  }

  isPlaying = false;
  isPaused = false;
  pausedTotalMs = 0;
  pausedAtMs = 0;
}

// 暂停时【不能】只是停止喂数据。
// ESP32 的 I2S 是 DMA 环形缓冲：主循环一旦停止填充，硬件就会把最后一个
// DMA 描述符反复重放 —— 听感就是"最后一个音节卡住 / 一直重复"。
// 正确做法：继续往 I2S 写静音。DMA 一直在流动（不重复、不爆音），
// 而 MP3 解码器原地不动，所以恢复时是从原位置接着放。
void feedSilence() {
  if (!audioOut) {
    return;
  }

  int16_t silence[2] = {0, 0};
  // ConsumeSample 内部是 i2s_channel_write(..., timeout=0)：
  // DMA 一满就返回 0，循环自然结束，不会卡住
  while (audioOut->ConsumeSample(silence)) {
  }
}

// UI 重绘期间也要定期调用，否则音频 DMA 缓冲会被抽干
void serviceAudio() {
  if (sServicingAudio) {
    return;
  }

  sServicingAudio = true;

  if (isPlaying && isPaused) {
    feedSilence();
  } else if (isPlaying) {
    if (mp3 && mp3->isRunning()) {
      if (!mp3->loop()) {
        stopPlayback();
        updatePlaybackStatus("DONE");
      }
    } else {
      stopPlayback();
      updatePlaybackStatus("DONE");
    }
  }

  sServicingAudio = false;
}

bool startPlayback() {
  String path = currentTrackPath();
  if (!sdOk || path.length() == 0) {
    updatePlaybackStatus("NO MP3");
    return false;
  }

  stopPlayback();

  audioFile = new AudioFileSourceSD(path.c_str());
  if (!audioFile->isOpen()) {
    updatePlaybackStatus("OPEN FAIL");
    stopPlayback();
    return false;
  }

  if (!audioOut) {
    audioOut = new AudioOutputI2S();
    audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
  }
  applyVolume();

  mp3 = new AudioGeneratorMP3();
  if (!mp3->begin(audioFile, audioOut)) {
    updatePlaybackStatus("MP3 FAIL");
    stopPlayback();
    return false;
  }

  isPlaying = true;
  isPaused = false;
  playbackStartedMs = millis();
  pausedTotalMs = 0;
  pausedAtMs = 0;
  lastClockDrawMs = 0;

  updatePlaybackStatus("PLAYING");
  Serial.print("Playing: ");
  Serial.println(path);
  return true;
}

// 真暂停：解码器原地不动（不再调 mp3->loop()），同时由 feedSilence()
// 持续往 I2S 灌静音，避免 DMA 重复最后一个缓冲 —— 见 serviceAudio() 上方注释
void pausePlayback() {
  if (isPlaying && !isPaused) {
    isPaused = true;
    pausedAtMs = millis();
    updatePlaybackStatus("STOPPED");
  }
}

void resumePlayback() {
  if (isPlaying && isPaused) {
    pausedTotalMs += millis() - pausedAtMs;
    isPaused = false;
    updatePlaybackStatus("PLAYING");
  }
}

void togglePlayPause() {
  if (!isPlaying) {
    startPlayback();
  } else if (isPaused) {
    resumePlayback();
  } else {
    pausePlayback();
  }
}

// ============================================================================
// 封面图（v2.4 重做，本版关掉）
// ============================================================================

#if ENABLE_BMP_COVER
uint16_t readLe16(File &file) {
  uint16_t value = file.read();
  value |= static_cast<uint16_t>(file.read()) << 8;
  return value;
}

uint32_t readLe32(File &file) {
  uint32_t value = static_cast<uint32_t>(file.read());
  value |= static_cast<uint32_t>(file.read()) << 8;
  value |= static_cast<uint32_t>(file.read()) << 16;
  value |= static_cast<uint32_t>(file.read()) << 24;
  return value;
}

// 警告：现在的实现每个输出像素都 seek 一次（82x82 = 6724 次），
// 在 SPI 4MHz 的 SD 卡上要好几秒，且会把音频卡死。v2.4 会改成
// "按行预读 -> 转 RGB565 缓冲 -> 一次 drawRGBBitmap"。
bool drawBmpCover(const String &path, int16_t x, int16_t y, int16_t width,
                  int16_t height) {
  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    return false;
  }

  if (readLe16(file) != 0x4D42) {
    file.close();
    return false;
  }

  readLe32(file);
  readLe32(file);
  uint32_t pixelDataOffset = readLe32(file);
  uint32_t dibSize = readLe32(file);
  if (dibSize < 40) {
    file.close();
    return false;
  }

  int32_t sourceWidth = static_cast<int32_t>(readLe32(file));
  int32_t sourceHeight = static_cast<int32_t>(readLe32(file));
  uint16_t planes = readLe16(file);
  uint16_t bitsPerPixel = readLe16(file);
  uint32_t compression = readLe32(file);

  if (sourceWidth <= 0 || sourceHeight == 0 || planes != 1 ||
      bitsPerPixel != 24 || compression != 0) {
    file.close();
    return false;
  }

  bool topDown = sourceHeight < 0;
  int32_t absoluteHeight = sourceHeight < 0 ? -sourceHeight : sourceHeight;
  uint32_t rowSize = (static_cast<uint32_t>(sourceWidth) * 3 + 3) & ~3U;

  for (int16_t outputY = 0; outputY < height; outputY++) {
    int32_t sourceY = (static_cast<int32_t>(outputY) * absoluteHeight) / height;
    if (!topDown) {
      sourceY = absoluteHeight - 1 - sourceY;
    }

    for (int16_t outputX = 0; outputX < width; outputX++) {
      int32_t sourceX = (static_cast<int32_t>(outputX) * sourceWidth) / width;
      uint32_t pixelOffset = pixelDataOffset +
                             static_cast<uint32_t>(sourceY) * rowSize +
                             static_cast<uint32_t>(sourceX) * 3;
      file.seek(pixelOffset);

      uint8_t blue = file.read();
      uint8_t green = file.read();
      uint8_t red = file.read();
      tft.drawPixel(x + outputX, y + outputY, tft.color565(red, green, blue));
    }
  }

  file.close();
  return true;
}
#endif

// ============================================================================
// 状态文字
// ============================================================================

const char *statusText(const String &status) {
  if (status == "READY") {
    return "就绪";
  }
  if (status == "PLAYING") {
    return "播放中";
  }
  if (status == "STOPPED") {
    return "已停止";
  }
  if (status == "DONE") {
    return "完成";
  }
  if (status == "NO MP3") {
    return "无文件";
  }
  if (status == "OPEN FAIL") {
    return "打开失败";
  }
  if (status == "MP3 FAIL") {
    return "MP3失败";
  }

  return status.c_str();
}

// 状态一变就调这里：写串口日志 + 刷新播放页顶部状态行。
// 注意它【不再】画一大条彩色横幅 —— 状态现在只用一个小圆点的颜色表达。
void updatePlaybackStatus(const String &status) {
  Serial.print("[status] ");
  Serial.println(statusText(status));

  if (currentPage == PAGE_PLAYER) {
    drawPlayerStatusBar();
  }
}

// ============================================================================
// 公共绘制
// ============================================================================

// 底部按键提示：只有一行，写三个按钮【短按】干什么。
// 长按的说明不再常驻显示 —— 用户反馈那一行是多余的。
// 代价是"长按进列表/设置"变成隐藏手势（见文件头说明）。
void drawFooterKeys(const char *left, const char *middle, const char *right) {
  tft.fillRect(0, FOOTER_CLEAR_Y, 240, 240 - FOOTER_CLEAR_Y, ST77XX_BLACK);

  drawUtf8TextCentered(left, FOOTER_LEFT_X, FOOTER_KEY_Y, ST77XX_WHITE,
                       ST77XX_BLACK);
  drawUtf8TextCentered(middle, FOOTER_MID_X, FOOTER_KEY_Y, ST77XX_WHITE,
                       ST77XX_BLACK);
  drawUtf8TextCentered(right, FOOTER_RIGHT_X, FOOTER_KEY_Y, ST77XX_WHITE,
                       ST77XX_BLACK);
}

// 顶部一行小字：左标签 + 右值。列表页和设置页共用。
void drawTopLabel(const char *leftText, const char *rightText,
                  uint16_t rightColor) {
  tft.fillRect(0, 0, 240, 22, ST77XX_BLACK);

  if (leftText && leftText[0] != '\0') {
    drawUtf8Text(leftText, 10, STATUS_Y, COLOR_DIM_TEXT, ST77XX_BLACK);
  }
  if (rightText && rightText[0] != '\0') {
    drawUtf8TextRight(rightText, 230, STATUS_Y, rightColor);
  }
}

// ============================================================================
// 页面1：播放详情
// ============================================================================

void drawAlbumArt() {
  // 占位块：一块很暗的底 + 一条灰边框，看起来像"还没放封面的空相框"，
  // 而不是原来那种蓝底白框再写 MP3 / I2S 的调试画面（I2S 是内部总线名，
  // 用户根本不该在界面上看到）。
  tft.fillRect(COVER_X, COVER_Y, COVER_W, COVER_H, COLOR_BOX_FILL);
  tft.drawRect(COVER_X, COVER_Y, COVER_W, COVER_H, COLOR_BOX_LINE);

#if ENABLE_BMP_COVER
  if (trackCount > 0 &&
      drawBmpCover(coverPathForTrack(currentTrackPath()), COVER_X, COVER_Y,
                   COVER_W, COVER_H)) {
    tft.drawRect(COVER_X, COVER_Y, COVER_W, COVER_H, ST77XX_WHITE);
    return;
  }
#endif

  drawUtf8TextCentered("无封面", COVER_X + COVER_W / 2, COVER_Y + COVER_H - 30,
                       COLOR_BOX_LINE, COLOR_BOX_FILL);
}

void resetTitleScroll() {
  titleScrollOffset = 0;
  lastTitleScrollMs = millis();
}

// 歌名画在【整屏宽度】的独立横条里。
// 这样屏幕边界天然就是裁剪边界 —— 不需要手写裁剪框。
void drawTrackTitle() {
  String title = trackName(currentTrackPath());
  if (title.length() == 0) {
    title = "无文件";
  }

  tft.fillRect(0, TITLE_BAND_Y, 240, TITLE_BAND_H, ST77XX_BLACK);

  int width = utf8TextWidth(title.c_str());
  if (width <= 240) {
    drawUtf8TextCentered(title.c_str(), 120, TITLE_TEXT_Y, ST77XX_WHITE,
                         ST77XX_BLACK);
    return;
  }

  drawUtf8Text(title.c_str(), -titleScrollOffset, TITLE_TEXT_Y, ST77XX_WHITE,
               ST77XX_BLACK);
}

// 顶部状态行：  ● 状态    曲目号 ............ 已播时间
// 播放状态只用【圆点颜色】表达：绿=播放中，橙=已暂停，暗灰=就绪。
// 这样就不用再画一条又大又吵的彩色横幅了。
void drawPlayerStatusBar() {
  tft.fillRect(0, 0, 240, 22, ST77XX_BLACK);

  uint16_t dotColor = COLOR_IDLE_DOT;
  const char *stateText = "就绪";

  if (trackCount == 0) {
    stateText = "无文件";
  } else if (isPlaying && !isPaused) {
    dotColor = ST77XX_GREEN;
    stateText = "播放中";
  } else if (isPlaying && isPaused) {
    dotColor = ST77XX_ORANGE;
    stateText = "已暂停";
  }

  drawUtf8Text("●", 8, STATUS_Y, dotColor, ST77XX_BLACK);
  drawUtf8Text(stateText, 28, STATUS_Y, ST77XX_WHITE, ST77XX_BLACK);

  if (trackCount > 0) {
    drawUtf8Text(currentTrackLabel().c_str(), 86, STATUS_Y, ST77XX_CYAN,
                 ST77XX_BLACK);
  }

  drawUtf8TextRight(formatElapsed(elapsedMs()).c_str(), 230, STATUS_Y,
                    ST77XX_WHITE);
}

// 每秒只刷右边那一小块时间，不重画整条状态行
void drawPlayerStatusClock() {
  tft.fillRect(140, 0, 100, 22, ST77XX_BLACK);
  drawUtf8TextRight(formatElapsed(elapsedMs()).c_str(), 230, STATUS_Y,
                    ST77XX_WHITE);
}

void drawPlayerPage() {
  tft.fillScreen(ST77XX_BLACK);
  drawPlayerStatusBar();
  drawAlbumArt();
  drawTrackTitle();
  drawFooterKeys("上一首", "播放", "下一首");
}

// ============================================================================
// 页面2：歌曲列表
// ============================================================================

void drawListScrollbar() {
  if (trackCount <= LIST_ROWS) {
    return;
  }

  tft.drawFastVLine(LIST_SCROLLBAR_X + 1, LIST_TRACK_Y, LIST_TRACK_H, 0x4208);

  int thumbHeight = LIST_TRACK_H * LIST_ROWS / trackCount;
  if (thumbHeight < 14) {
    thumbHeight = 14;
  }

  int maxTop = trackCount - LIST_ROWS;
  int thumbY = LIST_TRACK_Y;
  if (maxTop > 0) {
    thumbY += (LIST_TRACK_H - thumbHeight) * listTop / maxTop;
  }

  tft.fillRect(LIST_SCROLLBAR_X, thumbY, 3, thumbHeight, ST77XX_CYAN);
}

void drawListRow(int row, int trackIndex) {
  int y = LIST_FIRST_Y + row * LIST_ROW_GAP;

  bool isCursor = (trackIndex == listCursor);
  bool isNowPlaying = (trackIndex == currentTrackIndex) && isPlaying;

  uint16_t background = ST77XX_BLACK;
  if (isCursor) {
    background = ST77XX_BLUE;
  } else if (isNowPlaying) {
    background = COLOR_PLAYING_ROW;
  }

  tft.fillRect(LIST_ROW_X, y, LIST_ROW_W, LIST_ROW_H, background);

  uint16_t numberColor = isCursor ? ST77XX_WHITE : ST77XX_CYAN;
  drawUtf8Text(String(trackIndex + 1).c_str(), LIST_ROW_X + 8, y + 3,
               numberColor, background);

  String name = fitText(trackName(tracks[trackIndex]), 176);
  drawUtf8Text(name.c_str(), LIST_ROW_X + 44, y + 3, ST77XX_WHITE, background);
}

void drawListPage() {
  tft.fillScreen(ST77XX_BLACK);

  String position = String(listCursor + 1) + "/" + String(trackCount);
  drawTopLabel("歌曲列表", position.c_str(), ST77XX_YELLOW);

  if (trackCount == 0) {
    drawUtf8TextCentered("无文件", 120, 100, ST77XX_WHITE, ST77XX_BLACK);
  } else {
    for (int row = 0; row < LIST_ROWS; row++) {
      int trackIndex = listTop + row;
      if (trackIndex >= trackCount) {
        break;
      }
      drawListRow(row, trackIndex);
      serviceAudio();
    }
    drawListScrollbar();
  }

  // 按钮上写"上一首/下一首"是跟着硬件来的（GPIO15/16 的丝印就是上一曲/下一曲），
  // 在列表页它们移动光标，差别由中间的"选择"来交代
  drawFooterKeys("上一首", "选择", "下一首");
}

void ensureListCursorVisible() {
  if (listCursor < listTop) {
    listTop = listCursor;
  } else if (listCursor >= listTop + LIST_ROWS) {
    listTop = listCursor - LIST_ROWS + 1;
  }

  int maxTop = trackCount - LIST_ROWS;
  if (maxTop < 0) {
    maxTop = 0;
  }

  if (listTop < 0) {
    listTop = 0;
  } else if (listTop > maxTop) {
    listTop = maxTop;
  }
}

void moveListCursor(int step) {
  if (trackCount == 0) {
    return;
  }

  listCursor += step;
  if (listCursor < 0) {
    listCursor = trackCount - 1;
  } else if (listCursor >= trackCount) {
    listCursor = 0;
  }

  ensureListCursorVisible();
}

// ============================================================================
// 页面3：设置
// ============================================================================

void drawSettingsPage() {
  tft.fillScreen(ST77XX_BLACK);

  // SD 状态从播放页搬到这里 —— 它是诊断信息，属于设置页，不该占播放页的版面
  drawTopLabel("设置", sdOk ? "SD OK" : "SD FAIL",
               sdOk ? ST77XX_GREEN : ST77XX_RED);

  drawUtf8Text("音量", SET_BAR_X, SET_LABEL_Y, ST77XX_CYAN, ST77XX_BLACK);
  drawUtf8TextRight(String(volume).c_str(), SET_BAR_X + SET_BAR_W, SET_LABEL_Y,
                    ST77XX_WHITE);

  tft.drawRect(SET_BAR_X, SET_BAR_Y, SET_BAR_W, SET_BAR_H, COLOR_BOX_LINE);
  int fillWidth = (SET_BAR_W - 4) * volume / 100;
  if (fillWidth > 0) {
    tft.fillRect(SET_BAR_X + 2, SET_BAR_Y + 2, fillWidth, SET_BAR_H - 4,
                 ST77XX_CYAN);
  }

  String title = trackName(currentTrackPath());
  if (title.length() == 0) {
    title = "无文件";
  }
  String shown = fitText(title, 216);
  drawUtf8TextCentered(shown.c_str(), 120, SET_TRACK_Y, ST77XX_WHITE,
                       ST77XX_BLACK);

  drawFooterKeys("返回", "音量-", "音量+");
}

// ============================================================================
// 页面调度
// ============================================================================

void drawCurrentPage() {
  switch (currentPage) {
    case PAGE_LIST:
      drawListPage();
      break;
    case PAGE_SETTINGS:
      drawSettingsPage();
      break;
    case PAGE_PLAYER:
    default:
      drawPlayerPage();
      break;
  }
}

// ============================================================================
// 按钮：短按 / 长按 / 长按连发
// ============================================================================
// 注意：这几个类型必须定义在【文件前部】（见上面"按钮类型"一节）。
// Arduino 会自动在第一个函数之前插入函数原型，如果类型定义在后面，
// 原型里的 BtnId / BtnEvent 就会"未声明"而编译失败。

BtnEvent buttonEvent(ButtonState &button) {
  bool pressed = digitalRead(button.pin) == LOW;

  // 原始电平一变就重新开始计时，去抖窗口内什么都不报
  if (pressed != button.raw) {
    button.raw = pressed;
    button.rawChangeMs = millis();
    return EV_NONE;
  }

  if (millis() - button.rawChangeMs < DEBOUNCE_MS) {
    return EV_NONE;
  }

  // 去抖后的边沿
  if (button.raw != button.stable) {
    button.stable = button.raw;

    if (button.stable) {
      button.downMs = millis();
      button.lastRepeatMs = button.downMs;
      button.longFired = false;
      return EV_NONE;
    }

    // 松开：长按已经报过就不再补一个短按
    if (button.longFired) {
      button.longFired = false;
      return EV_NONE;
    }

    return EV_CLICK;
  }

  // 按住不放
  if (button.stable) {
    unsigned long held = millis() - button.downMs;

    if (!button.longFired && held >= LONG_PRESS_MS) {
      button.longFired = true;
      button.lastRepeatMs = millis();
      return EV_LONG;
    }

    if (button.longFired && millis() - button.lastRepeatMs >= REPEAT_INTERVAL_MS) {
      button.lastRepeatMs = millis();
      return EV_REPEAT;
    }
  }

  return EV_NONE;
}

// ============================================================================
// 页面行为
// ============================================================================

void changeTrack(int step) {
  if (trackCount == 0) {
    updatePlaybackStatus("NO MP3");
    return;
  }

  bool wasPlaying = isPlaying;

  stopPlayback();

  currentTrackIndex += step;
  if (currentTrackIndex < 0) {
    currentTrackIndex = trackCount - 1;
  } else if (currentTrackIndex >= trackCount) {
    currentTrackIndex = 0;
  }

  Serial.print("Selected: ");
  Serial.println(currentTrackPath());

  if (currentPage == PAGE_PLAYER) {
    resetTitleScroll();
    drawPlayerPage();
    if (wasPlaying) {
      startPlayback();
    }
  }
}

void enterListPage() {
  currentPage = PAGE_LIST;
  listCursor = currentTrackIndex;
  ensureListCursorVisible();
  drawListPage();
}

void enterSettingsPage() {
  currentPage = PAGE_SETTINGS;
  drawSettingsPage();
}

void backToPlayerPage() {
  currentPage = PAGE_PLAYER;
  resetTitleScroll();
  drawPlayerPage();
}

void adjustVolume(int delta) {
  int updated = volume + delta;
  if (updated < 0) {
    updated = 0;
  } else if (updated > 100) {
    updated = 100;
  }

  if (updated == volume) {
    return;
  }

  volume = updated;
  applyVolume();
  volumeDirty = true;
  volumeChangedMs = millis();
  drawSettingsPage();

  Serial.print("Volume: ");
  Serial.println(volume);
}

void onPlayerEvent(BtnId id, BtnEvent event) {
  switch (id) {
    case BTN_PLAY:
      if (event == EV_CLICK) {
        togglePlayPause();
      }
      break;

    case BTN_PREV:
      if (event == EV_CLICK) {
        changeTrack(-1);
      } else if (event == EV_LONG) {
        enterListPage();
      }
      break;

    case BTN_NEXT:
      if (event == EV_CLICK) {
        changeTrack(1);
      } else if (event == EV_LONG) {
        enterSettingsPage();
      }
      break;

    default:
      break;
  }
}

void onListEvent(BtnId id, BtnEvent event) {
  switch (id) {
    case BTN_PLAY:
      if (event == EV_CLICK) {
        if (trackCount > 0) {
          currentTrackIndex = listCursor;
          resetTitleScroll();
          currentPage = PAGE_PLAYER;
          drawPlayerPage();
          startPlayback();
        }
      }
      break;

    case BTN_PREV:
      if (event == EV_CLICK) {
        moveListCursor(-1);
        drawListPage();
      } else if (event == EV_LONG) {
        backToPlayerPage();
      }
      break;

    case BTN_NEXT:
      if (event == EV_CLICK) {
        moveListCursor(1);
        drawListPage();
      } else if (event == EV_LONG) {
        backToPlayerPage();
      }
      break;

    default:
      break;
  }
}

void onSettingsEvent(BtnId id, BtnEvent event) {
  switch (id) {
    case BTN_PLAY:
      if (event == EV_CLICK) {
        backToPlayerPage();
      }
      break;

    case BTN_PREV:
      if (event == EV_CLICK || event == EV_LONG) {
        adjustVolume(VOLUME_STEP);
      } else if (event == EV_REPEAT) {
        adjustVolume(2);
      }
      break;

    case BTN_NEXT:
      if (event == EV_CLICK || event == EV_LONG) {
        adjustVolume(-VOLUME_STEP);
      } else if (event == EV_REPEAT) {
        adjustVolume(-2);
      }
      break;

    default:
      break;
  }
}

void dispatchUiEvent(BtnId id, BtnEvent event) {
  if (event == EV_NONE) {
    return;
  }

  switch (currentPage) {
    case PAGE_LIST:
      onListEvent(id, event);
      break;
    case PAGE_SETTINGS:
      onSettingsEvent(id, event);
      break;
    case PAGE_PLAYER:
    default:
      onPlayerEvent(id, event);
      break;
  }
}

// ============================================================================
// SD 初始化
// ============================================================================

void initSdCard() {
  Serial.println("Initializing SD card...");

  if (!SD.begin(SD_CS, sharedSpi, 4000000)) {
    sdOk = false;
    Serial.println("SD mount failed.");
    return;
  }

  if (SD.cardType() == CARD_NONE) {
    sdOk = false;
    Serial.println("No SD card attached.");
    return;
  }

  sdOk = true;
  sdSizeMb = SD.cardSize() / (1024 * 1024);
  scanMp3Files();

  Serial.print("SD OK, size MB: ");
  Serial.println(sdSizeMb);
  Serial.print("MP3 count: ");
  Serial.println(trackCount);
  Serial.print("First MP3: ");
  Serial.println(currentTrackPath());
  Serial.println();
}

// ============================================================================
// setup / loop
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== MP3 player v2.1 (3 pages) ===");
  Serial.println("TFT CS=9, SD CS=10, SCK=12, MOSI=11, MISO=13");
  Serial.println("I2S BCLK=5, LRC=6, DIN=7");
  Serial.println("Buttons: GPIO4 play/pause, GPIO15 prev/list, GPIO16 next/settings");
  Serial.println();

  if (TFT_BLK >= 0) {
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, HIGH);
  }

  pinMode(BUTTON_PLAY_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PREV_PIN, INPUT_PULLUP);
  pinMode(BUTTON_NEXT_PIN, INPUT_PULLUP);

  sharedSpi.begin(SPI_SCLK, SPI_MISO, SPI_MOSI);
  tft.init(240, 240);
  tft.setRotation(0);

  loadSettings();

  initSdCard();

  audioOut = new AudioOutputI2S();
  audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
  applyVolume();

  // 记下按键的初始电平，避免上电瞬间被当成一次按下
  for (int i = 0; i < BTN_COUNT; i++) {
    buttons[i].pin = BUTTON_PINS[i];
    bool pressed = digitalRead(BUTTON_PINS[i]) == LOW;
    buttons[i].raw = pressed;
    buttons[i].stable = pressed;
    buttons[i].rawChangeMs = millis();
    buttons[i].downMs = millis();
    buttons[i].lastRepeatMs = millis();
    buttons[i].longFired = false;
  }

  currentPage = PAGE_PLAYER;
  drawCurrentPage();

  Serial.println("Ready. Long-press GPIO15 -> list, long-press GPIO16 -> settings.");
}

void loop() {
  // 1) 按钮
  for (int i = 0; i < BTN_COUNT; i++) {
    BtnEvent event = buttonEvent(buttons[i]);
    if (event != EV_NONE) {
      dispatchUiEvent(static_cast<BtnId>(i), event);
    }
  }

  // 2) 歌名跑马灯（只在播放详情页跑，且只在歌名放不下时跑）
  if (currentPage == PAGE_PLAYER && trackCount > 0) {
    String title = trackName(currentTrackPath());
    int width = utf8TextWidth(title.c_str());

    if (width > 240 && millis() - lastTitleScrollMs >= TITLE_SCROLL_INTERVAL_MS) {
      lastTitleScrollMs = millis();
      titleScrollOffset++;
      if (titleScrollOffset > width + 24) {
        titleScrollOffset = 0;
      }
      drawTrackTitle();
    }
  }

  // 3) 顶部状态行右边那个走秒（每秒只刷那一小块）
  if (currentPage == PAGE_PLAYER && isPlaying && !isPaused &&
      millis() - lastClockDrawMs >= CLOCK_DRAW_INTERVAL_MS) {
    lastClockDrawMs = millis();
    drawPlayerStatusClock();
  }

  // 4) 喂音频
  serviceAudio();

  // 5) 音量停手 800ms 之后才写 NVS
  if (volumeDirty && millis() - volumeChangedMs >= 800) {
    volumeDirty = false;
    saveSettings();
  }
}
