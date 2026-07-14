#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <pdulib.h>
#define ENABLE_SMTP
#define ENABLE_DEBUG
#include <ReadyMail.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <WebServer.h>
#include <Preferences.h>
#include <base64.h>
#include <MD5Builder.h>
#include <mbedtls/md.h>
struct ConcatSms;
#include <stdarg.h>
#include <esp_idf_version.h>

// 看门狗超时（秒）
#define WDT_TIMEOUT_SEC 60

// HTTP 超时（毫秒）
#define HTTP_TIMEOUT_MS 15000

// 定时重启间隔（毫秒）- 默认每周重启一次
#define SCHEDULED_RESTART_INTERVAL_MS (7UL * 24UL * 60UL * 60UL * 1000UL) // 7天

// 引入配置文件（敏感信息在此文件中定义）
#include "config.h"

// 旧 config.h 不增加新宏也可编译，并保持原通用 HTTP 行为。
#ifndef HTTP_SMS_WEB_MODE
#define HTTP_SMS_WEB_MODE 0
#endif
#ifndef HTTP_API_KEY
#define HTTP_API_KEY ""
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "esp32-sms"
#endif
#ifndef SIM_SLOT
#define SIM_SLOT 1
#endif

// 上游功能按固定容量实现，避免在资源有限的 ESP32-C3 上保留大量 String。
#ifndef MODEM_EN_PIN
#define MODEM_EN_PIN 5
#endif
#ifndef MAX_PUSH_CHANNELS
#define MAX_PUSH_CHANNELS 3
#endif
#ifndef WEB_LOG_SIZE
#define WEB_LOG_SIZE 3072
#endif
#ifndef MAX_CONCAT_MESSAGES
#define MAX_CONCAT_MESSAGES 2
#endif
#ifndef MAX_CONCAT_PARTS
#define MAX_CONCAT_PARTS 8
#endif
#ifndef CONCAT_PART_TEXT_LEN
#define CONCAT_PART_TEXT_LEN 192
#endif
#define CONCAT_TIMEOUT_MS 30000UL
#if MAX_PUSH_CHANNELS > 16
#error "MAX_PUSH_CHANNELS must not exceed 16 (retry state uses a uint16_t bitmask)"
#endif
#if MAX_CONCAT_PARTS > 8
#error "MAX_CONCAT_PARTS must not exceed 8 (concat state uses a uint8_t bitmask)"
#endif

enum PushType : uint8_t {
  PUSH_NONE = 0, PUSH_POST_JSON, PUSH_BARK, PUSH_GET, PUSH_DINGTALK,
  PUSH_PUSHPLUS, PUSH_SERVERCHAN, PUSH_CUSTOM, PUSH_FEISHU, PUSH_GOTIFY,
  PUSH_TELEGRAM
};

struct PushChannelConfig {
  bool enabled;
  uint8_t type;
  char name[24];
  char url[192];
  char key1[96];
  char key2[96];
  char customBody[256];
};

// Web 服务器端口
#define WEB_SERVER_PORT 80

// Web 管理界面默认账号密码
#ifndef WEB_ADMIN_USER
#define WEB_ADMIN_USER "admin"
#endif
#ifndef WEB_ADMIN_PASS
#define WEB_ADMIN_PASS "admin123"
#endif

// Web 服务器实例
WebServer webServer(WEB_SERVER_PORT);

// 持久化存储实例
Preferences preferences;

// 运行时配置结构体（可通过Web界面修改）
struct RuntimeConfig {
  char wecomUrl[256];
  char simNumber[32];
  char smtpServer[64];
  uint16_t smtpPort;
  char smtpUser[64];
  char smtpPass[64];
  char smtpTo[64];
  char httpServerUrl[128];
  char httpApiKey[64];
  char deviceId[32];
  uint8_t simSlot;
  bool enableWecom;
  bool enableEmail;
  bool enableHttp;
  bool smsWebMode;
  char webUser[32];
  char webPass[32];
  char adminPhone[32];
  char numberBlacklist[256];
  PushChannelConfig pushChannels[MAX_PUSH_CHANNELS];
} rtConfig;

//串口映射
#define TXD 3
#define RXD 4

// 格式化 PDU 时间戳为可读格式，并统一转换为中国标准时间 (UTC+8)
// 输入格式: YYMMDDHHmmss+TZ (如 25112614465832)
// 输出格式: 20YY-MM-DD HH:mm:ss
String formatTimestamp(const char* pduTimestamp) {
  // 空指针保护
  if (pduTimestamp == NULL || strlen(pduTimestamp) < 12) {
    return pduTimestamp ? String(pduTimestamp) : String("未知时间");
  }
  
  // 解析 PDU 时间
  int year = 2000 + (pduTimestamp[0] - '0') * 10 + (pduTimestamp[1] - '0');
  int month = (pduTimestamp[2] - '0') * 10 + (pduTimestamp[3] - '0');
  int day = (pduTimestamp[4] - '0') * 10 + (pduTimestamp[5] - '0');
  int hour = (pduTimestamp[6] - '0') * 10 + (pduTimestamp[7] - '0');
  int minute = (pduTimestamp[8] - '0') * 10 + (pduTimestamp[9] - '0');
  int second = (pduTimestamp[10] - '0') * 10 + (pduTimestamp[11] - '0');
  
  // 解析时区 (如果有)
  int tzOffsetSeconds = 0;
  if (strlen(pduTimestamp) >= 14) {
    char tzStr[3] = {pduTimestamp[12], pduTimestamp[13], '\0'};
    if (isdigit(tzStr[0]) && isdigit(tzStr[1])) {
      // PDU 时区单位为 15 分钟
      // 注意：这里假设时区为正数（UK/CN 均为正或0），未处理负时区符号位
      tzOffsetSeconds = atoi(tzStr) * 15 * 60;
    }
  }

  struct tm tm_in = {0};
  tm_in.tm_year = year - 1900;
  tm_in.tm_mon = month - 1;
  tm_in.tm_mday = day;
  tm_in.tm_hour = hour;
  tm_in.tm_min = minute;
  tm_in.tm_sec = second;
  tm_in.tm_isdst = -1;

  // 计算 UTC 时间戳 (假设系统默认为 UTC)
  time_t t = mktime(&tm_in);
  
  // 转换为中国标准时间 (UTC+8)
  // 1. 减去 PDU 时区偏移，得到真实 UTC
  // 2. 加上 8 小时 (28800 秒)
  t = t - tzOffsetSeconds + 28800;
  
  struct tm *tm_out = gmtime(&t);
  
  char formatted[32];
  snprintf(formatted, sizeof(formatted), "%04d-%02d-%02d %02d:%02d:%02d",
           tm_out->tm_year + 1900, tm_out->tm_mon + 1, tm_out->tm_mday,
           tm_out->tm_hour, tm_out->tm_min, tm_out->tm_sec);
  return String(formatted);
}

// JSON 字符串转义函数，防止特殊字符破坏 JSON 格式
String escapeJson(const char* str) {
  String result = "";
  while (*str) {
    char c = *str++;
    switch (c) {
      case '"':  result += "\\\""; break;   // 双引号
      case '\\': result += "\\\\"; break;   // 反斜杠
      case '\n': result += "\\n"; break;    // 换行
      case '\r': result += "\\r"; break;    // 回车
      case '\t': result += "\\t"; break;    // 制表符
      case '\b': result += "\\b"; break;    // 退格
      case '\f': result += "\\f"; break;    // 换页
      default:
        // 控制字符使用 Unicode 转义
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

// pdulib 返回 YYMMDDHHmmssTZ，TZ 为 15 分钟单位。sms_web 使用 Unix 秒级时间戳。
time_t pduTimestampToUnix(const char* value) {
  if (!value || strlen(value) < 12) return 0;
  for (uint8_t i = 0; i < 12; ++i) if (!isdigit(value[i])) return 0;

  struct tm parsed = {0};
  parsed.tm_year = 100 + (value[0] - '0') * 10 + value[1] - '0';
  parsed.tm_mon  = (value[2] - '0') * 10 + value[3] - '0' - 1;
  parsed.tm_mday = (value[4] - '0') * 10 + value[5] - '0';
  parsed.tm_hour = (value[6] - '0') * 10 + value[7] - '0';
  parsed.tm_min  = (value[8] - '0') * 10 + value[9] - '0';
  parsed.tm_sec  = (value[10] - '0') * 10 + value[11] - '0';
  parsed.tm_isdst = 0;

  int timezoneSeconds = 0;
  const char* timezone = value + 12;
  int sign = 1;
  if (*timezone == '+' || *timezone == '-') {
    if (*timezone++ == '-') sign = -1;
  }
  if (isdigit(timezone[0]) && isdigit(timezone[1])) {
    timezoneSeconds = sign * ((timezone[0] - '0') * 10 + timezone[1] - '0') * 15 * 60;
  }
  time_t localEpoch = mktime(&parsed);
  return localEpoch > 0 ? localEpoch - timezoneSeconds : 0;
}


WiFiMulti WiFiMulti;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);

#define SERIAL_BUFFER_SIZE 2048
#define MAX_PDU_LENGTH 1024
char serialBuf[SERIAL_BUFFER_SIZE];
int serialBufLen = 0;

// 队列与重试配置
#ifndef SMS_QUEUE_SIZE
#define SMS_QUEUE_SIZE 8       // 默认约占 11KB；原值 20 会占用约 27KB 常驻 RAM
#endif
#define SMS_MAX_RETRIES 5
#define SMS_RETRY_INTERVAL_MS 60000UL // 60s

// 固定长度缓冲区（避免堆碎片化）
#define SMS_SENDER_LEN 32
#define SMS_TEXT_LEN 1280      // 支持长短信（约 400 个汉字）
#define SMS_TIMESTAMP_LEN 32

struct SMSItem {
  char sender[SMS_SENDER_LEN];
  char text[SMS_TEXT_LEN];
  char timestamp[SMS_TIMESTAMP_LEN];
  uint8_t retries;
  unsigned long lastAttempt;
  bool valid;  // 标记该槽位是否有效
  // 各渠道发送状态：true=已成功，false=待发送/重试
  bool wecomSent;
  bool emailSent;
  bool httpSent;
  uint16_t pushSentMask;
};

// 函数前向声明（解决编译顺序问题）
void enqueueSMS(const char* sender, const char* text, const char* timestamp);
void enqueueSMSWithStatus(const char* sender, const char* text, const char* timestamp, bool wecomOk, bool emailOk, bool httpOk, uint16_t pushMask = 0);
void removeHeadSMS();
bool trySendChannels(SMSItem &item);  // 改为非const，需要更新状态
void processSMSQueue();
void ensureWiFiConnected();
void loadConfig();
bool saveConfig();
bool resetConfig();
void setupWebServer();
bool checkAuth();
bool sendSMS(const char* phoneNumber, const char* message);
String htmlEncode(const String& str);
void processReceivedSMS(const char* sender, const char* text, const char* timestamp);
void processReceivedSegment(const char* sender, const char* text, const char* timestamp,
                            uint16_t reference, uint8_t part, uint8_t total);
void checkConcatTimeouts();
bool resetModem();
String sendATCommand(const char* command, unsigned long timeout = 3000);
void handleCtrl();
bool sendSmsWebDeviceEvent(uint16_t type);

SMSItem smsQueue[SMS_QUEUE_SIZE];
int sms_q_head = 0; // index of oldest
int sms_q_count = 0; // number of items

// WiFi 重连控制
unsigned long lastWifiAttempt = 0;
unsigned long wifiReconnectInterval = 5000; // 初始重连间隔 ms

// 系统启动时间（用于定时重启）
unsigned long bootTime = 0;
unsigned long lastSmsWebHeartbeat = 0;
#define SMS_WEB_HEARTBEAT_MS 120000UL

// 网页日志使用覆盖式环形缓冲；固定占用，不随运行时间增长。
char webLog[WEB_LOG_SIZE];
size_t webLogHead = 0;
size_t webLogLength = 0;

void logLine(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  Serial.println(line);
  size_t length = strnlen(line, sizeof(line));
  for (size_t i = 0; i < length; ++i) {
    webLog[webLogHead] = line[i];
    webLogHead = (webLogHead + 1) % WEB_LOG_SIZE;
    if (webLogLength < WEB_LOG_SIZE) ++webLogLength;
  }
  webLog[webLogHead] = '\n';
  webLogHead = (webLogHead + 1) % WEB_LOG_SIZE;
  if (webLogLength < WEB_LOG_SIZE) ++webLogLength;
}

String getWebLog() {
  String result;
  result.reserve(webLogLength + 1);
  size_t start = (webLogHead + WEB_LOG_SIZE - webLogLength) % WEB_LOG_SIZE;
  for (size_t i = 0; i < webLogLength; ++i) result += webLog[(start + i) % WEB_LOG_SIZE];
  return result;
}

struct ConcatSms {
  bool used;
  uint16_t reference;
  uint8_t total;
  uint8_t received;
  uint8_t presentMask;
  unsigned long startedAt;
  char sender[SMS_SENDER_LEN];
  char timestamp[SMS_TIMESTAMP_LEN];
  char parts[MAX_CONCAT_PARTS][CONCAT_PART_TEXT_LEN];
};
ConcatSms concatMessages[MAX_CONCAT_MESSAGES];

void loadPushChannelsFromNvs() {
  char key[12];
  for (uint8_t i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    PushChannelConfig &ch = rtConfig.pushChannels[i];
    snprintf(key, sizeof(key), "p%ue", i); ch.enabled = preferences.getBool(key, ch.enabled);
    snprintf(key, sizeof(key), "p%ut", i); ch.type = preferences.getUChar(key, ch.type);
    snprintf(key, sizeof(key), "p%un", i); strlcpy(ch.name, preferences.getString(key, ch.name).c_str(), sizeof(ch.name));
    snprintf(key, sizeof(key), "p%uu", i); strlcpy(ch.url, preferences.getString(key, ch.url).c_str(), sizeof(ch.url));
    snprintf(key, sizeof(key), "p%ua", i); strlcpy(ch.key1, preferences.getString(key, ch.key1).c_str(), sizeof(ch.key1));
    snprintf(key, sizeof(key), "p%ub", i); strlcpy(ch.key2, preferences.getString(key, ch.key2).c_str(), sizeof(ch.key2));
    snprintf(key, sizeof(key), "p%uc", i); strlcpy(ch.customBody, preferences.getString(key, ch.customBody).c_str(), sizeof(ch.customBody));
  }
}

void savePushChannelsToNvs() {
  char key[12];
  for (uint8_t i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    const PushChannelConfig &ch = rtConfig.pushChannels[i];
    snprintf(key, sizeof(key), "p%ue", i); preferences.putBool(key, ch.enabled);
    snprintf(key, sizeof(key), "p%ut", i); preferences.putUChar(key, ch.type);
    snprintf(key, sizeof(key), "p%un", i); preferences.putString(key, ch.name);
    snprintf(key, sizeof(key), "p%uu", i); preferences.putString(key, ch.url);
    snprintf(key, sizeof(key), "p%ua", i); preferences.putString(key, ch.key1);
    snprintf(key, sizeof(key), "p%ub", i); preferences.putString(key, ch.key2);
    snprintf(key, sizeof(key), "p%uc", i); preferences.putString(key, ch.customBody);
  }
}

// ==================== 持久化配置函数 ====================
void loadCompileTimeDefaults() {
  memset(&rtConfig, 0, sizeof(rtConfig));
  strlcpy(rtConfig.wecomUrl, WECHAT_WEBHOOK_URL, sizeof(rtConfig.wecomUrl));
  strlcpy(rtConfig.simNumber, LOCAL_SIM_NUMBER, sizeof(rtConfig.simNumber));
  strlcpy(rtConfig.smtpServer, SMTP_SERVER, sizeof(rtConfig.smtpServer));
  rtConfig.smtpPort = SMTP_SERVER_PORT;
  strlcpy(rtConfig.smtpUser, SMTP_USER, sizeof(rtConfig.smtpUser));
  strlcpy(rtConfig.smtpPass, SMTP_PASS, sizeof(rtConfig.smtpPass));
  strlcpy(rtConfig.smtpTo, SMTP_SEND_TO, sizeof(rtConfig.smtpTo));
  strlcpy(rtConfig.httpServerUrl, HTTP_SERVER_URL, sizeof(rtConfig.httpServerUrl));
  strlcpy(rtConfig.httpApiKey, HTTP_API_KEY, sizeof(rtConfig.httpApiKey));
  strlcpy(rtConfig.deviceId, DEVICE_ID, sizeof(rtConfig.deviceId));
  rtConfig.simSlot = constrain(SIM_SLOT, 1, 2);
  rtConfig.enableWecom = ENABLE_WECOM_BOT;
  rtConfig.enableEmail = ENABLE_EMAIL;
  rtConfig.enableHttp = ENABLE_HTTP_SERVER;
  rtConfig.smsWebMode = HTTP_SMS_WEB_MODE;
  strlcpy(rtConfig.webUser, WEB_ADMIN_USER, sizeof(rtConfig.webUser));
  strlcpy(rtConfig.webPass, WEB_ADMIN_PASS, sizeof(rtConfig.webPass));
}

void loadConfig() {
  // config.h 是默认值；只有 NVS 键存在时才覆盖。
  loadCompileTimeDefaults();
  if (!preferences.begin("sms_config", true)) {
    Serial.println("❌ 无法以只读模式打开 NVS，使用 config.h 默认值");
    return;
  }
  
  // 加载配置，如果不存在则使用默认值
  strlcpy(rtConfig.wecomUrl, preferences.getString("wecomUrl", WECHAT_WEBHOOK_URL).c_str(), sizeof(rtConfig.wecomUrl));
  strlcpy(rtConfig.simNumber, preferences.getString("simNumber", LOCAL_SIM_NUMBER).c_str(), sizeof(rtConfig.simNumber));
  strlcpy(rtConfig.smtpServer, preferences.getString("smtpServer", SMTP_SERVER).c_str(), sizeof(rtConfig.smtpServer));
  rtConfig.smtpPort = preferences.getUShort("smtpPort", SMTP_SERVER_PORT);
  strlcpy(rtConfig.smtpUser, preferences.getString("smtpUser", SMTP_USER).c_str(), sizeof(rtConfig.smtpUser));
  strlcpy(rtConfig.smtpPass, preferences.getString("smtpPass", SMTP_PASS).c_str(), sizeof(rtConfig.smtpPass));
  strlcpy(rtConfig.smtpTo, preferences.getString("smtpTo", SMTP_SEND_TO).c_str(), sizeof(rtConfig.smtpTo));
  strlcpy(rtConfig.httpServerUrl, preferences.getString("httpUrl", HTTP_SERVER_URL).c_str(), sizeof(rtConfig.httpServerUrl));
  strlcpy(rtConfig.httpApiKey, preferences.getString("httpKey", HTTP_API_KEY).c_str(), sizeof(rtConfig.httpApiKey));
  strlcpy(rtConfig.deviceId, preferences.getString("deviceId", DEVICE_ID).c_str(), sizeof(rtConfig.deviceId));
  rtConfig.simSlot = constrain(preferences.getUChar("simSlot", SIM_SLOT), 1, 2);
  rtConfig.enableWecom = preferences.getBool("enWecom", ENABLE_WECOM_BOT);
  rtConfig.enableEmail = preferences.getBool("enEmail", ENABLE_EMAIL);
  rtConfig.enableHttp = preferences.getBool("enHttp", ENABLE_HTTP_SERVER);
  rtConfig.smsWebMode = preferences.getBool("smsWeb", HTTP_SMS_WEB_MODE);
  strlcpy(rtConfig.webUser, preferences.getString("webUser", WEB_ADMIN_USER).c_str(), sizeof(rtConfig.webUser));
  strlcpy(rtConfig.webPass, preferences.getString("webPass", WEB_ADMIN_PASS).c_str(), sizeof(rtConfig.webPass));
  strlcpy(rtConfig.adminPhone, preferences.getString("adminPhone", "").c_str(), sizeof(rtConfig.adminPhone));
  strlcpy(rtConfig.numberBlacklist, preferences.getString("blacklist", "").c_str(), sizeof(rtConfig.numberBlacklist));
  loadPushChannelsFromNvs();
  
  preferences.end();
  Serial.println("配置已从 NVS 加载");
}

bool verifyStoredConfig() {
  if (!preferences.begin("sms_config", true)) return false;
  bool ok =
    preferences.getString("wecomUrl", "") == rtConfig.wecomUrl &&
    preferences.getString("simNumber", "") == rtConfig.simNumber &&
    preferences.getString("smtpServer", "") == rtConfig.smtpServer &&
    preferences.getUShort("smtpPort", 0) == rtConfig.smtpPort &&
    preferences.getString("smtpUser", "") == rtConfig.smtpUser &&
    preferences.getString("smtpPass", "") == rtConfig.smtpPass &&
    preferences.getString("smtpTo", "") == rtConfig.smtpTo &&
    preferences.getString("httpUrl", "") == rtConfig.httpServerUrl &&
    preferences.getString("httpKey", "") == rtConfig.httpApiKey &&
    preferences.getString("deviceId", "") == rtConfig.deviceId &&
    preferences.getUChar("simSlot", 0) == rtConfig.simSlot &&
    preferences.getBool("enWecom", !rtConfig.enableWecom) == rtConfig.enableWecom &&
    preferences.getBool("enEmail", !rtConfig.enableEmail) == rtConfig.enableEmail &&
    preferences.getBool("enHttp", !rtConfig.enableHttp) == rtConfig.enableHttp &&
    preferences.getBool("smsWeb", !rtConfig.smsWebMode) == rtConfig.smsWebMode &&
    preferences.getString("webUser", "") == rtConfig.webUser &&
    preferences.getString("webPass", "") == rtConfig.webPass &&
    preferences.getString("adminPhone", "") == rtConfig.adminPhone &&
    preferences.getString("blacklist", "") == rtConfig.numberBlacklist;
  char key[12];
  for (uint8_t i = 0; ok && i < MAX_PUSH_CHANNELS; ++i) {
    const PushChannelConfig &ch = rtConfig.pushChannels[i];
    snprintf(key, sizeof(key), "p%ue", i); ok = preferences.getBool(key, !ch.enabled) == ch.enabled;
    snprintf(key, sizeof(key), "p%ut", i); ok = ok && preferences.getUChar(key, 255) == ch.type;
    snprintf(key, sizeof(key), "p%un", i); ok = ok && preferences.getString(key, "") == ch.name;
    snprintf(key, sizeof(key), "p%uu", i); ok = ok && preferences.getString(key, "") == ch.url;
    snprintf(key, sizeof(key), "p%ua", i); ok = ok && preferences.getString(key, "") == ch.key1;
    snprintf(key, sizeof(key), "p%ub", i); ok = ok && preferences.getString(key, "") == ch.key2;
    snprintf(key, sizeof(key), "p%uc", i); ok = ok && preferences.getString(key, "") == ch.customBody;
  }
  preferences.end();
  return ok;
}

bool saveConfig() {
  if (!preferences.begin("sms_config", false)) {
    Serial.println("❌ 无法以读写模式打开 NVS");
    return false;
  }

  bool ok = true;
  preferences.putString("wecomUrl", rtConfig.wecomUrl);
  preferences.putString("simNumber", rtConfig.simNumber);
  preferences.putString("smtpServer", rtConfig.smtpServer);
  ok = preferences.putUShort("smtpPort", rtConfig.smtpPort) > 0 && ok;
  preferences.putString("smtpUser", rtConfig.smtpUser);
  preferences.putString("smtpPass", rtConfig.smtpPass);
  preferences.putString("smtpTo", rtConfig.smtpTo);
  preferences.putString("httpUrl", rtConfig.httpServerUrl);
  preferences.putString("httpKey", rtConfig.httpApiKey);
  preferences.putString("deviceId", rtConfig.deviceId);
  ok = preferences.putUChar("simSlot", rtConfig.simSlot) > 0 && ok;
  ok = preferences.putBool("enWecom", rtConfig.enableWecom) > 0 && ok;
  ok = preferences.putBool("enEmail", rtConfig.enableEmail) > 0 && ok;
  ok = preferences.putBool("enHttp", rtConfig.enableHttp) > 0 && ok;
  ok = preferences.putBool("smsWeb", rtConfig.smsWebMode) > 0 && ok;
  preferences.putString("webUser", rtConfig.webUser);
  preferences.putString("webPass", rtConfig.webPass);
  preferences.putString("adminPhone", rtConfig.adminPhone);
  preferences.putString("blacklist", rtConfig.numberBlacklist);
  savePushChannelsToNvs();
  preferences.end();

  if (ok) ok = verifyStoredConfig();
  Serial.println(ok ? "✅ 配置已写入 NVS 并通过回读校验" : "❌ NVS 配置写入或回读校验失败");
  return ok;
}

bool resetConfig() {
  if (!preferences.begin("sms_config", false)) {
    Serial.println("❌ 无法打开 NVS，恢复默认配置失败");
    return false;
  }
  bool ok = preferences.clear();
  preferences.end();
  if (ok) {
    loadCompileTimeDefaults();
    Serial.println("✅ NVS 配置已清空，已恢复 config.h 默认值");
  } else {
    Serial.println("❌ NVS 配置清空失败");
  }
  return ok;
}

// HTML 编码函数（防止 XSS）
String htmlEncode(const String& str) {
  String result = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    switch (c) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '"': result += "&quot;"; break;
      case '\'': result += "&#39;"; break;
      default: result += c;
    }
  }
  return result;
}

// HTTP Basic 认证检查
// 本地 Base64 解码（返回解码后的字符串）
String base64Decode(const String& input) {
  auto idx = [](char c)->int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };

  String out = "";
  int len = input.length();
  int i = 0;
  while (i < len) {
    int vals[4] = {0,0,0,0};
    int vcount = 0;
    int pad = 0;
    for (int j = 0; j < 4 && i < len; ++j, ++i) {
      char c = input.charAt(i);
      if (c == '=') { vals[j] = 0; pad++; vcount++; continue; }
      int v = idx(c);
      if (v < 0) { --j; continue; } // skip invalid chars
      vals[j] = v;
      vcount++;
    }
    if (vcount == 0) break;
    out += (char)((vals[0] << 2) | ((vals[1] & 0x30) >> 4));
    if (pad < 2) out += (char)(((vals[1] & 0x0F) << 4) | ((vals[2] & 0x3C) >> 2));
    if (pad < 1) out += (char)(((vals[2] & 0x03) << 6) | (vals[3] & 0x3F));
  }
  return out;
}

bool checkAuth() {
  if (!webServer.hasHeader("Authorization")) {
    return false;
  }
  String authHeader = webServer.header("Authorization");
  if (!authHeader.startsWith("Basic ")) {
    return false;
  }
  String encoded = authHeader.substring(6);
  String decoded = base64Decode(encoded);

  String expected = String(rtConfig.webUser) + ":" + String(rtConfig.webPass);
  return decoded == expected;
}

void requestAuth() {
  webServer.sendHeader("WWW-Authenticate", "Basic realm=\"SMS Forwarder\"");
  webServer.send(401, "text/plain", "Authentication Required");
}


bool postHttpJson(String& jsonData) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HTTP 推送失败: WiFi 未连接");
    return false;
  }
  HTTPClient http;
  http.begin(rtConfig.httpServerUrl);
  http.useHTTP10(true); // 关闭长连接，减少长时间占用的 socket/堆内存
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  if (rtConfig.httpApiKey[0]) http.addHeader("X-API-Key", rtConfig.httpApiKey);

  int httpCode = http.POST(jsonData);
  bool ok = httpCode >= 200 && httpCode < 300;
  if (!ok && httpCode <= 0) {
    Serial.printf("HTTP请求失败: %s\n", http.errorToString(httpCode).c_str());
  } else {
    Serial.printf("HTTP 响应码: %d\n", httpCode);
  }
  http.end();
  return ok;
}

String urlEncode(const char* value) {
  static const char hex[] = "0123456789ABCDEF";
  String result;
  result.reserve(strlen(value) * 2);
  while (*value) {
    uint8_t c = (uint8_t)*value++;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += (char)c;
    else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}

String hmacSha256Base64(const String& secret, const String& message) {
  uint8_t digest[32];
  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md_setup(&context, info, 1) != 0) {
    mbedtls_md_free(&context);
    return "";
  }
  mbedtls_md_hmac_starts(&context, (const unsigned char*)secret.c_str(), secret.length());
  mbedtls_md_hmac_update(&context, (const unsigned char*)message.c_str(), message.length());
  mbedtls_md_hmac_finish(&context, digest);
  mbedtls_md_free(&context);
  return base64::encode(digest, sizeof(digest));
}

bool isPushChannelValid(const PushChannelConfig& channel) {
  if (!channel.enabled || channel.type == PUSH_NONE || channel.type > PUSH_TELEGRAM) return false;
  if ((channel.type == PUSH_PUSHPLUS || channel.type == PUSH_SERVERCHAN) && !channel.key1[0]) return false;
  if (channel.type == PUSH_TELEGRAM && (!channel.key1[0] || !channel.key2[0])) return false;
  if (channel.type == PUSH_GOTIFY && (!channel.url[0] || !channel.key1[0])) return false;
  return channel.url[0] || channel.type == PUSH_PUSHPLUS || channel.type == PUSH_SERVERCHAN || channel.type == PUSH_TELEGRAM;
}

bool sendPushChannel(const PushChannelConfig& channel, const char* sender,
                     const char* message, const char* timestamp) {
  if (!isPushChannelValid(channel)) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  String senderJson = escapeJson(sender);
  String messageJson = escapeJson(message);
  String timeJson = escapeJson(timestamp ? timestamp : "");
  String url = channel.url;
  String body;
  String contentType = "application/json";
  bool useGet = false;

  switch ((PushType)channel.type) {
    case PUSH_POST_JSON:
      body = "{\"sender\":\"" + senderJson + "\",\"message\":\"" + messageJson + "\",\"timestamp\":\"" + timeJson + "\"}";
      break;
    case PUSH_BARK:
      body = "{\"title\":\"" + senderJson + "\",\"body\":\"" + messageJson + "\"}";
      break;
    case PUSH_GET:
      url += (url.indexOf('?') >= 0 ? '&' : '?');
      url += "sender=" + urlEncode(sender) + "&message=" + urlEncode(message) + "&timestamp=" + urlEncode(timestamp ? timestamp : "");
      useGet = true;
      break;
    case PUSH_DINGTALK: {
      if (channel.key1[0]) {
        int64_t ms = (int64_t)time(nullptr) * 1000LL;
        String sign = hmacSha256Base64(channel.key1, String(ms) + "\n" + channel.key1);
        url += (url.indexOf('?') >= 0 ? '&' : '?');
        url += "timestamp=" + String(ms) + "&sign=" + urlEncode(sign.c_str());
      }
      body = "{\"msgtype\":\"text\",\"text\":{\"content\":\"短信来自: " + senderJson + "\\n" + messageJson + "\\n" + timeJson + "\"}}";
      break;
    }
    case PUSH_PUSHPLUS:
      url = channel.url[0] ? channel.url : "https://www.pushplus.plus/send";
      body = "{\"token\":\"" + escapeJson(channel.key1) + "\",\"title\":\"短信来自: " + senderJson + "\",\"content\":\"" + messageJson + "\\n" + timeJson + "\"";
      if (channel.key2[0]) body += ",\"channel\":\"" + escapeJson(channel.key2) + "\"";
      body += "}";
      break;
    case PUSH_SERVERCHAN:
      if (!url.length()) url = "https://sctapi.ftqq.com/" + String(channel.key1) + ".send";
      contentType = "application/x-www-form-urlencoded";
      body = "title=" + urlEncode(("短信来自: " + String(sender)).c_str());
      body += "&desp=" + urlEncode((String(message) + "\n\n" + (timestamp ? timestamp : "")).c_str());
      break;
    case PUSH_CUSTOM:
      body = channel.customBody;
      body.replace("{sender}", senderJson);
      body.replace("{message}", messageJson);
      body.replace("{timestamp}", timeJson);
      break;
    case PUSH_FEISHU: {
      body = "{";
      if (channel.key1[0]) {
        time_t seconds = time(nullptr);
        String sign = hmacSha256Base64(String(seconds) + "\n" + channel.key1, "");
        body += "\"timestamp\":\"" + String(seconds) + "\",\"sign\":\"" + sign + "\",";
      }
      body += "\"msg_type\":\"text\",\"content\":{\"text\":\"短信来自: " + senderJson + "\\n" + messageJson + "\\n" + timeJson + "\"}}";
      break;
    }
    case PUSH_GOTIFY:
      if (!url.endsWith("/")) url += '/';
      url += "message?token=" + urlEncode(channel.key1);
      body = "{\"title\":\"短信来自: " + senderJson + "\",\"message\":\"" + messageJson + "\\n" + timeJson + "\",\"priority\":5}";
      break;
    case PUSH_TELEGRAM:
      if (!url.length()) url = "https://api.telegram.org";
      if (url.endsWith("/")) url.remove(url.length() - 1);
      url += "/bot" + String(channel.key2) + "/sendMessage";
      body = "{\"chat_id\":\"" + escapeJson(channel.key1) + "\",\"text\":\"短信来自: " + senderJson + "\\n" + messageJson + "\\n" + timeJson + "\"}";
      break;
    default:
      return true;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return false;
  http.useHTTP10(true);
  int status;
  if (useGet) status = http.GET();
  else {
    http.addHeader("Content-Type", contentType);
    status = http.POST(body);
  }
  http.end();
  bool ok = status >= 200 && status < 300;
  logLine("推送通道[%s] HTTP=%d", channel.name[0] ? channel.name : "未命名", status);
  return ok;
}

// 发送短信数据到服务器。sms_web 模式使用其开发板推送协议。
bool sendSMSToServer(const char* sender, const char* message, const char* timestamp,
                     uint16_t messageType = 501, const char* tid = NULL) {
  if (!rtConfig.enableHttp) return true;

  String jsonData;
  jsonData.reserve(strlen(message) + strlen(sender) + 180);
  if (rtConfig.smsWebMode) {
    jsonData = "{\"devId\":\"";
    jsonData += escapeJson(rtConfig.deviceId);
    jsonData += "\",\"type\":" + String(messageType);
    jsonData += ",\"slot\":" + String(rtConfig.simSlot);
    jsonData += ",\"phNum\":\"" + escapeJson(sender);
    jsonData += "\",\"smsBd\":\"" + escapeJson(message);
    jsonData += "\",\"msIsdn\":\"" + escapeJson(rtConfig.simNumber) + "\"";
    time_t smsTime = timestamp ? pduTimestampToUnix(timestamp) : time(NULL);
    if (smsTime > 0) jsonData += ",\"smsTs\":" + String((unsigned long)smsTime);
    if (tid && *tid) jsonData += ",\"tid\":\"" + escapeJson(tid) + "\"";
    jsonData += "}";
  } else {
    jsonData = "{\"sender\":\"" + escapeJson(sender);
    jsonData += "\",\"message\":\"" + escapeJson(message);
    jsonData += "\",\"timestamp\":\"" + escapeJson(timestamp ? timestamp : "") + "\"}";
  }
  return postHttpJson(jsonData);
}

bool sendSmsWebDeviceEvent(uint16_t type) {
  if (!rtConfig.enableHttp || !rtConfig.smsWebMode) return true;
  String jsonData;
  jsonData.reserve(180);
  jsonData = "{\"devId\":\"" + escapeJson(rtConfig.deviceId);
  jsonData += "\",\"type\":" + String(type);
  if (type == 100) {
    jsonData += ",\"ip\":\"" + WiFi.localIP().toString();
    jsonData += "\",\"ssid\":\"" + escapeJson(WiFi.SSID().c_str());
    jsonData += "\",\"dbm\":" + String(WiFi.RSSI());
    jsonData += ",\"hwVer\":\"ESP32-sms_forwarding\"";
  }
  jsonData += "}";
  return postHttpJson(jsonData);
}

// 通过企业微信机器人发送短信内容
// 发送到企业微信机器人，返回是否成功
bool sendSMSToWeComBot(const char* sender, const char* message, const char* timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("sendSMSToWeComBot: WiFi 未连接");
    return false;
  }
  if (!rtConfig.enableWecom) {
    return true;  // 未启用视为成功
  }

  HTTPClient http;
  http.begin(rtConfig.wecomUrl);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json; charset=utf-8");

  String content = "";
  content += "📩 【新短信提醒】\n";
  content += "📱 接收号码："; content += rtConfig.simNumber; content += "\n";
  content += "👤 发送者："; content += sender; content += "\n";
  content += "⏰ 时间："; content += formatTimestamp(timestamp); content += "\n";
  content += "📝 内容："; content += message;

  String escapedContent = escapeJson(content.c_str());

  String jsonData = "{";
  jsonData += "\"msgtype\":\"text\",";
  jsonData += "\"text\":{";
  jsonData += "\"content\":\"" + escapedContent + "\"";
  jsonData += "}";
  jsonData += "}";

  Serial.println("发送到企业微信机器人: " + jsonData);

  int httpCode = http.POST(jsonData);
  bool ok = false;
  if (httpCode > 0) {
    Serial.printf("WeCom HTTP 响应码: %d\n", httpCode);
    String resp = http.getString();
    Serial.println("WeCom 响应: " + resp);
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) ok = true;
  } else {
    Serial.printf("WeCom HTTP 请求失败: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
  return ok;
}

// 发送邮件通知，返回是否成功
bool sendSMSToEmail(const char* sender, const char* message, const char* timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("sendSMSToEmail: WiFi 未连接");
    return false;
  }
  if (!rtConfig.enableEmail) {
    return true;  // 未启用视为成功
  }
  auto statusCallback = [](SMTPStatus status) {
    Serial.println(status.text);
    esp_task_wdt_reset(); // 喂狗防止超时
  };
  smtp.connect(rtConfig.smtpServer, rtConfig.smtpPort, statusCallback);
  if (!smtp.isConnected()) {
    Serial.println("sendSMSToEmail: SMTP 连接失败");
    return false;
  }
  smtp.authenticate(rtConfig.smtpUser, rtConfig.smtpPass, readymail_auth_password);

  SMTPMessage msg;
  String from = "SMS Forwarder <"; from+=rtConfig.smtpUser; from+=">"; 
  msg.headers.add(rfc822_from, from.c_str());
  String to = "Recipient <"; to+=rtConfig.smtpTo; to+=">"; 
  msg.headers.add(rfc822_to, to.c_str());
  
  // 构建简洁的邮件主题：【短信转发】发送者号码
  String subject = "【短信转发】";
  subject += sender;
  msg.headers.add(rfc822_subject, subject.c_str());
  
  // 构建格式化的邮件正文
  String formattedTime = formatTimestamp(timestamp);
  String body = "";
  body += "短信内容："; body += message; body += "\n";  
  body += "📞 发送者："; body += sender; body += "\n";
  body += "🕐 时  间："; body += formattedTime; body += "\n";
  body += "📍 接收卡："; body += rtConfig.simNumber; body += "\n";
  body += "此邮件由 SMS Forwarder 自动发送\n";
  msg.text.body(body.c_str());
  
  // NTP 同步（带超时保护，最多等待 10 秒）
  configTime(0, 0, "ntp.ntsc.ac.cn");
  unsigned long ntpStart = millis();
  while (time(nullptr) < 100000) {
    if (millis() - ntpStart > 10000) {
      Serial.println("sendSMSToEmail: NTP 同步超时");
      return false;
    }
    delay(100);
    esp_task_wdt_reset(); // 喂狗防止超时
  }
  msg.timestamp = time(nullptr);
  esp_task_wdt_reset(); // 发送前喂狗
  bool res = smtp.send(msg);
  if (!res) Serial.println("sendSMSToEmail: 发送失败");
  return res;
}

// 读取串口一行（含回车换行），返回行字符串，无新行时返回空
String readSerialLine(HardwareSerial& port) {
  static char lineBuf[SERIAL_BUFFER_SIZE];
  static int linePos = 0;

  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      lineBuf[linePos] = 0;
      String res = String(lineBuf);
      linePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过\r
      if (linePos < SERIAL_BUFFER_SIZE - 1)
        lineBuf[linePos++] = c;
      else
        linePos = 0;  //超长报错保护，重头计
    }
  }
  return "";
}

// 检查字符串是否为有效的十六进制PDU数据
bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

String normalizedPhone(const char* phone) {
  String value = phone ? phone : "";
  value.trim();
  if (value.startsWith("+86")) value.remove(0, 3);
  return value;
}

bool isBlacklisted(const char* sender) {
  String target = normalizedPhone(sender);
  const char* cursor = rtConfig.numberBlacklist;
  while (*cursor) {
    while (*cursor == '\n' || *cursor == '\r' || *cursor == ',' || *cursor == ' ') ++cursor;
    const char* end = cursor;
    while (*end && *end != '\n' && *end != '\r' && *end != ',') ++end;
    String entry(cursor, end - cursor);
    entry.trim();
    if (normalizedPhone(entry.c_str()) == target && target.length()) return true;
    cursor = end;
  }
  return false;
}

bool resetModem() {
  logLine("正在通过 GPIO%d 硬重启模组", MODEM_EN_PIN);
  digitalWrite(MODEM_EN_PIN, LOW);
  delay(1500);
  digitalWrite(MODEM_EN_PIN, HIGH);
  delay(5000);
  bool ok = sendATandWaitOK("AT", 3000);
  if (ok) {
    sendATandWaitOK("AT+CNMI=2,2,0,0,0", 2000);
    sendATandWaitOK("AT+CMGF=0", 2000);
  }
  logLine("模组硬重启%s", ok ? "完成" : "后无响应");
  return ok;
}

bool handleAdminSms(const char* sender, const char* text) {
  if (!rtConfig.adminPhone[0] || normalizedPhone(sender) != normalizedPhone(rtConfig.adminPhone)) return false;
  String command = text;
  command.trim();
  if (command == "RESET") {
    logLine("收到管理员 RESET 命令");
    resetModem();
    delay(200);
    ESP.restart();
    return true;
  }
  if (command.startsWith("SMS:")) {
    int separator = command.indexOf(':', 4);
    if (separator <= 4) {
      logLine("管理员 SMS 命令格式错误");
      return true;
    }
    String phone = command.substring(4, separator);
    String content = command.substring(separator + 1);
    phone.trim(); content.trim();
    bool ok = phone.length() && content.length() && sendSMS(phone.c_str(), content.c_str());
    logLine("管理员远程发短信%s", ok ? "成功" : "失败");
    if (ok && rtConfig.enableHttp && rtConfig.smsWebMode) sendSMSToServer(phone.c_str(), content.c_str(), NULL, 502);
    return true;
  }
  return false;
}

void clearConcatMessage(ConcatSms& item) {
  memset(&item, 0, sizeof(item));
}

void deliverConcatMessage(ConcatSms& item, bool timedOut) {
  char fullText[SMS_TEXT_LEN] = {0};
  for (uint8_t i = 0; i < item.total && i < MAX_CONCAT_PARTS; ++i) {
    if (item.presentMask & (1U << i)) strlcat(fullText, item.parts[i], sizeof(fullText));
    else if (timedOut) {
      char missing[24];
      snprintf(missing, sizeof(missing), "[缺失分段%u]", (unsigned int)(i + 1));
      strlcat(fullText, missing, sizeof(fullText));
    }
  }
  logLine("长短信%s，分段 %u/%u", timedOut ? "超时" : "合并完成", item.received, item.total);
  processReceivedSMS(item.sender, fullText, item.timestamp);
  clearConcatMessage(item);
}

void processReceivedSegment(const char* sender, const char* text, const char* timestamp,
                            uint16_t reference, uint8_t part, uint8_t total) {
  if (total <= 1 || part == 0) {
    processReceivedSMS(sender, text, timestamp);
    return;
  }
  if (total > MAX_CONCAT_PARTS || part > total) {
    char marked[SMS_TEXT_LEN];
    snprintf(marked, sizeof(marked), "[长短信分段 %u/%u] %s", part, total, text);
    processReceivedSMS(sender, marked, timestamp);
    return;
  }

  ConcatSms* slot = NULL;
  ConcatSms* oldest = &concatMessages[0];
  for (uint8_t i = 0; i < MAX_CONCAT_MESSAGES; ++i) {
    ConcatSms &candidate = concatMessages[i];
    if (candidate.used && candidate.reference == reference &&
        normalizedPhone(candidate.sender) == normalizedPhone(sender)) {
      slot = &candidate;
      break;
    }
    if (!candidate.used && !slot) slot = &candidate;
    if (candidate.startedAt < oldest->startedAt) oldest = &candidate;
  }
  if (!slot || (slot->used && (slot->reference != reference || normalizedPhone(slot->sender) != normalizedPhone(sender)))) {
    logLine("长短信缓存已满，先转发最早的不完整消息");
    deliverConcatMessage(*oldest, true);
    slot = oldest;
  }
  if (!slot->used) {
    clearConcatMessage(*slot);
    slot->used = true;
    slot->reference = reference;
    slot->total = total;
    slot->startedAt = millis();
    strlcpy(slot->sender, sender, sizeof(slot->sender));
    strlcpy(slot->timestamp, timestamp ? timestamp : "", sizeof(slot->timestamp));
  }
  uint8_t index = part - 1;
  if (!(slot->presentMask & (1U << index))) {
    strlcpy(slot->parts[index], text, sizeof(slot->parts[index]));
    slot->presentMask |= (1U << index);
    ++slot->received;
  }
  if (slot->received >= slot->total) deliverConcatMessage(*slot, false);
}

void checkConcatTimeouts() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < MAX_CONCAT_MESSAGES; ++i) {
    if (concatMessages[i].used && now - concatMessages[i].startedAt >= CONCAT_TIMEOUT_MS)
      deliverConcatMessage(concatMessages[i], true);
  }
}

// 处理接收到的短信内容（分发到各渠道）
void processReceivedSMS(const char* sender, const char* text, const char* timestamp) {
  logLine("收到短信 [%s]: %.160s", sender ? sender : "", text ? text : "");
  if (isBlacklisted(sender)) {
    logLine("短信已被号码黑名单过滤: %s", sender);
    return;
  }
  if (handleAdminSms(sender, text)) return;

  // 各渠道发送状态
  bool wecomOk = true, emailOk = true, httpOk = true;
  uint16_t pushMask = 0;
  if (rtConfig.enableWecom) {
    wecomOk = sendSMSToWeComBot(sender, text, timestamp);
  }
  if (rtConfig.enableHttp) {
    httpOk = sendSMSToServer(sender, text, timestamp);
  }
  if (rtConfig.enableEmail) {
    emailOk = sendSMSToEmail(sender, text, timestamp);
  }
  for (uint8_t i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    if (!isPushChannelValid(rtConfig.pushChannels[i]) || sendPushChannel(rtConfig.pushChannels[i], sender, text, timestamp))
      pushMask |= (1U << i);
  }
  // 只有存在失败的渠道才入队，并记录各渠道状态
  bool needRetry = (rtConfig.enableWecom && !wecomOk) || 
                   (rtConfig.enableEmail && !emailOk) || 
                   (rtConfig.enableHttp && !httpOk);
  for (uint8_t i = 0; i < MAX_PUSH_CHANNELS; ++i)
    if (isPushChannelValid(rtConfig.pushChannels[i]) && !(pushMask & (1U << i))) needRetry = true;
  if (needRetry) {
    Serial.println("部分或全部发送失败，入队以便重试");
    enqueueSMSWithStatus(sender, text, timestamp, wecomOk, emailOk, httpOk, pushMask);
  }
}

// 处理URC和PDU
void checkSerial1URC() {
  static enum { IDLE,
                WAIT_PDU } state = IDLE;

  String line = readSerialLine(Serial1);
  if (line.length() == 0) return;

  // 打印到调试串口
  logLine("MODEM: %.220s", line.c_str());

  if (state == IDLE) {
    // 检测到短信上报URC头
    if (line.startsWith("+CMT:")) {
      Serial.println("检测到+CMT，等待PDU数据...");
      state = WAIT_PDU;
    }
  } else if (state == WAIT_PDU) {
    // 跳过空行
    if (line.length() == 0) {
      return;
    }
    
    // 如果是十六进制字符串，认为是PDU数据
    if (isHexString(line)) {
      Serial.println("收到PDU数据: " + line);
      Serial.println("PDU长度: " + String(line.length()) + " 字符");
      
      // 解析PDU
      if (!pdu.decodePDU(line.c_str())) {
        Serial.println("❌ PDU解析失败！");
      } else {
        Serial.println("✓ PDU解析成功");
        Serial.println("=== 短信内容 ===");
        Serial.println("发送者: " + String(pdu.getSender()));
        Serial.println("时间戳: " + String(pdu.getTimeStamp()));
        Serial.println("内容: " + String(pdu.getText()));
        Serial.println("===============");

        int* concat = pdu.getConcatInfo();
        uint16_t reference = concat ? concat[0] : 0;
        uint8_t part = concat ? concat[1] : 0;
        uint8_t total = concat ? concat[2] : 1;
        processReceivedSegment(pdu.getSender(), pdu.getText(), pdu.getTimeStamp(), reference, part, total);
      }
      
      // 返回IDLE状态
      state = IDLE;
    } 
    // 如果是其他内容（OK、ERROR等），也返回IDLE
    else {
      Serial.println("收到非PDU数据，返回IDLE状态");
      state = IDLE;
    }
  }
}

void blink_short(unsigned long gap_time = 500) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) return true;
      if (resp.indexOf("ERROR") >= 0) return false;
    }
  }
  return false;
}

String sendATCommand(const char* command, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(command);
  String response;
  response.reserve(512);
  unsigned long started = millis();
  unsigned long lastByte = started;
  bool terminal = false;
  while (millis() - started < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (response.length() < 1024) response += c;
      lastByte = millis();
      if (response.indexOf("\r\nOK\r\n") >= 0 || response.indexOf("\r\nERROR\r\n") >= 0) terminal = true;
    }
    if (terminal && millis() - lastByte > 150) break;
    esp_task_wdt_reset();
    delay(1);
  }
  logLine("AT命令: %s", command);
  return response;
}

bool waitCGATT1() {
  Serial1.println("AT+CGATT?");
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 2000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("+CGATT: 1") >= 0) return true;
      if (resp.indexOf("+CGATT: 0") >= 0) return false;
    }
  }
  return false;
}

void setup() {
  // 记录启动时间
  bootTime = millis();
  
  // 初始化看门狗（防止死锁）- 兼容 ESP-IDF 5.x
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);
  
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(MODEM_EN_PIN, OUTPUT);
  digitalWrite(MODEM_EN_PIN, HIGH);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  
  // 加载持久化配置
  loadConfig();
  
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASS);
  Serial.println("连接wifi");
  unsigned long wifiStarted = millis();
  while (WiFiMulti.run() != WL_CONNECTED && millis() - wifiStarted < 30000UL) {
    esp_task_wdt_reset();
    blink_short(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 连接超时，重启后重试");
    delay(100);
    ESP.restart();
  }
  Serial.println("wifi已连接");
  Serial.print("IP 地址: ");
  Serial.println(WiFi.localIP());
  
  // 启动 Web 服务器
  setupWebServer();
  sendSmsWebDeviceEvent(100); // 让 sms_web 建立设备记录并保存可控 IP
  lastSmsWebHeartbeat = millis();
  
  ssl_client.setInsecure();
  ssl_client.setTimeout(30000); // 设置 SSL 超时 30s，防止网络卡死触发看门狗
  unsigned long modemStarted = millis();
  while (!sendATandWaitOK("AT", 1000) && millis() - modemStarted < 30000UL) {
    Serial.println("AT未响应，重试...");
    webServer.handleClient();
    esp_task_wdt_reset();
    blink_short();
  }
  bool modemReady = millis() - modemStarted < 30000UL;
  Serial.println(modemReady ? "模组AT响应正常" : "模组无响应，Web 管理仍可使用");
  if (!modemReady) return;
  sendATandWaitOK("AT+CMGF=0", 1000);
  modemStarted = millis();
  while (!sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000) && millis() - modemStarted < 30000UL) {
    Serial.println("设置CNMI失败，重试...");
    webServer.handleClient();
    esp_task_wdt_reset();
    blink_short();
  }
  Serial.println("CNMI参数设置完成");
  modemStarted = millis();
  while (!waitCGATT1() && millis() - modemStarted < 90000UL) {
    Serial.println("等待CGATT附着...");
    webServer.handleClient();
    esp_task_wdt_reset();
    blink_short();
  }
  Serial.println(millis() - modemStarted < 90000UL ? "CGATT已附着" : "网络注册超时，后台继续运行");
  digitalWrite(LED_BUILTIN, LOW);
}

// 堆内存监控间隔
static unsigned long lastHeapCheck = 0;
#define HEAP_CHECK_INTERVAL_MS (12UL * 60UL * 60UL * 1000UL) // 12 小时

// 计算运行时间（天）
unsigned long getUptimeDays() {
  return (millis() - bootTime) / (24UL * 60UL * 60UL * 1000UL);
}

void loop() {
  // 喂狗
  esp_task_wdt_reset();
  
  // 处理 Web 请求
  webServer.handleClient();
  
  // 定时重启检查（仅在队列为空时重启，避免丢失数据）
  if ((millis() - bootTime) >= SCHEDULED_RESTART_INTERVAL_MS && sms_q_count == 0) {
    Serial.println("\n🔄 已运行7天，执行计划重启以保持系统稳定...");
    Serial.flush();
    delay(100);
    ESP.restart();
  }
  
  // 本地透传
  if (Serial.available()) Serial1.write(Serial.read());
  // 尝试保持 WiFi 连接
  ensureWiFiConnected();
  if (rtConfig.enableHttp && rtConfig.smsWebMode &&
      (millis() - lastSmsWebHeartbeat) >= SMS_WEB_HEARTBEAT_MS) {
    lastSmsWebHeartbeat = millis();
    sendSmsWebDeviceEvent(998);
  }
  // 处理队列中的待重试短信
  processSMSQueue();
  checkConcatTimeouts();
  // 检查URC和解析
  checkSerial1URC();
  
  // 定期检查堆内存（监控碎片化）
  unsigned long now = millis();
  if ((now - lastHeapCheck) > HEAP_CHECK_INTERVAL_MS) {
    lastHeapCheck = now;
    Serial.printf("[健康检查] 运行: %lu 天, 空闲堆: %u 字节, 最大连续块: %u 字节, 队列: %d 条\n",
                  getUptimeDays(), ESP.getFreeHeap(), ESP.getMaxAllocHeap(), sms_q_count);
    // 如果碎片化严重（最大块 < 空闲的 30%），提前重启
    if (ESP.getMaxAllocHeap() < ESP.getFreeHeap() / 3 && sms_q_count == 0) {
      Serial.println("⚠️ 堆碎片化严重，提前重启...");
      Serial.flush();
      delay(100);
      ESP.restart();
    }
  }
}

// 队列操作函数
void enqueueSMS(const char* sender, const char* text, const char* timestamp) {
  // 默认所有渠道都未发送成功
  enqueueSMSWithStatus(sender, text, timestamp, false, false, false);
}

// 带渠道状态的入队函数
void enqueueSMSWithStatus(const char* sender, const char* text, const char* timestamp, bool wecomOk, bool emailOk, bool httpOk, uint16_t pushMask) {
  int insertIdx = (sms_q_head + sms_q_count) % SMS_QUEUE_SIZE;
  if (sms_q_count == SMS_QUEUE_SIZE) {
    // 队列已满，丢弃最老一条以腾出空间
    Serial.println("短信队列已满，丢弃最老一条条目");
    smsQueue[sms_q_head].valid = false;
    sms_q_head = (sms_q_head + 1) % SMS_QUEUE_SIZE;
    sms_q_count--;
    insertIdx = (sms_q_head + sms_q_count) % SMS_QUEUE_SIZE;
  }
  // 使用 strncpy 安全拷贝，确保 null 结尾
  strncpy(smsQueue[insertIdx].sender, sender, SMS_SENDER_LEN - 1);
  smsQueue[insertIdx].sender[SMS_SENDER_LEN - 1] = '\0';
  strncpy(smsQueue[insertIdx].text, text, SMS_TEXT_LEN - 1);
  smsQueue[insertIdx].text[SMS_TEXT_LEN - 1] = '\0';
  strncpy(smsQueue[insertIdx].timestamp, timestamp, SMS_TIMESTAMP_LEN - 1);
  smsQueue[insertIdx].timestamp[SMS_TIMESTAMP_LEN - 1] = '\0';
  smsQueue[insertIdx].retries = 0;
  smsQueue[insertIdx].lastAttempt = 0;
  smsQueue[insertIdx].valid = true;
  // 记录各渠道发送状态（true=已成功，无需重试）
  smsQueue[insertIdx].wecomSent = wecomOk;
  smsQueue[insertIdx].emailSent = emailOk;
  smsQueue[insertIdx].httpSent = httpOk;
  smsQueue[insertIdx].pushSentMask = pushMask;
  sms_q_count++;
  Serial.printf("已入队，队列长度=%d\n", sms_q_count);
}

void removeHeadSMS() {
  if (sms_q_count == 0) return;
  smsQueue[sms_q_head].valid = false;
  sms_q_head = (sms_q_head + 1) % SMS_QUEUE_SIZE;
  sms_q_count--;
}

// 尝试发送单条短信到未成功的渠道，返回是否全部成功
// 注意：只重试之前失败的渠道，避免重复发送
bool trySendChannels(SMSItem &item) {
  bool allOk = true;
  if (rtConfig.enableWecom && !item.wecomSent) {
    if (sendSMSToWeComBot(item.sender, item.text, item.timestamp)) {
      item.wecomSent = true;  // 标记为已成功
      Serial.println("企业微信发送成功");
    } else {
      allOk = false;
    }
  }
  if (rtConfig.enableHttp && !item.httpSent) {
    if (sendSMSToServer(item.sender, item.text, item.timestamp)) {
      item.httpSent = true;  // 标记为已成功
      Serial.println("HTTP服务器发送成功");
    } else {
      allOk = false;
    }
  }
  if (rtConfig.enableEmail && !item.emailSent) {
    if (sendSMSToEmail(item.sender, item.text, item.timestamp)) {
      item.emailSent = true;  // 标记为已成功
      Serial.println("邮件发送成功");
    } else {
      allOk = false;
    }
  }
  for (uint8_t i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    if (!isPushChannelValid(rtConfig.pushChannels[i])) continue;
    if (!(item.pushSentMask & (1U << i))) {
      if (sendPushChannel(rtConfig.pushChannels[i], item.sender, item.text, item.timestamp))
        item.pushSentMask |= (1U << i);
      else
        allOk = false;
    }
  }
  return allOk;
}

// 处理队列：在 WiFi 已连接时重试队列中的短信
void processSMSQueue() {
  if (sms_q_count == 0) return;
  if (WiFi.status() != WL_CONNECTED) return; // 未连接时不处理（要等重连）

  int checked = 0;
  // 逐个检查队列条目（注意：可能在循环中移除head）
  while (checked < sms_q_count) {
    int idx = (sms_q_head + checked) % SMS_QUEUE_SIZE;
    SMSItem &it = smsQueue[idx];
    unsigned long now = millis();
    if (it.lastAttempt == 0 || (now - it.lastAttempt) >= SMS_RETRY_INTERVAL_MS) {
      Serial.printf("尝试重发队列第%d项，已重试%d次\n", checked + 1, it.retries);
      bool ok = trySendChannels(it);
      it.lastAttempt = now;
      if (ok) {
        // 成功发送，移除该项（如果是head则直接移除，否则需要移动元素）
        if (idx == sms_q_head) {
          removeHeadSMS();
        } else {
          // 将后面的元素前移一位
          int cur = idx;
          while (cur != (sms_q_head + sms_q_count - 1) % SMS_QUEUE_SIZE) {
            int next = (cur + 1) % SMS_QUEUE_SIZE;
            smsQueue[cur] = smsQueue[next];
            cur = next;
          }
          // 删除尾部
          sms_q_count--;
        }
        // 不增加 checked，因为队列缩短了，继续检查同位置
        continue;
      } else {
        it.retries++;
        if (it.retries >= SMS_MAX_RETRIES) {
          Serial.println("队列项达到最大重试次数，丢弃");
          if (idx == sms_q_head) removeHeadSMS();
          else {
            int cur = idx;
            while (cur != (sms_q_head + sms_q_count - 1) % SMS_QUEUE_SIZE) {
              int next = (cur + 1) % SMS_QUEUE_SIZE;
              smsQueue[cur] = smsQueue[next];
              cur = next;
            }
            sms_q_count--;
          }
          continue; // 继续，不增加 checked
        }
      }
    }
    checked++;
  }
}

// WiFi 自动重连（指数回退，处理 millis 溢出）
void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  // 使用无符号减法自动处理溢出
  if ((now - lastWifiAttempt) < wifiReconnectInterval) return;
  lastWifiAttempt = now;
  Serial.println("尝试重连 WiFi...");
  if (WiFiMulti.run() == WL_CONNECTED) {
    Serial.println("WiFi 已重连");
    // reset interval
    wifiReconnectInterval = 5000;
    sendSmsWebDeviceEvent(100);
    lastSmsWebHeartbeat = millis();
  } else {
    // 增长间隔，最大 60s
    wifiReconnectInterval = min(wifiReconnectInterval * 2, 60000UL);
    Serial.printf("WiFi 重连失败，下次间隔 %lu ms\n", wifiReconnectInterval);
  }
}

// ----------------- Web 管理与短信发送功能 -----------------

// 发送短信到手机（使用模组的文本模式 AT+CMGF）
bool sendSMS(const char* phoneNumber, const char* message) {
  // 确保串口空
  while (Serial1.available()) Serial1.read();
  Serial.printf("发送短信: 到 %s, 内容: %s\n", phoneNumber, message);

  // 设置文本模式
  Serial1.println("AT+CMGF=1");
  unsigned long start = millis();
  String resp = "";
  bool ok = false;
  while (millis() - start < 2000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) { ok = true; break; }
      if (resp.indexOf("ERROR") >= 0) { ok = false; break; }
    }
    if (ok) break;
  }
  if (!ok) {
    Serial.println("模组设置文本模式失败");
    return false;
  }

  // 发送短信命令
  resp = "";
  String cmd = String("AT+CMGS=\"") + phoneNumber + "\"";
  Serial1.println(cmd);
  start = millis();
  bool prompt = false;
  while (millis() - start < 5000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      // 有些模组返回 '>' 提示输入短信内容
      if (resp.indexOf('>') >= 0) { prompt = true; break; }
      if (resp.indexOf("ERROR") >= 0) { break; }
    }
    if (prompt) break;
  }
  if (!prompt) {
    Serial.println("模组未返回输入提示，发送失败");
    return false;
  }

  // 发送消息体并以 Ctrl+Z 终止
  Serial1.print(message);
  Serial1.write((char)26);

  // 等待 +CMGS 或 OK
  resp = "";
  start = millis();
  bool sent = false;
  while (millis() - start < 15000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("+CMGS:") >= 0 || resp.indexOf("OK") >= 0) { sent = true; break; }
      if (resp.indexOf("ERROR") >= 0) { sent = false; break; }
    }
    if (sent) break;
  }
  Serial.printf("模组返回: %s\n", resp.c_str());
  return sent;
}

// 生成 HTML 头部（含 CSS 样式）
String getHeader(String title) {
  String h = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>" + title + "</title>";
  h += "<style>";
  h += "body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,Helvetica,Arial,sans-serif;max-width:800px;margin:0 auto;padding:10px;background:#f0f2f5;color:#333}";
  h += ".card{background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);margin-bottom:20px}";
  h += "h2{margin-top:0;color:#1a73e8;border-bottom:2px solid #1a73e8;padding-bottom:10px}";
  h += "input[type=text],input[type=password],input[type=number],textarea,select{width:100%;padding:10px;margin:5px 0 15px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;font-size:14px}";
  h += "input[type=submit],button{background:#1a73e8;color:#fff;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;font-size:16px;width:100%}";
  h += "input[type=submit]:hover{background:#1557b0}";
  h += "table{width:100%;border-collapse:collapse;margin-top:10px}th,td{padding:12px;border-bottom:1px solid #ddd;text-align:left}th{background:#f8f9fa}";
  h += ".nav{margin-bottom:20px;background:#fff;padding:15px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}";
  h += ".nav a{margin-right:20px;text-decoration:none;color:#1a73e8;font-weight:600;font-size:16px}.nav a:hover{text-decoration:underline}";
  h += "label{font-weight:600;display:block;margin-bottom:5px;color:#555}.check-group{margin-bottom:15px;display:flex;align-items:center;background:#f8f9fa;padding:10px;border-radius:4px}.check-group input{width:auto;margin:0 10px 0 0}";
  h += ".status-ok{color:#28a745;font-weight:bold}.status-err{color:#dc3545;font-weight:bold}";
  h += "</style></head><body>";
  h += "<div class='nav'><a href='/'>🏠 仪表盘</a><a href='/config'>⚙️ 配置</a><a href='/queue'>📨 队列</a><a href='/modem'>📶 模组</a><a href='/logs'>📜 日志</a></div>";
  return h;
}

// 生成 HTML 尾部
String getFooter() {
  return "<div style='text-align:center;margin-top:30px;color:#888;font-size:12px'>SMS Forwarder</div></body></html>";
}

// Web 页面：根目录（仪表盘 + 发送表单）
void handleRoot() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = getHeader("SMS Forwarder - 仪表盘");
  html += "<div class='card'><h2>发送短信</h2>";
  html += "<form method=\"POST\" action=\"/send\">";
  html += "<label>目标号码</label><input name=\"to\" placeholder=\"输入手机号\">";
  html += "<label>消息内容</label><textarea name=\"msg\" rows=5 placeholder=\"输入短信内容\"></textarea>";
  html += "<input type=\"submit\" value=\"发送短信\">";
  html += "</form></div>";

  html += "<div class='card'><h2>模拟接收短信 (测试)</h2>";
  html += "<form method=\"POST\" action=\"/simulate\">";
  html += "<label>模拟发送者</label><input name=\"sender\" value=\"10086\">";
  html += "<label>模拟内容</label><textarea name=\"text\" rows=3>这是一条测试短信</textarea>";
  html += "<input type=\"submit\" value=\"模拟接收\">";
  html += "</form></div>";

  html += getFooter();
  webServer.send(200, "text/html", html);
}

// 发送接口（表单提交）
void handleSend() {
  if (!checkAuth()) { requestAuth(); return; }
  if (webServer.method() != HTTP_POST) {
    webServer.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  String to = webServer.arg("to");
  String msg = webServer.arg("msg");
  if (to.length() == 0 || msg.length() == 0) {
    webServer.send(400, "text/plain; charset=utf-8", "参数缺失");
    return;
  }
  bool ok = sendSMS(to.c_str(), msg.c_str());
  if (ok && rtConfig.enableHttp && rtConfig.smsWebMode) {
    sendSMSToServer(to.c_str(), msg.c_str(), NULL, 502);
  }
  
  String html = getHeader("发送结果");
  html += "<div class='card' style='text-align:center'><h2>" + String(ok ? "✅ 发送成功" : "❌ 发送失败") + "</h2>";
  html += "<p><a href='/'>返回首页</a></p></div>";
  html += getFooter();
  webServer.send(200, "text/html", html);
}

// 配置页面（查看与保存）
void handleConfigGet() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = getHeader("系统配置");
  html += "<div class='card'><h2>参数设置</h2>";
  html += "<form method=\"POST\" action=\"/config\">";
  
  html += "<label>企业微信 Webhook</label><input name=\"wecom\" value=\"" + htmlEncode(String(rtConfig.wecomUrl)) + "\">";
  html += "<div class='check-group'><input type=\"checkbox\" name=\"enwecom\" " + String(rtConfig.enableWecom?"checked":"") + "><label class='checkbox-label'>启用企业微信推送</label></div>";
  
  html += "<label>本机号码 (用于标识)</label><input name=\"simnum\" value=\"" + htmlEncode(String(rtConfig.simNumber)) + "\">";
  html += "<label>管理员号码（允许 SMS:号码:内容 和 RESET 命令）</label><input name=\"adminphone\" value=\"" + htmlEncode(String(rtConfig.adminPhone)) + "\">";
  html += "<label>号码黑名单（每行或逗号分隔）</label><textarea name=\"blacklist\" rows=4>" + htmlEncode(String(rtConfig.numberBlacklist)) + "</textarea>";
  
  html += "<hr style='margin:20px 0;border:0;border-top:1px solid #eee'>";
  html += "<label>SMTP 服务器</label><input name=\"smtpserver\" value=\"" + htmlEncode(String(rtConfig.smtpServer)) + "\">";
  html += "<label>SMTP 端口</label><input name=\"smtpport\" type=\"number\" value=\"" + String(rtConfig.smtpPort) + "\">";
  html += "<label>SMTP 用户 (邮箱)</label><input name=\"smtpuser\" value=\"" + htmlEncode(String(rtConfig.smtpUser)) + "\">";
  html += "<label>SMTP 授权码/密码</label><input name=\"smtppass\" type=\"password\" value=\"" + htmlEncode(String(rtConfig.smtpPass)) + "\">";
  html += "<label>接收邮箱</label><input name=\"smtpto\" value=\"" + htmlEncode(String(rtConfig.smtpTo)) + "\">";
  html += "<div class='check-group'><input type=\"checkbox\" name=\"enemail\" " + String(rtConfig.enableEmail?"checked":"") + "><label class='checkbox-label'>启用邮件推送</label></div>";

  html += "<hr style='margin:20px 0;border:0;border-top:1px solid #eee'>";
  html += "<label>HTTP 推送 URL</label><input name=\"httpurl\" value=\"" + htmlEncode(String(rtConfig.httpServerUrl)) + "\">";
  html += "<div class='check-group'><input type=\"checkbox\" name=\"enhttp\" " + String(rtConfig.enableHttp?"checked":"") + "><label class='checkbox-label'>启用 HTTP 推送</label></div>";
  html += "<div class='check-group'><input type=\"checkbox\" name=\"smsweb\" " + String(rtConfig.smsWebMode?"checked":"") + "><label class='checkbox-label'>使用 sms_web 协议</label></div>";
  html += "<label>sms_web API Key</label><input name=\"httpkey\" type=\"password\" value=\"" + htmlEncode(String(rtConfig.httpApiKey)) + "\">";
  html += "<label>设备 ID</label><input name=\"deviceid\" value=\"" + htmlEncode(String(rtConfig.deviceId)) + "\">";
  html += "<label>SIM 卡槽 (1/2)</label><input name=\"simslot\" type=\"number\" min=\"1\" max=\"2\" value=\"" + String(rtConfig.simSlot) + "\">";

  html += "<hr style='margin:20px 0;border:0;border-top:1px solid #eee'><h3>扩展推送通道</h3>";
  html += "<p>类型：1 POST JSON，2 Bark，3 GET，4 钉钉，5 PushPlus，6 Server酱，7 自定义，8 飞书，9 Gotify，10 Telegram。</p>";
  for (uint8_t i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    PushChannelConfig &ch = rtConfig.pushChannels[i];
    String prefix = "p" + String(i);
    html += "<div style='border:1px solid #ddd;padding:12px;margin:10px 0'><strong>通道 " + String(i + 1) + "</strong>";
    html += "<div class='check-group'><input type='checkbox' name='" + prefix + "e' " + String(ch.enabled ? "checked" : "") + "><label>启用</label></div>";
    html += "<label>类型</label><input type='number' min='0' max='10' name='" + prefix + "t' value='" + String(ch.type) + "'>";
    html += "<label>名称</label><input name='" + prefix + "n' value='" + htmlEncode(ch.name) + "'>";
    html += "<label>URL / Webhook</label><input name='" + prefix + "u' value='" + htmlEncode(ch.url) + "'>";
    html += "<label>Key 1（Secret/Token/Chat ID）</label><input name='" + prefix + "a' value='" + htmlEncode(ch.key1) + "'>";
    html += "<label>Key 2（Channel/Bot Token）</label><input name='" + prefix + "b' value='" + htmlEncode(ch.key2) + "'>";
    html += "<label>自定义 JSON 模板</label><textarea name='" + prefix + "c' rows='3'>" + htmlEncode(ch.customBody) + "</textarea></div>";
  }

  html += "<hr style='margin:20px 0;border:0;border-top:1px solid #eee'>";
  html += "<label>Web 管理员用户名</label><input name=\"webuser\" value=\"" + htmlEncode(String(rtConfig.webUser)) + "\">";
  html += "<label>Web 管理员密码</label><input name=\"webpass\" type=\"password\" value=\"" + htmlEncode(String(rtConfig.webPass)) + "\">";

  html += "<input type=\"submit\" value=\"保存配置\">";
  html += "</form></div>";
  html += "<div class='card'><h2>恢复默认配置</h2>";
  html += "<p>清空 NVS 后立即恢复 config.h 中的默认值，包括 Web 管理账号密码。</p>";
  html += "<form method=\"POST\" action=\"/config/reset\" onsubmit=\"return confirm('确定清空 NVS 配置？');\">";
  html += "<input type=\"submit\" style=\"background:#dc3545\" value=\"清空 NVS 并恢复默认\"></form></div>";
  html += getFooter();
  webServer.send(200, "text/html", html);
}

void handleConfigPost() {
  if (!checkAuth()) { requestAuth(); return; }
  // 保存表单
  String wecom = webServer.arg("wecom");
  String simnum = webServer.arg("simnum");
  String smtpserver = webServer.arg("smtpserver");
  uint16_t smtpport = webServer.arg("smtpport").toInt();
  String smtpuser = webServer.arg("smtpuser");
  String smtppass = webServer.arg("smtppass");
  String smtpto = webServer.arg("smtpto");
  String httpurl = webServer.arg("httpurl");
  String httpkey = webServer.arg("httpkey");
  String deviceid = webServer.arg("deviceid");
  uint8_t simslot = constrain(webServer.arg("simslot").toInt(), 1, 2);
  bool enwecom = webServer.hasArg("enwecom");
  bool enemail = webServer.hasArg("enemail");
  bool enhttp = webServer.hasArg("enhttp");
  bool smsweb = webServer.hasArg("smsweb");
  String webuser = webServer.arg("webuser");
  String webpass = webServer.arg("webpass");
  String adminphone = webServer.arg("adminphone");
  String blacklist = webServer.arg("blacklist");

  strlcpy(rtConfig.wecomUrl, wecom.c_str(), sizeof(rtConfig.wecomUrl));
  strlcpy(rtConfig.simNumber, simnum.c_str(), sizeof(rtConfig.simNumber));
  strlcpy(rtConfig.smtpServer, smtpserver.c_str(), sizeof(rtConfig.smtpServer));
  rtConfig.smtpPort = smtpport;
  strlcpy(rtConfig.smtpUser, smtpuser.c_str(), sizeof(rtConfig.smtpUser));
  strlcpy(rtConfig.smtpPass, smtppass.c_str(), sizeof(rtConfig.smtpPass));
  strlcpy(rtConfig.smtpTo, smtpto.c_str(), sizeof(rtConfig.smtpTo));
  strlcpy(rtConfig.httpServerUrl, httpurl.c_str(), sizeof(rtConfig.httpServerUrl));
  strlcpy(rtConfig.httpApiKey, httpkey.c_str(), sizeof(rtConfig.httpApiKey));
  if (deviceid.length()) strlcpy(rtConfig.deviceId, deviceid.c_str(), sizeof(rtConfig.deviceId));
  rtConfig.simSlot = simslot;
  rtConfig.enableWecom = enwecom;
  rtConfig.enableEmail = enemail;
  rtConfig.enableHttp = enhttp;
  rtConfig.smsWebMode = smsweb;
  strlcpy(rtConfig.webUser, webuser.c_str(), sizeof(rtConfig.webUser));
  strlcpy(rtConfig.webPass, webpass.c_str(), sizeof(rtConfig.webPass));
  strlcpy(rtConfig.adminPhone, adminphone.c_str(), sizeof(rtConfig.adminPhone));
  strlcpy(rtConfig.numberBlacklist, blacklist.c_str(), sizeof(rtConfig.numberBlacklist));
  for (uint8_t i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    PushChannelConfig &ch = rtConfig.pushChannels[i];
    String prefix = "p" + String(i);
    ch.enabled = webServer.hasArg(prefix + "e");
    ch.type = constrain(webServer.arg(prefix + "t").toInt(), 0, (int)PUSH_TELEGRAM);
    strlcpy(ch.name, webServer.arg(prefix + "n").c_str(), sizeof(ch.name));
    strlcpy(ch.url, webServer.arg(prefix + "u").c_str(), sizeof(ch.url));
    strlcpy(ch.key1, webServer.arg(prefix + "a").c_str(), sizeof(ch.key1));
    strlcpy(ch.key2, webServer.arg(prefix + "b").c_str(), sizeof(ch.key2));
    strlcpy(ch.customBody, webServer.arg(prefix + "c").c_str(), sizeof(ch.customBody));
  }

  bool saved = saveConfig();
  if (!saved) loadConfig(); // 写入失败时回到 NVS 中实际可读的状态
  if (saved && rtConfig.enableHttp && rtConfig.smsWebMode) {
    sendSmsWebDeviceEvent(100);
    lastSmsWebHeartbeat = millis();
  }
  String html = getHeader(saved ? "保存成功" : "保存失败");
  html += saved
    ? "<div class='card' style='text-align:center'><h2>✅ 配置已保存</h2><p>NVS 写入和回读校验均成功，新配置已生效。</p><p><a href='/config'>返回配置页</a></p></div>"
    : "<div class='card' style='text-align:center'><h2>❌ 配置保存失败</h2><p>NVS 写入或回读校验失败，请查看串口日志。</p><p><a href='/config'>返回配置页</a></p></div>";
  html += getFooter();
  webServer.send(saved ? 200 : 500, "text/html", html);
}

void handleConfigReset() {
  if (!checkAuth()) { requestAuth(); return; }
  bool reset = resetConfig();
  String html = getHeader(reset ? "已恢复默认配置" : "恢复失败");
  html += reset
    ? "<div class='card' style='text-align:center'><h2>✅ NVS 已清空</h2><p>已恢复 config.h 默认值。Web 账号密码也可能已改回默认值。</p><p><a href='/'>返回首页</a></p></div>"
    : "<div class='card' style='text-align:center'><h2>❌ NVS 清空失败</h2><p>请查看串口日志。</p><p><a href='/config'>返回配置页</a></p></div>";
  html += getFooter();
  webServer.send(reset ? 200 : 500, "text/html", html);
}

// 队列查看页面
void handleQueue() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = getHeader("消息队列");
  html += "<div class='card'><h2>待重试短信队列</h2>";
  
  if (sms_q_count == 0) {
    html += "<p style='text-align:center;padding:20px;color:#666'>队列为空，所有消息已处理。</p>";
  } else {
    html += "<div style='overflow-x:auto'><table><thead><tr><th>#</th><th>发送者</th><th>时间</th><th>内容</th><th>状态</th></tr></thead><tbody>";
    for (int i = 0; i < sms_q_count; i++) {
      int idx = (sms_q_head + i) % SMS_QUEUE_SIZE;
      SMSItem &it = smsQueue[idx];
      html += "<tr>";
      html += "<td>" + String(i+1) + "</td>";
      html += "<td>" + String(it.sender) + "</td>";
      html += "<td>" + String(it.timestamp) + "</td>";
      html += "<td>" + htmlEncode(String(it.text)) + "</td>";
      String st = "";
      if(it.wecomSent) st += "<span class='status-ok'>WeCom✓</span> "; else if(rtConfig.enableWecom) st += "<span class='status-err'>WeCom✗</span> ";
      if(it.emailSent) st += "<span class='status-ok'>Email✓</span> "; else if(rtConfig.enableEmail) st += "<span class='status-err'>Email✗</span> ";
      if(it.httpSent) st += "<span class='status-ok'>HTTP✓</span> "; else if(rtConfig.enableHttp) st += "<span class='status-err'>HTTP✗</span> ";
      for (uint8_t channel = 0; channel < MAX_PUSH_CHANNELS; ++channel) {
        if (!isPushChannelValid(rtConfig.pushChannels[channel])) continue;
        bool sent = it.pushSentMask & (1U << channel);
        st += sent ? "<span class='status-ok'>P" + String(channel + 1) + "✓</span> "
                   : "<span class='status-err'>P" + String(channel + 1) + "✗</span> ";
      }
      html += "<td>" + st + "</td>";
      html += "</tr>";
    }
    html += "</tbody></table></div>";
  }
  html += "</div>";
  html += getFooter();
  webServer.send(200, "text/html", html);
}

// 模拟接收短信接口
void handleSimulateReceive() {
  if (!checkAuth()) { requestAuth(); return; }
  if (webServer.method() != HTTP_POST) {
    webServer.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  String sender = webServer.arg("sender");
  String text = webServer.arg("text");
  
  // 生成一个模拟的 PDU 时间戳 (YYMMDDHHmmss+TZ)
  // 25010112000032 -> 2025-01-01 12:00:00 +8h
  String timestamp = "25010112000032"; 
  
  processReceivedSMS(sender.c_str(), text.c_str(), timestamp.c_str());
  
  String html = getHeader("模拟接收结果");
  html += "<div class='card' style='text-align:center'><h2>✅ 模拟接收已触发</h2>";
  html += "<p>发送者: " + htmlEncode(sender) + "</p>";
  html += "<p>内容: " + htmlEncode(text) + "</p>";
  html += "<p>请检查各推送渠道是否收到消息。</p>";
  html += "<p><a href='/'>返回首页</a></p></div>";
  html += getFooter();
  webServer.send(200, "text/html", html);
}

String controlToken() {
  MD5Builder md5;
  md5.begin();
  md5.add(String(rtConfig.webUser) + "|" + rtConfig.webPass);
  md5.calculate();
  return md5.toString();
}

// sms_web 会以 GET /ctrl?token=...&cmd=sendsms&p1=...&p2=...&p3=... 控制开发板。
// 仅实现本单卡短信硬件有意义的命令，避免为不存在的 X 系列功能浪费 Flash/RAM。
void handleCtrl() {
  if (!webServer.hasArg("token") || webServer.arg("token") != controlToken()) {
    webServer.send(403, "application/json", "{\"code\":-1,\"message\":\"invalid token\"}");
    return;
  }

  String cmd = webServer.arg("cmd");
  if (cmd == "ping" || cmd == "stat") {
    String response;
    response.reserve(160);
    response = "{\"code\":0,\"devId\":\"" + escapeJson(rtConfig.deviceId);
    response += "\",\"ip\":\"" + WiFi.localIP().toString();
    response += "\",\"slot\":" + String(rtConfig.simSlot);
    response += ",\"dbm\":" + String(WiFi.RSSI());
    response += ",\"freeHeap\":" + String(ESP.getFreeHeap()) + "}";
    webServer.send(200, "application/json", response);
    return;
  }

  if (cmd == "sendsms") {
    String phone = webServer.arg("p2");
    String content = webServer.arg("p3");
    int slot = webServer.arg("p1").toInt();
    if (!phone.length() || !content.length() || slot != rtConfig.simSlot) {
      webServer.send(400, "application/json", "{\"code\":-1,\"message\":\"invalid sms parameters or slot\"}");
      return;
    }
    bool ok = sendSMS(phone.c_str(), content.c_str());
    if (ok && rtConfig.enableHttp && rtConfig.smsWebMode) {
      String tid = webServer.arg("tid");
      sendSMSToServer(phone.c_str(), content.c_str(), NULL, 502, tid.c_str());
    }
    webServer.send(ok ? 200 : 500, "application/json",
                   ok ? "{\"code\":0,\"message\":\"OK\"}" : "{\"code\":-1,\"message\":\"send failed\"}");
    return;
  }

  webServer.send(400, "application/json", "{\"code\":-1,\"message\":\"unsupported command\"}");
}

void handleLogs() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = getHeader("串口日志");
  html += "<div class='card'><h2>运行日志</h2><p>固定 " + String(WEB_LOG_SIZE) + " 字节环形缓冲，不会持续占用内存。</p>";
  html += "<pre style='white-space:pre-wrap;background:#111;color:#ddd;padding:12px;max-height:65vh;overflow:auto'>";
  html += htmlEncode(getWebLog());
  html += "</pre><p><a href='/logs'>刷新</a></p></div>" + getFooter();
  webServer.send(200, "text/html", html);
}

void handleModemPage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = getHeader("模组管理");
  html += "<div class='card'><h2>模组操作</h2>";
  const char* actions[][2] = {{"info","模组信息"},{"signal","查询信号"},{"operator","查询运营商"},
    {"imei","查询 IMEI"},{"flight-on","开启飞行模式"},{"flight-off","关闭飞行模式"},
    {"ping","Ping 保号"},{"soft-reset","软重启模组"},{"hard-reset","硬重启模组"},{"wifi","重连 WiFi"}};
  for (uint8_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
    html += "<form method='POST' action='/modem/action' style='margin:8px 0'><input type='hidden' name='action' value='" + String(actions[i][0]) + "'><input type='submit' value='" + actions[i][1] + "'></form>";
  html += "</div><div class='card'><h2>AT 控制台</h2><form method='POST' action='/at'><input name='command' maxlength='80' value='AT'><input type='submit' value='发送 AT 指令'></form></div>";
  html += getFooter();
  webServer.send(200, "text/html", html);
}

void sendOperationResult(const String& title, const String& result) {
  String html = getHeader(title);
  html += "<div class='card'><h2>" + htmlEncode(title) + "</h2><pre style='white-space:pre-wrap'>" + htmlEncode(result) + "</pre><p><a href='/modem'>返回模组管理</a></p></div>" + getFooter();
  webServer.send(200, "text/html", html);
}

void handleAtConsole() {
  if (!checkAuth()) { requestAuth(); return; }
  String command = webServer.arg("command");
  command.trim();
  if (!command.startsWith("AT") || command.length() > 80 || command.indexOf('\r') >= 0 || command.indexOf('\n') >= 0) {
    webServer.send(400, "text/plain; charset=utf-8", "AT 指令不合法");
    return;
  }
  sendOperationResult(command, sendATCommand(command.c_str(), 5000));
}

String performPing() {
  String activate = sendATCommand("AT+CGACT=1,1", 10000);
  while (Serial1.available()) Serial1.read();
  Serial1.println("AT+MPING=\"8.8.8.8\",1,32,10000");
  unsigned long started = millis();
  String result = "PDP 激活响应:\n" + activate + "\nPing 响应:\n";
  while (millis() - started < 12000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (result.length() < 1024) result += c;
    }
    if (result.indexOf("+MPING:") >= 0 || result.indexOf("ERROR") >= 0) break;
    esp_task_wdt_reset();
    delay(1);
  }
  sendATCommand("AT+CGACT=0,1", 5000);
  logLine("Ping 保号操作完成");
  return result;
}

void handleModemAction() {
  if (!checkAuth()) { requestAuth(); return; }
  String action = webServer.arg("action");
  String result;
  if (action == "info") result = sendATCommand("ATI");
  else if (action == "signal") result = sendATCommand("AT+CSQ");
  else if (action == "operator") result = sendATCommand("AT+COPS?");
  else if (action == "imei") result = sendATCommand("AT+GSN");
  else if (action == "flight-on") result = sendATCommand("AT+CFUN=4", 10000);
  else if (action == "flight-off") result = sendATCommand("AT+CFUN=1", 10000);
  else if (action == "ping") result = performPing();
  else if (action == "soft-reset") result = sendATCommand("AT+CFUN=1,1", 10000);
  else if (action == "hard-reset") result = resetModem() ? "OK" : "模组无响应";
  else if (action == "wifi") {
    WiFi.disconnect();
    lastWifiAttempt = millis() - wifiReconnectInterval;
    ensureWiFiConnected();
    result = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "后台重连中";
  } else {
    webServer.send(400, "text/plain; charset=utf-8", "未知操作");
    return;
  }
  sendOperationResult("操作结果", result);
}

// 初始化 Web 路由
void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/send", HTTP_POST, handleSend);
  webServer.on("/simulate", HTTP_POST, handleSimulateReceive);
  webServer.on("/config", HTTP_GET, handleConfigGet);
  webServer.on("/config", HTTP_POST, handleConfigPost);
  webServer.on("/config/reset", HTTP_POST, handleConfigReset);
  webServer.on("/queue", HTTP_GET, handleQueue);
  webServer.on("/ctrl", HTTP_GET, handleCtrl);
  webServer.on("/logs", HTTP_GET, handleLogs);
  webServer.on("/modem", HTTP_GET, handleModemPage);
  webServer.on("/modem/action", HTTP_POST, handleModemAction);
  webServer.on("/at", HTTP_POST, handleAtConsole);
  webServer.begin();
  Serial.println("Web 服务器已启动");
}
