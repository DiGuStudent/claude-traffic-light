/*
 * Claude Traffic Light — ESP8266 固件
 * 红黄绿三颗 LED 实时指示 Claude Code 工作状态
 *
 * 接线（NodeMCU / D1 mini 通用）：
 *   红 → D5 (GPIO14)    黄 → D6 (GPIO12)    绿 → D7 (GPIO13)
 *   每路：引脚 → 220Ω 电阻 → LED 阳极，LED 阴极 → GND
 *
 * 命令（串口 115200 或 WiFi UDP 8266 端口，一行一条）：
 *   new   新对话开始：红黄绿同闪一下 → 绿灯常亮
 *   work  处理中：黄灯常亮
 *   wait  等待确认：黄灯闪烁
 *   error 出错：红灯闪烁
 *   idle  空闲：绿灯常亮
 *   bye   对话结束：绿灯长周期呼吸
 *   off   全灭
 *   status         回显当前状态
 *   wifi:SSID:PASS 保存 WiFi 凭证到 EEPROM 并连接（连上后 UDP 可用）
 *
 * 上电直接绿灯常亮（不闪烁）；OTA 升级端口 8267，升级期间黄灯。
 */
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

const uint8_t PIN_R = 14;  // D5
const uint8_t PIN_Y = 12;  // D6
const uint8_t PIN_G = 13;  // D7

enum Effect : uint8_t { FX_OFF, FX_NEW, FX_WORK, FX_WAIT, FX_ERROR, FX_IDLE, FX_BYE };
Effect cur = FX_OFF;
uint32_t tStart = 0;      // 效果起始时刻（millis）

WiFiUDP udp;
bool udpReady = false;

// ---------- EEPROM：magic(4B) + ssid\0 + pass\0 ----------
const uint32_t MAGIC = 0x544C5743;  // "CLWT"
char ssid[33] = {0};
char pass[65] = {0};

void loadWiFiCfg() {
  EEPROM.begin(128);
  uint32_t magic = 0;
  EEPROM.get(0, magic);
  if (magic == MAGIC) {
    int p = 4;
    for (int i = 0; i < 32; i++) { ssid[i] = EEPROM.read(p + i); if (!ssid[i]) break; }
    p += strlen(ssid) + 1;
    for (int i = 0; i < 64; i++) { pass[i] = EEPROM.read(p + i); if (!pass[i]) break; }
  }
  EEPROM.end();
}

void saveWiFiCfg(const char* s, const char* pw) {
  strncpy(ssid, s, 32); ssid[32] = 0;
  strncpy(pass, pw, 64); pass[64] = 0;
  EEPROM.begin(128);
  EEPROM.put(0, MAGIC);
  int p = 4;
  for (size_t i = 0; i <= strlen(ssid); i++) EEPROM.write(p + i, ssid[i]);
  p += strlen(ssid) + 1;
  for (size_t i = 0; i <= strlen(pass); i++) EEPROM.write(p + i, pass[i]);
  EEPROM.commit();
  EEPROM.end();
}

// ---------- 灯效 ----------
void show(Effect fx) {
  cur = fx;
  tStart = millis();
  if (fx == FX_NEW) setRGB(1, 1, 1);  // 立即点亮，后续状态机处理
}

void setRGB(bool r, bool y, bool g) {
  digitalWrite(PIN_R, r);
  digitalWrite(PIN_Y, y);
  digitalWrite(PIN_G, g);
}

void runEffect() {
  uint32_t t = millis();
  switch (cur) {
    case FX_OFF:  setRGB(0, 0, 0); break;
    case FX_WORK: setRGB(0, 1, 0); break;
    case FX_IDLE: setRGB(0, 0, 1); break;
    case FX_WAIT:  setRGB(0, (t / 300) % 2, 0); break;
    case FX_ERROR: setRGB((t / 150) % 2, 0, 0); break;
    case FX_NEW: {
      uint32_t e = t - tStart;
      if (e < 400) setRGB(1, 1, 1);          // 三灯同亮
      else if (e < 700) setRGB(0, 0, 0);     // 熄灭
      else show(FX_IDLE);                    // 转入绿灯常亮
      break;
    }
    case FX_BYE: {
      // 绿灯呼吸：4s 周期正弦，从最暗淡入
      uint32_t ph = (t - tStart) % 4000;
      int v = (int)(1023.0 * (sin(ph * (2.0 * PI) / 4000.0 - PI / 2.0) + 1.0) / 2.0);
      analogWrite(PIN_G, v);
      digitalWrite(PIN_R, 0);
      digitalWrite(PIN_Y, 0);
      break;
    }
  }
}

// ---------- 命令处理 ----------
void handleCmd(const String& cmd, IPAddress* rip, uint16_t rport) {
  String ack;
  if (cmd == "new")      { show(FX_NEW);   ack = "OK new"; }
  else if (cmd == "work")    { show(FX_WORK);  ack = "OK work"; }
  else if (cmd == "wait")    { show(FX_WAIT);  ack = "OK wait"; }
  else if (cmd == "error")   { show(FX_ERROR); ack = "OK error"; }
  else if (cmd == "idle")    { show(FX_IDLE);  ack = "OK idle"; }
  else if (cmd == "bye")     { show(FX_BYE);   ack = "OK bye"; }
  else if (cmd == "off")     { show(FX_OFF);   ack = "OK off"; }
  else if (cmd == "status") {
    ack = "STATUS fx=" + String(cur) + " wifi=" +
          (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "off");
  }
  else if (cmd.startsWith("wifi:")) {
    int c1 = cmd.indexOf(':', 5);
    if (c1 > 5) {
      String s = cmd.substring(5, c1);
      String p = cmd.substring(c1 + 1);
      saveWiFiCfg(s.c_str(), p.c_str());
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, pass);
      udpReady = false;
      ack = "OK wifi saved, connecting...";
    } else {
      ack = "ERR usage: wifi:SSID:PASS";
    }
  }
  else { ack = "ERR unknown:" + cmd; }

  if (rip) { udp.beginPacket(*rip, rport); udp.print(ack); udp.endPacket(); }
  Serial.println(ack);
}

void handleSerial() {
  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length()) handleCmd(line, NULL, 0);
  }
}

void handleUDP() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!udpReady) {
    udp.begin(8266);
    udpReady = true;
    Serial.println("UDP ready, IP=" + WiFi.localIP().toString());
  }
  int n = udp.parsePacket();
  if (n > 0) {
    char buf[128];
    int len = udp.read(buf, sizeof(buf) - 1);
    buf[len] = 0;
    String line = String(buf);
    line.trim();
    if (line.length()) {
      IPAddress rip = udp.remoteIP();
      uint16_t rp = udp.remotePort();
      handleCmd(line, &rip, rp);
    }
  }
}

// ---------- 主流程 ----------
void setup() {
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_Y, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  Serial.begin(115200);
  Serial.println("\nClaude Traffic Light boot");
  loadWiFiCfg();
  if (ssid[0]) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
  }

  // OTA 无线升级：端口 8267（与命令 UDP 8266 错开）
  ArduinoOTA.setHostname("claude-light");
  ArduinoOTA.setPort(8267);
  ArduinoOTA.onStart([]() {
    Serial.println("OTA start");
    show(FX_WORK);  // 升级期间黄灯
  });
  ArduinoOTA.onEnd([]() { Serial.println("OTA done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); });
  ArduinoOTA.begin();

  show(FX_IDLE);  // 上电：直接绿灯常亮（空闲态），不闪烁
}

void loop() {
  ArduinoOTA.handle();
  handleSerial();
  handleUDP();
  runEffect();
}
