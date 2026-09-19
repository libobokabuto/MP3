/*
 * ============================================================================
 *  MP3 播放器 —— v2.5 播放模式 + 设置菜单
 * ============================================================================
 *
 *  硬件：ESP32-S3-DevKitC-1 + ST7789(240x240) + TF卡(SPI) + MAX98357A(I2S) + 3按键
 *  库  ：ESP8266Audio / Adafruit_ST7789 / Adafruit_GFX / SD(ESP32核心自带)
 *
 *  ── 上手前先做一件事 ──────────────────────────────────────────────────────
 *   在 PC 上跑一次  python prepare_sd.py   （默认读 D:\software\Music，
 *   输出到 E:\Study\codes\Mp3\sd_ready），把产物拷到 SD 卡根目录，再上电。
 *   不跑也能用：会退回 flash 里的小字库，只是歌名没中文、封面是空相框。
 *
 *  ── 五个页面 ──────────────────────────────────────────────────────────────
 *
 *   【页面1 播放详情】
 *       B1 短按 -> 播放 / 暂停
 *       B2 短按 -> 上一首          B2 长按 -> 进入【页面2 歌曲列表】
 *       B3 短按 -> 下一首          B3 长按 -> 进入【页面3 设置菜单】
 *
 *   【页面2 歌曲列表】
 *       B2 短按 -> 光标上移        B3 短按 -> 光标下移（到顶/到底循环）
 *       B1 短按 -> 播放光标所在曲目 + 回到页面1
 *       B2/B3 长按 -> 放弃选择，直接回到页面1
 *       注意：移动光标【不会】切歌，正在播的那首用暗绿底标出
 *
 *   【页面3 设置菜单】两个入口，右边直接显示当前值
 *       B2 短按 -> 光标上移        B3 短按 -> 光标下移
 *       B1 短按 -> 进入选中的那一项
 *       B2/B3 长按 -> 回到页面1
 *
 *   【页面4 音量设置】
 *       B2 -> 音量 +（长按连调）   B3 -> 音量 -（长按连调）
 *       B1 短按 -> 回设置菜单
 *
 *   【页面5 播放模式】光标（蓝底）和"当前生效"（绿点）是分开的
 *       B2 短按 -> 光标上移        B3 短按 -> 光标下移
 *       B1 短按 -> 确认生效并回设置菜单
 *       B2/B3 长按 -> 放弃改动，回设置菜单
 *
 *  ── v2.2 中/日文歌名 ──────────────────────────────────────────────────────
 *   字库放 SD 卡（font16.bin，约 680KB），开机【一次性读进 PSRAM】。
 *   覆盖 U+00A0-00FF / 2000-206F / 3000-30FF / 4E00-9FFF / FF00-FFEF 共约 21600 字。
 *   实测确认过 ESP32 的 SD 库返回的就是标准 UTF-8，所以不需要任何转码。
 *   汉字取自 simhei.ttf，假名取自 msgothic.ttc。
 *   查不到的冷门码位（比如 U+1407 ᐇ）画一个空心"豆腐块"，不再是一片空白。
 *
 *  ── v2.4 封面 ─────────────────────────────────────────────────────────────
 *   封面在 PC 端就转成 140x140 的 RGB565 原始数据，存成 <歌名>.cov。
 *   板子只做"一次顺序读 + 一次整块刷屏"（约 20ms @ SPI 16MHz）。
 *   这样固件里不用塞图片解码器，也不会因为解码把音频卡爆。
 *   来源优先级：MP3 内嵌 ID3 封面 -> 同名 .jpg/.png/.bmp -> 没有就画空相框。
 *
 *  ── v2.5 播放模式 ─────────────────────────────────────────────────────────
 *   四种，存在 NVS 里掉电不丢，播放页状态行中间有个两字标记（顺序/循环/单曲/随机）：
 *
 *     顺序播放  放到最后一首就停，显示"完成"
 *     列表循环  最后一首放完回到第一首
 *     单曲循环  一直重复当前这首
 *     随机播放  随机跳到另一首（不会随机到自己）
 *
 *   【重要边界】只有【自动续播】遵守模式；手动按上一首/下一首永远循环，
 *   不管当前是什么模式 —— 不然顺序播放时按"下一首"会没反应，很难用。
 *
 *  ── v2.3 消除闪烁 ─────────────────────────────────────────────────────────
 *   页内更新（切歌 / 移光标 / 调音量）不再整屏 fillScreen，只重画变化的部分。
 *   整屏刷黑要发 115KB、约 58ms，那 58ms 的纯黑就是原来"闪一下"的来源。
 *   只有【换页】时才 fillScreen —— 布局完全不同，必须清。
 *
 *  ── 已实现 / 未实现 ────────────────────────────────────────────────────────
 *   ✔ 按钮事件抽象（短按 / 长按 / 长按连发）
 *   ✔ 五页面状态机；列表页光标与"正在播放的曲目"分离
 *   ✔ 播放模式（顺序/列表循环/单曲循环/随机）+ 自动续播，存 NVS
 *   ✔ 真·暂停（暂停期间持续往 I2S 灌静音，避免 DMA 重放最后一个缓冲）
 *   ✔ 音量存 NVS（停手 800ms 才落盘，省闪存寿命）
 *   ✔ 中/日文歌名（SD 全字库 -> PSRAM）+ 缺字豆腐块
 *   ✔ 封面图（PC 预处理 + 一次 blit）
 *   ✔ 局部重绘，切歌 / 移光标不再闪黑
 *   ✔ 歌名跑马灯画在整屏宽度的独立横条里（屏幕边界天然就是裁剪边界）
 *   ✔ 列表页歌名按 UTF-8 字符边界截断 + "…"，不会切出半个汉字
 *   ✔ 重绘过程穿插 serviceAudio()，避免 UI 阻塞导致音频 underrun
 *
 *   ✘ 真实进度条 / 时长解析（原来的进度是假的，已移除）
 *   ✘ 列表超过 MAX_TRACKS 首 / 子目录 / 排序
 *   ✘ 大字号排版 —— 现在全局只有 16px 一种字号，层级只能靠留白做
 *
 *  ⚠️ 【已知边界】如果一个 mp3 能打开但立刻播完（文件损坏），列表循环模式下
 *     会快速连跳到下一首。正常文件不会触发。
 *  ⚠️ 【已知取舍】"长按2进列表 / 长按3进设置" 是隐藏手势，界面上没有常驻提示
 * ============================================================================
 */

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "esp_heap_caps.h"
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

// SD 卡上的成品文件名 —— 都由 PC 端 prepare_sd.py 生成，拷到卡根目录
#define FONT_FILE "/font16.bin"  // 中/日文全字库（约 680KB），开机载入 PSRAM
#define COVER_EXT ".cov"         // 封面：<歌名>.cov，140x140 的 RGB565 原始数据

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

const int MAX_TRACKS = 500;
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

// 页面3 设置菜单（两个入口）
const int MENU_ITEM_X = 16, MENU_ITEM_W = 208, MENU_ITEM_H = 48;
const int MENU_FIRST_Y = 56, MENU_ITEM_GAP = 62;
const int MENU_ITEM_COUNT = 2;

// 页面4 音量设置
const int SET_LABEL_Y = 60;
const int SET_BAR_X = 20, SET_BAR_Y = 92, SET_BAR_W = 200, SET_BAR_H = 34;
const int SET_TRACK_Y = 158;

// 页面5 播放模式
const int MODE_ITEM_X = 16, MODE_ITEM_W = 208, MODE_ITEM_H = 36;
const int MODE_FIRST_Y = 24, MODE_ITEM_GAP = 44;

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
  PAGE_PLAYER,     // 播放详情
  PAGE_LIST,       // 歌曲列表
  PAGE_SETTINGS,   // 设置菜单（音量设置 / 播放设置 两个入口）
  PAGE_VOLUME,     // 音量设置
  PAGE_PLAY_MODE   // 播放模式设置（顺序/列表循环/单曲循环/随机）
};

UiPage currentPage = PAGE_PLAYER;

// changeTrack() 期间置 true，用来掐掉状态行的第一遍重画（见那里注释）。
// 默认 false —— 平时状态一变就应该立刻刷新状态行。
bool deferStatusRedraw = false;

// serviceAudio() 发现一首放完了就置这个标记，真正的处理交给 loop()。
// 原因：serviceAudio() 可能是在 drawUtf8Text() 内部被调用的，
// 在那里直接切歌会嵌套触发一次整页绘制。
bool trackFinishedPending = false;

// ============================================================================
// 播放模式
// ============================================================================

enum PlayMode {
  MODE_SEQUENTIAL = 0,  // 顺序播放：放到最后一首就停
  MODE_REPEAT_ALL,      // 列表循环：最后一首放完回到第一首
  MODE_REPEAT_ONE,      // 单曲循环：一直重复当前这首
  MODE_SHUFFLE,         // 随机播放
  PLAY_MODE_COUNT
};

int playMode = MODE_SEQUENTIAL;

// 两个设置页各自的光标
int settingsCursor = 0;  // 设置菜单：0 = 音量设置，1 = 播放设置
int modeCursor = 0;      // 播放模式页：确认之前的临时选择

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
void handleTrackFinished();
const char *playModeName(int mode);

// ============================================================================
// SD 卡全字库（中/日文歌名靠它）
// ============================================================================
//
// 为什么字库放 SD 卡而不是塞 flash：
//   16x16 全字库约 680KB。塞 flash 会让编译变慢、换字体要重烧，而且以后想加
//   24/32 号大字就彻底放不下了。放 SD 卡 + 开机一次性读进 PSRAM（板子有 8MB），
//   换字体只是换个文件。
//
// 文件格式（由 prepare_sd.py 生成）：
//   头部 16 字节:  "MPF1" + 字宽u16 + 字高u16 + 每字字节数u16 + 段数u16 + 保留u32
//   段表 12×N:     起始码点u32 + 结束码点u32 + 数据偏移u32
//   字形数据:      每字 32 字节 = 16 行 × 2 字节（高字节在左，bit15 = 最左像素）

struct FontSegment {
  uint32_t startCodepoint;
  uint32_t endCodepoint;
  uint32_t dataOffset;
};

const int MAX_FONT_SEGMENTS = 8;

uint8_t *sdFontData = nullptr;      // 字库整块（PSRAM）
uint32_t sdFontSize = 0;
uint16_t sdFontSegmentCount = 0;
FontSegment sdFontSegments[MAX_FONT_SEGMENTS];

uint16_t readLeU16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLeU32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// 一行 2 字节，高字节 = 左边 8 像素
uint16_t sdGlyphRow(const uint8_t *glyph, int row) {
  return static_cast<uint16_t>(
      (static_cast<uint16_t>(glyph[row * 2]) << 8) | glyph[row * 2 + 1]);
}

// 返回该码点的 32 字节点阵指针；不在字库里返回 nullptr
const uint8_t *sdGlyphFor(uint32_t codepoint) {
  if (!sdFontData) {
    return nullptr;
  }

  for (uint16_t i = 0; i < sdFontSegmentCount; i++) {
    if (codepoint < sdFontSegments[i].startCodepoint ||
        codepoint > sdFontSegments[i].endCodepoint) {
      continue;
    }

    uint32_t offset = sdFontSegments[i].dataOffset +
                      (codepoint - sdFontSegments[i].startCodepoint) * 32U;

    if (offset + 32U > sdFontSize) {
      return nullptr;
    }

    return sdFontData + offset;
  }

  return nullptr;
}

// 载入失败不算错误：界面文案仍然由 flash 里的 60 字小字库兜底，只是歌名没中文
bool loadSdFont() {
  if (!sdOk) {
    return false;
  }

  File file = SD.open(FONT_FILE, FILE_READ);
  if (!file) {
    Serial.println("font16.bin not found (歌名将没有中文)");
    return false;
  }

  uint32_t size = file.size();
  if (size < 16 || size > 4UL * 1024 * 1024) {
    Serial.println("font16.bin: 文件大小不对");
    file.close();
    return false;
  }

  // 优先放 PSRAM（8MB 闲着也是闲着），不行再退回内部 RAM
  uint8_t *buffer = static_cast<uint8_t *>(
      heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (!buffer) {
    buffer = static_cast<uint8_t *>(malloc(size));
  }
  if (!buffer) {
    Serial.println("font16.bin: 内存不够");
    file.close();
    return false;
  }

  unsigned long startedMs = millis();
  size_t got = file.read(buffer, size);
  file.close();

  if (got != size || memcmp(buffer, "MPF1", 4) != 0) {
    Serial.println("font16.bin: 读取失败或格式不对");
    free(buffer);
    return false;
  }

  uint16_t glyphWidth = readLeU16(buffer + 4);
  uint16_t glyphHeight = readLeU16(buffer + 6);
  uint16_t bytesPerGlyph = readLeU16(buffer + 8);
  uint16_t segmentCount = readLeU16(buffer + 10);

  if (glyphWidth != 16 || glyphHeight != 16 || bytesPerGlyph != 32 ||
      segmentCount == 0 || segmentCount > MAX_FONT_SEGMENTS ||
      16U + static_cast<uint32_t>(segmentCount) * 12U > size) {
    Serial.println("font16.bin: 头部参数不支持");
    free(buffer);
    return false;
  }

  for (uint16_t i = 0; i < segmentCount; i++) {
    const uint8_t *entry = buffer + 16 + i * 12;
    sdFontSegments[i].startCodepoint = readLeU32(entry);
    sdFontSegments[i].endCodepoint = readLeU32(entry + 4);
    sdFontSegments[i].dataOffset = readLeU32(entry + 8);
  }

  sdFontData = buffer;
  sdFontSize = size;
  sdFontSegmentCount = segmentCount;

  Serial.print("font16.bin OK: ");
  Serial.print(size / 1024);
  Serial.print(" KB, ");
  Serial.print(segmentCount);
  Serial.print(" 段, 耗时 ");
  Serial.print(millis() - startedMs);
  Serial.println(" ms");
  return true;
}

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

// 画一个 16x16 点阵字。字形来源依次尝试：
//   1. SD 卡全字库    —— 中/日文歌名走这里
//   2. flash 内置小字库 —— 固定界面文案；SD 字库没载入时也保证 UI 有中文
//   3. 都没有 —— 画一个空心方块（印刷里的"豆腐块"），而不是像以前那样留一片空白。
//      像 U+1407 (ᐇ) 这种冷门码位就走这条，至少让人知道"这里有个字渲染不出来"
void drawWideGlyph(uint32_t codepoint, int16_t x, int16_t y, uint16_t color) {
  const uint8_t *sdGlyph = sdGlyphFor(codepoint);

  if (sdGlyph) {
    for (int row = 0; row < 16; row++) {
      uint16_t bits = sdGlyphRow(sdGlyph, row);
      if (!bits) {
        continue;
      }
      for (int column = 0; column < 16; column++) {
        if (bits & (1U << (15 - column))) {
          tft.drawPixel(x + column, y + row, color);
        }
      }
    }
    return;
  }

  const ChineseGlyph *flashGlyph = findChineseGlyph(codepoint);

  if (flashGlyph) {
    for (int row = 0; row < 16; row++) {
      uint16_t bits = flashGlyph->rows[row];
      if (!bits) {
        continue;
      }
      for (int column = 0; column < 16; column++) {
        if (bits & (1U << (15 - column))) {
          tft.drawPixel(x + column, y + row, color);
        }
      }
    }
    return;
  }

  tft.drawRect(x + 3, y + 3, 10, 10, color);
}

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
      drawWideGlyph(codepoint, x, y, color);
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

  coverPath += COVER_EXT;  // ".cov"
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
  playMode = prefs.getInt("mode", MODE_SEQUENTIAL);

  if (volume < 0) {
    volume = 0;
  } else if (volume > 100) {
    volume = 100;
  }

  if (playMode < 0 || playMode >= PLAY_MODE_COUNT) {
    playMode = MODE_SEQUENTIAL;
  }

  Serial.print("Volume from NVS: ");
  Serial.println(volume);
  Serial.print("Play mode from NVS: ");
  Serial.println(playModeName(playMode));
}

void saveSettings() {
  prefs.putInt("vol", volume);
}

// 播放模式是一次性确认（不是长按连调），所以确认时立即落盘
void savePlayMode() {
  prefs.putInt("mode", playMode);
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
    bool finished = false;

    if (mp3 && mp3->isRunning()) {
      finished = !mp3->loop();
    } else {
      finished = true;
    }

    if (finished) {
      stopPlayback();
      // 只记标记，真正的处理放到 loop() 里（见 trackFinishedPending 的声明处）
      trackFinishedPending = true;
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
// 封面（v2.4）
// ============================================================================
//
// 这里【不做解码】—— JPEG 解码和缩放都在 PC 端由 prepare_sd.py 做完，
// 板子只干两件事：一次顺序读 + 一次整块刷屏。
// 好处：固件里不用塞图片解码器，也不会因为解码把音频卡爆。
// 代价：往卡里加新歌要重跑一次 prepare_sd.py。

// 封面缓冲放在【内部 RAM】而不是 PSRAM：整块刷屏最终走 SPI DMA，
// 内部 RAM 才是稳妥的 DMA 源。140*140*2 = 39200 字节，内部 RAM 够用。
uint16_t coverBuffer[COVER_W * COVER_H];

// 把 <歌名>.cov 读进缓冲。分块读并穿插 serviceAudio()，避免长时间不喂音频。
bool loadCoverForTrack(const String &audioPath) {
  if (!sdOk) {
    return false;
  }

  File file = SD.open(coverPathForTrack(audioPath).c_str(), FILE_READ);
  if (!file) {
    return false;
  }

  if (file.size() != sizeof(coverBuffer)) {
    file.close();
    return false;
  }

  uint8_t *target = reinterpret_cast<uint8_t *>(coverBuffer);
  size_t total = 0;

  while (total < sizeof(coverBuffer)) {
    size_t got = file.read(target + total, 4096);
    if (got == 0) {
      break;
    }
    total += got;
    serviceAudio();
  }

  file.close();
  return total == sizeof(coverBuffer);
}

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
// 模式全名 —— 设置菜单和播放模式页用
const char *playModeName(int mode) {
  switch (mode) {
    case MODE_REPEAT_ALL:
      return "列表循环";
    case MODE_REPEAT_ONE:
      return "单曲循环";
    case MODE_SHUFFLE:
      return "随机播放";
    case MODE_SEQUENTIAL:
    default:
      return "顺序播放";
  }
}

// 模式简写 —— 播放页状态行里那一小块用（只占两个字宽）
const char *playModeBadge(int mode) {
  switch (mode) {
    case MODE_REPEAT_ALL:
      return "循环";
    case MODE_REPEAT_ONE:
      return "单曲";
    case MODE_SHUFFLE:
      return "随机";
    case MODE_SEQUENTIAL:
    default:
      return "顺序";
  }
}

void updatePlaybackStatus(const String &status) {
  Serial.print("[status] ");
  Serial.println(statusText(status));

  if (currentPage == PAGE_PLAYER && !deferStatusRedraw) {
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
  // 优先画真封面：PC 端已经把它转成 140x140 的 RGB565 原始数据，
  // 这里只需要一次顺序读 + 一次整块刷屏（约 20ms @ SPI 16MHz）
  if (trackCount > 0 && loadCoverForTrack(currentTrackPath())) {
    tft.drawRGBBitmap(COVER_X, COVER_Y, coverBuffer, COVER_W, COVER_H);
    tft.drawRect(COVER_X, COVER_Y, COVER_W, COVER_H, COLOR_BOX_LINE);
    return;
  }

  // 没有 <歌名>.cov 就画个空相框。
  // 注意别再往封面里写 MP3 / I2S 之类的字样 —— I2S 是芯片内部总线名，
  // 用户不该在界面上看到它（这是上一版被吐槽的点之一）。
  tft.fillRect(COVER_X, COVER_Y, COVER_W, COVER_H, COLOR_BOX_FILL);
  tft.drawRect(COVER_X, COVER_Y, COVER_W, COVER_H, COLOR_BOX_LINE);
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

  // 当前播放模式（顺序/循环/单曲/随机），暗色，不抢注意力
  drawUtf8TextCentered(playModeBadge(playMode), 155, STATUS_Y, COLOR_DIM_TEXT,
                       ST77XX_BLACK);

  drawUtf8TextRight(formatElapsed(elapsedMs()).c_str(), 230, STATUS_Y,
                    ST77XX_WHITE);
}

// 每秒只刷右边那一小块时间，不重画整条状态行。
// 清屏区必须从 180 开始 —— 再往左会把上面那个模式标记（约 139~171）擦掉。
void drawPlayerStatusClock() {
  tft.fillRect(180, 0, 60, 22, ST77XX_BLACK);
  drawUtf8TextRight(formatElapsed(elapsedMs()).c_str(), 230, STATUS_Y,
                    ST77XX_WHITE);
}

// fullClear=true 才会整屏刷黑。
//
// 切歌时传 false：状态行、封面、歌名条这三个组件本来就各给自己铺底，
// 页脚和四周背景也没变，所以完全不需要再花 58ms 把整屏刷黑一遍 ——
// 那 58ms 的纯黑，就是"切歌闪一下"的来源。
//
// 从别的页面切进来（布局完全不同）时必须传 true，否则会留下上个页面的残影。
void drawPlayerPage(bool fullClear) {
  if (fullClear) {
    tft.fillScreen(ST77XX_BLACK);
  }

  drawPlayerStatusBar();
  drawAlbumArt();
  drawTrackTitle();
  drawFooterKeys("上一首", "播放", "下一首");
}

// ============================================================================
// 页面2：歌曲列表
// ============================================================================

void drawListScrollbar() {
  // 先整列擦掉。列表页现在页内更新不整屏清空了，
  // 滑块换位置时必须自己把旧位置抹掉，否则会拖一条青色尾巴下来。
  tft.fillRect(LIST_SCROLLBAR_X, LIST_TRACK_Y, 4, LIST_TRACK_H, ST77XX_BLACK);

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

// fullClear=true 只在【换页进来】时用。页内移动光标走 refreshListCursor()。
void drawListPage(bool fullClear) {
  if (fullClear) {
    tft.fillScreen(ST77XX_BLACK);
  }

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

// 光标只挪了一格、视野没翻页时用这个：只重画【旧光标行 + 新光标行】+ 顶栏的 n/N。
//
// 这是列表页体验的关键：原来每次按上下键都要 fillScreen + 重画 7 行（约 110ms，
// 其中 58ms 是黑屏）；现在只碰两行（约 11ms），而且完全不黑屏。
void refreshListCursor(int previousCursor) {
  String position = String(listCursor + 1) + "/" + String(trackCount);
  drawTopLabel("歌曲列表", position.c_str(), ST77XX_YELLOW);

  // 先画旧那行（此时 listCursor 已经变了，它会自动按"非光标"的样式重画）
  if (previousCursor >= listTop && previousCursor < listTop + LIST_ROWS) {
    drawListRow(previousCursor - listTop, previousCursor);
  }
  // 再画新那行，让它拿到高亮
  if (listCursor >= listTop && listCursor < listTop + LIST_ROWS) {
    drawListRow(listCursor - listTop, listCursor);
  }
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

// 移动光标 + 只重画该重画的部分。
//   没翻页  -> 只刷旧行和新行（约 11ms）
//   翻页了  -> 重画 7 行（约 38ms），但依然【不整屏刷黑】
// 对比原来的 fillScreen + 整页重画（约 110ms，其中 58ms 黑屏）
void stepListCursor(int step) {
  int previousCursor = listCursor;
  int previousTop = listTop;

  moveListCursor(step);

  if (listTop == previousTop) {
    refreshListCursor(previousCursor);
  } else {
    drawListPage(false);
  }
}

// ============================================================================
// 页面3：设置菜单
// ============================================================================
//
// 进入设置先看到这个菜单，再从两个入口进各自的子页。
// 右边直接显示"当前值"，不进去也知道现在是多少。

const char *menuItemLabel(int index) {
  return index == 0 ? "音量设置" : "播放设置";
}

// 右边那个当前值：音量显示数字，播放设置显示模式名
String menuItemValue(int index) {
  if (index == 0) {
    return String(volume);
  }
  return String(playModeName(playMode));
}

void drawMenuItem(int index, bool selected) {
  int y = MENU_FIRST_Y + index * MENU_ITEM_GAP;
  uint16_t background = selected ? ST77XX_BLUE : 0x1082;
  uint16_t labelColor = selected ? ST77XX_WHITE : COLOR_DIM_TEXT;

  tft.fillRect(MENU_ITEM_X, y, MENU_ITEM_W, MENU_ITEM_H, background);

  if (selected) {
    tft.drawRect(MENU_ITEM_X, y, MENU_ITEM_W, MENU_ITEM_H, ST77XX_WHITE);
  }

  drawUtf8Text(menuItemLabel(index), MENU_ITEM_X + 16, y + 16, labelColor,
               background);
  drawUtf8TextRight(menuItemValue(index).c_str(),
                    MENU_ITEM_X + MENU_ITEM_W - 16, y + 16, labelColor);
}

void drawSettingsPage(bool fullClear) {
  if (fullClear) {
    tft.fillScreen(ST77XX_BLACK);
  }

  // SD 状态放在这里 —— 它是诊断信息，不该占播放页的版面
  drawTopLabel("设置", sdOk ? "SD OK" : "SD FAIL",
               sdOk ? ST77XX_GREEN : ST77XX_RED);

  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    drawMenuItem(i, i == settingsCursor);
    serviceAudio();
  }

  drawFooterKeys("上一首", "选择", "下一首");
}

// 光标只挪一格：只重画【旧项 + 新项】两行，不清屏
void refreshSettingsCursor(int previousCursor) {
  if (previousCursor >= 0 && previousCursor < MENU_ITEM_COUNT) {
    drawMenuItem(previousCursor, previousCursor == settingsCursor);
  }
  drawMenuItem(settingsCursor, true);
}

// ============================================================================
// 页面4：音量设置
// ============================================================================

// 只重画"数值 + 进度条"这两块。调音量时用这个，不要整页重画。
//
// 两处必须先擦后画，否则不清屏时会留残影：
//   1) 数值从 100 变 50 时，右边少一位，旧的高位会留在原处
//   2) 进度条变短时，右边多余的青色会留下
void refreshVolumeDisplay() {
  tft.fillRect(SET_BAR_X + SET_BAR_W - 48, SET_LABEL_Y, 48, 18, ST77XX_BLACK);
  drawUtf8TextRight(String(volume).c_str(), SET_BAR_X + SET_BAR_W, SET_LABEL_Y,
                    ST77XX_WHITE);

  int innerWidth = SET_BAR_W - 4;
  int innerHeight = SET_BAR_H - 4;
  tft.fillRect(SET_BAR_X + 2, SET_BAR_Y + 2, innerWidth, innerHeight,
               ST77XX_BLACK);

  int fillWidth = innerWidth * volume / 100;
  if (fillWidth > 0) {
    tft.fillRect(SET_BAR_X + 2, SET_BAR_Y + 2, fillWidth, innerHeight,
                 ST77XX_CYAN);
  }
}

void drawVolumePage(bool fullClear) {
  if (fullClear) {
    tft.fillScreen(ST77XX_BLACK);
  }

  drawTopLabel("音量设置", "", ST77XX_WHITE);

  drawUtf8Text("音量", SET_BAR_X, SET_LABEL_Y, ST77XX_CYAN, ST77XX_BLACK);
  tft.drawRect(SET_BAR_X, SET_BAR_Y, SET_BAR_W, SET_BAR_H, COLOR_BOX_LINE);
  refreshVolumeDisplay();

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
// 页面5：播放模式
// ============================================================================
//
// 光标（蓝底）和"当前生效的模式"（右边一个绿点）是分开的两件事：
// 移动光标只是"我在选什么"，按 B1 确认之后才会真的生效。

void drawModeItem(int index, bool selected) {
  int y = MODE_FIRST_Y + index * MODE_ITEM_GAP;
  bool active = (index == playMode);

  uint16_t background = selected ? ST77XX_BLUE : 0x1082;
  uint16_t labelColor = selected ? ST77XX_WHITE : COLOR_DIM_TEXT;

  tft.fillRect(MODE_ITEM_X, y, MODE_ITEM_W, MODE_ITEM_H, background);

  if (selected) {
    tft.drawRect(MODE_ITEM_X, y, MODE_ITEM_W, MODE_ITEM_H, ST77XX_WHITE);
  }

  drawUtf8Text(playModeName(index), MODE_ITEM_X + 16, y + 10, labelColor,
               background);

  // 绿点 = 现在真正生效的模式
  if (active) {
    drawUtf8Text("●", MODE_ITEM_X + MODE_ITEM_W - 30, y + 10, ST77XX_GREEN,
                 background);
  }
}

void drawPlayModePage(bool fullClear) {
  if (fullClear) {
    tft.fillScreen(ST77XX_BLACK);
  }

  drawTopLabel("播放设置", "", ST77XX_WHITE);

  for (int i = 0; i < PLAY_MODE_COUNT; i++) {
    drawModeItem(i, i == modeCursor);
    serviceAudio();
  }

  drawFooterKeys("返回", "上一个", "下一个");
}

// 光标只挪一格：只重画【旧项 + 新项】两行，不清屏
void refreshModeCursor(int previousCursor) {
  if (previousCursor >= 0 && previousCursor < PLAY_MODE_COUNT) {
    drawModeItem(previousCursor, previousCursor == modeCursor);
  }
  drawModeItem(modeCursor, true);
}

// ============================================================================
// 页面调度
// ============================================================================

void drawCurrentPage() {
  switch (currentPage) {
    case PAGE_LIST:
      drawListPage(true);
      break;
    case PAGE_SETTINGS:
      drawSettingsPage(true);
      break;
    case PAGE_VOLUME:
      drawVolumePage(true);
      break;
    case PAGE_PLAY_MODE:
      drawPlayModePage(true);
      break;
    case PAGE_PLAYER:
    default:
      drawPlayerPage(true);
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

    // 顺序很重要：先起播（只改状态、不画），最后整页画一次。
    // 原来先 drawPlayerPage() 再 startPlayback()，状态行会被画两遍 —— 那 22px
    // 小条会在切歌时闪一下。deferStatusRedraw 就是用来掐掉第一遍的。
    deferStatusRedraw = true;
    if (wasPlaying) {
      startPlayback();
    }
    deferStatusRedraw = false;

    // 传 false：不整屏刷黑，只重画状态行/封面/歌名条（页脚没变）
    drawPlayerPage(false);
  }
}

// 随机挑一首，不会挑到正在放的这首
void pickRandomTrack() {
  if (trackCount <= 1) {
    return;
  }

  int next = currentTrackIndex;
  while (next == currentTrackIndex) {
    next = random(trackCount);
  }

  currentTrackIndex = next;
}

// 列表页里"正在播放"那一行的底色变了，重画受影响的两行
void refreshNowPlayingRows(int previousTrack) {
  if (previousTrack >= listTop && previousTrack < listTop + LIST_ROWS) {
    drawListRow(previousTrack - listTop, previousTrack);
  }
  if (currentTrackIndex >= listTop && currentTrackIndex < listTop + LIST_ROWS) {
    drawListRow(currentTrackIndex - listTop, currentTrackIndex);
  }
}

// 一首放完了（由 loop() 在 serviceAudio() 之后调用）。
// 按当前播放模式决定接下来怎么办：
//
//   顺序播放   往后走一首；已经是最后一首就停下，显示"完成"
//   列表循环   往后走一首；最后一首之后回到第一首
//   单曲循环   索引不动，原地重放
//   随机播放   随机跳到另一首
//
// 注意：这里是【自动续播】，遵守模式；手动按上一首/下一首永远循环，不遵守模式。
void handleTrackFinished() {
  int previousTrack = currentTrackIndex;

  if (trackCount == 0) {
    updatePlaybackStatus("DONE");
    return;
  }

  if (playMode == MODE_REPEAT_ONE) {
    // 单曲循环：索引不动
  } else if (playMode == MODE_SEQUENTIAL &&
             currentTrackIndex + 1 >= trackCount) {
    // 顺序播放走到头了：停
    Serial.println("Playlist finished (sequential mode).");
    updatePlaybackStatus("DONE");

    if (currentPage == PAGE_LIST) {
      refreshNowPlayingRows(previousTrack);
    }
    return;
  } else if (playMode == MODE_SHUFFLE) {
    pickRandomTrack();
  } else {
    // 列表循环，或顺序播放的中间某一首
    currentTrackIndex = (currentTrackIndex + 1) % trackCount;
  }

  Serial.print("Auto next: ");
  Serial.println(currentTrackPath());

  if (currentPage == PAGE_PLAYER) {
    resetTitleScroll();
  }

  // 先起播（只改状态、不画），最后再刷新界面 —— 避免状态行被画两遍
  deferStatusRedraw = true;
  startPlayback();
  deferStatusRedraw = false;

  if (currentPage == PAGE_PLAYER) {
    drawPlayerPage(false);
  } else if (currentPage == PAGE_LIST) {
    refreshNowPlayingRows(previousTrack);
  }
}

void enterListPage() {
  currentPage = PAGE_LIST;
  listCursor = currentTrackIndex;
  ensureListCursorVisible();
  drawListPage(true);
}

// 长按 B3 进设置 —— 先进菜单，不再直接进音量页
void enterSettingsPage() {
  currentPage = PAGE_SETTINGS;
  settingsCursor = 0;
  drawSettingsPage(true);
}

void enterVolumePage() {
  currentPage = PAGE_VOLUME;
  drawVolumePage(true);
}

void enterPlayModePage() {
  currentPage = PAGE_PLAY_MODE;
  modeCursor = playMode;  // 光标先停在当前生效的那一项
  drawPlayModePage(true);
}

// 子页 -> 回设置菜单
void backToSettingsMenu() {
  currentPage = PAGE_SETTINGS;
  drawSettingsPage(true);
}

void backToPlayerPage() {
  currentPage = PAGE_PLAYER;
  resetTitleScroll();
  drawPlayerPage(true);
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

  // 只刷数值和进度条。整页重画会在长按连调时每秒闪 8 次黑屏。
  refreshVolumeDisplay();

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
          drawPlayerPage(true);   // 换页了，布局完全不同，必须整屏清一次
          startPlayback();
        }
      }
      break;

    case BTN_PREV:
      if (event == EV_CLICK) {
        stepListCursor(-1);
      } else if (event == EV_LONG) {
        backToPlayerPage();
      }
      break;

    case BTN_NEXT:
      if (event == EV_CLICK) {
        stepListCursor(1);
      } else if (event == EV_LONG) {
        backToPlayerPage();
      }
      break;

    default:
      break;
  }
}

// ---- 设置菜单：B2/B3 移光标，B1 进入选中的那一项 ----------------------------
void onSettingsMenuEvent(BtnId id, BtnEvent event) {
  switch (id) {
    case BTN_PLAY:
      if (event == EV_CLICK) {
        if (settingsCursor == 0) {
          enterVolumePage();
        } else {
          enterPlayModePage();
        }
      }
      break;

    case BTN_PREV:
      if (event == EV_CLICK) {
        int previous = settingsCursor;
        settingsCursor = (settingsCursor + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
        refreshSettingsCursor(previous);
      } else if (event == EV_LONG) {
        backToPlayerPage();
      }
      break;

    case BTN_NEXT:
      if (event == EV_CLICK) {
        int previous = settingsCursor;
        settingsCursor = (settingsCursor + 1) % MENU_ITEM_COUNT;
        refreshSettingsCursor(previous);
      } else if (event == EV_LONG) {
        backToPlayerPage();
      }
      break;

    default:
      break;
  }
}

// ---- 音量页：B2/B3 调（长按连调），B1 回菜单 --------------------------------
void onVolumeEvent(BtnId id, BtnEvent event) {
  switch (id) {
    case BTN_PLAY:
      if (event == EV_CLICK) {
        backToSettingsMenu();
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

// ---- 播放模式页：B2/B3 移光标，B1 确认生效并回菜单，长按 B2/B3 放弃返回 -----
void onPlayModeEvent(BtnId id, BtnEvent event) {
  switch (id) {
    case BTN_PLAY:
      if (event == EV_CLICK) {
        playMode = modeCursor;
        savePlayMode();
        Serial.print("Play mode: ");
        Serial.println(playModeName(playMode));
        backToSettingsMenu();
      }
      break;

    case BTN_PREV:
      if (event == EV_CLICK) {
        int previous = modeCursor;
        modeCursor = (modeCursor + PLAY_MODE_COUNT - 1) % PLAY_MODE_COUNT;
        refreshModeCursor(previous);
      } else if (event == EV_LONG) {
        backToSettingsMenu();  // 放弃这次改动
      }
      break;

    case BTN_NEXT:
      if (event == EV_CLICK) {
        int previous = modeCursor;
        modeCursor = (modeCursor + 1) % PLAY_MODE_COUNT;
        refreshModeCursor(previous);
      } else if (event == EV_LONG) {
        backToSettingsMenu();
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
      onSettingsMenuEvent(id, event);
      break;
    case PAGE_VOLUME:
      onVolumeEvent(id, event);
      break;
    case PAGE_PLAY_MODE:
      onPlayModeEvent(id, event);
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
  Serial.println("=== MP3 player v2.5 (4 pages + play modes) ===");
  Serial.println("TFT CS=9, SD CS=10, SCK=12, MOSI=11, MISO=13");
  Serial.println("I2S BCLK=5, LRC=6, DIN=7");
  Serial.println("Buttons: GPIO4 play/pause, GPIO15 prev/list, GPIO16 next/settings");
  Serial.println();

  // 随机播放要用，不播种的话每次上电的随机序列都一样
  randomSeed(micros());

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

  // 载入中/日文全字库（约 680KB，读进 PSRAM，SD 时钟 4MHz 下大概 1~2 秒）。
  // 载入失败不影响使用：界面文案靠 flash 里那 60 个字兜底，只是歌名没中文。
  if (sdOk) {
    tft.fillScreen(ST77XX_BLACK);
    drawUtf8TextCentered("载入字库", 120, 110, ST77XX_WHITE, ST77XX_BLACK);
  }
  loadSdFont();

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

  // 5) 曲终处理。必须放在 serviceAudio() 之后、并且【不】在绘制中途做 ——
  //    serviceAudio() 可能是在 drawUtf8Text() 内部被调用的，
  //    在那里直接切歌会嵌套触发一次整页绘制。
  if (trackFinishedPending) {
    trackFinishedPending = false;
    handleTrackFinished();
  }

  // 6) 音量停手 800ms 之后才写 NVS
  if (volumeDirty && millis() - volumeChangedMs >= 800) {
    volumeDirty = false;
    saveSettings();
  }
}
