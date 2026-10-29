/*
 * esp32-c3-hid-agent — 让一台 ESP32-C3 变成 AI agent 的手
 *
 * 板子连上家里的 WiFi，打开一条 WebSocket 到你的服务器（跑 server.py 的那台），
 * 把服务器下发的 JSON 命令变成真实的 BLE HID 键盘 / 鼠标报告。
 * 配对过的 Android 手机把它当作一把真正的蓝牙键盘鼠标，输入走的是
 * 和物理键盘完全相同的驱动路径 —— 不是注入的虚拟事件。
 *
 * 手机和服务器之间不需要 USB 线。
 *
 * ---------------------------------------------------------------------------
 * 工具链（在真实 ESP32-C3 devkit 上验证通过的组合）
 * ---------------------------------------------------------------------------
 *   - Arduino ESP32 core 2.0.17
 *     为什么锁 2.x：core 3.x 改了 BLE HID 那套 API。换 3.x 不是不行，
 *     但要把 HID 层改写成 IDF 原生实现 —— 见 docs/PORTING.md。
 *   - WebSockets by Markus Sattler >= 2.7   （库管理器搜 "WebSockets"）
 *   - ESP32 BLE HID by Autfy/0cococ       （源码安装，见 README）
 *     提供 BleComboKeyboard + BleComboMouse：复合 HID，一个设备里同时有
 *     键盘、媒体键、绝对坐标 digitizer 鼠标、相对鼠标。C3 上已验证，
 *     BLE+WiFi 共存也由该库自带示例验证过。
 *
 *   注意：core 2.0.17 自带的 bootloader 在部分 C3 上会崩溃循环。
 *   README「烧录」一节给了可靠配方（IDF bootloader + esptool 手动烧）。
 *
 * ---------------------------------------------------------------------------
 * 安全模型（与 server 端对齐）
 * ---------------------------------------------------------------------------
 *   - 只实现服务端白名单里的固定 op，没有「发送任意 keycode」的代码路径。
 *   - 文本输入仅 ASCII 可打印字符，上限 200 字符，固件侧二次校验。
 *   - 按住 BOOT 按钮 ⇒ HID 输出立即永久停止，直到重启。这是物理急停。
 *     （协议里没有时间戳字段，过期控制由服务端的令牌桶 + 审计日志负责。）
 *   - 坐标在固件里按屏幕尺寸 clamp，即使服务端校验漏了也不会越界。
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <BleCombo.h>

#include "config.h"   // ← 你的配置（WiFi / 服务器 / 屏幕 / 配对码）

// ------------------------------------------------------------ 内部常量 ----

static const uint16_t MAX_TEXT_CHARS     = 200;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;
static const uint32_t RECONNECT_DELAY_MS = 3000;
static const uint32_t SETUP_AP_AFTER_MS  = 90000;  // 出厂+NVS 都连不上 90 秒后开配网热点

// ------------------------------------------------------------ 网络状态 ----
// NVS 里存的用户 WiFi（wifi op 下发 / 配网网页写入），优先于 config.h 出厂值。
static Preferences prefs;
static char     savedSsid[33] = "";
static char     savedPass[65] = "";
static uint32_t noWifiSinceMs = 0;
static bool     setupApUp = false;
static WebServer setupWeb(80);

static void setupApStart();   // 前置声明

// ---------------------------------------------------------- 键名映射表 ----

/*
 * 服务端键名 -> BleCombo 的键常量。
 *
 * 用的是 Arduino Keyboard 风格的常量（KEY_RETURN 等），不是裸 USB HID usage ——
 * BleComboKeyboard::press() 自己会做映射。
 *
 * 服务端白名单里还有 APP_SWITCH / MENU / CAMERA / POWER / WAKEUP / SLEEP。
 * 这些键在 BLE HID 键盘 usage 表里没有可靠定义，硬映射会打到普通键上
 * （早期版本把 POWER 映到 0x30 也就是 ")" 键），所以统一回
 * "unsupported" 而不是猜一个。
 */
struct KeyMap { const char *name; uint8_t code; };
static const KeyMap KEYMAP[] = {
  {"ENTER",        KEY_RETURN},
  {"BACK",         KEY_BACKSPACE},
  {"BACKSPACE",    KEY_BACKSPACE},
  {"BACKSPACE_DEL",KEY_DELETE},   // 组合键里的「退格」语义
  {"DEL",          KEY_DELETE},
  {"TAB",          KEY_TAB},
  {"TAB_FORWARD",  KEY_TAB},
  {"ESC",          KEY_ESC},
  {"SPACE",        ' '},
  {"UP",           KEY_UP_ARROW},
  {"DOWN",         KEY_DOWN_ARROW},
  {"LEFT",         KEY_LEFT_ARROW},
  {"RIGHT",        KEY_RIGHT_ARROW},
  {"HOME",         KEY_HOME},
  {"PAGE_UP",      KEY_PAGE_UP},
  {"PAGE_DOWN",    KEY_PAGE_DOWN},
};
static const size_t KEYMAP_N = sizeof(KEYMAP) / sizeof(KEYMAP[0]);

/* 媒体键走的是 2 字节 consumer report，不是 keycode。 */
struct MediaKeyMap { const char *name; uint8_t report[2]; };
static const MediaKeyMap MEDIAMAP[] = {
  {"VOLUME_UP",   {32, 0}},
  {"VOLUME_DOWN", {64, 0}},
  {"MUTE",        {16, 0}},
};
static const size_t MEDIAMAP_N = sizeof(MEDIAMAP) / sizeof(MEDIAMAP[0]);

/* 这些名字服务端允许，但 BLE HID 打不出来 —— 回明确的错误而不是猜。 */
static const char *UNSUPPORTED_KEYS[] = {
  "APP_SWITCH", "MENU", "CAMERA", "POWER", "WAKEUP", "SLEEP",
};

static uint8_t lookupKey(const char *name) {
  for (size_t i = 0; i < KEYMAP_N; i++)
    if (strcmp(KEYMAP[i].name, name) == 0) return KEYMAP[i].code;
  return 0;
}

static const uint8_t *lookupMedia(const char *name) {
  for (size_t i = 0; i < MEDIAMAP_N; i++)
    if (strcmp(MEDIAMAP[i].name, name) == 0) return MEDIAMAP[i].report;
  return nullptr;
}

static bool isUnsupported(const char *name) {
  for (size_t i = 0; i < sizeof(UNSUPPORTED_KEYS) / sizeof(UNSUPPORTED_KEYS[0]); i++)
    if (strcmp(UNSUPPORTED_KEYS[i], name) == 0) return true;
  return false;
}

// ---------------------------------------------------------------- 状态 ----

// 第三个参数是 batteryLevel（上报给手机的电量百分比），不是配对码。
// 这个库没有配对码 API —— BLE HID 键盘走标准 JustWorks 配对，首次在手机
// 蓝牙设置里点「配对」即可，不需要 PIN。
static BleComboKeyboard bleKeyboard(BLE_DEVICE_NAME, "Espressif", 100);
static BleComboMouse    bleMouse(&bleKeyboard);
static WebSocketsClient ws;

static volatile bool killed = false;
static char     wsPath[256];
static uint32_t lastMsgMs = 0;

// ------------------------------------------------------- JSON 取值助手 ----
// 固定 op 集合只需要取几个标量，不值得为它拉进 ArduinoJson。

static bool jsonString(const char *json, const char *key, char *out, size_t outLen) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i < outLen - 1) out[i++] = *p++;
  out[i] = 0;
  return true;
}

static bool jsonInt(const char *json, const char *key, long *out) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  // 允许前导 '-'：scroll 的 dy 合法范围是 -20..20，只认数字会把它全拒了。
  if ((*p < '0' || *p > '9') && *p != '-') return false;
  *out = strtol(p, nullptr, 10);
  return true;
}

static void reply(const char *id, const char *ev, bool ok, const char *msg = nullptr) {
  char buf[192];
  if (msg && *msg)
    snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"ev\":\"%s\",\"ok\":%s,\"msg\":\"%s\"}",
             id, ev, ok ? "true" : "false", msg);
  else
    snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"ev\":\"%s\",\"ok\":%s}",
             id, ev, ok ? "true" : "false");
  ws.sendTXT((uint8_t *)buf, strlen(buf));
}

static bool killAsserted() {
  if (KILL_PIN_IS_BOOT) return digitalRead(KILL_PIN) == LOW;   // BOOT 低有效
  return digitalRead(KILL_PIN) == HIGH;
}

// ------------------------------------------------------------ 命令实现 ----

static void doType(const char *id, const char *text) {
  size_t n = strlen(text);
  if (n > MAX_TEXT_CHARS) { reply(id, "done", false, "too long"); return; }
  for (size_t i = 0; i < n; i++) {
    char c = text[i];
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) {
      reply(id, "done", false, "non-ascii"); return;
    }
    bleKeyboard.write((uint8_t)c);
    delay(12);              // 接近真人速度；部分输入框/安全键盘会拒掉突发输入
    if (killAsserted()) { reply(id, "done", false, "killed"); return; }
  }
  reply(id, "done", true);
}

static void doKey(const char *id, const char *name) {
  if (strcmp(name, "SPACE") == 0) {        // 可打印名走 write()（按下+抬起）
    bleKeyboard.write(' ');
    reply(id, "done", true);
    return;
  }
  const uint8_t *media = lookupMedia(name);
  if (media != nullptr) {
    bleKeyboard.press(media);
    bleKeyboard.release(media);
    reply(id, "done", true);
    return;
  }
  if (isUnsupported(name)) {
    reply(id, "done", false, "no BLE HID usage for this key");
    return;
  }
  uint8_t code = lookupKey(name);
  if (!code) { reply(id, "done", false, "unknown key"); return; }
  bleKeyboard.press(code);
  bleKeyboard.releaseAll();
  reply(id, "done", true);
}

/* 解析 ["CTRL","A"] 或 ["CTRL","ENTER"] 这样的组合键 JSON 片段。 */
static void doCombo(const char *id, const char *keysJson) {
  char mods[2][24]; int modCount = 0;
  char tailKey[24] = ""; char tailChar = 0;

  const char *p = strchr(keysJson, '[');
  if (!p) { reply(id, "done", false, "bad combo"); return; }
  p++;
  char buf[24];
  while (*p && *p != ']') {
    if (*p == '"') {
      p++; size_t i = 0;
      while (*p && *p != '"' && i < sizeof(buf) - 1) buf[i++] = *p++;
      buf[i] = 0;
      if (strcmp(buf, "CTRL") == 0 || strcmp(buf, "ALT") == 0 || strcmp(buf, "SHIFT") == 0) {
        if (modCount < 2) strcpy(mods[modCount++], buf);
      } else if (i == 1) {
        tailChar = buf[0];
      } else {
        strncpy(tailKey, buf, sizeof(tailKey) - 1);
      }
      if (*p == '"') p++;
    }
    p++;
  }
  if (modCount == 0 || (!tailChar && !tailKey[0])) {
    reply(id, "done", false, "bad combo"); return;
  }

  for (int i = 0; i < modCount; i++)
    bleKeyboard.press(strcmp(mods[i], "CTRL")  == 0 ? KEY_LEFT_CTRL
                    : strcmp(mods[i], "ALT")   == 0 ? KEY_LEFT_ALT
                                                    : KEY_LEFT_SHIFT);

  uint8_t code = 0;
  if (tailKey[0]) {
    code = lookupKey(tailKey);
  } else if (tailChar >= 0x20 && tailChar <= 0x7E) {
    code = (uint8_t)tailChar;      // asciimap 里就是这个映射
  }
  if (code) bleKeyboard.press(code);
  bleKeyboard.releaseAll();
  reply(id, "done", true);
}

/*
 * 绝对坐标点按：用 HID digitizer。
 * 按下 sendAbsolutePixel(x,y)，抬起 sendAbsolutePixel(x,y,false,false)。
 * 只用 click() 走的是相对鼠标报告，手机屏幕不吃那一套。
 */
static void doTap(const char *id, long x, long y, long ms) {
  // 服务端已经校验过 0..4000，但屏幕只有 SCREEN_W x SCREEN_H，
  // 这里再 clamp 一次，避免越界值被库的缩放映射到别处。
  if (x < 0) x = 0; if (x > SCREEN_W - 1) x = SCREEN_W - 1;
  if (y < 0) y = 0; if (y > SCREEN_H - 1) y = SCREEN_H - 1;

  bleMouse.sendAbsolutePixel((uint16_t)x, (uint16_t)y);
  delay(40);
  bleMouse.sendAbsolutePixel((uint16_t)x, (uint16_t)y, false, false);  // 抬起
  if (ms > 20) delay(ms < 1000 ? ms : 1000);
  reply(id, "done", true);
}

static void doSwipe(const char *id, long x1, long y1, long x2, long y2, long ms) {
  if (x1 < 0) x1 = 0; if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
  if (y1 < 0) y1 = 0; if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;
  if (x2 < 0) x2 = 0; if (x2 > SCREEN_W - 1) x2 = SCREEN_W - 1;
  if (y2 < 0) y2 = 0; if (y2 > SCREEN_H - 1) y2 = SCREEN_H - 1;

  long clampedMs = ms < 50 ? 50 : (ms > 5000 ? 5000 : ms);
  const int steps = 24;

  /* swipeLinear() 是阻塞的，整段手势跑完才返回，所以急停只能在开始前查。
   * 服务端把手势时长限制在 5 秒内，最坏情况就是 5 秒后才会响应 BOOT。 */
  if (killAsserted()) { reply(id, "done", false, "killed"); return; }
  bleMouse.swipeLinear((uint16_t)x1, (uint16_t)y1,
                       (uint16_t)x2, (uint16_t)y2,
                       (uint16_t)steps, (uint16_t)(clampedMs / steps));
  reply(id, "done", true);
}

static void doScroll(const char *id, long dy) {
  bleMouse.move(0, 0, (signed char)(dy < -20 ? -20 : (dy > 20 ? 20 : dy)));
  reply(id, "done", true);
}

/*
 * OTA：服务端下发 {"op":"ota","url":"...","md5":"..."}。
 *
 * 安全（与 server 端对齐的双层白名单）：
 *   - url 必须以 config.h 的 OTA_URL_PREFIX 开头（编译期定死，默认指向
 *     跑 ota_httpd.py 的那台），防止服务端被攻破后把板子刷成别的包。
 *   - md5 必须 32 位 hex；传输完整性由 OTA 服务器响应头 x-MD5 兜底
 *     （HTTPUpdate 自动比对下载内容与该头）。
 *
 * 流程：先回 ACK（升级中 WebSocket 会断），HTTPUpdate 写 ota_1 分区 +
 * otadata 切换，成功自动重启进新固件；3 秒后 WS 自动重连，/status 里的
 * fw 版本号可用于核验升级是否真的生效。
 * 物理急停同样管 OTA：killed 状态下这里根本进不来（handleCommand 已拦）。
 */
static void doOta(const char *id, const char *url, const char *md5) {
  // 多源白名单：云端 / 家里 LAN / 任意可信分发点，编译期定死
  static const char *const prefixes[] = OTA_URL_PREFIX_LIST;
  bool allowed = false;
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    if (strncmp(url, prefixes[i], strlen(prefixes[i])) == 0) { allowed = true; break; }
  }
  if (!allowed) { reply(id, "done", false, "url not allowed"); return; }
  if (strlen(md5) != 32) { reply(id, "done", false, "bad md5"); return; }
  reply(id, "done", true);
  Serial.printf("[ota] start %s (md5 %.8s...)\n", url, md5);
  WiFiClient client;
  HTTPUpdate httpUp;                       // rebootOnUpdate 默认 true
  HTTPUpdateResult r = httpUp.update(client, url);
  /* 能执行到这里说明失败了 —— 成功会在写完后直接重启。 */
  Serial.printf("[ota] failed(%d): %s\n", r, httpUp.getLastErrorString().c_str());
}

static void handleCommand(const char *json) {
  char id[24] = "0", op[24] = "";
  jsonString(json, "id", id, sizeof(id));
  if (!jsonString(json, "op", op, sizeof(op))) return;

  lastMsgMs = millis();

  if (killed) { reply(id, "done", false, "killed"); return; }

  if (strcmp(op, "ping") == 0) { reply(id, "done", true); return; }

  if (strcmp(op, "type") == 0) {
    char text[MAX_TEXT_CHARS + 1] = {0};
    if (!jsonString(json, "text", text, sizeof(text))) { reply(id, "done", false, "no text"); return; }
    doType(id, text); return;
  }
  if (strcmp(op, "key") == 0) {
    char key[24] = "";
    if (!jsonString(json, "key", key, sizeof(key))) { reply(id, "done", false, "no key"); return; }
    doKey(id, key); return;
  }
  if (strcmp(op, "combo") == 0) {
    const char *p = strstr(json, "\"keys\"");
    if (!p) { reply(id, "done", false, "no keys"); return; }
    p = strchr(p, '[');
    char buf[80]; size_t i = 0;
    if (p) { p++; while (*p && *p != ']' && i < sizeof(buf) - 1) buf[i++] = *p++; }
    buf[i] = 0;
    doCombo(id, buf); return;
  }
  if (strcmp(op, "tap") == 0) {
    long x, y, ms = 70;
    if (!jsonInt(json, "x", &x) || !jsonInt(json, "y", &y)) { reply(id, "done", false, "bad tap"); return; }
    jsonInt(json, "ms", &ms);
    doTap(id, x, y, ms); return;
  }
  if (strcmp(op, "swipe") == 0) {
    long x1, y1, x2, y2, ms = 300;
    if (!jsonInt(json, "x1", &x1) || !jsonInt(json, "y1", &y1) ||
        !jsonInt(json, "x2", &x2) || !jsonInt(json, "y2", &y2)) {
      reply(id, "done", false, "bad swipe"); return;
    }
    jsonInt(json, "ms", &ms);
    doSwipe(id, x1, y1, x2, y2, ms); return;
  }
  if (strcmp(op, "scroll") == 0) {
    long dy;
    if (!jsonInt(json, "dy", &dy)) { reply(id, "done", false, "bad scroll"); return; }
    doScroll(id, dy); return;
  }
  if (strcmp(op, "ota") == 0) {
    char url[200] = {0}, md5[40] = {0};
    if (!jsonString(json, "url", url, sizeof(url))) { reply(id, "done", false, "no url"); return; }
    if (!jsonString(json, "md5", md5, sizeof(md5))) { reply(id, "done", false, "no md5"); return; }
    doOta(id, url, md5); return;
  }
  if (strcmp(op, "wifi") == 0) {
    // 运行时切网：手机热点（走流量 OTA）、换路由都靠这个。存 NVS，重启仍生效。
    char ssid[33] = {0}, pass[65] = {0};
    if (!jsonString(json, "ssid", ssid, sizeof(ssid))) { reply(id, "done", false, "no ssid"); return; }
    jsonString(json, "pass", pass, sizeof(pass));
    prefs.begin("hid", false);
    prefs.putString("ssid", ssid); prefs.putString("pass", pass);
    prefs.end();
    strlcpy(savedSsid, ssid, sizeof(savedSsid));
    strlcpy(savedPass, pass, sizeof(savedPass));
    reply(id, "done", true);   // 切网会断 WS，先 ACK
    Serial.printf("[wifi] 切换到 \"%s\" …\n", ssid);
    WiFi.disconnect();
    delay(300);
    if (tryConnect(savedSsid, savedPass, 20000)) {
      if (setupApUp) { setupApUp = false; setupWeb.stop(); WiFi.softAPdisconnect(true); }
      WiFi.mode(WIFI_STA);
      Serial.printf("[wifi] 已切换: %s\n", WiFi.localIP().toString().c_str());
    } else {
      noWifiSinceMs = millis();  // 失败则进无网自救流程（配网热点）
      Serial.println("[wifi] 切换失败，稍后重试/开配网热点");
    }
    return;
  }
  reply(id, "done", false, "unknown op");
}

/*
 * WebSockets 2.x 只有单一事件回调 (WStype_t, payload, length)。
 * 2.7.x 里不存在旧版的 onMessage() 钩子，也不存在带 client 参数的签名。
 */
static void onEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      // 用 strlen 而不是手数长度：早期版本给 39 字符的串发了 40 字节，
      // 尾部那个 NUL 让服务端 json.loads 失败，board_hello 永远进不了审计日志。
      static const char *hello = "{\"ev\":\"hello\",\"fw\":\"c3-1.1\",\"ble\":true}";
      ws.sendTXT((uint8_t *)hello, strlen(hello));
      Serial.println("[ws] connected");
      break;
    }
    case WStype_DISCONNECTED:
      Serial.println("[ws] closed, retrying...");
      break;
    case WStype_TEXT: {
      char buf[1024];
      size_t n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
      memcpy(buf, payload, n);
      buf[n] = 0;
      handleCommand(buf);
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------- BLE ----

static void setupHid() {
  Serial.println("[ble] starting");
  bleKeyboard.setMTU(185);                          // 库的建议值
  bleKeyboard.setPreferredConnectionParams(0x06, 0x12);
  bleKeyboard.begin();
  bleMouse.begin();
  bleMouse.setScreenSize(SCREEN_W, SCREEN_H);       // tap/swipe 用像素坐标
  Serial.printf("[ble] %s up, screen %ux%u\n", BLE_DEVICE_NAME, SCREEN_W, SCREEN_H);
}

static bool tryConnect(const char *ssid, const char *pass, uint32_t ms) {
  if (!ssid[0]) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-hid");
  WiFi.begin(ssid, pass);
  Serial.printf("[wifi] try \"%s\"", ssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) { delay(250); Serial.print("."); }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

static void loadSavedWifi() {
  prefs.begin("hid", true);
  strlcpy(savedSsid, prefs.getString("ssid", "").c_str(), sizeof(savedSsid));
  strlcpy(savedPass, prefs.getString("pass", "").c_str(), sizeof(savedPass));
  prefs.end();
  if (savedSsid[0]) Serial.printf("[wifi] NVS 已存网络: \"%s\"\n", savedSsid);
}

// 候选列表：NVS（wifi op/配网页写的）> 出厂主 WiFi > 出厂备选列表
struct WifiCand { const char *ssid; const char *pass; };
static bool connectAll(uint32_t perTryMs) {
  if (tryConnect(savedSsid, savedPass, perTryMs)) { Serial.printf("[wifi] up: %s (nvs)\n", WiFi.localIP().toString().c_str()); return true; }
  if (tryConnect(WIFI_SSID, WIFI_PASSWORD, perTryMs)) { Serial.printf("[wifi] up: %s\n", WiFi.localIP().toString().c_str()); return true; }
  static const WifiCand fallback[] = WIFI_FALLBACK_LIST;
  for (auto &c : fallback) {
    if (tryConnect(c.ssid, c.pass, perTryMs)) { Serial.printf("[wifi] up: %s (fallback)\n", WiFi.localIP().toString().c_str()); return true; }
  }
  return false;
}

static void connectWifi() {
  if (connectAll(CONNECT_TIMEOUT_MS)) return;
  Serial.println("[wifi] FAILED - check SSID spelling/case and 2.4GHz band");
  noWifiSinceMs = millis();   // loop 里会定期重试，超时开配网热点
}

// --------------------------------------------- SoftAP 配网 + 网页升级 ----
// 板子在没有任何已知 WiFi 的环境也能活：手机连它的热点 "ESP32-HID-Setup"，
// 浏览器开 http://192.168.4.1/ —— 填 WiFi、上传固件都行（这就是"无网升级"通道，
// 比 BLE 传固件快一个数量级且不占用 HID 的 GATT）。

static const char *SETUP_INDEX_HTML =
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<h3>ESP32 HID 配网</h3>"
  "<form method=POST action=/save>"
  "WiFi名: <input name=ssid><br>密码: <input name=pass><br>"
  "<button>保存并连接</button></form><hr>"
  "<h3>固件升级（无网）</h3>"
  "<form method=POST action=/update enctype='multipart/form-data'>"
  "<input type=file name=bin accept='.bin'><button>上传并升级</button></form>";

static void handleFwUpload() {
  HTTPUpload &up = setupWeb.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[setup] 上传固件: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[setup] 固件完成 %u 字节，重启\n", up.totalSize);
      setupWeb.send(200, "text/plain", "升级成功，重启中。请把手机 WiFi 切回原来的网络。");
      delay(800);
      ESP.restart();
    } else {
      Update.printError(Serial);
      setupWeb.send(500, "text/plain", "升级失败（固件不完整或不匹配）");
    }
  }
}

static void setupApStart() {
  if (setupApUp) return;
  setupApUp = true;
  WiFi.mode(WIFI_AP_STA);                       // STA 保留：后台继续试家里网
  WiFi.softAP("ESP32-HID-Setup");               // 开放热点；配网完成即关
  Serial.print("[setup] 配网热点开启，访问 http://");
  Serial.println(WiFi.softAPIP());
  setupWeb.on("/", HTTP_GET, [] { setupWeb.send(200, "text/html", SETUP_INDEX_HTML); });
  setupWeb.on("/save", HTTP_POST, [] {
    String s = setupWeb.arg("ssid"), p = setupWeb.arg("pass");
    if (!s.length()) { setupWeb.send(400, "text/plain", "ssid 为空"); return; }
    prefs.begin("hid", false);
    prefs.putString("ssid", s); prefs.putString("pass", p);
    prefs.end();
    setupWeb.send(200, "text/plain", "已保存，正在连接 " + s + " …");
    strlcpy(savedSsid, s.c_str(), sizeof(savedSsid));
    strlcpy(savedPass, p.c_str(), sizeof(savedPass));
    tryConnect(savedSsid, savedPass, 20000);
    if (WiFi.status() == WL_CONNECTED) { setupApUp = false; setupWeb.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); }
  });
  setupWeb.on("/update", HTTP_POST, [] {
    if (!Update.hasError()) setupWeb.send(200, "text/plain", "处理中");
  }, handleFwUpload);
  setupWeb.begin();
}

// --------------------------------------------------------------- setup ----

void setup() {
  pinMode(KILL_PIN, KILL_PIN_IS_BOOT ? INPUT_PULLUP : INPUT);
  Serial.begin(115200);
  delay(400);
  Serial.println("\n[boot] esp32-c3-hid-agent");

  loadSavedWifi();

  // HID 先起：已配对的手机几秒内自己就重连了，
  // 下面等 WiFi 最多 15 秒会拖慢它。
  setupHid();

  connectWifi();

  snprintf(wsPath, sizeof(wsPath), "/ws?token=%s", SERVER_TOKEN);
  Serial.printf("[ws] ws://%s:%d/ws?token=***\n", SERVER_HOST, SERVER_PORT);

  ws.begin(SERVER_HOST, SERVER_PORT, wsPath);
  ws.onEvent(onEvent);
  ws.setReconnectInterval(RECONNECT_DELAY_MS);   // 服务端重启后 3s 自愈
}

// ---------------------------------------------------------------- loop ----

void loop() {
  ws.loop();

  // 无网自救：周期重试已知网络；一直连不上就开配网热点（网页可配网 + 无网传固件）
  if (WiFi.status() != WL_CONNECTED && WiFi.getMode() & WIFI_STA) {
    if (!noWifiSinceMs) noWifiSinceMs = millis();
    if (!setupApUp && millis() - noWifiSinceMs > SETUP_AP_AFTER_MS) setupApStart();
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry > 60000) {
      lastRetry = millis();
      if (connectAll(8000)) {
        noWifiSinceMs = 0;
        if (setupApUp) { setupApUp = false; setupWeb.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); }
      }
    }
  }
  if (setupApUp) setupWeb.handleClient();

  // 物理急停：按住 BOOT，所有 HID 输出立刻停止，直到重启。
  if (!killed && killAsserted()) {
    killed = true;
    bleKeyboard.releaseAll();
    bleMouse.release();
    Serial.println("[kill] BOOT held - HID output disabled until reboot");
  }
  if (killed) delay(50);

  // 串口诊断：不接任何按键，纯被动打印。刷完先看这里。
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 5000) {
    lastPrint = millis();
    Serial.printf("[stat] wifi=%s ip=%s ws=%s killed=%d idle=%lums\n",
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  WiFi.localIP().toString().c_str(),
                  ws.isConnected() ? "up" : "down",
                  killed ? 1 : 0,
                  (unsigned long)(millis() - lastMsgMs));
  }
}