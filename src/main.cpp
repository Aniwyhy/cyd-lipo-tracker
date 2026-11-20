// LiPo field tracker for the 4" CYD (ESP32-3248S040: ST7796 320x480 portrait, XPT2046 resistive touch)
// - Arsenal: every pack you own (name, cells, HV, capacity). Tap to put it in / take it out of the kit,
//   hold to edit, + New to add one.
// - Kit: the packs you brought, as cards. Tap a card to set Charged / Storage / Discharged / Dead.
// - Everything persists in LittleFS. Touch calibration in NVS (hold BOOT 2 s to recalibrate).
// - Serial (115200): "shot" dumps the screen (RLE rgb888), "t x y" taps, "h x y" long-presses.

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>

// ---------------------------------------------------------------- display
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

 public:
  LGFX() {
    auto b = _bus.config();
    b.spi_host = HSPI_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.pin_sclk = 14;
    b.pin_mosi = 13;
    b.pin_miso = 12;
    b.pin_dc = 2;
    _bus.config(b);
    _panel.setBus(&_bus);
    auto p = _panel.config();
    p.pin_cs = 15;
    p.pin_rst = -1;
    p.panel_width = 320;
    p.panel_height = 480;
    p.readable = true;
    p.bus_shared = true;
    _panel.config(p);
    auto l = _light.config();
    l.pin_bl = 27;
    l.freq = 44100;
    l.pwm_channel = 7;
    _light.config(l);
    _panel.setLight(&_light);
    auto t = _touch.config();
    t.x_min = 300;
    t.x_max = 3900;
    t.y_min = 200;
    t.y_max = 3900;
    t.pin_int = -1;
    t.bus_shared = true;
    t.offset_rotation = 0;
    t.spi_host = HSPI_HOST;
    t.freq = 1000000;
    t.pin_sclk = 14;
    t.pin_mosi = 13;
    t.pin_miso = 12;
    t.pin_cs = 33;
    _touch.config(t);
    _panel.setTouch(&_touch);
    setPanel(&_panel);
  }
};

static LGFX tft;
static LGFX_Sprite band(&tft);    // full-width strip, everything is composed in these
static LGFX_Sprite keySpr(&tft);  // one keyboard key
static Preferences prefs;

static const int SCR_W = 320, SCR_H = 480;
static const int BAND_H = 96, NBANDS = SCR_H / BAND_H;

// ---------------------------------------------------------------- palette
static constexpr uint32_t C_BG = 0x151413;
static constexpr uint32_t C_CARD = 0x24221F;
static constexpr uint32_t C_CARD_DIM = 0x1B1A18;
static constexpr uint32_t C_CARD_HI = 0x37342F;
static constexpr uint32_t C_LINE = 0x4A4742;
static constexpr uint32_t C_BATT_LINE = 0x6E6A63;
static constexpr uint32_t C_BATT_IN = 0x161514;
static constexpr uint32_t C_SEG_EMPTY = 0x2E2C29;
static constexpr uint32_t C_FG = 0xF4F1EA;
static constexpr uint32_t C_DIM = 0x8F8B82;
static constexpr uint32_t C_FAINT = 0x5E5A54;
static constexpr uint32_t C_ACCENT = 0xE2795A;
static constexpr uint32_t C_ACCENT_DK = 0xB95E45;
static constexpr uint32_t C_KEY = 0x2C2A27;
static constexpr uint32_t C_KEY_SP = 0x3D3A36;
static constexpr uint32_t C_KEY_HI = 0x5C5852;

static constexpr uint32_t C_CHG = 0x4CC38A;
static constexpr uint32_t C_STO = 0x5AA9E6;
static constexpr uint32_t C_DIS = 0xE8A33D;
static constexpr uint32_t C_DEAD = 0xE5615A;
static constexpr uint32_t C_HV = 0xB79BFF;
static constexpr uint32_t C_HV_INK = 0x1D1630;

static bool gDim = false;  // draw everything darkened (behind a sheet)
static uint32_t col(uint32_t c) {
  if (!gDim) return c;
  uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  r = r * 2 / 5; g = g * 2 / 5; b = b * 2 / 5;
  return (r << 16) | (g << 8) | b;
}

static const lgfx::IFont* F_TITLE = &fonts::FreeSansBold18pt7b;
static const lgfx::IFont* F_H = &fonts::FreeSansBold12pt7b;
static const lgfx::IFont* F_UI = &fonts::FreeSans9pt7b;
static const lgfx::IFont* F_UIB = &fonts::FreeSansBold9pt7b;
static const lgfx::IFont* F_KEY = &fonts::FreeSans12pt7b;

// ---------------------------------------------------------------- packs
enum : uint8_t { ST_CHG, ST_STO, ST_DIS, ST_DEAD, ST_N, ST_NONE = 255 };
static const char* ST_NAME[] = {"Charged", "Storage", "Discharged", "Dead"};
static const uint32_t ST_COL[] = {C_CHG, C_STO, C_DIS, C_DEAD};
static const float ST_LVL[] = {1.0f, 0.55f, 0.2f, 0.0f};

struct Pack {
  String name;
  uint8_t cells;
  bool hv;
  uint16_t mah;
  bool inKit;
  uint8_t state;
};
static std::vector<Pack> packs;
static std::vector<int> kit;  // indices into packs, arsenal order
static const char* PACK_FILE = "/packs.txt";
static const int MAX_PACKS = 60, MAX_NAME = 14;

static float cellV(uint8_t st, bool hv) {
  switch (st) {
    case ST_CHG: return hv ? 4.35f : 4.20f;
    case ST_STO: return hv ? 3.85f : 3.80f;
    case ST_DIS: return 3.50f;
    default: return 0;
  }
}
static String packV(uint8_t st, const Pack& p) {
  if (st == ST_DEAD) return "--";
  return String(st == ST_DIS ? "~" : "") + String(cellV(st, p.hv) * p.cells, 1) + "V";
}

static void rebuildKit() {
  kit.clear();
  for (int i = 0; i < (int)packs.size(); i++)
    if (packs[i].inKit) kit.push_back(i);
}

// packs.txt: one pack per line, fields separated by "|", in any order after the name:
//   Race A      | 6S | 1300mAh |    | kit | charged
// Lines starting with # are comments. The board rewrites the file (and its header) when you change something.
static const char* STATE_WORD[] = {"charged", "storage", "discharged", "dead"};

static void writePacks(Print& out) {
  out.print("# LiPo tracker packs. One per line:  name | cells | capacity | HV | kit | state\n"
            "#   cells: 1S..8S    capacity: e.g. 1300mAh    HV: write HV for high-voltage packs\n"
            "#   kit: write kit if the pack is coming with you    state: charged, storage, discharged, dead\n"
            "# Fields after the name can be in any order; missing ones default to 4S 1300mAh, not HV, home, charged.\n\n");
  for (auto& p : packs) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%-14s | %dS | %5umAh | %-2s | %-3s | %s\n", p.name.c_str(), p.cells, p.mah,
             p.hv ? "HV" : "", p.inKit ? "kit" : "", STATE_WORD[p.state]);
    out.print(buf);
  }
}

static void savePacks() {
  File f = LittleFS.open("/packs.tmp", "w");
  if (!f) return;
  writePacks(f);
  f.close();
  LittleFS.remove(PACK_FILE);
  LittleFS.rename("/packs.tmp", PACK_FILE);
}

// One token after the name ("6S", "1300mAh", "HV", "kit", "storage", ...).
static void applyToken(Pack& p, String t) {
  t.trim();
  t.toLowerCase();
  if (!t.length() || t == "-") return;
  int n = t.toInt();
  if (t == "hv" || t == "lihv") p.hv = true;
  else if (t == "std" || t == "lv" || t == "standard") p.hv = false;
  else if (t == "kit" || t == "in" || t == "yes") p.inKit = true;
  else if (t == "home" || t == "out" || t == "no") p.inKit = false;
  else if (t.startsWith("charg") || t == "full") p.state = ST_CHG;
  else if (t.startsWith("stor")) p.state = ST_STO;
  else if (t.startsWith("disch") || t == "empty" || t == "flat" || t == "used") p.state = ST_DIS;
  else if (t == "dead" || t == "bad" || t == "puffy" || t == "broken") p.state = ST_DEAD;
  else if (n > 0 && t.endsWith("s") && t.length() <= 2) p.cells = constrain(n, 1, 8);
  else if (n > 0 && (t.endsWith("mah") || n >= 50)) p.mah = constrain(n, 50, 30000);
}

static bool parsePackLine(String line, Pack& p) {
  line.replace("\r", "");
  line.trim();
  if (!line.length() || line[0] == '#') return false;
  p = {"", 4, false, 1300, false, ST_CHG};
  if (line.indexOf('|') < 0 && line.indexOf('\t') >= 0) {  // v1 format: name\tcells\thv\tmah\tkit\tstate
    int t[5], pos = 0;
    for (int j = 0; j < 5; j++) {
      t[j] = line.indexOf('\t', pos);
      if (t[j] < 0) return false;
      pos = t[j] + 1;
    }
    p.name = line.substring(0, t[0]);
    p.cells = constrain(line.substring(t[0] + 1, t[1]).toInt(), 1, 8);
    p.hv = line.substring(t[1] + 1, t[2]).toInt() != 0;
    p.mah = constrain(line.substring(t[2] + 1, t[3]).toInt(), 50, 30000);
    p.inKit = line.substring(t[3] + 1, t[4]).toInt() != 0;
    p.state = constrain(line.substring(t[4] + 1).toInt(), 0, 3);
    return true;
  }
  int bar = line.indexOf('|');
  p.name = bar < 0 ? line : line.substring(0, bar);
  p.name.trim();
  if (p.name.length() > MAX_NAME) p.name = p.name.substring(0, MAX_NAME);
  if (!p.name.length()) return false;
  if (bar < 0) return true;
  String rest = line.substring(bar + 1);
  rest.replace('|', ' ');
  rest.replace(',', ' ');
  int pos = 0;
  while (pos < (int)rest.length()) {  // whitespace-separated tokens
    int e = rest.indexOf(' ', pos);
    if (e < 0) e = rest.length();
    applyToken(p, rest.substring(pos, e));
    pos = e + 1;
  }
  return true;
}

static void loadPacks() {
  packs.clear();
  File f = LittleFS.open(PACK_FILE, "r");
  if (!f) {
    packs = {
        {"Race A", 6, false, 1300, true, ST_CHG},  {"Race B", 6, false, 1300, true, ST_CHG},
        {"Race C", 6, false, 1300, true, ST_STO},  {"Cine 4S", 4, false, 1500, true, ST_CHG},
        {"Whoop 1", 1, true, 450, true, ST_DIS},   {"Whoop 2", 1, true, 450, true, ST_CHG},
        {"Long range", 6, false, 3000, false, ST_STO}, {"Puffy", 4, false, 1500, false, ST_DEAD},
    };
    savePacks();
    rebuildKit();
    return;
  }
  bool legacy = false;
  while (f.available() && (int)packs.size() < MAX_PACKS) {
    String line = f.readStringUntil('\n');
    if (line.indexOf('\t') >= 0 && line.indexOf('|') < 0) legacy = true;
    Pack p;
    if (parsePackLine(line, p)) packs.push_back(p);
  }
  f.close();
  if (legacy) savePacks();  // migrate to the text format
  rebuildKit();
}

// ---------------------------------------------------------------- drawing helpers
struct Rect {
  int x, y, w, h;
  bool hit(int px, int py, int m = 0) const { return px >= x - m && px < x + w + m && py >= y - m && py < y + h + m; }
};

template <typename G>
static void rrect(G& s, int x, int y, int w, int h, int r, uint32_t c) {
  if (w <= 0 || h <= 0) return;
  r = min(r, min(w, h) / 2);
  s.fillSmoothRoundRect(x, y, w, h, r, col(c));
}

template <typename G>
static void text(G& s, const String& t, int x, int y, const lgfx::IFont* f, uint32_t c,
                 textdatum_t d = textdatum_t::middle_left) {
  s.setFont(f);
  s.setTextColor(col(c));
  s.setTextDatum(d);
  s.drawString(t, x, y);
}

template <typename G>
static String fitText(G& g, const String& s, int maxW) {
  if (g.textWidth(s) <= maxW) return s;
  String t = s;
  int e = g.textWidth("..");
  while (t.length() && g.textWidth(t) + e > maxW) t.remove(t.length() - 1);
  return t + "..";
}

// A LiPo: rounded body + terminal nub, one segment per cell filled to the state's level.
template <typename G>
static void drawBattery(G& s, int x, int y, int w, int h, int cells, uint8_t st) {
  int nubW = max(3, w / 22), nubH = h * 2 / 5;
  int bw = w - nubW;
  int r = max(3, h / 6);
  uint32_t outline = st == ST_DEAD ? C_DEAD : C_BATT_LINE;
  int t = h >= 40 ? 3 : 2;
  rrect(s, x + bw - 2, y + (h - nubH) / 2, nubW + 2, nubH, max(1, nubW / 2), outline);
  rrect(s, x, y, bw, h, r, outline);
  rrect(s, x + t, y + t, bw - 2 * t, h - 2 * t, max(1, r - t), C_BATT_IN);
  int pad = t + max(2, h / 14);
  int gap = (w < 70 || cells > 6) ? 2 : 3;
  int ix = x + pad, iy = y + pad, iw = bw - 2 * pad, ih = h - 2 * pad;
  float segW = (iw - gap * (cells - 1)) / (float)cells;
  float lvl = st < ST_N ? ST_LVL[st] : 0;
  uint32_t on = st < ST_N ? ST_COL[st] : C_SEG_EMPTY;
  for (int i = 0; i < cells; i++) {
    int x0 = ix + (int)roundf(i * (segW + gap));
    int x1 = ix + (int)roundf(i * (segW + gap) + segW);
    int sw = max(1, x1 - x0);
    rrect(s, x0, iy, sw, ih, 2, C_SEG_EMPTY);
    int fh = (int)roundf(ih * lvl);
    if (fh > 0) rrect(s, x0, iy + ih - fh, sw, fh, 2, on);
  }
  if (st == ST_DEAD) {
    int cx = x + bw / 2, cy = y + h / 2, d = min(bw, h) / 3;
    float lw = h >= 40 ? 2.6f : 1.6f;
    s.drawWideLine(cx - d, cy - d, cx + d, cy + d, lw, col(C_DEAD));
    s.drawWideLine(cx - d, cy + d, cx + d, cy - d, lw, col(C_DEAD));
  }
}

// "⚡HV" pill; returns its width.
template <typename G>
static int drawHV(G& s, int x, int y, int h) {
  int w = (int)(h * 2.35f);
  rrect(s, x, y, w, h, h / 2, C_HV);
  float u = h / 18.0f;
  int bx = x + (int)(h * 0.58f), cy = y + h / 2;
  s.fillTriangle(bx + (int)(2.5f * u), cy - (int)(6.5f * u), bx - (int)(3.5f * u), cy + (int)(1 * u),
                 bx + (int)(0.5f * u), cy + (int)(1 * u), col(C_HV_INK));
  s.fillTriangle(bx - (int)(0.5f * u), cy - (int)(1 * u), bx + (int)(3.5f * u), cy - (int)(1 * u),
                 bx - (int)(2.5f * u), cy + (int)(6.5f * u), col(C_HV_INK));
  text(s, "HV", x + w - (int)(h * 0.33f), cy + 1, F_UIB, C_HV_INK, textdatum_t::middle_right);
  return w;
}

template <typename G>
static void drawCheck(G& s, int cx, int cy, bool on, uint32_t cardCol) {
  if (on) {
    s.fillSmoothCircle(cx, cy, 13, col(C_ACCENT));
    s.drawWideLine(cx - 6, cy + 1, cx - 2, cy + 5, 1.5f, col(C_BG));
    s.drawWideLine(cx - 2, cy + 5, cx + 6, cy - 4, 1.5f, col(C_BG));
  } else {
    s.fillSmoothCircle(cx, cy, 13, col(C_LINE));
    s.fillSmoothCircle(cx, cy, 11, col(cardCol));
  }
}

// ---------------------------------------------------------------- UI state
enum Screen { S_KIT, S_ARS, S_STATE, S_CONFIRM, S_EDIT, S_KB };
static Screen screen = S_KIT, baseScreen = S_KIT;  // baseScreen: what a sheet sits on

enum Id {
  ID_NONE = -1, ID_TAB_KIT = 1, ID_TAB_ARS, ID_NEW, ID_FOOT_CHG, ID_FOOT_STO, ID_EMPTY_GO,
  ID_ST0 = 20, ID_ST1, ID_ST2, ID_ST3, ID_REMOVE, ID_BG,
  ID_OK = 30, ID_CANCEL,
  ID_NAME = 40, ID_CELL1 = 41,  // ..48
  ID_STD = 50, ID_HV, ID_MINUS, ID_PLUS, ID_DEL, ID_ECANCEL, ID_SAVE,
  ID_ITEM = 1000,
};
static int pressed = ID_NONE;

static const int LIST_Y = BAND_H, LIST_HT = SCR_H - LIST_Y;
static const int PAD = 10, GAP = 10;
static const int CARD_W = 145, CARD_H = 124, CARD_X0 = 10, CARD_X1 = 165;
static const int FOOT_H = 44;
static const int ROW_H = 64, ROW_GAP = 8;

static const Rect TABS = {12, 10, 296, 42};
static const Rect BTN_NEW = {SCR_W - 12 - 92, 58, 92, 32};
static const Rect EMPTY_GO = {85, 340, 150, 46};

static float scrollKit = 0, scrollArs = 0, scrollVel = 0;
static float& scrollRef(Screen w) { return w == S_ARS ? scrollArs : scrollKit; }

static int kitFooterY() { return PAD + ((int)kit.size() + 1) / 2 * (CARD_H + GAP); }
static int kitContentH() { return kit.empty() ? 0 : kitFooterY() + FOOT_H + PAD; }
static int arsContentH() { return PAD + (int)packs.size() * (ROW_H + ROW_GAP) + 40; }
static int maxScroll(Screen w) { return max(0, (w == S_ARS ? arsContentH() : kitContentH()) - LIST_HT); }
static void clampScroll(Screen w) { scrollRef(w) = constrain(scrollRef(w), 0.0f, (float)maxScroll(w)); }

static Rect kitCardRect(int i) {  // content coords
  return {i % 2 ? CARD_X1 : CARD_X0, PAD + (i / 2) * (CARD_H + GAP), CARD_W, CARD_H};
}
static Rect kitFootRect(int which) { return {which ? CARD_X1 : CARD_X0, kitFooterY(), CARD_W, FOOT_H}; }
static Rect arsRowRect(int i) { return {10, PAD + i * (ROW_H + ROW_GAP), 300, ROW_H}; }

// state sheet
static const int STATE_Y = 2 * BAND_H;  // 192
static int sheetPack = -1;
static Rect stateBtn(int st) { return {st % 2 ? 163 : 12, 272 + (st / 2) * 82, 145, 76}; }
static const Rect BTN_REMOVE = {70, 440, 180, 34};

// confirm sheet
static const int CF_Y = 3 * BAND_H;  // 288
enum ConfirmKind { CF_ALL_CHG, CF_ALL_STO, CF_DELETE };
static ConfirmKind cfKind;
static const Rect CF_CANCEL = {12, 404, 142, 56}, CF_OK = {166, 404, 142, 56};

// editor
static Pack ed;
static int edIndex = -1;  // -1 = new pack
static const Rect E_NAME = {12, 62, 296, 52};
static const int E_CELLS_Y = 250, E_CELLS_H = 44;
static Rect cellRect(int n) {  // n = 1..8
  float w = (296 - 7 * 4) / 8.0f;
  int x0 = 12 + (int)roundf((n - 1) * (w + 4)), x1 = 12 + (int)roundf((n - 1) * (w + 4) + w);
  return {x0, E_CELLS_Y, x1 - x0, E_CELLS_H};
}
static const Rect E_STD = {12, 306, 146, 44}, E_HVB = {162, 306, 146, 44};
static const Rect E_MINUS = {12, 362, 60, 46}, E_PLUS = {248, 362, 60, 46};
static Rect E_DEL() { return {12, 422, 90, 48}; }
static Rect E_CANCEL() { return edIndex >= 0 ? Rect{108, 422, 96, 48} : Rect{12, 422, 144, 48}; }
static Rect E_SAVE() { return edIndex >= 0 ? Rect{210, 422, 98, 48} : Rect{164, 422, 144, 48}; }
static const uint16_t CAPS[] = {150, 200, 250, 300, 350, 450, 550, 650, 750, 850, 1000, 1100, 1300, 1400, 1500,
                                1550, 1800, 2000, 2200, 2600, 3000, 3300, 4000, 4500, 5000, 6000, 6500, 8000, 10000};

// ---------------------------------------------------------------- drawing: kit / arsenal
static void drawHeader(LGFX_Sprite& s, Screen w) {
  bool k = w == S_KIT;
  rrect(s, TABS.x, TABS.y, TABS.w, TABS.h, 21, C_CARD);
  rrect(s, k ? TABS.x : TABS.x + TABS.w / 2, TABS.y, TABS.w / 2, TABS.h, 21, C_ACCENT);
  text(s, String("Kit  ") + kit.size(), TABS.x + TABS.w / 4, TABS.y + 22, F_UIB, k ? C_FG : C_DIM,
       textdatum_t::middle_center);
  text(s, String("Arsenal  ") + packs.size(), TABS.x + TABS.w * 3 / 4, TABS.y + 22, F_UIB, k ? C_DIM : C_FG,
       textdatum_t::middle_center);

  if (k) {  // one chip per state with its count
    int n[ST_N] = {0};
    for (int i : kit) n[packs[i].state]++;
    for (int st = 0; st < ST_N; st++) {
      int cx = 12 + st * 76;
      rrect(s, cx, 58, 68, 32, 16, C_CARD);
      drawBattery(s, cx + 10, 66, 26, 16, 2, n[st] ? st : ST_NONE);
      text(s, String(n[st]), cx + 46, 75, F_H, n[st] ? ST_COL[st] : C_FAINT, textdatum_t::middle_left);
    }
  } else {
    text(s, String(kit.size()) + " of " + packs.size() + " in your kit", 16, 75, F_UI, C_DIM);
    bool p = pressed == ID_NEW;
    rrect(s, BTN_NEW.x, BTN_NEW.y, BTN_NEW.w, BTN_NEW.h, 16, p ? C_ACCENT_DK : C_ACCENT);
    int px = BTN_NEW.x + 20, py = BTN_NEW.y + BTN_NEW.h / 2;
    s.fillRect(px - 6, py - 1, 13, 3, col(C_FG));
    s.fillRect(px - 1, py - 6, 3, 13, col(C_FG));
    text(s, "New", px + 14, py + 1, F_UIB, C_FG);
  }
}

static void drawCard(LGFX_Sprite& s, int i, int x, int y) {
  const Pack& p = packs[kit[i]];
  bool dead = p.state == ST_DEAD;
  uint32_t bg = pressed == ID_ITEM + i ? C_CARD_HI : dead ? C_CARD_DIM : C_CARD;
  rrect(s, x, y, CARD_W, CARD_H, 14, bg);
  // row 1: name + cell count
  text(s, String(p.cells) + "S", x + CARD_W - 12, y + 21, F_H, dead ? C_DIM : C_FG, textdatum_t::middle_right);
  s.setFont(F_UIB);
  text(s, fitText(s, p.name, CARD_W - 24 - 34), x + 12, y + 20, F_UIB, dead ? C_DIM : C_FG);
  // row 2: capacity + HV
  text(s, String(p.mah) + " mAh", x + 12, y + 43, F_UI, C_DIM);
  if (p.hv) drawHV(s, x + CARD_W - 12 - 42, y + 34, 18);
  // battery
  drawBattery(s, x + 12, y + 56, CARD_W - 24, 34, p.cells, p.state);
  // state
  s.fillSmoothCircle(x + 16, y + 108, 4, col(ST_COL[p.state]));
  text(s, ST_NAME[p.state], x + 26, y + 108, F_UIB, ST_COL[p.state]);
}

static void drawKitFooter(LGFX_Sprite& s, int fy) {
  for (int which = 0; which < 2; which++) {
    Rect r = kitFootRect(which);
    bool p = pressed == (which ? ID_FOOT_STO : ID_FOOT_CHG);
    uint32_t c = which ? C_STO : C_CHG;
    rrect(s, r.x, fy, r.w, r.h, r.h / 2, p ? C_KEY_HI : C_LINE);
    rrect(s, r.x + 1, fy + 1, r.w - 2, r.h - 2, r.h / 2 - 1, p ? C_KEY_HI : C_BG);
    s.fillSmoothCircle(r.x + 22, fy + r.h / 2, 5, col(c));
    text(s, which ? "All storage" : "All charged", r.x + 36, fy + r.h / 2 + 1, F_UIB, C_DIM);
  }
}

static void drawKitEmpty(LGFX_Sprite& s, int off) {
  drawBattery(s, 105, 160 - off, 110, 48, 4, ST_NONE);
  text(s, "Your kit is empty", SCR_W / 2, 250 - off, F_H, C_FG, textdatum_t::middle_center);
  text(s, "Pick the packs you're bringing", SCR_W / 2, 282 - off, F_UI, C_DIM, textdatum_t::middle_center);
  text(s, "from your arsenal.", SCR_W / 2, 304 - off, F_UI, C_DIM, textdatum_t::middle_center);
  bool p = pressed == ID_EMPTY_GO;
  rrect(s, EMPTY_GO.x, EMPTY_GO.y - off, EMPTY_GO.w, EMPTY_GO.h, EMPTY_GO.h / 2, p ? C_ACCENT_DK : C_ACCENT);
  text(s, "Open Arsenal", SCR_W / 2, EMPTY_GO.y + EMPTY_GO.h / 2 + 1 - off, F_UIB, C_FG, textdatum_t::middle_center);
}

static void drawArsRow(LGFX_Sprite& s, int i, int y) {
  const Pack& p = packs[i];
  uint32_t bg = pressed == ID_ITEM + i ? C_CARD_HI : p.inKit ? C_CARD : C_CARD_DIM;
  rrect(s, 10, y, 300, ROW_H, 14, bg);
  drawBattery(s, 22, y + 19, 52, 26, p.cells, p.state);
  s.setFont(F_UIB);
  text(s, fitText(s, p.name, 170), 88, y + 23, F_UIB, p.inKit ? C_FG : C_DIM);
  String sub = String(p.cells) + "S   " + p.mah + " mAh";
  text(s, sub, 88, y + 45, F_UI, C_DIM);
  if (p.hv) {
    s.setFont(F_UI);
    drawHV(s, 88 + s.textWidth(sub) + 10, y + 37, 16);
  }
  drawCheck(s, 10 + 300 - 30, y + ROW_H / 2, p.inKit, bg);
}

static void drawScrollbar(LGFX_Sprite& s, Screen w, int k) {
  int ms = maxScroll(w);
  if (ms <= 0) return;
  int ch = w == S_ARS ? arsContentH() : kitContentH();
  int th = max(28, LIST_HT * LIST_HT / ch);
  int ty = (int)((LIST_HT - th) * scrollRef(w) / ms) - (k - 1) * BAND_H;
  rrect(s, SCR_W - 5, ty, 3, th, 1, C_FAINT);
}

static void drawMainBand(LGFX_Sprite& s, int k, Screen w) {
  s.fillScreen(col(C_BG));
  if (k == 0) { drawHeader(s, w); return; }
  int y0 = (int)scrollRef(w) + (k - 1) * BAND_H;  // content y at the band's top
  if (w == S_KIT) {
    if (kit.empty()) { drawKitEmpty(s, k * BAND_H); return; }
    for (int i = 0; i < (int)kit.size(); i++) {
      Rect r = kitCardRect(i);
      int ry = r.y - y0;
      if (ry < BAND_H && ry + r.h > 0) drawCard(s, i, r.x, ry);
    }
    int fy = kitFooterY() - y0;
    if (fy < BAND_H && fy + FOOT_H > 0) drawKitFooter(s, fy);
  } else {
    if (packs.empty()) {
      text(s, "No packs yet", SCR_W / 2, 220 - k * BAND_H, F_H, C_FG, textdatum_t::middle_center);
      text(s, "Tap + New to add one", SCR_W / 2, 250 - k * BAND_H, F_UI, C_DIM, textdatum_t::middle_center);
      return;
    }
    for (int i = 0; i < (int)packs.size(); i++) {
      Rect r = arsRowRect(i);
      int ry = r.y - y0;
      if (ry >= BAND_H) break;
      if (ry + r.h > 0) drawArsRow(s, i, ry);
    }
    int hy = arsRowRect(packs.size()).y + 12 - y0;
    if (hy > -20 && hy < BAND_H + 20)
      text(s, "Tap to pack it, hold to edit", SCR_W / 2, hy, F_UI, C_FAINT, textdatum_t::middle_center);
  }
  drawScrollbar(s, w, k);
}

// ---------------------------------------------------------------- drawing: sheets
static void sheetCard(LGFX_Sprite& s, int top, int off) {
  gDim = true;
  s.fillScreen(col(C_BG));
  gDim = false;
  rrect(s, 0, top - off, SCR_W, SCR_H - top + 30, 22, C_CARD);
  rrect(s, SCR_W / 2 - 20, top + 10 - off, 40, 5, 2, C_LINE);
}

static void drawStateSheet(LGFX_Sprite& s, int off) {
  sheetCard(s, STATE_Y, off);
  const Pack& p = packs[sheetPack];
  int Y = STATE_Y - off;
  drawBattery(s, 20, Y + 30, 64, 32, p.cells, p.state);
  s.setFont(F_H);
  text(s, fitText(s, p.name, SCR_W - 100 - 16), 100, Y + 38, F_H, C_FG);
  String sub = String(p.cells) + "S   " + p.mah + " mAh";
  text(s, sub, 100, Y + 63, F_UI, C_DIM);
  if (p.hv) { s.setFont(F_UI); drawHV(s, 100 + s.textWidth(sub) + 10, Y + 55, 16); }

  for (int st = 0; st < ST_N; st++) {
    Rect r = stateBtn(st);
    int ry = r.y - off;
    bool cur = st == p.state, pr = pressed == ID_ST0 + st;
    if (cur) {
      rrect(s, r.x, ry, r.w, r.h, 16, ST_COL[st]);
      rrect(s, r.x + 3, ry + 3, r.w - 6, r.h - 6, 13, pr ? C_KEY_HI : C_KEY_SP);
    } else {
      rrect(s, r.x, ry, r.w, r.h, 16, pr ? C_KEY_HI : C_KEY_SP);
    }
    drawBattery(s, r.x + 12, ry + 12, 54, 26, p.cells, st);
    String v = st == ST_DEAD ? String("<") + String(3.0f * p.cells, 1) + "V" : packV(st, p);
    text(s, v, r.x + r.w - 12, ry + 25, F_UI, C_DIM, textdatum_t::middle_right);
    text(s, ST_NAME[st], r.x + 12, ry + 57, F_H, ST_COL[st]);
  }
  bool pr = pressed == ID_REMOVE;
  rrect(s, BTN_REMOVE.x, BTN_REMOVE.y - off, BTN_REMOVE.w, BTN_REMOVE.h, 17, pr ? C_KEY_HI : C_LINE);
  rrect(s, BTN_REMOVE.x + 1, BTN_REMOVE.y + 1 - off, BTN_REMOVE.w - 2, BTN_REMOVE.h - 2, 16, pr ? C_KEY_HI : C_CARD);
  text(s, "Remove from kit", SCR_W / 2, BTN_REMOVE.y + BTN_REMOVE.h / 2 + 1 - off, F_UI, C_DIM,
       textdatum_t::middle_center);
}

static int bulkCount(uint8_t to) {
  int n = 0;
  for (int i : kit) n += packs[i].state != ST_DEAD && packs[i].state != to;
  return n;
}

static void drawConfirm(LGFX_Sprite& s, int off) {
  sheetCard(s, CF_Y, off);
  int Y = CF_Y - off;
  String title, l1, l2, ok;
  uint32_t okc;
  if (cfKind == CF_DELETE) {
    title = "Delete " + ed.name + "?";
    l1 = "It will be removed from";
    l2 = "your arsenal for good.";
    ok = "Delete";
    okc = C_DEAD;
  } else {
    bool chg = cfKind == CF_ALL_CHG;
    int n = bulkCount(chg ? ST_CHG : ST_STO);
    title = chg ? "Mark all charged?" : "Put all in storage?";
    l1 = String(n) + (n == 1 ? " pack" : " packs") + " will be set to " + (chg ? "Charged." : "Storage.");
    l2 = "Dead packs stay dead.";
    ok = chg ? "Charged" : "Storage";
    okc = chg ? C_CHG : C_STO;
  }
  s.setFont(F_H);
  text(s, fitText(s, title, SCR_W - 32), SCR_W / 2, Y + 44, F_H, C_FG, textdatum_t::middle_center);
  text(s, l1, SCR_W / 2, Y + 76, F_UI, C_DIM, textdatum_t::middle_center);
  text(s, l2, SCR_W / 2, Y + 98, F_UI, C_DIM, textdatum_t::middle_center);
  rrect(s, CF_CANCEL.x, CF_CANCEL.y - off, CF_CANCEL.w, CF_CANCEL.h, 16, pressed == ID_CANCEL ? C_KEY_HI : C_KEY_SP);
  text(s, "Cancel", CF_CANCEL.x + CF_CANCEL.w / 2, CF_CANCEL.y + CF_CANCEL.h / 2 + 1 - off, F_UIB, C_FG,
       textdatum_t::middle_center);
  rrect(s, CF_OK.x, CF_OK.y - off, CF_OK.w, CF_OK.h, 16, pressed == ID_OK ? C_KEY_HI : okc);
  text(s, ok, CF_OK.x + CF_OK.w / 2, CF_OK.y + CF_OK.h / 2 + 1 - off, F_UIB, C_BG, textdatum_t::middle_center);
}

// ---------------------------------------------------------------- drawing: editor
static void editButton(LGFX_Sprite& s, const Rect& r, int off, bool sel, bool pr, uint32_t selCol) {
  uint32_t bg = pr ? C_KEY_HI : sel ? selCol : C_KEY;
  rrect(s, r.x, r.y - off, r.w, r.h, 12, bg);
}

static void drawEditBand(LGFX_Sprite& s, int off) {
  s.fillScreen(col(C_BG));
  text(s, edIndex >= 0 ? "Edit pack" : "New pack", 16, 32 - off, F_TITLE, C_FG);

  // name
  bool pn = pressed == ID_NAME;
  rrect(s, E_NAME.x, E_NAME.y - off, E_NAME.w, E_NAME.h, 14, pn ? C_CARD_HI : C_CARD);
  int ny = E_NAME.y + E_NAME.h / 2 - off;
  if (ed.name.length()) text(s, ed.name, E_NAME.x + 16, ny + 1, F_H, C_FG);
  else text(s, "Tap to name it", E_NAME.x + 16, ny + 1, F_H, C_FAINT);
  int px = E_NAME.x + E_NAME.w - 26;  // pencil
  s.drawWideLine(px - 7, ny + 7, px + 5, ny - 5, 2.2f, col(C_DIM));
  s.fillTriangle(px - 10, ny + 10, px - 9, ny + 5, px - 5, ny + 9, col(C_DIM));

  // preview
  uint8_t st = edIndex >= 0 ? ed.state : ST_CHG;
  int bx = 60, by = 128;
  drawBattery(s, bx, by - off, 200, 62, ed.cells, st);
  if (ed.hv) drawHV(s, bx + 200 - 12 - 50 - 8, by - 11 - off, 22);
  String info = String(ed.cells) + "S " + ed.mah + " mAh" + (ed.hv ? " HV" : "") + "   " +
                String(cellV(ST_CHG, ed.hv) * ed.cells, 1) + "V full";
  text(s, info, SCR_W / 2, 212 - off, F_UI, C_DIM, textdatum_t::middle_center);

  // cells
  text(s, "CELLS", 16, 236 - off, F_UI, C_FAINT);
  for (int n = 1; n <= 8; n++) {
    Rect r = cellRect(n);
    bool sel = ed.cells == n;
    editButton(s, r, off, sel, pressed == ID_CELL1 + n - 1, C_ACCENT);
    text(s, String(n) + "S", r.x + r.w / 2, r.y + r.h / 2 + 1 - off, F_UIB, sel ? C_FG : C_DIM,
         textdatum_t::middle_center);
  }

  // chemistry
  editButton(s, E_STD, off, !ed.hv, pressed == ID_STD, C_FG);
  text(s, "Standard 4.20V", E_STD.x + E_STD.w / 2, E_STD.y + E_STD.h / 2 + 1 - off, F_UIB, ed.hv ? C_DIM : C_BG,
       textdatum_t::middle_center);
  editButton(s, E_HVB, off, ed.hv, pressed == ID_HV, C_HV);
  text(s, "HV 4.35V", E_HVB.x + E_HVB.w / 2, E_HVB.y + E_HVB.h / 2 + 1 - off, F_UIB, ed.hv ? C_HV_INK : C_DIM,
       textdatum_t::middle_center);

  // capacity
  for (int k = 0; k < 2; k++) {
    const Rect& r = k ? E_PLUS : E_MINUS;
    editButton(s, r, off, false, pressed == (k ? ID_PLUS : ID_MINUS), 0);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2 - off;
    s.fillRect(cx - 8, cy - 1, 17, 3, col(C_FG));
    if (k) s.fillRect(cx - 1, cy - 8, 3, 17, col(C_FG));
  }
  text(s, String(ed.mah), SCR_W / 2 - 4, E_MINUS.y + E_MINUS.h / 2 + 1 - off, F_TITLE, C_FG,
       textdatum_t::middle_right);
  text(s, "mAh", SCR_W / 2 + 2, E_MINUS.y + E_MINUS.h / 2 + 3 - off, F_UI, C_DIM);

  // actions
  if (edIndex >= 0) {
    Rect r = E_DEL();
    editButton(s, r, off, false, pressed == ID_DEL, 0);
    text(s, "Delete", r.x + r.w / 2, r.y + r.h / 2 + 1 - off, F_UIB, C_DEAD, textdatum_t::middle_center);
  }
  Rect rc = E_CANCEL(), rs = E_SAVE();
  rrect(s, rc.x, rc.y - off, rc.w, rc.h, 14, pressed == ID_ECANCEL ? C_KEY_HI : C_KEY_SP);
  text(s, "Cancel", rc.x + rc.w / 2, rc.y + rc.h / 2 + 1 - off, F_UIB, C_FG, textdatum_t::middle_center);
  rrect(s, rs.x, rs.y - off, rs.w, rs.h, 14, pressed == ID_SAVE ? C_ACCENT_DK : C_ACCENT);
  text(s, edIndex >= 0 ? "Save" : "Add", rs.x + rs.w / 2, rs.y + rs.h / 2 + 1 - off, F_UIB, C_FG,
       textdatum_t::middle_center);
}

// ---------------------------------------------------------------- keyboard (pack name)
enum KeyType : uint8_t { K_CHAR, K_SHIFT, K_BACK, K_SYM, K_SPACE, K_DONE, K_CANCEL };
struct Key { int16_t x, y, w, h; KeyType type; char ch; };
static std::vector<Key> keys;
static String kbText;
static bool kbShift = false, kbCaps = false, kbSym = false;
static int pressedKey = -1;
static uint32_t lastShiftTap = 0, cursorBlinkAt = 0, backRepeatAt = 0;
static bool cursorOn = true;

static const int FIELD_H = 2 * BAND_H;  // 192
static const int KB_Y = 240, KEY_H = 54, KEY_ROW = 59, KEY_GAP = 4, KB_M = 3;
static const float UNIT = (SCR_W - 2 * KB_M - 9.0f * KEY_GAP) / 10.0f;

static Key mk(KeyType t, char c = 0) { return Key{0, 0, 0, 0, t, c}; }

static void addRow(int row, std::initializer_list<std::pair<float, Key>> items) {
  float total = 0;
  for (auto& it : items) total += it.first;
  float x = KB_M + (10 - total) * (UNIT + KEY_GAP) / 2;
  for (auto& it : items) {
    Key k = it.second;
    float w = it.first * UNIT + (it.first - 1) * KEY_GAP;
    k.x = (int16_t)roundf(x);
    k.w = (int16_t)roundf(x + w) - k.x;
    k.y = KB_Y + row * KEY_ROW;
    k.h = KEY_H;
    keys.push_back(k);
    x += w + KEY_GAP;
  }
}

static void addCharRow(int row, const char* chars) {
  float x = KB_M;
  for (int i = 0; chars[i]; i++) {
    Key k = mk(K_CHAR, chars[i]);
    k.x = (int16_t)roundf(x);
    k.w = (int16_t)roundf(x + UNIT) - k.x;
    k.y = KB_Y + row * KEY_ROW;
    k.h = KEY_H;
    keys.push_back(k);
    x += UNIT + KEY_GAP;
  }
}

static void buildKeys() {
  keys.clear();
  addCharRow(0, kbSym ? "1234567890" : "qwertyuiop");
  addCharRow(1, kbSym ? "-/:;()&@#'" : "asdfghjkl-");
  if (kbSym) {
    addRow(2, {{1, mk(K_CHAR, '.')}, {1, mk(K_CHAR, ',')}, {1, mk(K_CHAR, '?')}, {1, mk(K_CHAR, '!')},
               {1, mk(K_CHAR, '_')}, {1, mk(K_CHAR, '%')}, {1, mk(K_CHAR, '+')}, {1, mk(K_CHAR, '=')},
               {2, mk(K_BACK)}});
  } else {
    addRow(2, {{1.5f, mk(K_SHIFT)}, {1, mk(K_CHAR, 'z')}, {1, mk(K_CHAR, 'x')}, {1, mk(K_CHAR, 'c')},
               {1, mk(K_CHAR, 'v')}, {1, mk(K_CHAR, 'b')}, {1, mk(K_CHAR, 'n')}, {1, mk(K_CHAR, 'm')},
               {1.5f, mk(K_BACK)}});
  }
  addRow(3, {{1, mk(K_CANCEL)}, {1.5f, mk(K_SYM)}, {5, mk(K_SPACE)}, {2.5f, mk(K_DONE)}});
}

static bool upperNow() { return kbShift || kbCaps; }

static void drawKey(int i) {
  const Key& k = keys[i];
  bool pr = i == pressedKey;
  if (keySpr.width() != k.w || keySpr.height() != k.h) {
    keySpr.deleteSprite();
    keySpr.createSprite(k.w, k.h);
  }
  auto& s = keySpr;
  uint32_t bg;
  switch (k.type) {
    case K_CHAR: case K_SPACE: bg = C_KEY; break;
    case K_DONE: bg = C_ACCENT; break;
    default: bg = C_KEY_SP;
  }
  if (k.type == K_SHIFT && upperNow()) bg = C_FG;
  if (pr) bg = k.type == K_DONE ? C_ACCENT_DK : C_KEY_HI;
  s.fillScreen(C_BG);
  s.fillSmoothRoundRect(0, 0, k.w, k.h, 8, bg);
  int cx = k.w / 2, cy = k.h / 2;
  s.setTextDatum(textdatum_t::middle_center);
  s.setTextColor(C_FG);
  switch (k.type) {
    case K_CHAR: {
      char c = k.ch;
      if (upperNow() && c >= 'a' && c <= 'z') c -= 32;
      char str[2] = {c, 0};
      s.setFont(F_KEY);
      s.drawString(str, cx, cy + (c >= 'a' && c <= 'z' ? -2 : 1));
      break;
    }
    case K_SHIFT: {
      uint32_t fc = upperNow() ? C_BG : C_FG;
      s.fillTriangle(cx, cy - 11, cx - 10, cy, cx + 10, cy, fc);
      s.fillRect(cx - 4, cy, 9, 8, fc);
      if (kbCaps) s.fillRect(cx - 6, cy + 11, 13, 3, fc);
      break;
    }
    case K_BACK: {
      int l = cx - 13, r = cx + 11, t = cy - 9, b = cy + 9;
      s.drawWideLine(l, cy, l + 8, t, 1.0f, C_FG);
      s.drawWideLine(l, cy, l + 8, b, 1.0f, C_FG);
      s.drawWideLine(l + 8, t, r, t, 1.0f, C_FG);
      s.drawWideLine(l + 8, b, r, b, 1.0f, C_FG);
      s.drawWideLine(r, t, r, b, 1.0f, C_FG);
      s.drawWideLine(cx - 1, cy - 4, cx + 6, cy + 4, 1.0f, C_FG);
      s.drawWideLine(cx - 1, cy + 4, cx + 6, cy - 4, 1.0f, C_FG);
      break;
    }
    case K_SYM:
      s.setFont(F_UIB);
      s.drawString(kbSym ? "ABC" : "123", cx, cy + 1);
      break;
    case K_SPACE:
      s.setFont(F_UI);
      s.setTextColor(C_DIM);
      s.drawString("space", cx, cy);
      break;
    case K_DONE:
      s.setFont(&fonts::FreeSansBold12pt7b);
      s.drawString("OK", cx, cy);
      break;
    case K_CANCEL:
      s.drawWideLine(cx - 6, cy - 6, cx + 6, cy + 6, 1.2f, C_DIM);
      s.drawWideLine(cx - 6, cy + 6, cx + 6, cy - 6, 1.2f, C_DIM);
      break;
  }
  s.pushSprite(k.x, k.y);
}

static void drawKeysOfType(KeyType t) {
  for (int i = 0; i < (int)keys.size(); i++) if (keys[i].type == t) drawKey(i);
}

static void drawKeyboard() {
  tft.fillRect(0, FIELD_H, SCR_W, SCR_H - FIELD_H, C_BG);
  for (int i = 0; i < (int)keys.size(); i++) drawKey(i);
}

static const Rect BOX = {10, 44, SCR_W - 20, FIELD_H - 44 - 8};

static void drawFieldBand(LGFX_Sprite& s, int off) {
  s.fillScreen(C_BG);
  text(s, "PACK NAME", 16, 24 - off, F_UIB, C_DIM);
  text(s, String(kbText.length()) + "/" + MAX_NAME, SCR_W - 16, 24 - off, F_UI, C_FAINT, textdatum_t::middle_right);
  rrect(s, BOX.x, BOX.y - off, BOX.w, BOX.h, 16, C_ACCENT);
  rrect(s, BOX.x + 2, BOX.y + 2 - off, BOX.w - 4, BOX.h - 4, 14, C_CARD);
  int cy = BOX.y + BOX.h / 2 - off, tx = BOX.x + 18;
  const lgfx::IFont* f = F_TITLE;
  s.setFont(f);
  if (s.textWidth(kbText) > BOX.w - 44) f = F_H;
  int curX = tx;
  if (kbText.length()) {
    text(s, kbText, tx, cy + 2, f, C_FG);
    curX = tx + s.textWidth(kbText) + 2;
  } else {
    text(s, "e.g. Race A", tx + 6, cy + 2, f, C_FAINT);
  }
  if (cursorOn) s.fillRect(curX, cy - 16, 3, 32, C_ACCENT);
}

static void drawField() {
  for (int k = 0; k < FIELD_H / BAND_H; k++) {
    drawFieldBand(band, k * BAND_H);
    band.pushSprite(0, k * BAND_H);
  }
}

static void kbRefresh() {
  cursorOn = true;
  cursorBlinkAt = millis() + 530;
  drawField();
}

static void autoShift() {
  bool want = !kbSym && kbText.length() == 0;
  if (want != kbShift && !kbCaps) {
    kbShift = want;
    drawKeysOfType(K_SHIFT);
    drawKeysOfType(K_CHAR);
  }
}

// ---------------------------------------------------------------- rendering dispatch
static bool composeBand(int k) {
  auto& s = band;
  int off = k * BAND_H;
  switch (screen) {
    case S_KIT: case S_ARS: drawMainBand(s, k, screen); break;
    case S_EDIT: drawEditBand(s, off); break;
    case S_STATE: case S_CONFIRM: {
      int top = screen == S_STATE ? STATE_Y : CF_Y;
      if (off >= top) {
        if (screen == S_STATE) drawStateSheet(s, off);
        else drawConfirm(s, off);
      } else {
        gDim = true;
        if (baseScreen == S_EDIT) drawEditBand(s, off);
        else drawMainBand(s, k, baseScreen);
        gDim = false;
      }
      break;
    }
    case S_KB:
      if (off < FIELD_H) drawFieldBand(s, off);
      else return false;
      break;
  }
  return true;
}

static void renderBand(int k) {
  if (composeBand(k)) band.pushSprite(0, k * BAND_H);
}

static void renderRange(int y0, int y1) {
  y0 = max(0, y0);
  y1 = min(SCR_H, y1);
  for (int k = y0 / BAND_H; k < NBANDS && k * BAND_H < y1; k++) renderBand(k);
}
static void renderAll() { renderRange(0, SCR_H); }

static void goScreen(Screen s) {
  screen = s;
  pressed = ID_NONE;
  if (s == S_KIT || s == S_ARS) clampScroll(s);
  scrollVel = 0;
  renderAll();
}

// Screen rect of a pressable element (for partial redraws).
static bool idRect(int id, Rect& r) {
  if (id < 0) return false;
  if (screen == S_KIT || screen == S_ARS) {
    if (id < ID_ST0 && id != ID_FOOT_CHG && id != ID_FOOT_STO && id != ID_EMPTY_GO) { r = {0, 0, SCR_W, BAND_H}; return true; }
    if (id == ID_EMPTY_GO) { r = EMPTY_GO; return true; }
    float sc = scrollRef(screen);
    if (id == ID_FOOT_CHG || id == ID_FOOT_STO) r = kitFootRect(id == ID_FOOT_STO);
    else if (id >= ID_ITEM) r = screen == S_KIT ? kitCardRect(id - ID_ITEM) : arsRowRect(id - ID_ITEM);
    else return false;
    r.y += LIST_Y - (int)sc;
    if (r.y < LIST_Y) { r.h -= LIST_Y - r.y; r.y = LIST_Y; }
    return r.h > 0;
  }
  if (screen == S_STATE) {
    if (id >= ID_ST0 && id <= ID_ST3) { r = stateBtn(id - ID_ST0); return true; }
    if (id == ID_REMOVE) { r = BTN_REMOVE; return true; }
    return false;
  }
  if (screen == S_CONFIRM) {
    if (id == ID_OK) { r = CF_OK; return true; }
    if (id == ID_CANCEL) { r = CF_CANCEL; return true; }
    return false;
  }
  if (screen == S_EDIT) {
    if (id == ID_NAME) r = E_NAME;
    else if (id >= ID_CELL1 && id < ID_CELL1 + 8) r = cellRect(id - ID_CELL1 + 1);
    else if (id == ID_STD) r = E_STD;
    else if (id == ID_HV) r = E_HVB;
    else if (id == ID_MINUS) r = E_MINUS;
    else if (id == ID_PLUS) r = E_PLUS;
    else if (id == ID_DEL) r = E_DEL();
    else if (id == ID_ECANCEL) r = E_CANCEL();
    else if (id == ID_SAVE) r = E_SAVE();
    else return false;
    return true;
  }
  return false;
}

static void refreshId(int id) {
  Rect r;
  if (idRect(id, r)) renderRange(r.y, r.y + r.h);
}

static void setPressed(int id) {
  if (id == pressed) return;
  int old = pressed;
  pressed = id;
  refreshId(old);
  refreshId(id);
}

static int hitTest(int x, int y) {
  switch (screen) {
    case S_KIT: case S_ARS: {
      if (y < LIST_Y) {
        if (TABS.hit(x, y, 6)) return x < SCR_W / 2 ? ID_TAB_KIT : ID_TAB_ARS;
        if (screen == S_ARS && BTN_NEW.hit(x, y, 8)) return ID_NEW;
        return ID_NONE;
      }
      if (screen == S_KIT && kit.empty()) return EMPTY_GO.hit(x, y, 8) ? ID_EMPTY_GO : ID_NONE;
      int cy = y - LIST_Y + (int)scrollRef(screen);
      if (screen == S_KIT) {
        for (int i = 0; i < (int)kit.size(); i++)
          if (kitCardRect(i).hit(x, cy, 4)) return ID_ITEM + i;
        if (kitFootRect(0).hit(x, cy, 4)) return ID_FOOT_CHG;
        if (kitFootRect(1).hit(x, cy, 4)) return ID_FOOT_STO;
      } else {
        for (int i = 0; i < (int)packs.size(); i++)
          if (arsRowRect(i).hit(x, cy, 3)) return ID_ITEM + i;
      }
      return ID_NONE;
    }
    case S_STATE:
      if (y < STATE_Y) return ID_BG;
      for (int st = 0; st < ST_N; st++) if (stateBtn(st).hit(x, y, 3)) return ID_ST0 + st;
      if (BTN_REMOVE.hit(x, y, 6)) return ID_REMOVE;
      return ID_NONE;
    case S_CONFIRM:
      if (y < CF_Y) return ID_BG;
      if (CF_OK.hit(x, y, 6)) return ID_OK;
      if (CF_CANCEL.hit(x, y, 6)) return ID_CANCEL;
      return ID_NONE;
    case S_EDIT:
      if (E_NAME.hit(x, y, 4)) return ID_NAME;
      for (int n = 1; n <= 8; n++) if (cellRect(n).hit(x, y, 2)) return ID_CELL1 + n - 1;
      if (E_STD.hit(x, y, 2)) return ID_STD;
      if (E_HVB.hit(x, y, 2)) return ID_HV;
      if (E_MINUS.hit(x, y, 6)) return ID_MINUS;
      if (E_PLUS.hit(x, y, 6)) return ID_PLUS;
      if (edIndex >= 0 && E_DEL().hit(x, y, 2)) return ID_DEL;
      if (E_CANCEL().hit(x, y, 2)) return ID_ECANCEL;
      if (E_SAVE().hit(x, y, 2)) return ID_SAVE;
      return ID_NONE;
    default:
      return ID_NONE;
  }
}

// ---------------------------------------------------------------- actions
static void openEditor(int index) {
  edIndex = index;
  if (index >= 0) ed = packs[index];
  else ed = {"", 4, false, 1500, false, ST_CHG};
  goScreen(S_EDIT);
}

static void openConfirm(ConfirmKind k, Screen base) {
  cfKind = k;
  baseScreen = base;
  goScreen(S_CONFIRM);
}

static void stepCap(int dir) {
  int n = sizeof(CAPS) / sizeof(CAPS[0]);
  if (dir > 0) {
    for (int i = 0; i < n; i++) if (CAPS[i] > ed.mah) { ed.mah = CAPS[i]; return; }
  } else {
    for (int i = n - 1; i >= 0; i--) if (CAPS[i] < ed.mah) { ed.mah = CAPS[i]; return; }
  }
}

static void saveEditor() {
  ed.name.trim();
  if (!ed.name.length()) {  // auto-name: "4S #3"
    int n = 1;
    for (auto& p : packs) n += p.cells == ed.cells;
    ed.name = String(ed.cells) + "S #" + n;
  }
  if (edIndex >= 0 && edIndex < (int)packs.size()) packs[edIndex] = ed;
  else if ((int)packs.size() < MAX_PACKS) packs.push_back(ed);
  savePacks();
  rebuildKit();
  if (edIndex < 0) scrollArs = 1e6;  // show the new pack (clamped)
  goScreen(S_ARS);
}

static void openKeyboard() {
  kbText = ed.name;
  kbSym = false;
  kbCaps = false;
  kbShift = kbText.length() == 0;
  pressedKey = -1;
  screen = S_KB;
  buildKeys();
  drawKeyboard();
  kbRefresh();
}

static void closeKeyboard(bool commit) {
  if (commit) {
    kbText.trim();
    ed.name = kbText;
  }
  pressedKey = -1;
  goScreen(S_EDIT);
}

static void typeChar(char c) {
  if ((int)kbText.length() >= MAX_NAME) return;
  if (upperNow() && c >= 'a' && c <= 'z') c -= 32;
  kbText += c;
  if (kbShift && !kbCaps) {
    kbShift = false;
    drawKeysOfType(K_SHIFT);
    drawKeysOfType(K_CHAR);
  }
  kbRefresh();
}

static void backspace() {
  if (!kbText.length()) return;
  kbText.remove(kbText.length() - 1);
  autoShift();
  kbRefresh();
}

static void keyAction(const Key& k) {
  switch (k.type) {
    case K_CHAR: typeChar(k.ch); break;
    case K_BACK: break;  // handled on press (with auto-repeat)
    case K_SHIFT: {
      uint32_t now = millis();
      if (kbCaps) { kbCaps = false; kbShift = false; }
      else if (kbShift && now - lastShiftTap < 400) kbCaps = true;
      else kbShift = !kbShift;
      lastShiftTap = now;
      drawKeysOfType(K_CHAR);
      drawKeysOfType(K_SHIFT);
      break;
    }
    case K_SYM:
      kbSym = !kbSym;
      if (kbSym) kbShift = false;
      buildKeys();
      drawKeyboard();
      autoShift();
      break;
    case K_SPACE:
      if (kbText.length() && (int)kbText.length() < MAX_NAME) kbText += ' ';
      kbRefresh();
      break;
    case K_DONE: closeKeyboard(true); break;
    case K_CANCEL: closeKeyboard(false); break;
  }
}

static int keyAt(int x, int y) {
  int best = -1, bestD = 1 << 30;
  for (int i = 0; i < (int)keys.size(); i++) {
    const Key& k = keys[i];
    if (y < k.y - KEY_GAP || y >= k.y + k.h + KEY_GAP) continue;
    int dx = x < k.x ? k.x - x : x >= k.x + k.w ? x - (k.x + k.w) + 1 : 0;
    int dy = y < k.y ? k.y - y : y >= k.y + k.h ? y - (k.y + k.h) + 1 : 0;
    int d = dx * dx + dy * dy;
    if (d < bestD) { bestD = d; best = i; }
  }
  return bestD <= 64 ? best : -1;
}

static void doAction(int id) {
  switch (screen) {
    case S_KIT: case S_ARS:
      if (id == ID_TAB_KIT) { if (screen != S_KIT) goScreen(S_KIT); return; }
      if (id == ID_TAB_ARS || id == ID_EMPTY_GO) { if (screen != S_ARS) goScreen(S_ARS); return; }
      if (id == ID_NEW) { openEditor(-1); return; }
      if (id == ID_FOOT_CHG) { openConfirm(CF_ALL_CHG, S_KIT); return; }
      if (id == ID_FOOT_STO) { openConfirm(CF_ALL_STO, S_KIT); return; }
      if (id >= ID_ITEM && screen == S_KIT) {
        sheetPack = kit[id - ID_ITEM];
        baseScreen = S_KIT;
        goScreen(S_STATE);
        return;
      }
      if (id >= ID_ITEM && screen == S_ARS) {
        Pack& p = packs[id - ID_ITEM];
        p.inKit = !p.inKit;
        savePacks();
        rebuildKit();
        refreshId(id);
        renderRange(0, BAND_H);
        return;
      }
      return;
    case S_STATE:
      if (id >= ID_ST0 && id <= ID_ST3) {
        packs[sheetPack].state = id - ID_ST0;
        savePacks();
      } else if (id == ID_REMOVE) {
        packs[sheetPack].inKit = false;
        savePacks();
        rebuildKit();
      } else if (id != ID_BG) {
        return;
      }
      goScreen(S_KIT);
      return;
    case S_CONFIRM:
      if (id == ID_OK) {
        if (cfKind == CF_DELETE) {
          if (edIndex >= 0 && edIndex < (int)packs.size()) packs.erase(packs.begin() + edIndex);
          savePacks();
          rebuildKit();
          goScreen(S_ARS);
          return;
        }
        uint8_t to = cfKind == CF_ALL_CHG ? ST_CHG : ST_STO;
        for (int i : kit) if (packs[i].state != ST_DEAD) packs[i].state = to;
        savePacks();
        goScreen(S_KIT);
        return;
      }
      if (id == ID_CANCEL || id == ID_BG) goScreen(baseScreen);
      return;
    case S_EDIT:
      if (id == ID_NAME) { openKeyboard(); return; }
      if (id >= ID_CELL1 && id < ID_CELL1 + 8) ed.cells = id - ID_CELL1 + 1;
      else if (id == ID_STD) ed.hv = false;
      else if (id == ID_HV) ed.hv = true;
      else if (id == ID_MINUS || id == ID_PLUS) { refreshId(id); return; }  // stepped on press
      else if (id == ID_DEL) { openConfirm(CF_DELETE, S_EDIT); return; }
      else if (id == ID_ECANCEL) { goScreen(S_ARS); return; }
      else if (id == ID_SAVE) { saveEditor(); return; }
      renderAll();
      return;
    default:
      return;
  }
}

// ---------------------------------------------------------------- touch handling
static bool tDown = false, dragging = false, consumed = false;
static int sx, sy, lx, ly;
static float startScroll;
static uint32_t tStart, lastMoveT, repeatAt;
static uint32_t lastActivity = 0;
static bool dimmed = false;

static bool isList() { return screen == S_KIT || screen == S_ARS; }

static void onDown(int x, int y) {
  sx = lx = x; sy = ly = y;
  tStart = lastMoveT = millis();
  dragging = consumed = false;
  if (screen == S_KB) {
    pressedKey = keyAt(x, y);
    if (pressedKey >= 0) {
      drawKey(pressedKey);
      if (keys[pressedKey].type == K_BACK) {
        backspace();
        backRepeatAt = millis() + 450;
      }
    }
    return;
  }
  scrollVel = 0;
  if (isList()) startScroll = scrollRef(screen);
  int id = hitTest(x, y);
  if (id == ID_BG) { pressed = id; return; }
  setPressed(id);
  if (id == ID_MINUS || id == ID_PLUS) {
    stepCap(id == ID_PLUS ? 1 : -1);
    renderAll();
    repeatAt = millis() + 450;
  }
}

static void onMove(int x, int y) {
  uint32_t now = millis();
  if (isList() && !consumed && sy >= LIST_Y) {
    if (!dragging && abs(y - sy) > 10) {
      dragging = true;
      setPressed(ID_NONE);
    }
    if (dragging) {
      float& sc = scrollRef(screen);
      float ns = constrain(startScroll - (y - sy), 0.0f, (float)maxScroll(screen));
      uint32_t dt = max<uint32_t>(1, now - lastMoveT);
      scrollVel = 0.6f * scrollVel + 0.4f * (-(float)(y - ly) * 16.0f / dt);
      if ((int)ns != (int)sc) { sc = ns; renderRange(LIST_Y, SCR_H); }
    }
  }
  lx = x; ly = y; lastMoveT = now;
}

static void onHold() {
  uint32_t now = millis(), held = now - tStart;
  if (screen == S_ARS && !dragging && !consumed && pressed >= ID_ITEM && held > 550) {
    consumed = true;
    openEditor(pressed - ID_ITEM);
    return;
  }
  if (screen == S_EDIT && (pressed == ID_MINUS || pressed == ID_PLUS) && now >= repeatAt) {
    stepCap(pressed == ID_PLUS ? 1 : -1);
    renderAll();
    repeatAt = now + 110;
  }
  if (screen == S_KB && pressedKey >= 0 && keys[pressedKey].type == K_BACK && now >= backRepeatAt) {
    backspace();
    backRepeatAt = now + 70;
  }
}

static void onUp() {
  if (consumed) { consumed = false; return; }
  if (screen == S_KB) {
    int k = pressedKey;
    pressedKey = -1;
    if (k >= 0) {
      drawKey(k);
      keyAction(keys[k]);
    }
    return;
  }
  if (dragging) {
    if (millis() - lastMoveT > 80) scrollVel = 0;
    dragging = false;
    return;
  }
  int id = pressed;
  pressed = ID_NONE;
  if (id == ID_NONE) return;
  int up = hitTest(lx, ly);
  if (up != id) { refreshId(id); return; }  // slid off
  doAction(id);
}

static int dnCount = 0, upCount = 0;

static void pollTouch() {
  int32_t x, y;
  bool raw = tft.getTouch(&x, &y);
  if (raw) {
    upCount = 0;
    lastActivity = millis();
    if (dimmed) {  // first touch only wakes the screen
      dimmed = false;
      tft.setBrightness(210);
      consumed = true;
      tDown = true;
      sx = lx = x; sy = ly = y;
      tStart = millis();
      return;
    }
    if (!tDown) {
      if (++dnCount >= 2) { tDown = true; onDown(x, y); }
    } else {
      int fx = (lx * 2 + x) / 3, fy = (ly * 2 + y) / 3;
      onMove(fx, fy);
      onHold();
    }
  } else {
    dnCount = 0;
    if (tDown && ++upCount >= 3) {
      tDown = false;
      onUp();
    }
  }
}

// ---------------------------------------------------------------- calibration
static const char* CAL_KEY = "cal_p";

static void runCalibration() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setFont(F_UIB);
  tft.setTextColor(C_FG);
  tft.drawString("Touch calibration", SCR_W / 2, SCR_H / 2 - 16);
  tft.setFont(F_UI);
  tft.setTextColor(C_DIM);
  tft.drawString("Tap the tip of each arrow", SCR_W / 2, SCR_H / 2 + 12);
  uint16_t cal[8];
  tft.calibrateTouch(cal, C_ACCENT, C_BG, 18);
  prefs.putBytes(CAL_KEY, cal, sizeof(cal));
  tft.fillScreen(C_BG);
}

static bool loadCalibration() {
  uint16_t cal[8];
  if (prefs.getBytes(CAL_KEY, cal, sizeof(cal)) == sizeof(cal)) { tft.setTouchCalibrate(cal); return true; }
  Preferences other;  // reuse the CYD Checklist's calibration if this board already has one
  if (other.begin("checklist", true)) {
    bool ok = other.getBytes(CAL_KEY, cal, sizeof(cal)) == sizeof(cal);
    other.end();
    if (ok) { prefs.putBytes(CAL_KEY, cal, sizeof(cal)); tft.setTouchCalibrate(cal); return true; }
  }
  return false;
}

// ---------------------------------------------------------------- serial (debug / remote)
static bool composeBand(int k);
static void dumpScreen() {
  static lgfx::rgb888_t line[SCR_W];
  Serial.printf("\nSHOT %d %d\n", SCR_W, SCR_H);
  for (int y = 0; y < SCR_H; y++) {
    if (y % BAND_H == 0 && !composeBand(y / BAND_H)) band.fillScreen(0x000000);
    band.readRect(0, y % BAND_H, SCR_W, 1, line);
    int i = 0;
    while (i < SCR_W) {
      int j = i + 1;
      while (j < SCR_W && j - i < 255 && line[j].r == line[i].r && line[j].g == line[i].g && line[j].b == line[i].b) j++;
      uint8_t rec[4] = {(uint8_t)(j - i), line[i].r, line[i].g, line[i].b};
      Serial.write(rec, 4);
      i = j;
    }
  }
  Serial.print("\nEND\n");
}

static void goScreen(Screen s);
static bool putMode = false;
static String putBuf;

// "get": print packs.txt between BEGIN/END.  "put": following lines up to "END" replace it.
static void handleCmd(const String& c) {
  int x, y;
  if (putMode) {
    if (c == "END") {
      putMode = false;
      File f = LittleFS.open("/packs.tmp", "w");
      if (f) { f.print(putBuf); f.close(); LittleFS.remove(PACK_FILE); LittleFS.rename("/packs.tmp", PACK_FILE); }
      putBuf = "";
      loadPacks();
      savePacks();  // normalise the formatting
      if (sheetPack >= (int)packs.size() || screen != S_ARS) screen = S_KIT;
      goScreen(screen == S_ARS ? S_ARS : S_KIT);
      Serial.printf("ok %d packs\n", (int)packs.size());
    } else if (putBuf.length() < 16000) {
      putBuf += c;
      putBuf += '\n';
    }
    return;
  }
  if (c == "ping") { Serial.println("pong"); return; }
  if (c == "get") {
    Serial.print("BEGIN\n");
    writePacks(Serial);
    Serial.print("END\n");
    return;
  }
  if (c == "put") { putMode = true; putBuf = ""; Serial.println("ready"); return; }
  if (c == "shot") { dumpScreen(); return; }
  if ((c[0] == 't' || c[0] == 'h') && sscanf(c.c_str() + 1, "%d %d", &x, &y) == 2) {
    lastActivity = millis();
    onDown(x, y);
    lx = x; ly = y;
    if (c[0] == 'h') { tStart = millis() - 1000; onHold(); }
    onUp();
    Serial.println("ok");
  }
}

static void pollSerial() {
  static String buf;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') {
      if (buf.length() || putMode) handleCmd(buf);
      buf = "";
    } else if (ch != '\r' && buf.length() < 160) {
      buf += ch;
    }
  }
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP);
  tft.init();
  tft.setRotation(0);  // portrait, USB at the bottom
  tft.setBrightness(210);
  tft.fillScreen(C_BG);
  band.setColorDepth(16);
  band.createSprite(SCR_W, BAND_H);
  keySpr.setColorDepth(16);

  prefs.begin("lipo", false);
  if (digitalRead(0) == LOW || !loadCalibration()) runCalibration();

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  loadPacks();
  goScreen(S_KIT);
  lastActivity = millis();
}

void loop() {
  static uint32_t nextFrame = 0, bootHeld = 0;
  uint32_t now = millis();
  pollSerial();
  if (now < nextFrame) { delay(1); return; }
  nextFrame = now + 14;

  pollTouch();

  if (isList() && !tDown && fabsf(scrollVel) > 0.4f) {  // fling
    float& sc = scrollRef(screen);
    float ns = constrain(sc + scrollVel, 0.0f, (float)maxScroll(screen));
    scrollVel *= 0.92f;
    if (ns == 0 || ns == maxScroll(screen)) scrollVel = 0;
    if ((int)ns != (int)sc) { sc = ns; renderRange(LIST_Y, SCR_H); }
  }

  if (screen == S_KB && now >= cursorBlinkAt) {
    cursorOn = !cursorOn;
    cursorBlinkAt = now + 530;
    drawField();
  }

  if (!dimmed && now - lastActivity > 120000) {
    dimmed = true;
    tft.setBrightness(25);
  }

  if (digitalRead(0) == LOW) {  // hold BOOT 2 s: recalibrate touch
    if (!bootHeld) bootHeld = now;
    else if (now - bootHeld > 2000) {
      bootHeld = 0;
      runCalibration();
      goScreen(screen == S_KB ? S_EDIT : screen);
      lastActivity = millis();
    }
  } else {
    bootHeld = 0;
  }
}
