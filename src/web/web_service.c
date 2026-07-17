#include "web/web_service.h"

#include "common/log.h"
#include "common/sha256.h"
#include "media/media_service.h"
#include "system/config_service.h"
#include "system/network_service.h"
#include "system/storage_service.h"
#include "system/system_service.h"
#include "detect/detect_service.h"
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
  int q_r;
  int q_n;
  PreviewItem q[PREVIEW_Q];
} PreviewClient;

typedef struct {
  pthread_mutex_t lock;
  Fmp4Mux mux;
  int mux_ready;
  int listening;
  int media_owned;
  int nclients;
  PreviewClient clients[PREVIEW_MAX_CLIENTS];
  uint8_t *init;
  uint32_t init_len;
  char codec[32];
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

static int web_start_motion_automation_if_needed(void);
static void web_stop_motion_automation(void);

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
  int gen = ++g_motion_defer_gen;
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
  if (!user || !pass) {
    cJSON_Delete(body);
    json_reply(c, 1002, "username/password required", NULL);
    return;
  }

  pthread_mutex_lock(&g_web.lock);
  hash_password(g_web.salt_hex, pass, expect);
  if (strcmp(user, g_web.user) != 0 || strcmp(expect, g_web.pass_hash) != 0) {
    pthread_mutex_unlock(&g_web.lock);
    cJSON_Delete(body);
    json_reply(c, 1001, "invalid credentials", NULL);
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
  if (cJSON_IsBool(item)) {
    img.mirror = cJSON_IsTrue(item) ? 1 : 0;
  }
  item = cJSON_GetObjectItem(root, "flip");
  if (cJSON_IsBool(item)) {
    img.flip = cJSON_IsTrue(item) ? 1 : 0;
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
  if (mg_http_get_var(&hm->query, "date", date, sizeof(date)) > 0 && is_date8(date)) {
    have_date = 1;
  }
  if (mg_http_get_var(&hm->query, "limit", limit_s, sizeof(limit_s)) > 0) {
    int v = atoi(limit_s);
    if (v > 0 && v <= 200) {
      limit = v;
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
    int out_n = n;
    if (out_n > limit) {
      out_n = limit;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON *records = cJSON_CreateArray();
    int i;
    cJSON_AddStringToObject(data, "base", base);
    cJSON_AddNumberToObject(data, "total", (double)n);
    cJSON_AddNumberToObject(data, "count", (double)out_n);
    if (have_date) {
      cJSON_AddStringToObject(data, "date", date);
    }
    for (i = 0; i < out_n; i++) {
      cJSON *it = cJSON_CreateObject();
      cJSON_AddStringToObject(it, "path", arr[i].rel_path);
      cJSON_AddStringToObject(it, "name", arr[i].name);
      cJSON_AddNumberToObject(it, "mtime", (double)arr[i].mtime);
      cJSON_AddNumberToObject(it, "size", (double)arr[i].size);
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

  /* Stop motion automation so open files under records/alarms can be wiped. */
  web_stop_motion_automation();
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
  (void)ev;
  (void)user;
  record_on_motion();
}

static int web_start_motion_automation_if_needed(void)
{
  int enabled = 1;
  DetectConfig dcfg;
  RecordConfig rcfg;

  if (g_motion_automation_on) {
    return 0;
  }

  config_get_bool("detect.enabled", &enabled, 1);
  if (!enabled) {
    return 0;
  }

  if (!media_stream_is_up() || !media_stream_detect_enabled()) {
    log_warn("web", "motion automation needs preview detect VI");
    return -1;
  }

  /* Record: motion mode + muxing happens on detect callback. */
  memset(&rcfg, 0, sizeof(rcfg));
  config_get_int("record.segment_sec", &rcfg.segment_sec, 30);
  config_get_int("record.recycle_free_percent", &rcfg.recycle_free_percent, 10);
  config_get_int("record.pre_record_sec", &rcfg.pre_record_sec, 4);
  config_get_int("record.quiet_sec", &rcfg.quiet_sec, 30);

  config_get_int("video.encode.main_w", &rcfg.main_w, 1920);
  config_get_int("video.encode.main_h", &rcfg.main_h, 1080);
  config_get_int("video.encode.main_fps", &rcfg.main_fps, 15);
  config_get_int("video.encode.main_bitrate_kbps", &rcfg.main_bitrate_kbps, 2048);

  detect_stop();
  if (record_init(&rcfg) != 0 || record_arm_motion() != 0) {
    log_error("web", "motion automation record init/arm failed");
    return -2;
  }

  detect_config_load(&dcfg);
  dcfg.enabled = enabled;

  detect_set_motion_cb(on_detect_motion_record_cb, NULL);
  if (detect_init(&dcfg) != 0 || detect_start() != 0) {
    log_error("web", "motion automation detect init/start failed");
    return -3;
  }

  g_motion_automation_on = 1;
  return 0;
}

static void web_stop_motion_automation(void)
{
  if (!g_motion_automation_on) {
    return;
  }
  detect_stop();
  record_stop();
  g_motion_automation_on = 0;
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
  cJSON *region = cJSON_CreateObject();
  cJSON *schedule = cJSON_CreateObject();
  cJSON_AddBoolToObject(region, "enabled", cfg->region_enabled ? 1 : 0);
  cJSON_AddNumberToObject(region, "x", cfg->region_x);
  cJSON_AddNumberToObject(region, "y", cfg->region_y);
  cJSON_AddNumberToObject(region, "w", cfg->region_w);
  cJSON_AddNumberToObject(region, "h", cfg->region_h);
  cJSON_AddItemToObject(data, "region", region);

  cJSON_AddBoolToObject(schedule, "enabled", cfg->schedule_enabled ? 1 : 0);
  cJSON_AddNumberToObject(schedule, "start_min", cfg->schedule_start_min);
  cJSON_AddNumberToObject(schedule, "end_min", cfg->schedule_end_min);
  cJSON_AddNumberToObject(schedule, "days", (double)cfg->schedule_days);
  cJSON_AddItemToObject(data, "schedule", schedule);
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
  cJSON *schedule;
  int apply = 1;

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

  region = cJSON_GetObjectItem(root, "region");
  if (cJSON_IsObject(region)) {
    (void)json_read_bool(cJSON_GetObjectItem(region, "enabled"), &cfg.region_enabled);
    item = cJSON_GetObjectItem(region, "x");
    if (cJSON_IsNumber(item)) {
      cfg.region_x = item->valueint;
    }
    item = cJSON_GetObjectItem(region, "y");
    if (cJSON_IsNumber(item)) {
      cfg.region_y = item->valueint;
    }
    item = cJSON_GetObjectItem(region, "w");
    if (cJSON_IsNumber(item)) {
      cfg.region_w = item->valueint;
    }
    item = cJSON_GetObjectItem(region, "h");
    if (cJSON_IsNumber(item)) {
      cfg.region_h = item->valueint;
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
  if (cfg.region_x < 0) {
    cfg.region_x = 0;
  }
  if (cfg.region_y < 0) {
    cfg.region_y = 0;
  }
  if (cfg.region_w < 0) {
    cfg.region_w = 0;
  }
  if (cfg.region_h < 0) {
    cfg.region_h = 0;
  }
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
    /* Restart runtime automation to apply IVS thresholds immediately. */
    web_stop_motion_automation();
    if (cfg.enabled) {
      if (web_start_motion_automation_if_needed() != 0) {
        json_reply(c, 2402, "apply failed (open preview or ensure detect VI)", NULL);
        return;
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
  char line[512];
  char lines[200][512];
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
  cJSON_AddNumberToObject(data, "count", (double)stored);

  {
    int out_n = stored < limit ? stored : limit;
    int start = (stored < limit) ? 0 : pos;
    for (i = 0; i < out_n; i++) {
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

/* ---- system (T4.8) ---- */

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
  struct mg_http_serve_opts opts;

  if (!auth_ok(hm)) {
    json_reply(c, 1001, "unauthorized", NULL);
    return;
  }
  memset(name, 0, sizeof(name));
  if (mg_http_get_var(&hm->query, "file", name, sizeof(name)) <= 0 || !name[0]) {
    snprintf(name, sizeof(name), "ipc_app.log");
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

static void preview_enqueue_locked(const uint8_t *data, uint32_t len, int key)
{
  int i;
  for (i = 0; i < PREVIEW_MAX_CLIENTS; i++) {
    PreviewClient *pc = &g_prev.clients[i];
    PreviewItem *it;
    int slot;
    if (!pc->used || !pc->c) {
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

/* Called from fmp4_mux_push while g_prev.lock is held. */
static void preview_emit(const uint8_t *data, uint32_t len, int is_init, int is_key, void *user)
{
  (void)user;
  if (is_init) {
    free(g_prev.init);
    g_prev.init = (uint8_t *)malloc(len);
    if (g_prev.init) {
      memcpy(g_prev.init, data, len);
      g_prev.init_len = len;
      snprintf(g_prev.codec, sizeof(g_prev.codec), "%s", g_prev.mux.codec);
    } else {
      g_prev.init_len = 0;
    }
  } else if (g_prev.init_len > 0) {
    preview_enqueue_locked(data, len, is_key);
  }
}

static void preview_on_packet(int chn, const uint8_t *data, uint32_t len, int key_frame,
                              int64_t pts_us, void *user)
{
  (void)user;
  if (chn != 1 || !data || len == 0) {
    return;
  }
  pthread_mutex_lock(&g_prev.lock);
  if (g_prev.mux_ready && g_prev.listening) {
    fmp4_mux_push(&g_prev.mux, data, len, key_frame, pts_us);
  }
  pthread_mutex_unlock(&g_prev.lock);
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

  pthread_mutex_lock(&g_prev.lock);
  if (!media_stream_is_up()) {
    need_start = 1;
    g_prev.media_owned = 1;
  }
  if (!g_prev.listening) {
    if (!g_prev.mux_ready) {
      if (fmp4_mux_init(&g_prev.mux, preview_emit, NULL) != 0) {
        g_prev.media_owned = 0;
        pthread_mutex_unlock(&g_prev.lock);
        return -2;
      }
      g_prev.mux_ready = 1;
    } else {
      fmp4_mux_reset(&g_prev.mux);
    }
    free(g_prev.init);
    g_prev.init = NULL;
    g_prev.init_len = 0;
    g_prev.codec[0] = '\0';
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
  /*
   * Defer IVS+record ~3s after preview media is up so cold-start stays stable.
   * Closing the last preview client cancels the schedule and stops automation.
   */
  schedule_motion_automation();
  media_stream_request_idr(1);
  return 0;
}

/* Stop media/listener outside lock to avoid deadlock with packet callback. */
static void preview_shutdown_pipeline(void)
{
  int media_owned;
  int listening;

  pthread_mutex_lock(&g_prev.lock);
  listening = g_prev.listening;
  media_owned = g_prev.media_owned;
  g_prev.listening = 0;
  g_prev.media_owned = 0;
  free(g_prev.init);
  g_prev.init = NULL;
  g_prev.init_len = 0;
  g_prev.codec[0] = '\0';
  if (g_prev.mux_ready) {
    fmp4_mux_reset(&g_prev.mux);
  }
  pthread_mutex_unlock(&g_prev.lock);

  if (listening) {
    media_stream_remove_packet_listener(preview_on_packet, NULL);
  }
  cancel_motion_automation_schedule();
  web_stop_motion_automation();
  if (media_owned) {
    media_stream_stop();
  }
}

static int preview_add_client(struct mg_connection *c)
{
  int i;
  int slot = -1;
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
  preview_client_clear_q(&g_prev.clients[slot]);
  g_prev.nclients++;
  c->data[0] = PREVIEW_MARK;
  pthread_mutex_unlock(&g_prev.lock);

  if (preview_ensure_pipeline() != 0) {
    pthread_mutex_lock(&g_prev.lock);
    g_prev.clients[slot].used = 0;
    g_prev.clients[slot].c = NULL;
    g_prev.nclients--;
    c->data[0] = 0;
    pthread_mutex_unlock(&g_prev.lock);
    return -2;
  }
  log_info("web", "preview client + n=%d", g_prev.nclients);
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
  if (empty) {
    preview_shutdown_pipeline();
    log_info("web", "preview idle (media stopped)");
  }
}

static void preview_poll_client(PreviewClient *pc)
{
  char meta[64];
  if (!pc->used || !pc->c || !pc->c->is_websocket) {
    return;
  }
  if (!pc->bootstrapped) {
    if (g_prev.init_len == 0 || g_prev.codec[0] == '\0') {
      return;
    }
    snprintf(meta, sizeof(meta), "{\"codec\":\"%s\"}", g_prev.codec);
    mg_ws_send(pc->c, meta, strlen(meta), WEBSOCKET_OP_TEXT);
    mg_ws_send(pc->c, g_prev.init, g_prev.init_len, WEBSOCKET_OP_BINARY);
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
  pthread_mutex_lock(&g_prev.lock);
  for (i = 0; i < PREVIEW_MAX_CLIENTS; i++) {
    if (g_prev.clients[i].used) {
      preview_poll_client(&g_prev.clients[i]);
    }
  }
  pthread_mutex_unlock(&g_prev.lock);
}

static void handle_preview_ws(struct mg_connection *c, struct mg_http_message *hm)
{
  if (!auth_ok(hm)) {
    mg_http_reply(c, 401, "Content-Type: application/json\r\n",
                  "{\"code\":1001,\"msg\":\"unauthorized\",\"data\":{}}");
    return;
  }
  mg_ws_upgrade(c, hm, NULL);
  if (preview_add_client(c) != 0) {
    mg_ws_send(c, "{\"err\":\"preview busy\"}", 22, WEBSOCKET_OP_TEXT);
    c->is_closing = 1;
  }
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
  if (mg_match(hm->uri, mg_str("/api/v1/system/info"), NULL) &&
      mg_strcmp(hm->method, mg_str("GET")) == 0) {
    handle_system_info(c, hm);
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
  if (g_prev.mux_ready) {
    fmp4_mux_deinit(&g_prev.mux);
    g_prev.mux_ready = 0;
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
