#include "web/web_service.h"

#include "aov/aov_service.h"
#include "common/log.h"
#include "common/sha256.h"
#include "media/media_service.h"
#include "system/config_service.h"
#include "system/network_service.h"
#include "system/storage_service.h"
#include "system/system_service.h"
#include "detect/detect_service.h"
#include "face/face_service.h"
#include "record/record_service.h"
#include "web/fmp4_mux.h"

#include "cJSON.h"
#include "mongoose.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOKEN_LEN 64
#define SALT_LEN 16
#define PREVIEW_MAX_CLIENTS 4
#define PREVIEW_Q 16
#define PREVIEW_MARK 'P'

typedef struct {
  int inited;
  int running;
  int port;
  int token_ttl;
  char www_root[256];
  char listen_url[64];
  struct mg_mgr mgr;
  pthread_t tid;
  volatile int quit;

  char user[64];
  char salt_hex[SALT_LEN * 2 + 1];
  char pass_hash[65];
  int must_change;

  char token[TOKEN_LEN + 1];
  time_t token_expire;
  pthread_mutex_t lock;
} WebCtx;

typedef struct {
  uint8_t *buf;
  uint32_t len;
  int key;
} PreviewItem;

typedef struct {
  struct mg_connection *c;
  int used;
  int bootstrapped;
  int wait_key;
  int stream; /* 0=sub(chn1), 1=main-preview(chn3) */
  int q_r;
  int q_n;
  PreviewItem q[PREVIEW_Q];
} PreviewClient;

typedef struct {
  Fmp4Mux mux;
  int mux_ready;
  uint8_t *init;
  uint32_t init_len;
  char codec[32];
} PreviewTrack;

typedef struct {
  pthread_mutex_t lock;
  PreviewTrack track[2];
  int listening;
  int media_owned;
  int nclients;
  PreviewClient clients[PREVIEW_MAX_CLIENTS];
  char iq_dir[256];
} PreviewCtx;

static WebCtx g_web = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static PreviewCtx g_prev = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* Motion-detect/record automation started when preview becomes active. */
static volatile int g_motion_automation_on = 0;
static volatile int g_motion_defer_gen = 0;

/* T7.6: continuous/schedule keep media+record without preview clients. */
static volatile int g_record_policy_hold = 0;
static volatile int g_schedule_poll_on = 0;
static char g_record_policy_mode[16] = "motion";
static time_t g_schedule_last_slot = -1;
/* Debounce preview teardown across route switches / WS reconnect. */
static volatile int64_t g_preview_idle_deadline_ms = 0;

static int64_t web_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int web_start_motion_automation_if_needed(void);
static void web_stop_motion_automation(void);
static int web_ensure_media_for_policy(void);
static void web_schedule_tick(void);
static void preview_load_encode(MediaEncodeConfig *ecfg);

static void *motion_defer_worker(void *arg)
{
  int gen = (int)(intptr_t)arg;
  int i;
  /* Let preview/AE settle before stacking IVS+record (avoids prior cold-start crash). */
  for (i = 0; i < 30 && gen == g_motion_defer_gen; i++) {
    usleep(100 * 1000);
  }
  if (gen != g_motion_defer_gen) {
    return NULL;
  }
  if (web_start_motion_automation_if_needed() != 0) {
    log_warn("web", "deferred motion automation not started");
  } else if (g_motion_automation_on) {
    log_info("web", "deferred motion automation on");
  }
  return NULL;
}

static void schedule_motion_automation(void)
{
  pthread_t th;
  int gen;
  /* Motion policy: defer IVS a few seconds so AE/VI settle (cold-start safety).
   * Continuous/schedule: detect is started from web_apply_record_policy. */
  if (strcmp(g_record_policy_mode, "motion") != 0) {
    return;
  }
  gen = ++g_motion_defer_gen;
  if (pthread_create(&th, NULL, motion_defer_worker, (void *)(intptr_t)gen) == 0) {
    pthread_detach(th);
  }
}

static void cancel_motion_automation_schedule(void)
{
  g_motion_defer_gen++;
}

static void json_reply(struct mg_connection *c, int code, const char *msg, cJSON *data)
{
  cJSON *root = cJSON_CreateObject();
  char *text;
  cJSON_AddNumberToObject(root, "code", code);
  cJSON_AddStringToObject(root, "msg", msg ? msg : "");
  if (data) {
    cJSON_AddItemToObject(root, "data", data);
  } else {
    cJSON_AddObjectToObject(root, "data");
  }
  text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) {
    mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                  "{\"code\":1002,\"msg\":\"oom\",\"data\":{}}");
    return;
  }
  mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", text);
  free(text);
}

static void random_hex(char *out, int nbytes)
{
  int i;
  static const char *hexd = "0123456789abcdef";
  FILE *fp = fopen("/dev/urandom", "rb");
  uint8_t buf[64];
  if (nbytes > 64) {
    nbytes = 64;
  }
  if (fp) {
    if (fread(buf, 1, (size_t)nbytes, fp) != (size_t)nbytes) {
      for (i = 0; i < nbytes; i++) {
        buf[i] = (uint8_t)(rand() & 0xff);
      }
    }
    fclose(fp);
  } else {
    for (i = 0; i < nbytes; i++) {
      buf[i] = (uint8_t)(rand() & 0xff);
    }
  }
  for (i = 0; i < nbytes; i++) {
    out[i * 2] = hexd[(buf[i] >> 4) & 0xf];
    out[i * 2 + 1] = hexd[buf[i] & 0xf];
  }
  out[nbytes * 2] = '\0';
}

static void hash_password(const char *salt_hex, const char *password, char out_hash[65])
{
  char material[256];
  snprintf(material, sizeof(material), "%s:%s", salt_hex, password ? password : "");
  sha256_hex((const uint8_t *)material, strlen(material), out_hash);
}

static void auth_load_or_default(void)
{
  char user[64];
  char salt[SALT_LEN * 2 + 1];
  char hash[65];
  int must = 1;

  if (config_get_string("auth.username", user, (int)sizeof(user), "") != 0 || user[0] == '\0' ||
      config_get_string("auth.salt", salt, (int)sizeof(salt), "") != 0 || salt[0] == '\0' ||
      config_get_string("auth.pass_hash", hash, (int)sizeof(hash), "") != 0 || hash[0] == '\0') {
    snprintf(g_web.user, sizeof(g_web.user), "admin");
    random_hex(g_web.salt_hex, SALT_LEN);
    hash_password(g_web.salt_hex, "admin", g_web.pass_hash);
    g_web.must_change = 1;
    config_set_string("auth.username", g_web.user);
    config_set_string("auth.salt", g_web.salt_hex);
    config_set_string("auth.pass_hash", g_web.pass_hash);
    config_set_bool("auth.must_change", 1);
    config_save();
    log_warn("web", "initialized default auth admin/admin (must change)");
    return;
  }

  snprintf(g_web.user, sizeof(g_web.user), "%s", user);
  snprintf(g_web.salt_hex, sizeof(g_web.salt_hex), "%s", salt);
  snprintf(g_web.pass_hash, sizeof(g_web.pass_hash), "%s", hash);
  config_get_bool("auth.must_change", &must, 1);
  g_web.must_change = must;
}

static int extract_token(struct mg_http_message *hm, char *out, size_t out_len)
{
  struct mg_str *hdr;
  char tmp[128];

  if (out_len < TOKEN_LEN + 1) {
    return -1;
  }
  out[0] = '\0';

  hdr = mg_http_get_header(hm, "Authorization");
  if (hdr && hdr->len > 7 && hdr->buf && memcmp(hdr->buf, "Bearer ", 7) == 0) {
    size_t n = hdr->len - 7;
    if (n > TOKEN_LEN) {
      n = TOKEN_LEN;
    }
    memcpy(out, hdr->buf + 7, n);
    out[n] = '\0';
    return 0;
  }

  hdr = mg_http_get_header(hm, "Cookie");
  if (hdr && mg_http_get_var(hdr, "ipc_token", tmp, sizeof(tmp)) > 0) {
    size_t n = strlen(tmp);
    if (n >= out_len) {
      n = out_len - 1;
    }
    memcpy(out, tmp, n);
    out[n] = '\0';
    return 0;
  }

  if (mg_http_get_var(&hm->query, "token", tmp, sizeof(tmp)) > 0) {
    size_t n = strlen(tmp);
    if (n >= out_len) {
      n = out_len - 1;
    }
    memcpy(out, tmp, n);
    out[n] = '\0';
    return 0;
  }
  return -1;
}

static int auth_ok(struct mg_http_message *hm)
{
  char tok[TOKEN_LEN + 1];
  int ok = 0;
  if (extract_token(hm, tok, sizeof(tok)) != 0) {
    return 0;
  }
  pthread_mutex_lock(&g_web.lock);
  if (g_web.token[0] && strcmp(g_web.token, tok) == 0 && time(NULL) < g_web.token_expire) {
    ok = 1;
  }
  pthread_mutex_unlock(&g_web.lock);
  return ok;
}

static void issue_token(char *out, size_t out_len)
{
  char hex[TOKEN_LEN + 1];
  random_hex(hex, TOKEN_LEN / 2);
  pthread_mutex_lock(&g_web.lock);
  snprintf(g_web.token, sizeof(g_web.token), "%s", hex);
  g_web.token_expire = time(NULL) + g_web.token_ttl;
  snprintf(out, out_len, "%s", g_web.token);
  pthread_mutex_unlock(&g_web.lock);
}

static void clear_token(void)
{
  pthread_mutex_lock(&g_web.lock);
  g_web.token[0] = '\0';
  g_web.token_expire = 0;
  pthread_mutex_unlock(&g_web.lock);
}

static cJSON *parse_body(struct mg_http_message *hm)
{
  if (hm->body.len == 0) {
    return cJSON_CreateObject();
  }
  return cJSON_ParseWithLength(hm->body.buf, hm->body.len);
}

static void handle_login(struct mg_connection *c, struct mg_http_message *hm)
{
  cJSON *body = parse_body(hm);
  const char *user;
  const char *pass;
  char expect[65];
  char token[TOKEN_LEN + 1];
  cJSON *data;
  char headers[256];

  if (!body) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  user = cJSON_GetStringValue(cJSON_GetObjectItem(body, "username"));
  pass = cJSON_GetStringValue(cJSON_GetObjectItem(body, "password"));
  if (!user || !pass || !user[0] || !pass[0]) {
    cJSON_Delete(body);
    json_reply(c, 1002, "请输入用户名和密码", NULL);
    return;
  }

  pthread_mutex_lock(&g_web.lock);
  hash_password(g_web.salt_hex, pass, expect);
  if (strcmp(user, g_web.user) != 0 || strcmp(expect, g_web.pass_hash) != 0) {
    pthread_mutex_unlock(&g_web.lock);
    cJSON_Delete(body);
    json_reply(c, 1001, "用户名或密码错误", NULL);
    return;
  }
  pthread_mutex_unlock(&g_web.lock);
  cJSON_Delete(body);

  issue_token(token, sizeof(token));
  data = cJSON_CreateObject();
  cJSON_AddStringToObject(data, "token", token);
  cJSON_AddBoolToObject(data, "must_change", g_web.must_change);
  cJSON_AddStringToObject(data, "username", g_web.user);

  snprintf(headers, sizeof(headers),
           "Content-Type: application/json\r\n"
           "Set-Cookie: ipc_token=%s; Path=/; HttpOnly; SameSite=Lax\r\n",
           token);
  {
    cJSON *root = cJSON_CreateObject();
    char *text;
    cJSON_AddNumberToObject(root, "code", 0);
    cJSON_AddStringToObject(root, "msg", "ok");
    cJSON_AddItemToObject(root, "data", data);
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, headers, "%s", text ? text : "{\"code\":0,\"msg\":\"ok\",\"data\":{}}");
    free(text);
  }
  log_info("web", "login ok user=%s", g_web.user);
}

static void handle_logout(struct mg_connection *c, struct mg_http_message *hm)
{
  (void)hm;
  clear_token();
  mg_http_reply(c, 200,
                "Content-Type: application/json\r\n"
                "Set-Cookie: ipc_token=; Path=/; Max-Age=0; HttpOnly\r\n",
                "{\"code\":0,\"msg\":\"ok\",\"data\":{}}");
}

static void handle_password(struct mg_connection *c, struct mg_http_message *hm)
{
  cJSON *body;
  const char *oldp;
  const char *newp;
  char expect[65];
  char nhash[65];

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  body = parse_body(hm);
  if (!body) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  oldp = cJSON_GetStringValue(cJSON_GetObjectItem(body, "old_password"));
  newp = cJSON_GetStringValue(cJSON_GetObjectItem(body, "new_password"));
  if (!oldp || !newp || strlen(newp) < 6) {
    cJSON_Delete(body);
    json_reply(c, 1002, "password too short", NULL);
    return;
  }

  pthread_mutex_lock(&g_web.lock);
  hash_password(g_web.salt_hex, oldp, expect);
  if (strcmp(expect, g_web.pass_hash) != 0) {
    pthread_mutex_unlock(&g_web.lock);
    cJSON_Delete(body);
    json_reply(c, 1001, "old password wrong", NULL);
    return;
  }
  random_hex(g_web.salt_hex, SALT_LEN);
  hash_password(g_web.salt_hex, newp, nhash);
  snprintf(g_web.pass_hash, sizeof(g_web.pass_hash), "%s", nhash);
  g_web.must_change = 0;
  pthread_mutex_unlock(&g_web.lock);

  config_set_string("auth.salt", g_web.salt_hex);
  config_set_string("auth.pass_hash", g_web.pass_hash);
  config_set_bool("auth.must_change", 0);
  config_save();
  cJSON_Delete(body);
  json_reply(c, 0, "ok", NULL);
  log_info("web", "password changed");
}

static void handle_me(struct mg_connection *c, struct mg_http_message *hm)
{
  cJSON *data;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  data = cJSON_CreateObject();
  cJSON_AddStringToObject(data, "username", g_web.user);
  cJSON_AddBoolToObject(data, "must_change", g_web.must_change);
  json_reply(c, 0, "ok", data);
}

/* ---- network (T4.4) ---- */

static cJSON *network_to_json(const NetworkConfig *cfg)
{
  cJSON *data = cJSON_CreateObject();
  cJSON_AddStringToObject(data, "iface", cfg->iface);
  cJSON_AddStringToObject(data, "mode", cfg->mode);
  cJSON_AddStringToObject(data, "ip", cfg->ip);
  cJSON_AddStringToObject(data, "netmask", cfg->netmask);
  cJSON_AddStringToObject(data, "gateway", cfg->gateway);
  cJSON_AddStringToObject(data, "dns1", cfg->dns1);
  cJSON_AddStringToObject(data, "dns2", cfg->dns2);
  cJSON_AddStringToObject(data, "link", cfg->link);
  cJSON_AddStringToObject(data, "current_ip", cfg->current_ip);
  cJSON_AddStringToObject(data, "current_netmask", cfg->current_netmask);
  cJSON_AddStringToObject(data, "current_gateway", cfg->current_gateway);
  cJSON_AddStringToObject(data, "usb0_ip", cfg->usb0_ip);
  return data;
}

static void handle_network_get(struct mg_connection *c, struct mg_http_message *hm)
{
  NetworkConfig cfg;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  if (network_get(&cfg) != 0) {
    json_reply(c, 2402, "network get failed", NULL);
    return;
  }
  json_reply(c, 0, "ok", network_to_json(&cfg));
}

static void handle_network_set(struct mg_connection *c, struct mg_http_message *hm)
{
  NetworkConfig cfg;
  NetworkConfig cur;
  cJSON *root;
  cJSON *item;
  int apply = 1;
  int rc;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  memset(&cfg, 0, sizeof(cfg));
  network_get(&cur);
  cfg = cur;

  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }

  item = cJSON_GetObjectItem(root, "iface");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(cfg.iface, sizeof(cfg.iface), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "mode");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(cfg.mode, sizeof(cfg.mode), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "ip");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(cfg.ip, sizeof(cfg.ip), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "netmask");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(cfg.netmask, sizeof(cfg.netmask), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "gateway");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(cfg.gateway, sizeof(cfg.gateway), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "dns1");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(cfg.dns1, sizeof(cfg.dns1), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "dns2");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(cfg.dns2, sizeof(cfg.dns2), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "apply");
  if (cJSON_IsBool(item)) {
    apply = cJSON_IsTrue(item) ? 1 : 0;
  } else if (cJSON_IsNumber(item)) {
    apply = item->valueint ? 1 : 0;
  }
  cJSON_Delete(root);

  rc = network_set(&cfg, apply);
  if (rc == -2) {
    json_reply(c, 1002, "invalid network params", NULL);
    return;
  }
  if (rc == -3) {
    json_reply(c, 2402, "apply failed", NULL);
    return;
  }
  if (rc != 0) {
    json_reply(c, 2401, "save failed", NULL);
    return;
  }
  network_get(&cur);
  json_reply(c, 0, "ok", network_to_json(&cur));
}

/* ---- video (T4.5) ---- */

static void video_load_encode(MediaEncodeConfig *ecfg, char *iq_dir, int iq_len)
{
  memset(ecfg, 0, sizeof(*ecfg));
  config_get_string("video.iq_dir", iq_dir, iq_len, "/oem/usr/share/iqfiles");
  config_get_int("video.encode.main_w", &ecfg->main_w, 1920);
  config_get_int("video.encode.main_h", &ecfg->main_h, 1080);
  config_get_int("video.encode.main_fps", &ecfg->main_fps, 15);
  config_get_int("video.encode.main_bitrate_kbps", &ecfg->main_bitrate_kbps, 2048);
  config_get_int("video.encode.main_gop", &ecfg->main_gop, 30);
  config_get_int("video.encode.sub_w", &ecfg->sub_w, 704);
  config_get_int("video.encode.sub_h", &ecfg->sub_h, 576);
  config_get_int("video.encode.sub_fps", &ecfg->sub_fps, 15);
  config_get_int("video.encode.sub_bitrate_kbps", &ecfg->sub_bitrate_kbps, 1024);
  config_get_int("video.encode.sub_gop", &ecfg->sub_gop, 30);
  ecfg->detect_w = media_stream_detect_enabled() ? media_stream_detect_width() : 0;
  ecfg->detect_h = media_stream_detect_enabled() ? media_stream_detect_height() : 0;
  ecfg->iq_dir = iq_dir;
}

static cJSON *video_image_to_json(const MediaImageConfig *img)
{
  cJSON *data = cJSON_CreateObject();
  cJSON_AddNumberToObject(data, "brightness", img->brightness);
  cJSON_AddNumberToObject(data, "contrast", img->contrast);
  cJSON_AddNumberToObject(data, "saturation", img->saturation);
  cJSON_AddBoolToObject(data, "mirror", img->mirror != 0);
  cJSON_AddBoolToObject(data, "flip", img->flip != 0);
  return data;
}

static cJSON *video_encode_to_json(const MediaEncodeConfig *ecfg)
{
  cJSON *data = cJSON_CreateObject();
  cJSON *jmain = cJSON_CreateObject();
  cJSON *sub = cJSON_CreateObject();
  cJSON_AddNumberToObject(jmain, "w", ecfg->main_w);
  cJSON_AddNumberToObject(jmain, "h", ecfg->main_h);
  cJSON_AddNumberToObject(jmain, "fps", ecfg->main_fps);
  cJSON_AddNumberToObject(jmain, "bitrate_kbps", ecfg->main_bitrate_kbps);
  cJSON_AddNumberToObject(jmain, "gop", ecfg->main_gop);
  cJSON_AddNumberToObject(sub, "w", ecfg->sub_w);
  cJSON_AddNumberToObject(sub, "h", ecfg->sub_h);
  cJSON_AddNumberToObject(sub, "fps", ecfg->sub_fps);
  cJSON_AddNumberToObject(sub, "bitrate_kbps", ecfg->sub_bitrate_kbps);
  cJSON_AddNumberToObject(sub, "gop", ecfg->sub_gop);
  cJSON_AddItemToObject(data, "main", jmain);
  cJSON_AddItemToObject(data, "sub", sub);
  cJSON_AddBoolToObject(data, "stream_up", media_stream_is_up() ? 1 : 0);
  return data;
}

static int video_encode_valid(const MediaEncodeConfig *ecfg)
{
  if (!ecfg) {
    return 0;
  }
  if (ecfg->main_w < 320 || ecfg->main_h < 240 || ecfg->sub_w < 320 || ecfg->sub_h < 240) {
    return 0;
  }
  if (ecfg->main_fps < 1 || ecfg->main_fps > 30 || ecfg->sub_fps < 1 || ecfg->sub_fps > 30) {
    return 0;
  }
  if (ecfg->main_bitrate_kbps < 128 || ecfg->main_bitrate_kbps > 8192 || ecfg->sub_bitrate_kbps < 128 ||
      ecfg->sub_bitrate_kbps > 4096) {
    return 0;
  }
  if (ecfg->main_gop < 1 || ecfg->main_gop > 300 || ecfg->sub_gop < 1 || ecfg->sub_gop > 300) {
    return 0;
  }
  return 1;
}

static void handle_video_image_get(struct mg_connection *c, struct mg_http_message *hm)
{
  MediaImageConfig img;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  if (media_image_get(&img) != 0) {
    json_reply(c, 2102, "image get failed", NULL);
    return;
  }
  json_reply(c, 0, "ok", video_image_to_json(&img));
}

static void handle_video_image_set(struct mg_connection *c, struct mg_http_message *hm)
{
  MediaImageConfig img;
  MediaImageConfig cur;
  cJSON *root;
  cJSON *item;
  int apply = 1;
  int rc;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  media_image_get(&cur);
  img = cur;

  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  item = cJSON_GetObjectItem(root, "brightness");
  if (cJSON_IsNumber(item)) {
    img.brightness = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "contrast");
  if (cJSON_IsNumber(item)) {
    img.contrast = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "saturation");
  if (cJSON_IsNumber(item)) {
    img.saturation = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "mirror");
  if (cJSON_IsBool(item) || cJSON_IsNumber(item)) {
    img.mirror = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint) ? 1 : 0;
  }
  item = cJSON_GetObjectItem(root, "flip");
  if (cJSON_IsBool(item) || cJSON_IsNumber(item)) {
    img.flip = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint) ? 1 : 0;
  }
  item = cJSON_GetObjectItem(root, "apply");
  if (cJSON_IsBool(item)) {
    apply = cJSON_IsTrue(item) ? 1 : 0;
  } else if (cJSON_IsNumber(item)) {
    apply = item->valueint ? 1 : 0;
  }
  cJSON_Delete(root);

  rc = media_image_set(&img, apply);
  if (rc == -2) {
    json_reply(c, 2102, "image apply failed", NULL);
    return;
  }
  if (rc != 0) {
    json_reply(c, 2101, "image save failed", NULL);
    return;
  }
  media_image_get(&cur);
  json_reply(c, 0, "ok", video_image_to_json(&cur));
}

static void handle_video_encode_get(struct mg_connection *c, struct mg_http_message *hm)
{
  MediaEncodeConfig ecfg;
  char iq_dir[256];
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  video_load_encode(&ecfg, iq_dir, (int)sizeof(iq_dir));
  json_reply(c, 0, "ok", video_encode_to_json(&ecfg));
}

static void handle_video_encode_set(struct mg_connection *c, struct mg_http_message *hm)
{
  MediaEncodeConfig ecfg;
  char iq_dir[256];
  cJSON *root;
  cJSON *jmain;
  cJSON *sub;
  cJSON *item;
  int apply = 1;
  int rc;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  video_load_encode(&ecfg, iq_dir, (int)sizeof(iq_dir));

  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  jmain = cJSON_GetObjectItem(root, "main");
  sub = cJSON_GetObjectItem(root, "sub");
  if (jmain) {
    item = cJSON_GetObjectItem(jmain, "w");
    if (cJSON_IsNumber(item)) {
      ecfg.main_w = item->valueint;
    }
    item = cJSON_GetObjectItem(jmain, "h");
    if (cJSON_IsNumber(item)) {
      ecfg.main_h = item->valueint;
    }
    item = cJSON_GetObjectItem(jmain, "fps");
    if (cJSON_IsNumber(item)) {
      ecfg.main_fps = item->valueint;
    }
    item = cJSON_GetObjectItem(jmain, "bitrate_kbps");
    if (cJSON_IsNumber(item)) {
      ecfg.main_bitrate_kbps = item->valueint;
    }
    item = cJSON_GetObjectItem(jmain, "gop");
    if (cJSON_IsNumber(item)) {
      ecfg.main_gop = item->valueint;
    }
  }
  if (sub) {
    item = cJSON_GetObjectItem(sub, "w");
    if (cJSON_IsNumber(item)) {
      ecfg.sub_w = item->valueint;
    }
    item = cJSON_GetObjectItem(sub, "h");
    if (cJSON_IsNumber(item)) {
      ecfg.sub_h = item->valueint;
    }
    item = cJSON_GetObjectItem(sub, "fps");
    if (cJSON_IsNumber(item)) {
      ecfg.sub_fps = item->valueint;
    }
    item = cJSON_GetObjectItem(sub, "bitrate_kbps");
    if (cJSON_IsNumber(item)) {
      ecfg.sub_bitrate_kbps = item->valueint;
    }
    item = cJSON_GetObjectItem(sub, "gop");
    if (cJSON_IsNumber(item)) {
      ecfg.sub_gop = item->valueint;
    }
  }
  item = cJSON_GetObjectItem(root, "apply");
  if (cJSON_IsBool(item)) {
    apply = cJSON_IsTrue(item) ? 1 : 0;
  } else if (cJSON_IsNumber(item)) {
    apply = item->valueint ? 1 : 0;
  }
  cJSON_Delete(root);

  if (!video_encode_valid(&ecfg)) {
    json_reply(c, 1002, "invalid encode params", NULL);
    return;
  }

  if (config_set_int("video.encode.main_w", ecfg.main_w) != 0 ||
      config_set_int("video.encode.main_h", ecfg.main_h) != 0 ||
      config_set_int("video.encode.main_fps", ecfg.main_fps) != 0 ||
      config_set_int("video.encode.main_bitrate_kbps", ecfg.main_bitrate_kbps) != 0 ||
      config_set_int("video.encode.main_gop", ecfg.main_gop) != 0 ||
      config_set_int("video.encode.sub_w", ecfg.sub_w) != 0 ||
      config_set_int("video.encode.sub_h", ecfg.sub_h) != 0 ||
      config_set_int("video.encode.sub_fps", ecfg.sub_fps) != 0 ||
      config_set_int("video.encode.sub_bitrate_kbps", ecfg.sub_bitrate_kbps) != 0 ||
      config_set_int("video.encode.sub_gop", ecfg.sub_gop) != 0 || config_save() != 0) {
    json_reply(c, 2101, "encode save failed", NULL);
    return;
  }

  if (apply && media_stream_is_up()) {
    rc = media_stream_apply(&ecfg);
    if (rc != 0) {
      json_reply(c, 2102, "encode apply failed", NULL);
      return;
    }
    media_stream_request_idr(0);
    media_stream_request_idr(1);
  }

  video_load_encode(&ecfg, iq_dir, (int)sizeof(iq_dir));
  json_reply(c, 0, "ok", video_encode_to_json(&ecfg));
}

static cJSON *video_osd_to_json(const MediaOsdConfig *cfg)
{
  cJSON *data = cJSON_CreateObject();
  cJSON_AddBoolToObject(data, "enabled", cfg->enabled ? 1 : 0);
  cJSON_AddBoolToObject(data, "show_time", cfg->show_time ? 1 : 0);
  cJSON_AddBoolToObject(data, "show_name", cfg->show_name ? 1 : 0);
  cJSON_AddNumberToObject(data, "pos_x", cfg->pos_x);
  cJSON_AddNumberToObject(data, "pos_y", cfg->pos_y);
  cJSON_AddStringToObject(data, "name", cfg->name);
  return data;
}

static void handle_video_osd_get(struct mg_connection *c, struct mg_http_message *hm)
{
  MediaOsdConfig cfg;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  media_osd_get(&cfg);
  json_reply(c, 0, "ok", video_osd_to_json(&cfg));
}

static void handle_video_osd_set(struct mg_connection *c, struct mg_http_message *hm)
{
  MediaOsdConfig cfg;
  cJSON *root;
  cJSON *item;
  const char *name;
  int apply = 1;
  int rc;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  media_osd_get(&cfg);
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  item = cJSON_GetObjectItem(root, "enabled");
  if (cJSON_IsBool(item) || cJSON_IsNumber(item)) {
    cfg.enabled = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint);
  }
  item = cJSON_GetObjectItem(root, "show_time");
  if (cJSON_IsBool(item) || cJSON_IsNumber(item)) {
    cfg.show_time = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint);
  }
  item = cJSON_GetObjectItem(root, "show_name");
  if (cJSON_IsBool(item) || cJSON_IsNumber(item)) {
    cfg.show_name = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint);
  }
  name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
  if (name) {
    snprintf(cfg.name, sizeof(cfg.name), "%.63s", name);
  }
  item = cJSON_GetObjectItem(root, "pos_x");
  if (cJSON_IsNumber(item)) {
    cfg.pos_x = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "pos_y");
  if (cJSON_IsNumber(item)) {
    cfg.pos_y = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "apply");
  if (cJSON_IsBool(item) || cJSON_IsNumber(item)) {
    apply = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint);
  }
  cJSON_Delete(root);
  rc = media_osd_set(&cfg, apply);
  if (rc == -2) {
    json_reply(c, 2102, "osd apply failed", NULL);
    return;
  }
  if (rc != 0) {
    json_reply(c, 2101, "osd save failed", NULL);
    return;
  }
  media_osd_get(&cfg);
  json_reply(c, 0, "ok", video_osd_to_json(&cfg));
}

/* ---- storage (T4.6) ---- */

typedef struct {
  char rel_path[512]; /* records/<day>/<file> relative: <day>/<file>. */
  time_t mtime;
  uint64_t size;
  char name[256];
} StorageRecord;

static int is_digits_str(const char *s)
{
  size_t n, i;
  if (!s) {
    return 0;
  }
  n = strlen(s);
  if (n == 0) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (!isdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return 1;
}

static int is_date8(const char *s)
{
  return s && strlen(s) == 8 && is_digits_str(s);
}

static int ends_with_mp4(const char *name)
{
  size_t n;
  if (!name) {
    return 0;
  }
  n = strlen(name);
  return n >= 5 && strcmp(name + (n - 4), ".mp4") == 0;
}

static int storage_cmp_mtime_desc(const void *a, const void *b)
{
  const StorageRecord *ra = (const StorageRecord *)a;
  const StorageRecord *rb = (const StorageRecord *)b;
  if (ra->mtime < rb->mtime) {
    return 1;
  }
  if (ra->mtime > rb->mtime) {
    return -1;
  }
  return 0;
}

static void storage_rec_sort_mtime_desc(StorageRecord *arr, int n)
{
  qsort(arr, (size_t)n, sizeof(StorageRecord), storage_cmp_mtime_desc);
}

static int storage_collect_mp4_in_dir(const char *dir, const char *day,
                                        StorageRecord **out, int *count, int *cap)
{
  DIR *dp;
  struct dirent *de;
  dp = opendir(dir);
  if (!dp) {
    return -1;
  }

  while ((de = readdir(dp)) != NULL) {
    char path[1024];
    struct stat st;
    size_t n;

    if (de->d_name[0] == '.') {
      continue;
    }
    n = strlen(de->d_name);
    if (n < 5) {
      continue;
    }
    if (!ends_with_mp4(de->d_name)) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
    if (stat(path, &st) != 0) {
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;
    }

    if (*count >= *cap) {
      int ncap = (*cap == 0) ? 64 : (*cap * 2);
      StorageRecord *nl = (StorageRecord *)realloc(*out, (size_t)ncap * sizeof(StorageRecord));
      if (!nl) {
        closedir(dp);
        return -2;
      }
      *out = nl;
      *cap = ncap;
    }

    snprintf((*out)[*count].rel_path, sizeof((*out)[*count].rel_path), "%s/%s", day, de->d_name);
    snprintf((*out)[*count].name, sizeof((*out)[*count].name), "%s", de->d_name);
    (*out)[*count].mtime = st.st_mtime;
    (*out)[*count].size = (uint64_t)st.st_size;
    (*count)++;
  }
  closedir(dp);
  return 0;
}

static void handle_storage_status(struct mg_connection *c, struct mg_http_message *hm)
{
  StorageStatus st;
  int fp;
  cJSON *data;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }

  if (storage_get_status(&st) != 0) {
    json_reply(c, 2402, "storage status failed", NULL);
    return;
  }
  fp = storage_free_percent();

  data = cJSON_CreateObject();
  cJSON_AddNumberToObject(data, "free_percent", fp >= 0 ? fp : 0);
  cJSON_AddNumberToObject(data, "total_bytes", (double)st.total_bytes);
  cJSON_AddNumberToObject(data, "free_bytes", (double)st.free_bytes);
  cJSON_AddStringToObject(data, "mount_path", st.mount_path);
  cJSON_AddStringToObject(data, "fstype", st.fstype);
  cJSON_AddBoolToObject(data, "mounted", st.mounted ? 1 : 0);
  json_reply(c, 0, "ok", data);
}

static void handle_storage_records(struct mg_connection *c, struct mg_http_message *hm)
{
  char date[9];
  char limit_s[16];
  int limit;
  int offset;
  int have_date;
  StorageRecord *arr;
  int n, cap;
  const char *base;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }

  memset(date, 0, sizeof(date));
  memset(limit_s, 0, sizeof(limit_s));
  have_date = 0;
  limit = 100;
  offset = 0;
  if (mg_http_get_var(&hm->query, "date", date, sizeof(date)) > 0 && is_date8(date)) {
    have_date = 1;
  }
  if (mg_http_get_var(&hm->query, "limit", limit_s, sizeof(limit_s)) > 0) {
    int v = atoi(limit_s);
    if (v > 0 && v <= 200) {
      limit = v;
    }
  }
  {
    char offset_s[16];
    memset(offset_s, 0, sizeof(offset_s));
    if (mg_http_get_var(&hm->query, "offset", offset_s, sizeof(offset_s)) > 0) {
      int v = atoi(offset_s);
      if (v > 0) {
        offset = v;
      }
    }
  }

  base = storage_records_path();
  arr = NULL;
  n = 0;
  cap = 0;

  if (have_date) {
    char day_dir[512];
    snprintf(day_dir, sizeof(day_dir), "%s/%s", base, date);
    storage_collect_mp4_in_dir(day_dir, date, &arr, &n, &cap);
  } else {
    DIR *dp = opendir(base);
    struct dirent *de;
    if (dp) {
      while ((de = readdir(dp)) != NULL) {
        char day_dir[512];
        struct stat st;
        size_t dn = strlen(de->d_name);
        if (de->d_name[0] == '.') {
          continue;
        }
        if (dn != 8 || !is_date8(de->d_name)) {
          continue;
        }
        snprintf(day_dir, sizeof(day_dir), "%s/%s", base, de->d_name);
        if (stat(day_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
          continue;
        }
        storage_collect_mp4_in_dir(day_dir, de->d_name, &arr, &n, &cap);
      }
      closedir(dp);
    }
  }

  if (arr && n > 0) {
    storage_rec_sort_mtime_desc(arr, n);
  }

  {
    int start = offset;
    int out_n = 0;
    cJSON *data = cJSON_CreateObject();
    cJSON *records = cJSON_CreateArray();
    int i;
    if (start < 0) {
      start = 0;
    }
    if (start > n) {
      start = n;
    }
    out_n = n - start;
    if (out_n > limit) {
      out_n = limit;
    }
    cJSON_AddStringToObject(data, "base", base);
    cJSON_AddNumberToObject(data, "total", (double)n);
    cJSON_AddNumberToObject(data, "count", (double)out_n);
    cJSON_AddNumberToObject(data, "offset", (double)start);
    cJSON_AddNumberToObject(data, "limit", (double)limit);
    if (have_date) {
      cJSON_AddStringToObject(data, "date", date);
    }
    for (i = 0; i < out_n; i++) {
      cJSON *it = cJSON_CreateObject();
      cJSON_AddStringToObject(it, "path", arr[start + i].rel_path);
      cJSON_AddStringToObject(it, "name", arr[start + i].name);
      cJSON_AddNumberToObject(it, "mtime", (double)arr[start + i].mtime);
      cJSON_AddNumberToObject(it, "size", (double)arr[start + i].size);
      cJSON_AddItemToArray(records, it);
    }
    cJSON_AddItemToObject(data, "records", records);
    json_reply(c, 0, "ok", data);
  }

  free(arr);
}

static void handle_storage_download(struct mg_connection *c, struct mg_http_message *hm)
{
  char rel[512];
  char full[1024];
  struct stat st;
  struct mg_http_serve_opts opts;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }

  memset(rel, 0, sizeof(rel));
  if (mg_http_get_var(&hm->query, "path", rel, sizeof(rel)) <= 0 || rel[0] == '\0') {
    json_reply(c, 1002, "path required", NULL);
    return;
  }

  if (strstr(rel, "..") || rel[0] == '/' || strchr(rel, ':') != NULL) {
    json_reply(c, 1002, "bad path", NULL);
    return;
  }

  {
    char day[9];
    char *slash = strchr(rel, '/');
    if (!slash || (slash - rel) != 8) {
      json_reply(c, 1002, "bad path", NULL);
      return;
    }
    memcpy(day, rel, 8);
    day[8] = '\0';
    if (!is_date8(day)) {
      json_reply(c, 1002, "bad path", NULL);
      return;
    }
    if (!ends_with_mp4(rel)) {
      json_reply(c, 1002, "only mp4", NULL);
      return;
    }
  }

  snprintf(full, sizeof(full), "%s/%s", storage_records_path(), rel);
  if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
    json_reply(c, 2402, "file not found", NULL);
    return;
  }

  memset(&opts, 0, sizeof(opts));
  opts.root_dir = "/";
  opts.extra_headers = "Accept-Ranges: bytes\r\n";
  mg_http_serve_file(c, hm, full, &opts);
}

static void handle_storage_format(struct mg_connection *c, struct mg_http_message *hm)
{
  cJSON *root;
  cJSON *item;
  int confirm = 0;
  int deleted;
  cJSON *data;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }

  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  item = cJSON_GetObjectItem(root, "confirm");
  if (cJSON_IsBool(item)) {
    confirm = cJSON_IsTrue(item) ? 1 : 0;
  } else if (cJSON_IsNumber(item)) {
    confirm = item->valueint ? 1 : 0;
  }
  cJSON_Delete(root);

  if (!confirm) {
    json_reply(c, 1002, "confirm required", NULL);
    return;
  }

  /* Stop recording so open files under records/alarms can be wiped. */
  cancel_motion_automation_schedule();
  web_stop_motion_automation();
  g_schedule_poll_on = 0;
  record_deinit();
  deleted = storage_format_clear();
  if (deleted < 0) {
    json_reply(c, 2403, "format failed", NULL);
    return;
  }

  data = cJSON_CreateObject();
  cJSON_AddNumberToObject(data, "deleted", (double)deleted);
  cJSON_AddStringToObject(data, "note", "soft clear records/snapshots/alarms (not mkfs)");
  json_reply(c, 0, "ok", data);
}

static void on_detect_motion_record_cb(const DetectEvent *ev, void *user)
{
  int link = 1;
  (void)ev;
  (void)user;
  /* Only motion-record policy consumes motion to start/extend segments.
   * Continuous must not be switched into motion mode by this callback. */
  if (strcmp(g_record_policy_mode, "motion") != 0) {
    return;
  }
  config_get_bool("detect.link.record", &link, 1);
  if (link) {
    record_on_motion();
  }
}

/* Start IVS whenever media+detect VI are up (all record policies). */
static int web_start_detect_runtime_if_needed(void)
{
  int enabled = 1;
  DetectConfig dcfg;

  if (detect_is_running()) {
    return 0;
  }

  config_get_bool("detect.enabled", &enabled, 1);
  if (!enabled) {
    return 0;
  }

  if (!media_stream_is_up() || !media_stream_detect_enabled()) {
    log_warn("web", "detect runtime needs media + VI2");
    return -1;
  }

  detect_config_load(&dcfg);
  dcfg.enabled = enabled;
  detect_set_motion_cb(on_detect_motion_record_cb, NULL);
  if (detect_init(&dcfg) != 0 || detect_start() != 0) {
    log_error("web", "detect init/start failed");
    return -2;
  }
  log_info("web", "detect runtime on (%dx%d)", media_stream_detect_width(),
           media_stream_detect_height());
  {
    FaceConfig fcfg;
    int face_en = 0;
    face_config_load(&fcfg);
    config_get_bool("face.enabled", &face_en, 0);
    fcfg.enabled = face_en;
    if (face_en) {
      if (face_init(&fcfg) == 0) {
        face_start();
      } else {
        log_warn("web", "face init failed (disabled until fixed)");
      }
    }
  }
  return 0;
}

static int web_start_motion_automation_if_needed(void)
{
  RecordConfig rcfg;

  if (strcmp(g_record_policy_mode, "motion") != 0) {
    /* Continuous/schedule: still run detect for alarms/events. */
    return web_start_detect_runtime_if_needed();
  }
  if (g_motion_automation_on) {
    return 0;
  }

  if (web_start_detect_runtime_if_needed() != 0) {
    return -1;
  }

  record_config_load(&rcfg);
  record_deinit();
  if (record_init(&rcfg) != 0 || record_arm_motion() != 0) {
    log_error("web", "motion automation record init/arm failed");
    detect_stop();
    return -2;
  }

  g_motion_automation_on = 1;
  return 0;
}

static void web_stop_motion_automation(void)
{
  if (g_motion_automation_on) {
    record_stop();
    g_motion_automation_on = 0;
  }
  detect_stop();
}

static int web_ensure_media_for_policy(void)
{
  MediaEncodeConfig ecfg;
  if (media_stream_is_up()) {
    return 0;
  }
  preview_load_encode(&ecfg);
  log_info("web", "policy starting media sub=%dx%d", ecfg.sub_w, ecfg.sub_h);
  if (media_stream_start(&ecfg) != 0) {
    log_error("web", "policy media_stream_start failed");
    return -1;
  }
  return 0;
}

static int web_start_continuous_record(void)
{
  RecordConfig rcfg;
  record_config_load(&rcfg);
  if (web_ensure_media_for_policy() != 0) {
    return -1;
  }
  record_deinit();
  if (record_init(&rcfg) != 0 || record_start() != 0) {
    log_error("web", "continuous record_init/start failed");
    return -2;
  }
  return 0;
}

static void web_schedule_tick(void)
{
  RecordConfig cfg;
  time_t now;
  struct tm tm_now;
  time_t slot;
  int allow;
  int running;

  if (!g_schedule_poll_on) {
    return;
  }
  record_config_load(&cfg);
  now = time(NULL);
  localtime_r(&now, &tm_now);
  slot = (time_t)(tm_now.tm_yday * 24 + tm_now.tm_hour);
  allow = record_schedule_allows_now(&cfg);
  running = record_is_running() && record_mode() == RECORD_MODE_CONTINUOUS;

  if (slot == g_schedule_last_slot && ((allow && running) || (!allow && !running))) {
    return;
  }
  g_schedule_last_slot = slot;

  if (allow) {
    if (!running) {
      if (web_start_continuous_record() == 0) {
        log_info("web", "schedule: continuous on");
      }
    }
  } else if (running) {
    record_stop();
    log_info("web", "schedule: continuous off (grid)");
  }
}

int web_apply_record_policy(void)
{
  RecordConfig cfg;
  record_config_load(&cfg);
  snprintf(g_record_policy_mode, sizeof(g_record_policy_mode), "%s",
           cfg.mode[0] ? cfg.mode : "motion");

  cancel_motion_automation_schedule();
  web_stop_motion_automation();
  g_schedule_poll_on = 0;
  g_schedule_last_slot = -1;
  record_deinit();

  if (strcmp(g_record_policy_mode, "continuous") == 0) {
    g_record_policy_hold = 1;
    if (web_start_continuous_record() != 0) {
      g_record_policy_hold = 0;
      return -1;
    }
    if (web_start_detect_runtime_if_needed() != 0) {
      log_warn("web", "continuous: detect runtime not started");
    }
    log_info("web", "record policy=continuous (hold media)");
    return 0;
  }

  if (strcmp(g_record_policy_mode, "schedule") == 0) {
    g_record_policy_hold = 1;
    if (web_ensure_media_for_policy() != 0) {
      g_record_policy_hold = 0;
      return -1;
    }
    if (web_start_detect_runtime_if_needed() != 0) {
      log_warn("web", "schedule: detect runtime not started");
    }
    g_schedule_poll_on = 1;
    web_schedule_tick();
    log_info("web", "record policy=schedule (hold media, poll grid)");
    return 0;
  }

  /* motion: hold media + start IVS/record when detect.enabled (not preview-gated). */
  {
    int detect_en = 1;
    config_get_bool("detect.enabled", &detect_en, 1);
    if (detect_en) {
      g_record_policy_hold = 1;
      if (web_ensure_media_for_policy() != 0) {
        g_record_policy_hold = 0;
        log_error("web", "record policy=motion media start failed");
        return -1;
      }
      schedule_motion_automation();
      log_info("web", "record policy=motion (hold media+detect)");
      return 0;
    }
  }
  g_record_policy_hold = 0;
  if (g_prev.nclients == 0 && media_stream_is_up()) {
    media_stream_stop();
  }
  log_info("web", "record policy=motion (detect disabled)");
  return 0;
}

static cJSON *record_config_to_json(const RecordConfig *cfg)
{
  cJSON *data = cJSON_CreateObject();
  cJSON *schedule;
  if (!cfg) {
    return data;
  }
  cJSON_AddStringToObject(data, "mode", cfg->mode[0] ? cfg->mode : "motion");
  cJSON_AddNumberToObject(data, "segment_sec", cfg->segment_sec);
  cJSON_AddNumberToObject(data, "pre_record_sec", cfg->pre_record_sec);
  cJSON_AddNumberToObject(data, "quiet_sec", cfg->quiet_sec);
  cJSON_AddNumberToObject(data, "recycle_free_percent", cfg->recycle_free_percent);
  schedule = cJSON_CreateObject();
  cJSON_AddBoolToObject(schedule, "enabled", cfg->schedule_enabled ? 1 : 0);
  cJSON_AddStringToObject(schedule, "grid", cfg->schedule_grid);
  cJSON_AddItemToObject(data, "schedule", schedule);
  cJSON_AddStringToObject(data, "runtime_mode",
                          record_mode() == RECORD_MODE_CONTINUOUS
                              ? "continuous"
                              : (record_mode() == RECORD_MODE_MOTION ? "motion" : "off"));
  cJSON_AddBoolToObject(data, "running", record_is_running() ? 1 : 0);
  cJSON_AddBoolToObject(data, "policy_hold", g_record_policy_hold ? 1 : 0);
  return data;
}

static void handle_record_get(struct mg_connection *c, struct mg_http_message *hm)
{
  RecordConfig cfg;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  record_config_load(&cfg);
  json_reply(c, 0, "ok", record_config_to_json(&cfg));
}

static void handle_record_set(struct mg_connection *c, struct mg_http_message *hm)
{
  RecordConfig cfg;
  cJSON *root;
  cJSON *item;
  cJSON *schedule;
  int apply = 1;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  if (aov_is_active()) {
    json_reply(c, 2601, "aov mode: record policy locked", NULL);
    return;
  }

  record_config_load(&cfg);
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }

  item = cJSON_GetObjectItem(root, "mode");
  if (cJSON_IsString(item) && item->valuestring) {
    if (strcmp(item->valuestring, "continuous") == 0 ||
        strcmp(item->valuestring, "schedule") == 0 ||
        strcmp(item->valuestring, "motion") == 0) {
      snprintf(cfg.mode, sizeof(cfg.mode), "%s", item->valuestring);
    }
  }
  item = cJSON_GetObjectItem(root, "segment_sec");
  if (cJSON_IsNumber(item)) {
    cfg.segment_sec = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "pre_record_sec");
  if (cJSON_IsNumber(item)) {
    cfg.pre_record_sec = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "quiet_sec");
  if (cJSON_IsNumber(item)) {
    cfg.quiet_sec = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "recycle_free_percent");
  if (cJSON_IsNumber(item)) {
    cfg.recycle_free_percent = item->valueint;
  }

  schedule = cJSON_GetObjectItem(root, "schedule");
  if (cJSON_IsObject(schedule)) {
    item = cJSON_GetObjectItem(schedule, "enabled");
    if (cJSON_IsBool(item)) {
      cfg.schedule_enabled = cJSON_IsTrue(item) ? 1 : 0;
    } else if (cJSON_IsNumber(item)) {
      cfg.schedule_enabled = item->valueint ? 1 : 0;
    }
    item = cJSON_GetObjectItem(schedule, "grid");
    if (cJSON_IsString(item) && item->valuestring &&
        strlen(item->valuestring) == RECORD_SCHEDULE_GRID_LEN) {
      memcpy(cfg.schedule_grid, item->valuestring, RECORD_SCHEDULE_GRID_LEN);
      cfg.schedule_grid[RECORD_SCHEDULE_GRID_LEN] = '\0';
    }
  }

  item = cJSON_GetObjectItem(root, "apply");
  if (cJSON_IsBool(item)) {
    apply = cJSON_IsTrue(item) ? 1 : 0;
  } else if (cJSON_IsNumber(item)) {
    apply = item->valueint ? 1 : 0;
  }
  cJSON_Delete(root);

  if (cfg.segment_sec <= 0) {
    cfg.segment_sec = 30;
  }
  if (cfg.pre_record_sec <= 0) {
    cfg.pre_record_sec = 4;
  }
  if (cfg.quiet_sec <= 0) {
    cfg.quiet_sec = 30;
  }
  if (cfg.recycle_free_percent <= 0) {
    cfg.recycle_free_percent = 10;
  }
  if (cfg.recycle_free_percent > 90) {
    cfg.recycle_free_percent = 90;
  }

  if (record_config_save(&cfg) != 0) {
    json_reply(c, 2401, "save failed", NULL);
    return;
  }
  if (apply && web_apply_record_policy() != 0) {
    json_reply(c, 2402, "apply failed (media/record)", NULL);
    return;
  }
  record_config_load(&cfg);
  json_reply(c, 0, "ok", record_config_to_json(&cfg));
}

/* ---- alarm (T4.7) ---- */

static cJSON *alarm_event_to_json(const DetectEvent *ev)
{
  cJSON *obj = cJSON_CreateObject();
  cJSON *rect = cJSON_CreateArray();
  if (!ev) {
    cJSON_AddNumberToObject(obj, "ts", 0);
    cJSON_AddNumberToObject(obj, "square", 0);
    cJSON_AddNumberToObject(obj, "pct_x10", 0);
    cJSON_AddItemToArray(rect, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(rect, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(rect, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(rect, cJSON_CreateNumber(0));
    cJSON_AddItemToObject(obj, "rect", rect);
    cJSON_AddStringToObject(obj, "snapshot", "");
    return obj;
  }

  cJSON_AddNumberToObject(obj, "ts", (double)ev->ts);
  cJSON_AddNumberToObject(obj, "square", (double)ev->square);
  cJSON_AddNumberToObject(obj, "pct_x10", (double)ev->square_pct_x10);
  cJSON_AddItemToArray(rect, cJSON_CreateNumber(ev->rect_x));
  cJSON_AddItemToArray(rect, cJSON_CreateNumber(ev->rect_y));
  cJSON_AddItemToArray(rect, cJSON_CreateNumber(ev->rect_w));
  cJSON_AddItemToArray(rect, cJSON_CreateNumber(ev->rect_h));
  cJSON_AddItemToObject(obj, "rect", rect);
  cJSON_AddStringToObject(obj, "snapshot", ev->snapshot[0] ? ev->snapshot : "");
  return obj;
}

static void alarm_add_region_schedule(cJSON *data, const DetectConfig *cfg)
{
  cJSON *region;
  cJSON *regions;
  cJSON *schedule;
  cJSON *link;
  int i;

  regions = cJSON_CreateArray();
  for (i = 0; i < DETECT_MAX_REGIONS; i++) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", cfg->regions[i].enabled ? 1 : 0);
    cJSON_AddNumberToObject(r, "x", cfg->regions[i].x);
    cJSON_AddNumberToObject(r, "y", cfg->regions[i].y);
    cJSON_AddNumberToObject(r, "w", cfg->regions[i].w);
    cJSON_AddNumberToObject(r, "h", cfg->regions[i].h);
    cJSON_AddItemToArray(regions, r);
  }
  cJSON_AddItemToObject(data, "regions", regions);

  /* Legacy single region = regions[0] */
  region = cJSON_CreateObject();
  cJSON_AddBoolToObject(region, "enabled", cfg->regions[0].enabled ? 1 : 0);
  cJSON_AddNumberToObject(region, "x", cfg->regions[0].x);
  cJSON_AddNumberToObject(region, "y", cfg->regions[0].y);
  cJSON_AddNumberToObject(region, "w", cfg->regions[0].w);
  cJSON_AddNumberToObject(region, "h", cfg->regions[0].h);
  cJSON_AddItemToObject(data, "region", region);

  schedule = cJSON_CreateObject();
  cJSON_AddBoolToObject(schedule, "enabled", cfg->schedule_enabled ? 1 : 0);
  cJSON_AddNumberToObject(schedule, "start_min", cfg->schedule_start_min);
  cJSON_AddNumberToObject(schedule, "end_min", cfg->schedule_end_min);
  cJSON_AddNumberToObject(schedule, "days", (double)cfg->schedule_days);
  cJSON_AddStringToObject(schedule, "grid", cfg->schedule_grid);
  cJSON_AddItemToObject(data, "schedule", schedule);

  link = cJSON_CreateObject();
  cJSON_AddBoolToObject(link, "record", cfg->link_record ? 1 : 0);
  cJSON_AddBoolToObject(link, "snapshot", cfg->link_snapshot ? 1 : 0);
  cJSON_AddItemToObject(data, "link", link);
}

static cJSON *alarm_motion_to_json(const DetectConfig *cfg)
{
  DetectEvent last;
  cJSON *data = cJSON_CreateObject();
  int running = detect_is_running() ? 1 : 0;
  int count = detect_motion_count();

  if (cfg) {
    cJSON_AddBoolToObject(data, "enabled", cfg->enabled ? 1 : 0);
    cJSON_AddNumberToObject(data, "sensitivity", cfg->sensitivity);
    cJSON_AddNumberToObject(data, "square_pct", cfg->square_pct);
    cJSON_AddNumberToObject(data, "hit_frames", cfg->hit_frames);
    alarm_add_region_schedule(data, cfg);
  }
  cJSON_AddBoolToObject(data, "running", running);
  cJSON_AddNumberToObject(data, "motion_count", (double)count);

  if (detect_last_event(&last) == 0) {
    cJSON_AddItemToObject(data, "last_event", alarm_event_to_json(&last));
  } else {
    cJSON_AddItemToObject(data, "last_event", alarm_event_to_json(NULL));
  }
  return data;
}

static int json_read_bool(cJSON *item, int *out)
{
  if (cJSON_IsBool(item)) {
    *out = cJSON_IsTrue(item) ? 1 : 0;
    return 1;
  }
  if (cJSON_IsNumber(item)) {
    *out = item->valueint ? 1 : 0;
    return 1;
  }
  return 0;
}

static void alarm_read_region_obj(cJSON *obj, DetectRegion *r)
{
  cJSON *item;
  if (!cJSON_IsObject(obj) || !r) {
    return;
  }
  (void)json_read_bool(cJSON_GetObjectItem(obj, "enabled"), &r->enabled);
  item = cJSON_GetObjectItem(obj, "x");
  if (cJSON_IsNumber(item)) {
    r->x = item->valueint;
  }
  item = cJSON_GetObjectItem(obj, "y");
  if (cJSON_IsNumber(item)) {
    r->y = item->valueint;
  }
  item = cJSON_GetObjectItem(obj, "w");
  if (cJSON_IsNumber(item)) {
    r->w = item->valueint;
  }
  item = cJSON_GetObjectItem(obj, "h");
  if (cJSON_IsNumber(item)) {
    r->h = item->valueint;
  }
}

static void handle_alarm_motion_get(struct mg_connection *c, struct mg_http_message *hm)
{
  DetectConfig cfg;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }

  detect_config_load(&cfg);
  json_reply(c, 0, "ok", alarm_motion_to_json(&cfg));
}

static void handle_alarm_motion_set(struct mg_connection *c, struct mg_http_message *hm)
{
  DetectConfig cfg;
  cJSON *root;
  cJSON *item;
  cJSON *region;
  cJSON *regions;
  cJSON *schedule;
  cJSON *link;
  int apply = 1;
  int i;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }

  detect_config_load(&cfg);

  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }

  item = cJSON_GetObjectItem(root, "enabled");
  (void)json_read_bool(item, &cfg.enabled);
  item = cJSON_GetObjectItem(root, "sensitivity");
  if (cJSON_IsNumber(item)) {
    cfg.sensitivity = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "square_pct");
  if (cJSON_IsNumber(item)) {
    cfg.square_pct = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "hit_frames");
  if (cJSON_IsNumber(item)) {
    cfg.hit_frames = item->valueint;
  }

  regions = cJSON_GetObjectItem(root, "regions");
  if (cJSON_IsArray(regions)) {
    int n = cJSON_GetArraySize(regions);
    if (n > DETECT_MAX_REGIONS) {
      n = DETECT_MAX_REGIONS;
    }
    for (i = 0; i < DETECT_MAX_REGIONS; i++) {
      memset(&cfg.regions[i], 0, sizeof(cfg.regions[i]));
    }
    for (i = 0; i < n; i++) {
      alarm_read_region_obj(cJSON_GetArrayItem(regions, i), &cfg.regions[i]);
    }
  } else {
    region = cJSON_GetObjectItem(root, "region");
    if (cJSON_IsObject(region)) {
      alarm_read_region_obj(region, &cfg.regions[0]);
    }
  }

  schedule = cJSON_GetObjectItem(root, "schedule");
  if (cJSON_IsObject(schedule)) {
    (void)json_read_bool(cJSON_GetObjectItem(schedule, "enabled"), &cfg.schedule_enabled);
    item = cJSON_GetObjectItem(schedule, "start_min");
    if (cJSON_IsNumber(item)) {
      cfg.schedule_start_min = item->valueint;
    }
    item = cJSON_GetObjectItem(schedule, "end_min");
    if (cJSON_IsNumber(item)) {
      cfg.schedule_end_min = item->valueint;
    }
    item = cJSON_GetObjectItem(schedule, "days");
    if (cJSON_IsNumber(item)) {
      cfg.schedule_days = (unsigned)item->valueint & 0x7fu;
    }
    item = cJSON_GetObjectItem(schedule, "grid");
    if (cJSON_IsString(item) && item->valuestring) {
      size_t glen = strlen(item->valuestring);
      if (glen == DETECT_SCHEDULE_GRID_LEN) {
        memcpy(cfg.schedule_grid, item->valuestring, DETECT_SCHEDULE_GRID_LEN);
        cfg.schedule_grid[DETECT_SCHEDULE_GRID_LEN] = '\0';
        detect_schedule_sync_days_from_grid(&cfg);
      } else if (glen == 0) {
        cfg.schedule_grid[0] = '\0';
      }
    }
  }

  link = cJSON_GetObjectItem(root, "link");
  if (cJSON_IsObject(link)) {
    (void)json_read_bool(cJSON_GetObjectItem(link, "record"), &cfg.link_record);
    (void)json_read_bool(cJSON_GetObjectItem(link, "snapshot"), &cfg.link_snapshot);
  }

  item = cJSON_GetObjectItem(root, "apply");
  if (cJSON_IsBool(item) || cJSON_IsNumber(item)) {
    (void)json_read_bool(item, &apply);
  }
  cJSON_Delete(root);

  if (cfg.sensitivity < 0) {
    cfg.sensitivity = 0;
  }
  if (cfg.sensitivity > 4) {
    cfg.sensitivity = 4;
  }
  if (cfg.square_pct <= 0) {
    cfg.square_pct = 8;
  }
  if (cfg.hit_frames <= 0) {
    cfg.hit_frames = 2;
  }
  for (i = 0; i < DETECT_MAX_REGIONS; i++) {
    if (cfg.regions[i].x < 0) {
      cfg.regions[i].x = 0;
    }
    if (cfg.regions[i].y < 0) {
      cfg.regions[i].y = 0;
    }
    if (cfg.regions[i].w < 0) {
      cfg.regions[i].w = 0;
    }
    if (cfg.regions[i].h < 0) {
      cfg.regions[i].h = 0;
    }
    cfg.regions[i].enabled = cfg.regions[i].enabled ? 1 : 0;
  }
  cfg.link_record = cfg.link_record ? 1 : 0;
  cfg.link_snapshot = cfg.link_snapshot ? 1 : 0;
  if (cfg.schedule_start_min < 0) {
    cfg.schedule_start_min = 0;
  }
  if (cfg.schedule_start_min > 1439) {
    cfg.schedule_start_min = 1439;
  }
  if (cfg.schedule_end_min < 0) {
    cfg.schedule_end_min = 0;
  }
  if (cfg.schedule_end_min > 1440) {
    cfg.schedule_end_min = 1440;
  }

  if (detect_config_save(&cfg) != 0) {
    json_reply(c, 2401, "save failed", NULL);
    return;
  }

  if (apply) {
    /* Restart detect runtime to apply IVS thresholds immediately. */
    detect_stop();
    g_motion_automation_on = 0;
    if (cfg.enabled) {
      if (strcmp(g_record_policy_mode, "motion") == 0) {
        g_record_policy_hold = 1;
        if (web_ensure_media_for_policy() != 0 ||
            web_start_motion_automation_if_needed() != 0) {
          json_reply(c, 2402, "apply failed (media/detect VI not ready)", NULL);
          return;
        }
      } else if (web_start_detect_runtime_if_needed() != 0) {
        json_reply(c, 2402, "apply failed (media/detect VI not ready)", NULL);
        return;
      }
    } else if (strcmp(g_record_policy_mode, "motion") == 0) {
      g_record_policy_hold = 0;
      if (g_prev.nclients == 0 && media_stream_is_up()) {
        media_stream_stop();
      }
    }
  }

  json_reply(c, 0, "ok", alarm_motion_to_json(&cfg));
}

static int is_snapshot_name(const char *name)
{
  /* YYYYMMDD_HHMMSS.jpg */
  size_t n;
  size_t i;
  if (!name) {
    return 0;
  }
  n = strlen(name);
  if (n != 19) {
    return 0;
  }
  if (strcmp(name + 15, ".jpg") != 0) {
    return 0;
  }
  if (name[8] != '_') {
    return 0;
  }
  for (i = 0; i < 8; i++) {
    if (!isdigit((unsigned char)name[i])) {
      return 0;
    }
  }
  for (i = 9; i < 15; i++) {
    if (!isdigit((unsigned char)name[i])) {
      return 0;
    }
  }
  return 1;
}

static void handle_alarm_snapshot(struct mg_connection *c, struct mg_http_message *hm)
{
  char file[64];
  char full[320];
  struct stat st;
  struct mg_http_serve_opts opts;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }

  memset(file, 0, sizeof(file));
  if (mg_http_get_var(&hm->query, "file", file, sizeof(file)) <= 0 || !is_snapshot_name(file)) {
    json_reply(c, 1002, "bad file", NULL);
    return;
  }

  snprintf(full, sizeof(full), "%s/%s", storage_snapshots_path(), file);
  if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
    json_reply(c, 2402, "file not found", NULL);
    return;
  }

  memset(&opts, 0, sizeof(opts));
  opts.root_dir = "/";
  opts.extra_headers = "Content-Type: image/jpeg\r\nCache-Control: no-store\r\n";
  mg_http_serve_file(c, hm, full, &opts);
}

static void handle_alarm_events(struct mg_connection *c, struct mg_http_message *hm)
{
  char path[256];
  char q_limit[16];
  int limit;
  FILE *fp;
  char line[768];
  char lines[200][768];
  int stored;
  int pos;
  int i;
  cJSON *data;
  cJSON *records;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }

  memset(q_limit, 0, sizeof(q_limit));
  limit = 50;
  if (mg_http_get_var(&hm->query, "limit", q_limit, sizeof(q_limit)) > 0) {
    int v = atoi(q_limit);
    if (v > 0 && v <= 200) {
      limit = v;
    }
  }

  snprintf(path, sizeof(path), "%s/alarms/alarms.log", storage_mount_path());
  fp = fopen(path, "r");
  if (!fp) {
    data = cJSON_CreateObject();
    records = cJSON_CreateArray();
    cJSON_AddItemToObject(data, "events", records);
    cJSON_AddNumberToObject(data, "count", 0);
    json_reply(c, 0, "ok", data);
    return;
  }

  stored = 0;
  pos = 0;
  while (fgets(line, sizeof(line), fp) != NULL) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
      line[--n] = '\0';
    }
    if (n == 0) {
      continue;
    }
    /* Face events share alarms.log; motion UI only wants detect rows. */
    if (strstr(line, "\"type\":\"face\"")) {
      continue;
    }
    if (stored < limit) {
      snprintf(lines[stored], sizeof(lines[stored]), "%s", line);
      stored++;
    } else {
      snprintf(lines[pos], sizeof(lines[pos]), "%s", line);
      pos = (pos + 1) % limit;
    }
  }
  fclose(fp);

  records = cJSON_CreateArray();
  data = cJSON_CreateObject();
  cJSON_AddNumberToObject(data, "count", (double)(stored < limit ? stored : limit));

  {
    int out_n = stored < limit ? stored : limit;
    int start = (stored < limit) ? 0 : pos;
    /* Newest first for the live event pane. */
    for (i = out_n - 1; i >= 0; i--) {
      int idx = (start + i) % limit;
      cJSON *obj = cJSON_ParseWithLength(lines[idx], strlen(lines[idx]));
      if (obj) {
        cJSON_AddItemToArray(records, obj);
      }
    }
  }

  cJSON_AddItemToObject(data, "events", records);
  json_reply(c, 0, "ok", data);
}

/* ---- face capture (F2) ---- */

static cJSON *face_cfg_to_json(const FaceConfig *cfg)
{
  cJSON *data = cJSON_CreateObject();
  cJSON *regions;
  cJSON *schedule;
  cJSON *link;
  cJSON *last;
  FaceEvent ev;
  int i;

  cJSON_AddBoolToObject(data, "enabled", cfg->enabled ? 1 : 0);
  cJSON_AddStringToObject(data, "model", "yunet_320");
  cJSON_AddStringToObject(data, "trigger", "motion_gate");
  cJSON_AddStringToObject(data, "model_path", cfg->model_path);
  cJSON_AddStringToObject(data, "sensitivity", cfg->sensitivity);
  cJSON_AddNumberToObject(data, "interval_ms", cfg->interval_ms);
  cJSON_AddNumberToObject(data, "hold_ms", cfg->hold_ms);
  cJSON_AddNumberToObject(data, "cooldown_ms", cfg->cooldown_ms);
  cJSON_AddNumberToObject(data, "confirm_frames", cfg->confirm_frames);
  cJSON_AddNumberToObject(data, "min_face_px", cfg->min_face_px);
  cJSON_AddNumberToObject(data, "max_faces", cfg->max_faces);
  cJSON_AddNumberToObject(data, "max_snap_per_frame", cfg->max_snap_per_frame);
  cJSON_AddNumberToObject(data, "face_jpeg_side", cfg->face_jpeg_side);
  cJSON_AddNumberToObject(data, "score_thr_x100", (int)(cfg->score_thr * 100.f + 0.5f));
  cJSON_AddNumberToObject(data, "margin_x100", (int)(cfg->margin * 100.f + 0.5f));
  cJSON_AddBoolToObject(data, "save_scene", cfg->save_scene ? 1 : 0);

  regions = cJSON_CreateArray();
  for (i = 0; i < FACE_MAX_REGIONS; i++) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", cfg->regions[i].enabled ? 1 : 0);
    cJSON_AddNumberToObject(r, "x", cfg->regions[i].x);
    cJSON_AddNumberToObject(r, "y", cfg->regions[i].y);
    cJSON_AddNumberToObject(r, "w", cfg->regions[i].w);
    cJSON_AddNumberToObject(r, "h", cfg->regions[i].h);
    cJSON_AddItemToArray(regions, r);
  }
  cJSON_AddItemToObject(data, "regions", regions);

  schedule = cJSON_CreateObject();
  cJSON_AddBoolToObject(schedule, "enabled", cfg->schedule_enabled ? 1 : 0);
  cJSON_AddStringToObject(schedule, "grid", cfg->schedule_grid);
  cJSON_AddItemToObject(data, "schedule", schedule);

  link = cJSON_CreateObject();
  cJSON_AddBoolToObject(link, "record", cfg->link_record ? 1 : 0);
  cJSON_AddBoolToObject(link, "snapshot", cfg->save_scene ? 1 : 0);
  cJSON_AddBoolToObject(link, "alarm", cfg->link_alarm ? 1 : 0);
  cJSON_AddItemToObject(data, "link", link);

  cJSON_AddNumberToObject(data, "quota_percent", cfg->quota_percent);
  cJSON_AddNumberToObject(data, "daily_limit", cfg->daily_limit);
  cJSON_AddBoolToObject(data, "running", face_is_running() ? 1 : 0);
  cJSON_AddNumberToObject(data, "snap_count", face_snap_count());
  cJSON_AddNumberToObject(data, "infer_count", face_infer_count());

  last = cJSON_CreateObject();
  if (face_last_event(&ev) == 0) {
    cJSON_AddNumberToObject(last, "ts", (double)ev.ts);
    cJSON_AddNumberToObject(last, "score", ev.score);
    cJSON_AddNumberToObject(last, "x", ev.x);
    cJSON_AddNumberToObject(last, "y", ev.y);
    cJSON_AddNumberToObject(last, "w", ev.w);
    cJSON_AddNumberToObject(last, "h", ev.h);
    cJSON_AddStringToObject(last, "face_file", ev.face_file);
    cJSON_AddStringToObject(last, "scene_file", ev.scene_file);
  }
  cJSON_AddItemToObject(data, "last_event", last);
  return data;
}

static int web_apply_face_runtime(const FaceConfig *cfg)
{
  face_stop();
  face_deinit();
  if (!cfg->enabled) {
    return 0;
  }
  (void)web_start_detect_runtime_if_needed();
  if (face_init(cfg) != 0) {
    return -1;
  }
  return face_start();
}

static void handle_alarm_face_get(struct mg_connection *c, struct mg_http_message *hm)
{
  FaceConfig cfg;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  face_config_load(&cfg);
  json_reply(c, 0, "ok", face_cfg_to_json(&cfg));
}

static void handle_alarm_face_set(struct mg_connection *c, struct mg_http_message *hm)
{
  FaceConfig cfg;
  cJSON *root;
  cJSON *item;
  cJSON *regions;
  cJSON *schedule;
  cJSON *link;
  int apply = 1;
  int i;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  face_config_load(&cfg);
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }

  (void)json_read_bool(cJSON_GetObjectItem(root, "enabled"), &cfg.enabled);
  item = cJSON_GetObjectItem(root, "sensitivity");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(cfg.sensitivity, sizeof(cfg.sensitivity), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "interval_ms");
  if (cJSON_IsNumber(item)) cfg.interval_ms = item->valueint;
  item = cJSON_GetObjectItem(root, "hold_ms");
  if (cJSON_IsNumber(item)) cfg.hold_ms = item->valueint;
  item = cJSON_GetObjectItem(root, "cooldown_ms");
  if (cJSON_IsNumber(item)) cfg.cooldown_ms = item->valueint;
  item = cJSON_GetObjectItem(root, "confirm_frames");
  if (cJSON_IsNumber(item)) cfg.confirm_frames = item->valueint;
  item = cJSON_GetObjectItem(root, "min_face_px");
  if (cJSON_IsNumber(item)) cfg.min_face_px = item->valueint;
  item = cJSON_GetObjectItem(root, "max_faces");
  if (cJSON_IsNumber(item)) cfg.max_faces = item->valueint;
  item = cJSON_GetObjectItem(root, "max_snap_per_frame");
  if (cJSON_IsNumber(item)) cfg.max_snap_per_frame = item->valueint;
  item = cJSON_GetObjectItem(root, "face_jpeg_side");
  if (cJSON_IsNumber(item)) cfg.face_jpeg_side = item->valueint;
  item = cJSON_GetObjectItem(root, "score_thr_x100");
  if (cJSON_IsNumber(item)) cfg.score_thr = item->valueint / 100.f;
  item = cJSON_GetObjectItem(root, "margin_x100");
  if (cJSON_IsNumber(item)) cfg.margin = item->valueint / 100.f;
  item = cJSON_GetObjectItem(root, "quota_percent");
  if (cJSON_IsNumber(item)) cfg.quota_percent = item->valueint;
  item = cJSON_GetObjectItem(root, "daily_limit");
  if (cJSON_IsNumber(item)) cfg.daily_limit = item->valueint;
  (void)json_read_bool(cJSON_GetObjectItem(root, "save_scene"), &cfg.save_scene);

  regions = cJSON_GetObjectItem(root, "regions");
  if (cJSON_IsArray(regions)) {
    int n = cJSON_GetArraySize(regions);
    if (n > FACE_MAX_REGIONS) n = FACE_MAX_REGIONS;
    for (i = 0; i < FACE_MAX_REGIONS; i++) memset(&cfg.regions[i], 0, sizeof(cfg.regions[i]));
    for (i = 0; i < n; i++) {
      cJSON *r = cJSON_GetArrayItem(regions, i);
      (void)json_read_bool(cJSON_GetObjectItem(r, "enabled"), &cfg.regions[i].enabled);
      item = cJSON_GetObjectItem(r, "x");
      if (cJSON_IsNumber(item)) cfg.regions[i].x = item->valueint;
      item = cJSON_GetObjectItem(r, "y");
      if (cJSON_IsNumber(item)) cfg.regions[i].y = item->valueint;
      item = cJSON_GetObjectItem(r, "w");
      if (cJSON_IsNumber(item)) cfg.regions[i].w = item->valueint;
      item = cJSON_GetObjectItem(r, "h");
      if (cJSON_IsNumber(item)) cfg.regions[i].h = item->valueint;
    }
  }

  schedule = cJSON_GetObjectItem(root, "schedule");
  if (cJSON_IsObject(schedule)) {
    (void)json_read_bool(cJSON_GetObjectItem(schedule, "enabled"), &cfg.schedule_enabled);
    item = cJSON_GetObjectItem(schedule, "grid");
    if (cJSON_IsString(item) && item->valuestring &&
        strlen(item->valuestring) == FACE_SCHEDULE_GRID_LEN) {
      memcpy(cfg.schedule_grid, item->valuestring, FACE_SCHEDULE_GRID_LEN);
      cfg.schedule_grid[FACE_SCHEDULE_GRID_LEN] = '\0';
    }
  }

  link = cJSON_GetObjectItem(root, "link");
  if (cJSON_IsObject(link)) {
    (void)json_read_bool(cJSON_GetObjectItem(link, "record"), &cfg.link_record);
    (void)json_read_bool(cJSON_GetObjectItem(link, "snapshot"), &cfg.save_scene);
    (void)json_read_bool(cJSON_GetObjectItem(link, "alarm"), &cfg.link_alarm);
  }

  (void)json_read_bool(cJSON_GetObjectItem(root, "apply"), &apply);
  cJSON_Delete(root);

  face_apply_sensitivity(&cfg);
  if (cfg.interval_ms < 100) cfg.interval_ms = 100;
  if (cfg.hold_ms < 500) cfg.hold_ms = 500;
  if (cfg.cooldown_ms < 500) cfg.cooldown_ms = 500;
  if (cfg.max_snap_per_frame < 1) cfg.max_snap_per_frame = 1;
  if (cfg.max_snap_per_frame > 4) cfg.max_snap_per_frame = 4;
  if (cfg.quota_percent < 1) cfg.quota_percent = 1;
  if (cfg.quota_percent > 50) cfg.quota_percent = 50;
  if (cfg.daily_limit < 10) cfg.daily_limit = 10;
  if (cfg.daily_limit > 20000) cfg.daily_limit = 20000;
  if (strcmp(cfg.sensitivity, "high") != 0 && strcmp(cfg.sensitivity, "low") != 0) {
    snprintf(cfg.sensitivity, sizeof(cfg.sensitivity), "normal");
  }
  for (i = 0; i < FACE_MAX_REGIONS; i++) {
    if (cfg.regions[i].x < 0) cfg.regions[i].x = 0;
    if (cfg.regions[i].y < 0) cfg.regions[i].y = 0;
    if (cfg.regions[i].w < 0) cfg.regions[i].w = 0;
    if (cfg.regions[i].h < 0) cfg.regions[i].h = 0;
    if (cfg.regions[i].x > 639) cfg.regions[i].x = 639;
    if (cfg.regions[i].y > 359) cfg.regions[i].y = 359;
    if (cfg.regions[i].w > 640) cfg.regions[i].w = 640;
    if (cfg.regions[i].h > 360) cfg.regions[i].h = 360;
    cfg.regions[i].enabled = cfg.regions[i].enabled ? 1 : 0;
  }
  snprintf(cfg.model_path, sizeof(cfg.model_path), "/mnt/sdcard/models/yunet_320.rknn");

  if (face_config_save(&cfg) != 0) {
    json_reply(c, 2301, "save failed", NULL);
    return;
  }
  if (apply) {
    if (web_apply_face_runtime(&cfg) != 0 && cfg.enabled) {
      json_reply(c, 2302, "apply failed (open preview / ensure model)", face_cfg_to_json(&cfg));
      return;
    }
  }
  json_reply(c, 0, "ok", face_cfg_to_json(&cfg));
}

static int is_face_rel_path(const char *rel)
{
  size_t n;
  size_t i;
  const char *base;
  if (!rel || strchr(rel, '\\') || strstr(rel, "..")) return 0;
  n = strlen(rel);
  if (n < 28 || n > 80) return 0;
  if (rel[8] != '/') return 0;
  for (i = 0; i < 8; i++) {
    if (!isdigit((unsigned char)rel[i])) return 0;
  }
  base = rel + 9;
  if (strncmp(base, rel, 8) != 0) return 0;
  if (base[8] != '_') return 0;
  if (!strstr(base, ".jpg")) return 0;
  return 1;
}

static void handle_alarm_face_events(struct mg_connection *c, struct mg_http_message *hm)
{
  char date[16];
  char limit_s[16];
  char offset_s[16];
  char dir[320];
  DIR *dp;
  struct dirent *de;
  int limit = 50;
  int offset = 0;
  int total = 0;
  int emitted = 0;
  cJSON *data;
  cJSON *arr;
  char names[256][96];
  int nnames = 0;
  int i;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  memset(date, 0, sizeof(date));
  memset(limit_s, 0, sizeof(limit_s));
  memset(offset_s, 0, sizeof(offset_s));
  if (mg_http_get_var(&hm->query, "date", date, sizeof(date)) <= 0 || !is_date8(date)) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof(date), "%Y%m%d", &tm);
  }
  if (mg_http_get_var(&hm->query, "limit", limit_s, sizeof(limit_s)) > 0) {
    int v = atoi(limit_s);
    if (v > 0 && v <= 200) limit = v;
  }
  if (mg_http_get_var(&hm->query, "offset", offset_s, sizeof(offset_s)) > 0) {
    int v = atoi(offset_s);
    if (v >= 0) offset = v;
  }

  snprintf(dir, sizeof(dir), "%s/%s", storage_faces_path(), date);
  dp = opendir(dir);
  data = cJSON_CreateObject();
  arr = cJSON_CreateArray();
  cJSON_AddStringToObject(data, "date", date);
  if (!dp) {
    cJSON_AddNumberToObject(data, "total", 0);
    cJSON_AddNumberToObject(data, "count", 0);
    cJSON_AddItemToObject(data, "events", arr);
    json_reply(c, 0, "ok", data);
    return;
  }
  while ((de = readdir(dp)) != NULL && nnames < 256) {
    size_t ln;
    if (de->d_name[0] == '.') continue;
    ln = strlen(de->d_name);
    if (ln < 10 || ln >= sizeof(names[0]) || strcmp(de->d_name + ln - 4, ".jpg") != 0) continue;
    memcpy(names[nnames], de->d_name, ln + 1);
    nnames++;
  }
  closedir(dp);

  for (i = 0; i < nnames / 2; i++) {
    char tmp[96];
    memcpy(tmp, names[i], sizeof(tmp));
    memcpy(names[i], names[nnames - 1 - i], sizeof(names[i]));
    memcpy(names[nnames - 1 - i], tmp, sizeof(names[nnames - 1 - i]));
  }

  total = nnames;
  for (i = 0; i < nnames; i++) {
    cJSON *ev;
    char rel[160];
    if (i < offset) continue;
    if (emitted >= limit) break;
    snprintf(rel, sizeof(rel), "%s/%s", date, names[i]);
    ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "face");
    cJSON_AddStringToObject(ev, "face_file", rel);
    cJSON_AddStringToObject(ev, "file", names[i]);
    cJSON_AddItemToArray(arr, ev);
    emitted++;
  }
  cJSON_AddNumberToObject(data, "total", total);
  cJSON_AddNumberToObject(data, "count", emitted);
  cJSON_AddNumberToObject(data, "offset", offset);
  cJSON_AddNumberToObject(data, "limit", limit);
  cJSON_AddItemToObject(data, "events", arr);
  json_reply(c, 0, "ok", data);
}

static void handle_alarm_face_image(struct mg_connection *c, struct mg_http_message *hm)
{
  char file[160];
  char full[400];
  struct stat st;
  struct mg_http_serve_opts opts;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  memset(file, 0, sizeof(file));
  if (mg_http_get_var(&hm->query, "file", file, sizeof(file)) <= 0 || !is_face_rel_path(file)) {
    json_reply(c, 1002, "bad file", NULL);
    return;
  }
  snprintf(full, sizeof(full), "%s/%s", storage_faces_path(), file);
  if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
    json_reply(c, 2303, "not found", NULL);
    return;
  }
  memset(&opts, 0, sizeof(opts));
  opts.root_dir = "/";
  opts.extra_headers = "Content-Type: image/jpeg\r\nCache-Control: no-store\r\n";
  mg_http_serve_file(c, hm, full, &opts);
}

static void handle_alarm_face_events_delete(struct mg_connection *c, struct mg_http_message *hm)
{
  cJSON *root;
  cJSON *files;
  cJSON *date_item;
  int deleted = 0;
  int i;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  files = cJSON_GetObjectItem(root, "files");
  date_item = cJSON_GetObjectItem(root, "date");
  if (cJSON_IsArray(files)) {
    int n = cJSON_GetArraySize(files);
    for (i = 0; i < n; i++) {
      cJSON *it = cJSON_GetArrayItem(files, i);
      char full[400];
      if (!cJSON_IsString(it) || !it->valuestring || !is_face_rel_path(it->valuestring)) continue;
      snprintf(full, sizeof(full), "%s/%s", storage_faces_path(), it->valuestring);
      if (unlink(full) == 0) deleted++;
    }
  } else if (cJSON_IsString(date_item) && date_item->valuestring && is_date8(date_item->valuestring)) {
    char dirpath[320];
    DIR *dp;
    struct dirent *de;
    snprintf(dirpath, sizeof(dirpath), "%s/%s", storage_faces_path(), date_item->valuestring);
    dp = opendir(dirpath);
    if (dp) {
      while ((de = readdir(dp)) != NULL) {
        char full[640];
        size_t dlen;
        size_t dn;
        if (de->d_name[0] == '.') continue;
        dn = strlen(de->d_name);
        dlen = strlen(dirpath);
        if (dn == 0 || dlen + 1 + dn + 1 > sizeof(full)) continue;
        memcpy(full, dirpath, dlen);
        full[dlen] = '/';
        memcpy(full + dlen + 1, de->d_name, dn + 1);
        if (unlink(full) == 0) deleted++;
      }
      closedir(dp);
      rmdir(dirpath);
    }
  } else {
    cJSON_Delete(root);
    json_reply(c, 1002, "files or date required", NULL);
    return;
  }
  cJSON_Delete(root);
  {
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "deleted", deleted);
    json_reply(c, 0, "ok", data);
  }
}

static cJSON *system_info_to_json(const SystemInfo *info)
{
  cJSON *data = cJSON_CreateObject();
  cJSON_AddStringToObject(data, "device_name", info->device_name);
  cJSON_AddStringToObject(data, "hostname", info->hostname);
  cJSON_AddStringToObject(data, "model", info->model);
  cJSON_AddStringToObject(data, "version", info->version);
  cJSON_AddNumberToObject(data, "uptime_sec", (double)info->uptime_sec);
  cJSON_AddNumberToObject(data, "mem_total_kb", (double)info->mem_total_kb);
  cJSON_AddNumberToObject(data, "mem_free_kb", (double)info->mem_free_kb);
  return data;
}

static cJSON *system_time_to_json(const SystemTimeConfig *tc)
{
  cJSON *data = cJSON_CreateObject();
  cJSON_AddNumberToObject(data, "unix_time", (double)tc->unix_time);
  cJSON_AddStringToObject(data, "timezone", tc->timezone);
  cJSON_AddBoolToObject(data, "ntp_enabled", tc->ntp_enabled ? 1 : 0);
  cJSON_AddStringToObject(data, "ntp_server", tc->ntp_server);
  return data;
}

static cJSON *aov_status_to_json(void)
{
  AovStatus st;
  AovConfig cfg;
  cJSON *data = cJSON_CreateObject();
  aov_get_status(&st);
  aov_load_config(&cfg);
  cJSON_AddBoolToObject(data, "in_aov", st.in_aov ? 1 : 0);
  cJSON_AddStringToObject(data, "state", aov_state_name(st.state));
  cJSON_AddStringToObject(data, "last_wake", aov_wake_name(st.last_wake));
  cJSON_AddNumberToObject(data, "cycles", (double)st.cycles);
  cJSON_AddNumberToObject(data, "lapse_frames", (double)st.lapse_frames);
  cJSON_AddNumberToObject(data, "event_records", (double)st.event_records);
  cJSON_AddNumberToObject(data, "sleep_fail", (double)st.sleep_fail);
  cJSON_AddNumberToObject(data, "media_reinit", (double)st.media_reinit);
  cJSON_AddNumberToObject(data, "last_wake_ms", st.last_wake_ms);
  cJSON_AddBoolToObject(data, "enabled", cfg.enabled ? 1 : 0);
  cJSON_AddNumberToObject(data, "suspend_ms", cfg.suspend_ms);
  cJSON_AddNumberToObject(data, "event_fps", cfg.event_fps);
  cJSON_AddNumberToObject(data, "quiet_sec", cfg.quiet_sec);
  cJSON_AddNumberToObject(data, "event_record_sec", cfg.event_record_sec);
  cJSON_AddBoolToObject(data, "allow_mem_suspend", cfg.allow_mem_suspend ? 1 : 0);
  cJSON_AddBoolToObject(data, "down_eth", cfg.down_eth ? 1 : 0);
  return data;
}

static void handle_aov_status(struct mg_connection *c, struct mg_http_message *hm)
{
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  aov_init();
  json_reply(c, 0, "ok", aov_status_to_json());
}

static void handle_aov_config_get(struct mg_connection *c, struct mg_http_message *hm)
{
  handle_aov_status(c, hm);
}

static void handle_aov_config_set(struct mg_connection *c, struct mg_http_message *hm)
{
  AovConfig cfg;
  cJSON *root;
  cJSON *item;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  aov_init();
  aov_load_config(&cfg);
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1003, "bad json", NULL);
    return;
  }
  if ((item = cJSON_GetObjectItem(root, "suspend_ms")) && cJSON_IsNumber(item)) {
    cfg.suspend_ms = item->valueint;
  }
  if ((item = cJSON_GetObjectItem(root, "event_fps")) && cJSON_IsNumber(item)) {
    cfg.event_fps = item->valueint;
  }
  if ((item = cJSON_GetObjectItem(root, "quiet_sec")) && cJSON_IsNumber(item)) {
    cfg.quiet_sec = item->valueint;
  }
  if ((item = cJSON_GetObjectItem(root, "event_record_sec")) && cJSON_IsNumber(item)) {
    cfg.event_record_sec = item->valueint;
  }
  if ((item = cJSON_GetObjectItem(root, "down_eth")) && (cJSON_IsBool(item) || cJSON_IsNumber(item))) {
    cfg.down_eth = cJSON_IsTrue(item) || item->valueint != 0;
  }
  if ((item = cJSON_GetObjectItem(root, "allow_mem_suspend")) &&
      (cJSON_IsBool(item) || cJSON_IsNumber(item))) {
    cfg.allow_mem_suspend = cJSON_IsTrue(item) || item->valueint != 0;
  }
  if ((item = cJSON_GetObjectItem(root, "enabled")) && (cJSON_IsBool(item) || cJSON_IsNumber(item))) {
    cfg.enabled = cJSON_IsTrue(item) || item->valueint != 0;
  }
  cJSON_Delete(root);
  if (aov_save_config(&cfg) != 0) {
    json_reply(c, 2602, "aov config save failed", NULL);
    return;
  }
  json_reply(c, 0, "ok", aov_status_to_json());
}

static void handle_aov_enter(struct mg_connection *c, struct mg_http_message *hm)
{
  cJSON *root;
  cJSON *item;
  int confirm = 0;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (root) {
    item = cJSON_GetObjectItem(root, "confirm");
    if (item && (cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint))) {
      confirm = 1;
    }
    cJSON_Delete(root);
  }
  if (!confirm) {
    json_reply(c, 1003, "confirm required", NULL);
    return;
  }
  aov_init();
  if (aov_enter() != 0) {
    json_reply(c, 2603, "aov enter failed", NULL);
    return;
  }
  json_reply(c, 0, "ok", aov_status_to_json());
}

static void handle_aov_leave(struct mg_connection *c, struct mg_http_message *hm)
{
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  (void)hm;
  aov_init();
  if (aov_leave() != 0) {
    json_reply(c, 2604, "aov leave failed", NULL);
    return;
  }
  (void)web_apply_record_policy();
  json_reply(c, 0, "ok", aov_status_to_json());
}

static void handle_system_info(struct mg_connection *c, struct mg_http_message *hm)
{
  SystemInfo info;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  if (system_get_info(&info) != 0) {
    json_reply(c, 2502, "system info failed", NULL);
    return;
  }
  json_reply(c, 0, "ok", system_info_to_json(&info));
}

/* Public device name for login / branding (no secrets). */
static void handle_system_device_name(struct mg_connection *c, struct mg_http_message *hm)
{
  SystemInfo info;
  cJSON *data;
  (void)hm;
  if (system_get_info(&info) != 0) {
    json_reply(c, 2502, "system info failed", NULL);
    return;
  }
  data = cJSON_CreateObject();
  cJSON_AddStringToObject(data, "device_name", info.device_name);
  json_reply(c, 0, "ok", data);
}

static void handle_system_info_set(struct mg_connection *c, struct mg_http_message *hm)
{
  cJSON *root;
  const char *name;
  SystemInfo info;
  int rc;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "device_name"));
  if (!name || !name[0]) {
    cJSON_Delete(root);
    json_reply(c, 1002, "device_name required", NULL);
    return;
  }
  rc = system_set_device_name(name);
  cJSON_Delete(root);
  if (rc != 0) {
    json_reply(c, 1002, "invalid device_name", NULL);
    return;
  }
  if (system_get_info(&info) != 0) {
    json_reply(c, 2502, "system info failed", NULL);
    return;
  }
  json_reply(c, 0, "ok", system_info_to_json(&info));
}

static void handle_system_time_get(struct mg_connection *c, struct mg_http_message *hm)
{
  SystemTimeConfig tc;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  if (system_get_time(&tc) != 0) {
    json_reply(c, 2502, "time get failed", NULL);
    return;
  }
  json_reply(c, 0, "ok", system_time_to_json(&tc));
}

static void handle_system_time_set(struct mg_connection *c, struct mg_http_message *hm)
{
  SystemTimeConfig tc;
  cJSON *root;
  cJSON *item;
  int apply_ntp = 0;
  int rc;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  system_get_time(&tc);

  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  item = cJSON_GetObjectItem(root, "unix_time");
  if (cJSON_IsNumber(item)) {
    tc.unix_time = (time_t)item->valuedouble;
  }
  item = cJSON_GetObjectItem(root, "timezone");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(tc.timezone, sizeof(tc.timezone), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "ntp_enabled");
  if (cJSON_IsBool(item)) {
    tc.ntp_enabled = cJSON_IsTrue(item) ? 1 : 0;
  } else if (cJSON_IsNumber(item)) {
    tc.ntp_enabled = item->valueint ? 1 : 0;
  }
  item = cJSON_GetObjectItem(root, "ntp_server");
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(tc.ntp_server, sizeof(tc.ntp_server), "%s", item->valuestring);
  }
  item = cJSON_GetObjectItem(root, "apply_ntp");
  if (cJSON_IsBool(item)) {
    apply_ntp = cJSON_IsTrue(item) ? 1 : 0;
  } else if (cJSON_IsNumber(item)) {
    apply_ntp = item->valueint ? 1 : 0;
  }
  cJSON_Delete(root);

  rc = system_set_time(&tc, apply_ntp);
  if (rc == -3) {
    json_reply(c, 2502, "ntp sync failed", NULL);
    return;
  }
  if (rc != 0) {
    json_reply(c, 2501, "time set failed", NULL);
    return;
  }
  system_get_time(&tc);
  json_reply(c, 0, "ok", system_time_to_json(&tc));
}

static void handle_system_reboot(struct mg_connection *c, struct mg_http_message *hm)
{
  cJSON *root;
  cJSON *item;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  item = cJSON_GetObjectItem(root, "confirm");
  cJSON_Delete(root);
  if (!cJSON_IsTrue(item)) {
    json_reply(c, 1002, "confirm required", NULL);
    return;
  }
  if (system_reboot() != 0) {
    json_reply(c, 2502, "reboot failed", NULL);
    return;
  }
  json_reply(c, 0, "ok", NULL);
}

static void handle_system_reset(struct mg_connection *c, struct mg_http_message *hm)
{
  cJSON *root;
  cJSON *item;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  item = cJSON_GetObjectItem(root, "confirm");
  cJSON_Delete(root);
  if (!cJSON_IsTrue(item)) {
    json_reply(c, 1002, "confirm required", NULL);
    return;
  }
  if (system_factory_reset() != 0) {
    json_reply(c, 2501, "reset failed", NULL);
    return;
  }
  clear_token();
  auth_load_or_default();
  json_reply(c, 0, "ok", NULL);
}

static void handle_system_log(struct mg_connection *c, struct mg_http_message *hm)
{
  char name[64];
  char path[512];
  char tail_s[16];
  int tail_n = 0;
  struct mg_http_serve_opts opts;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  memset(name, 0, sizeof(name));
  if (mg_http_get_var(&hm->query, "file", name, sizeof(name)) <= 0 || !name[0]) {
    snprintf(name, sizeof(name), "ipc_app.log");
  }
  memset(tail_s, 0, sizeof(tail_s));
  if (mg_http_get_var(&hm->query, "tail", tail_s, sizeof(tail_s)) > 0) {
    tail_n = atoi(tail_s);
  }
  if (tail_n > 0) {
    char *buf;
    cJSON *data;
    cJSON *arr;
    char *line;
    char *save = NULL;
    int cap = 256 * 1024;
    buf = (char *)malloc((size_t)cap);
    if (!buf) {
      json_reply(c, 2502, "oom", NULL);
      return;
    }
    if (system_log_tail(name, tail_n, buf, cap) != 0) {
      free(buf);
      json_reply(c, 2502, "log not found", NULL);
      return;
    }
    data = cJSON_CreateObject();
    arr = cJSON_CreateArray();
    cJSON_AddStringToObject(data, "file", name);
    cJSON_AddNumberToObject(data, "tail", tail_n);
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
      cJSON_AddItemToArray(arr, cJSON_CreateString(line));
    }
    cJSON_AddItemToObject(data, "lines", arr);
    free(buf);
    json_reply(c, 0, "ok", data);
    return;
  }
  if (system_log_path(name, path, (int)sizeof(path)) != 0) {
    json_reply(c, 2502, "log not found", NULL);
    return;
  }
  memset(&opts, 0, sizeof(opts));
  opts.root_dir = "/";
  opts.extra_headers = "Content-Disposition: attachment\r\n";
  mg_http_serve_file(c, hm, path, &opts);
}

static cJSON *reboot_schedule_to_json(const SystemRebootSchedule *cfg)
{
  cJSON *data = cJSON_CreateObject();
  cJSON_AddBoolToObject(data, "enabled", cfg->enabled ? 1 : 0);
  cJSON_AddNumberToObject(data, "hour", cfg->hour);
  cJSON_AddNumberToObject(data, "minute", cfg->minute);
  cJSON_AddStringToObject(data, "days", cfg->days);
  return data;
}

static void handle_reboot_schedule_get(struct mg_connection *c, struct mg_http_message *hm)
{
  SystemRebootSchedule cfg;
  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  system_reboot_schedule_get(&cfg);
  json_reply(c, 0, "ok", reboot_schedule_to_json(&cfg));
}

static void handle_reboot_schedule_set(struct mg_connection *c, struct mg_http_message *hm)
{
  SystemRebootSchedule cfg;
  cJSON *root;
  cJSON *item;
  const char *days;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  system_reboot_schedule_get(&cfg);
  root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
  if (!root) {
    json_reply(c, 1002, "bad json", NULL);
    return;
  }
  item = cJSON_GetObjectItem(root, "enabled");
  if (cJSON_IsBool(item) || cJSON_IsNumber(item)) {
    cfg.enabled = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint);
  }
  item = cJSON_GetObjectItem(root, "hour");
  if (cJSON_IsNumber(item)) {
    cfg.hour = item->valueint;
  }
  item = cJSON_GetObjectItem(root, "minute");
  if (cJSON_IsNumber(item)) {
    cfg.minute = item->valueint;
  }
  days = cJSON_GetStringValue(cJSON_GetObjectItem(root, "days"));
  if (days) {
    snprintf(cfg.days, sizeof(cfg.days), "%s", days);
  }
  cJSON_Delete(root);
  if (system_reboot_schedule_set(&cfg) != 0) {
    json_reply(c, 1002, "invalid reboot_schedule", NULL);
    return;
  }
  system_reboot_schedule_get(&cfg);
  json_reply(c, 0, "ok", reboot_schedule_to_json(&cfg));
}

/* ---- preview (T4.3) ---- */

static void preview_client_clear_q(PreviewClient *pc)
{
  int i;
  for (i = 0; i < PREVIEW_Q; i++) {
    free(pc->q[i].buf);
    pc->q[i].buf = NULL;
    pc->q[i].len = 0;
  }
  pc->q_r = 0;
  pc->q_n = 0;
}

static void preview_enqueue_locked(int stream, const uint8_t *data, uint32_t len, int key)
{
  int i;
  for (i = 0; i < PREVIEW_MAX_CLIENTS; i++) {
    PreviewClient *pc = &g_prev.clients[i];
    PreviewItem *it;
    int slot;
    if (!pc->used || !pc->c || pc->stream != stream) {
      continue;
    }
    if (pc->wait_key) {
      if (!key) {
        continue;
      }
      pc->wait_key = 0;
    }
    if (pc->c->send.len > 512 * 1024) {
      continue;
    }
    if (pc->q_n >= PREVIEW_Q) {
      free(pc->q[pc->q_r].buf);
      pc->q[pc->q_r].buf = NULL;
      pc->q[pc->q_r].len = 0;
      pc->q[pc->q_r].key = 0;
      pc->q_r = (pc->q_r + 1) % PREVIEW_Q;
      pc->q_n--;
    }
    slot = (pc->q_r + pc->q_n) % PREVIEW_Q;
    it = &pc->q[slot];
    it->buf = (uint8_t *)malloc(len);
    if (!it->buf) {
      continue;
    }
    memcpy(it->buf, data, len);
    it->len = len;
    it->key = key;
    pc->q_n++;
  }
}

/* Called from fmp4_mux_push while g_prev.lock is held. user = track index. */
static void preview_emit(const uint8_t *data, uint32_t len, int is_init, int is_key, void *user)
{
  int ti = (int)(intptr_t)user;
  PreviewTrack *tr;
  if (ti < 0 || ti > 1) {
    return;
  }
  tr = &g_prev.track[ti];
  if (is_init) {
    free(tr->init);
    tr->init = (uint8_t *)malloc(len);
    if (tr->init) {
      memcpy(tr->init, data, len);
      tr->init_len = len;
      snprintf(tr->codec, sizeof(tr->codec), "%s", tr->mux.codec);
    } else {
      tr->init_len = 0;
    }
  } else if (tr->init_len > 0) {
    preview_enqueue_locked(ti, data, len, is_key);
  }
}

static void preview_on_packet(int chn, const uint8_t *data, uint32_t len, int key_frame,
                              int64_t pts_us, void *user)
{
  int ti;
  int need = 0;
  int i;
  (void)user;
  if (!data || len == 0) {
    return;
  }
  if (chn == 1) {
    ti = 0;
  } else if (chn == 3) {
    ti = 1;
  } else {
    return;
  }
  pthread_mutex_lock(&g_prev.lock);
  for (i = 0; i < PREVIEW_MAX_CLIENTS; i++) {
    if (g_prev.clients[i].used && g_prev.clients[i].stream == ti) {
      need = 1;
      break;
    }
  }
  if (need && g_prev.track[ti].mux_ready && g_prev.listening) {
    fmp4_mux_push(&g_prev.track[ti].mux, data, len, key_frame, pts_us);
  }
  pthread_mutex_unlock(&g_prev.lock);
}

static int preview_count_stream_locked(int stream)
{
  int i;
  int n = 0;
  for (i = 0; i < PREVIEW_MAX_CLIENTS; i++) {
    if (g_prev.clients[i].used && g_prev.clients[i].stream == stream) {
      n++;
    }
  }
  return n;
}

static void preview_sync_main_encode(void)
{
  int main_n;
  pthread_mutex_lock(&g_prev.lock);
  main_n = preview_count_stream_locked(1);
  pthread_mutex_unlock(&g_prev.lock);
  (void)media_stream_preview_main_set_active(main_n > 0);
}

static void preview_load_encode(MediaEncodeConfig *ecfg)
{
  memset(ecfg, 0, sizeof(*ecfg));
  config_get_string("video.iq_dir", g_prev.iq_dir, (int)sizeof(g_prev.iq_dir),
                    "/oem/usr/share/iqfiles");
  config_get_int("video.encode.main_w", &ecfg->main_w, 1920);
  config_get_int("video.encode.main_h", &ecfg->main_h, 1080);
  config_get_int("video.encode.main_fps", &ecfg->main_fps, 15);
  config_get_int("video.encode.main_bitrate_kbps", &ecfg->main_bitrate_kbps, 2048);
  config_get_int("video.encode.main_gop", &ecfg->main_gop, 30);
  config_get_int("video.encode.sub_w", &ecfg->sub_w, 704);
  config_get_int("video.encode.sub_h", &ecfg->sub_h, 576);
  config_get_int("video.encode.sub_fps", &ecfg->sub_fps, 15);
  config_get_int("video.encode.sub_bitrate_kbps", &ecfg->sub_bitrate_kbps, 1024);
  config_get_int("video.encode.sub_gop", &ecfg->sub_gop, 30);
  /* Preview: enable detect VI when motion detect is on (automation starts deferred). */
  {
    int enabled = 1;
    config_get_bool("detect.enabled", &enabled, 1);
    if (enabled) {
      config_get_int("detect.width", &ecfg->detect_w, 640);
      config_get_int("detect.height", &ecfg->detect_h, 360);
    } else {
      ecfg->detect_w = 0;
      ecfg->detect_h = 0;
    }
  }
  ecfg->iq_dir = g_prev.iq_dir;
}

static int preview_ensure_pipeline(void)
{
  MediaEncodeConfig ecfg;
  int need_start = 0;
  int need_listen = 0;
  int ti;

  pthread_mutex_lock(&g_prev.lock);
  if (!media_stream_is_up()) {
    need_start = 1;
    g_prev.media_owned = 1;
  }
  if (!g_prev.listening) {
    for (ti = 0; ti < 2; ti++) {
      if (!g_prev.track[ti].mux_ready) {
        if (fmp4_mux_init(&g_prev.track[ti].mux, preview_emit, (void *)(intptr_t)ti) != 0) {
          g_prev.media_owned = 0;
          pthread_mutex_unlock(&g_prev.lock);
          return -2;
        }
        g_prev.track[ti].mux_ready = 1;
      } else {
        fmp4_mux_reset(&g_prev.track[ti].mux);
      }
      free(g_prev.track[ti].init);
      g_prev.track[ti].init = NULL;
      g_prev.track[ti].init_len = 0;
      g_prev.track[ti].codec[0] = '\0';
    }
    g_prev.listening = 1;
    need_listen = 1;
  }
  pthread_mutex_unlock(&g_prev.lock);

  if (need_start) {
    preview_load_encode(&ecfg);
    log_info("web", "preview starting media sub=%dx%d", ecfg.sub_w, ecfg.sub_h);
    if (media_stream_start(&ecfg) != 0) {
      log_error("web", "preview media_stream_start failed");
      pthread_mutex_lock(&g_prev.lock);
      g_prev.listening = 0;
      g_prev.media_owned = 0;
      pthread_mutex_unlock(&g_prev.lock);
      return -1;
    }
  }
  if (need_listen) {
    if (media_stream_add_packet_listener(preview_on_packet, NULL) != 0) {
      log_error("web", "preview add listener failed");
      pthread_mutex_lock(&g_prev.lock);
      g_prev.listening = 0;
      pthread_mutex_unlock(&g_prev.lock);
      return -3;
    }
  }
  schedule_motion_automation();
  media_stream_request_idr(1);
  preview_sync_main_encode();
  if (media_stream_preview_main_active()) {
    media_stream_request_idr(3);
  }
  return 0;
}

/* Stop media/listener outside lock to avoid deadlock with packet callback. */
static void preview_shutdown_pipeline(void)
{
  int media_owned;
  int listening;
  int ti;

  pthread_mutex_lock(&g_prev.lock);
  listening = g_prev.listening;
  media_owned = g_prev.media_owned;
  g_prev.listening = 0;
  g_prev.media_owned = 0;
  for (ti = 0; ti < 2; ti++) {
    free(g_prev.track[ti].init);
    g_prev.track[ti].init = NULL;
    g_prev.track[ti].init_len = 0;
    g_prev.track[ti].codec[0] = '\0';
    if (g_prev.track[ti].mux_ready) {
      fmp4_mux_reset(&g_prev.track[ti].mux);
    }
  }
  pthread_mutex_unlock(&g_prev.lock);

  if (listening) {
    media_stream_remove_packet_listener(preview_on_packet, NULL);
  }
  (void)media_stream_preview_main_set_active(0);
  /* With policy hold, keep pending/running IVS+record after preview clients leave. */
  if (strcmp(g_record_policy_mode, "motion") == 0 && !g_record_policy_hold) {
    cancel_motion_automation_schedule();
    web_stop_motion_automation();
  }
  if (media_owned && !g_record_policy_hold) {
    media_stream_stop();
  } else if (media_owned && g_record_policy_hold) {
    log_info("web", "preview idle (media held by record policy)");
  }
}

void web_preview_force_stop(void)
{
  int i;
  cancel_motion_automation_schedule();
  web_stop_motion_automation();
  g_schedule_poll_on = 0;
  g_record_policy_hold = 0;
  record_deinit();
  pthread_mutex_lock(&g_prev.lock);
  for (i = 0; i < PREVIEW_MAX_CLIENTS; i++) {
    if (g_prev.clients[i].used && g_prev.clients[i].c) {
      g_prev.clients[i].c->is_closing = 1;
      g_prev.clients[i].used = 0;
      g_prev.clients[i].c = NULL;
    }
  }
  g_prev.nclients = 0;
  pthread_mutex_unlock(&g_prev.lock);
  preview_shutdown_pipeline();
  log_info("web", "preview force-stopped (AOV)");
}

static int preview_add_client(struct mg_connection *c, int stream)
{
  int i;
  int slot = -1;
  if (stream != 0 && stream != 1) {
    stream = 0;
  }
  pthread_mutex_lock(&g_prev.lock);
  for (i = 0; i < PREVIEW_MAX_CLIENTS; i++) {
    if (!g_prev.clients[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    pthread_mutex_unlock(&g_prev.lock);
    return -1;
  }
  g_prev.clients[slot].used = 1;
  g_prev.clients[slot].c = c;
  g_prev.clients[slot].bootstrapped = 0;
  g_prev.clients[slot].wait_key = 1;
  g_prev.clients[slot].stream = stream;
  preview_client_clear_q(&g_prev.clients[slot]);
  g_prev.nclients++;
  c->data[0] = PREVIEW_MARK;
  pthread_mutex_unlock(&g_prev.lock);
  g_preview_idle_deadline_ms = 0;

  if (preview_ensure_pipeline() != 0) {
    pthread_mutex_lock(&g_prev.lock);
    g_prev.clients[slot].used = 0;
    g_prev.clients[slot].c = NULL;
    g_prev.nclients--;
    c->data[0] = 0;
    pthread_mutex_unlock(&g_prev.lock);
    return -2;
  }
  log_info("web", "preview client + n=%d stream=%s", g_prev.nclients, stream ? "main" : "sub");
  preview_sync_main_encode();
  if (stream == 1) {
    media_stream_request_idr(3);
  } else {
    media_stream_request_idr(1);
  }
  return 0;
}

static void preview_remove_client(struct mg_connection *c)
{
  int i;
  int empty = 0;
  pthread_mutex_lock(&g_prev.lock);
  for (i = 0; i < PREVIEW_MAX_CLIENTS; i++) {
    if (g_prev.clients[i].used && g_prev.clients[i].c == c) {
      preview_client_clear_q(&g_prev.clients[i]);
      g_prev.clients[i].used = 0;
      g_prev.clients[i].c = NULL;
      if (g_prev.nclients > 0) {
        g_prev.nclients--;
      }
      break;
    }
  }
  empty = (g_prev.nclients == 0);
  pthread_mutex_unlock(&g_prev.lock);
  preview_sync_main_encode();
  if (empty) {
    /* Keep mux alive briefly so route switch / WS reconnect does not black-screen. */
    g_preview_idle_deadline_ms = web_now_ms() + 2000;
  }
}

static void preview_poll_client(PreviewClient *pc)
{
  char meta[80];
  PreviewTrack *tr;
  if (!pc->used || !pc->c || !pc->c->is_websocket) {
    return;
  }
  tr = &g_prev.track[pc->stream == 1 ? 1 : 0];
  if (!pc->bootstrapped) {
    if (tr->init_len == 0 || tr->codec[0] == '\0') {
      return;
    }
    snprintf(meta, sizeof(meta), "{\"codec\":\"%s\",\"stream\":\"%s\"}", tr->codec,
             pc->stream ? "main" : "sub");
    mg_ws_send(pc->c, meta, strlen(meta), WEBSOCKET_OP_TEXT);
    mg_ws_send(pc->c, tr->init, tr->init_len, WEBSOCKET_OP_BINARY);
    pc->bootstrapped = 1;
  }
  while (pc->q_n > 0) {
    PreviewItem *it = &pc->q[pc->q_r];
    if (pc->c->send.len > 256 * 1024) {
      break;
    }
    mg_ws_send(pc->c, it->buf, it->len, WEBSOCKET_OP_BINARY);
    free(it->buf);
    it->buf = NULL;
    it->len = 0;
    pc->q_r = (pc->q_r + 1) % PREVIEW_Q;
    pc->q_n--;
  }
}

static void preview_on_poll(void)
{
  int i;
  int64_t deadline;
  pthread_mutex_lock(&g_prev.lock);
  for (i = 0; i < PREVIEW_MAX_CLIENTS; i++) {
    if (g_prev.clients[i].used) {
      preview_poll_client(&g_prev.clients[i]);
    }
  }
  pthread_mutex_unlock(&g_prev.lock);

  deadline = g_preview_idle_deadline_ms;
  if (deadline > 0 && web_now_ms() >= deadline) {
    int empty;
    pthread_mutex_lock(&g_prev.lock);
    empty = (g_prev.nclients == 0);
    pthread_mutex_unlock(&g_prev.lock);
    g_preview_idle_deadline_ms = 0;
    if (empty) {
      preview_shutdown_pipeline();
      log_info("web", "preview idle%s",
               g_record_policy_hold ? " (mux reset, media held)" : " (media stopped)");
    }
  }
}

static void handle_preview_ws(struct mg_connection *c, struct mg_http_message *hm)
{
  char stream_s[16];
  int stream = 0;
  if (!auth_ok(hm)) {
    mg_http_reply(c, 401, "Content-Type: application/json\r\n",
                  "{\"code\":1001,\"msg\":\"unauthorized\",\"data\":{}}");
    return;
  }
  if (aov_is_active()) {
    mg_http_reply(c, 403, "Content-Type: application/json\r\n",
                  "{\"code\":2601,\"msg\":\"aov mode: preview unavailable\",\"data\":{}}");
    return;
  }
  memset(stream_s, 0, sizeof(stream_s));
  if (mg_http_get_var(&hm->query, "stream", stream_s, sizeof(stream_s)) > 0) {
    if (strcmp(stream_s, "main") == 0) {
      stream = 1;
    }
  }
  mg_ws_upgrade(c, hm, NULL);
  if (preview_add_client(c, stream) != 0) {
    mg_ws_send(c, "{\"err\":\"preview busy\"}", 22, WEBSOCKET_OP_TEXT);
    c->is_closing = 1;
  }
}

/* T7.2.2: manual JPEG snapshot (VENC JPEG combo) while preview pipeline is up. */
static void handle_preview_snapshot_post(struct mg_connection *c, struct mg_http_message *hm)
{
  char name[64];
  char path[320];
  time_t now;
  struct tm tm_now;
  cJSON *data;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  if (aov_is_active()) {
    json_reply(c, 2601, "aov mode: preview unavailable", NULL);
    return;
  }
  if (preview_ensure_pipeline() != 0) {
    json_reply(c, 2102, "preview media not ready", NULL);
    return;
  }
  now = time(NULL);
  localtime_r(&now, &tm_now);
  strftime(name, sizeof(name), "%Y%m%d_%H%M%S.jpg", &tm_now);
  if (storage_ensure_dirs() != 0) {
    json_reply(c, 2201, "storage not ready", NULL);
    return;
  }
  snprintf(path, sizeof(path), "%s/%s", storage_snapshots_path(), name);
  if (media_snapshot_jpeg(path) != 0) {
    json_reply(c, 2102, "snapshot failed", NULL);
    return;
  }
  data = cJSON_CreateObject();
  if (!data) {
    json_reply(c, 2102, "oom", NULL);
    return;
  }
  cJSON_AddStringToObject(data, "file", name);
  cJSON_AddStringToObject(data, "url", "/api/v1/alarm/snapshot");
  json_reply(c, 0, "ok", data);
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
  if (ev == MG_EV_CLOSE) {
    if (c->data[0] == PREVIEW_MARK) {
      preview_remove_client(c);
      c->data[0] = 0;
    }
    return;
  }
  if (ev != MG_EV_HTTP_MSG) {
    return;
  }
  struct mg_http_message *hm = (struct mg_http_message *)ev_data;

  if (mg_match(hm->uri, mg_str("/api/v1/preview/ws"), NULL)) {
    handle_preview_ws(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/preview/snapshot"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_preview_snapshot_post(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/auth/login"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_login(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/auth/logout"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_logout(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/auth/password"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_password(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/auth/me"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_me(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/network/config"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_network_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/network/config"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_network_set(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/video/image"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_video_image_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/video/image"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_video_image_set(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/video/encode"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_video_encode_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/video/encode"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_video_encode_set(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/video/osd"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_video_osd_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/video/osd"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_video_osd_set(c, hm);
    return;
  }

  if (mg_match(hm->uri, mg_str("/api/v1/storage/status"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_storage_status(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/storage/records"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_storage_records(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/storage/download"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_storage_download(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/storage/format"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_storage_format(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/record"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_record_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/record"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_record_set(c, hm);
    return;
  }

  if (mg_match(hm->uri, mg_str("/api/v1/alarm/face/events"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_alarm_face_events(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/alarm/face/events"), NULL) &&
      mg_strcmp(hm->method, mg_str("DELETE")) == 0) {
    handle_alarm_face_events_delete(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/alarm/face/image"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_alarm_face_image(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/alarm/face"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_alarm_face_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/alarm/face"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_alarm_face_set(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/alarm/motion"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_alarm_motion_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/alarm/motion"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_alarm_motion_set(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/alarm/events"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_alarm_events(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/alarm/snapshot"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_alarm_snapshot(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/aov/status"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_aov_status(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/aov/config"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_aov_config_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/aov/config"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_aov_config_set(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/aov/enter"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_aov_enter(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/aov/leave"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_aov_leave(c, hm);
    return;
  }

  if (mg_match(hm->uri, mg_str("/api/v1/system/device_name"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_system_device_name(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/system/info"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_system_info(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/system/info"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_system_info_set(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/system/time"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_system_time_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/system/time"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_system_time_set(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/system/reboot"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_system_reboot(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/system/reboot_schedule"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_reboot_schedule_get(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/system/reboot_schedule"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_reboot_schedule_set(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/system/reset"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    handle_system_reset(c, hm);
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/v1/system/log"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_system_log(c, hm);
    return;
  }

  if (mg_match(hm->uri, mg_str("/api/#"), NULL)) {
    if (!auth_ok(hm)) {
      json_reply(c, 1001, "unauthorized", NULL);
    } else {
      json_reply(c, 1002, "not implemented", NULL);
    }
    return;
  }

  {
    struct mg_http_serve_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.root_dir = g_web.www_root;
    mg_http_serve_dir(c, hm, &opts);
  }
}

static void *web_thread(void *arg)
{
  (void)arg;
  log_info("web", "listen %s www=%s", g_web.listen_url, g_web.www_root);
  while (!g_web.quit) {
    mg_mgr_poll(&g_web.mgr, 50);
    preview_on_poll();
    web_schedule_tick();
    system_reboot_schedule_tick();
  }
  return NULL;
}

int web_init(const WebConfig *cfg)
{
  if (g_web.inited) {
    return 0;
  }
  memset(&g_web.mgr, 0, sizeof(g_web.mgr));
  g_web.port = (cfg && cfg->port > 0) ? cfg->port : 8080;
  g_web.token_ttl = (cfg && cfg->token_ttl_sec > 0) ? cfg->token_ttl_sec : 86400;
  snprintf(g_web.www_root, sizeof(g_web.www_root), "%s",
           (cfg && cfg->www_root && cfg->www_root[0]) ? cfg->www_root : "/userdata/www");
  snprintf(g_web.listen_url, sizeof(g_web.listen_url), "http://0.0.0.0:%d", g_web.port);

  srand((unsigned)time(NULL) ^ (unsigned)getpid());
  auth_load_or_default();

  mg_mgr_init(&g_web.mgr);
  if (mg_http_listen(&g_web.mgr, g_web.listen_url, ev_handler, NULL) == NULL) {
    log_error("web", "listen failed %s", g_web.listen_url);
    mg_mgr_free(&g_web.mgr);
    return -1;
  }
  g_web.inited = 1;
  return 0;
}

void web_deinit(void)
{
  web_stop();
  if (!g_web.inited) {
    return;
  }
  preview_shutdown_pipeline();
  pthread_mutex_lock(&g_prev.lock);
  {
    int ti;
    for (ti = 0; ti < 2; ti++) {
      if (g_prev.track[ti].mux_ready) {
        fmp4_mux_deinit(&g_prev.track[ti].mux);
        g_prev.track[ti].mux_ready = 0;
      }
      free(g_prev.track[ti].init);
      g_prev.track[ti].init = NULL;
      g_prev.track[ti].init_len = 0;
    }
  }
  pthread_mutex_unlock(&g_prev.lock);
  mg_mgr_free(&g_web.mgr);
  g_web.inited = 0;
}

int web_start(void)
{
  if (!g_web.inited) {
    return -1;
  }
  if (g_web.running) {
    return 0;
  }
  g_web.quit = 0;
  if (pthread_create(&g_web.tid, NULL, web_thread, NULL) != 0) {
    return -2;
  }
  g_web.running = 1;
  (void)web_apply_record_policy();
  return 0;
}

void web_stop(void)
{
  if (!g_web.running) {
    return;
  }
  g_web.quit = 1;
  pthread_join(g_web.tid, NULL);
  g_web.running = 0;
  log_info("web", "stopped");
}

int web_is_running(void)
{
  return g_web.running;
}
