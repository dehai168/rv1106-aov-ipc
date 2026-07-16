#include "system/config_service.h"

#include "common/log.h"
#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CFG_DEFAULT_PATH "/userdata/default_config.json"
#define CFG_USER_PATH "/userdata/ipc_config.json"
#define CFG_PATH_MAX 256

static cJSON *g_root;
static char g_default_path[CFG_PATH_MAX];
static char g_user_path[CFG_PATH_MAX];

static char *read_file(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    return NULL;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  long sz = ftell(fp);
  if (sz < 0) {
    fclose(fp);
    return NULL;
  }
  rewind(fp);

  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  buf[n] = '\0';
  return buf;
}

/* Merge patch into target (objects deep-merge; other types replace). */
static void json_merge(cJSON *target, const cJSON *patch) {
  if (!cJSON_IsObject(target) || !cJSON_IsObject(patch)) {
    return;
  }

  const cJSON *item = NULL;
  cJSON_ArrayForEach(item, patch) {
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(target, item->string);
    if (cJSON_IsObject(existing) && cJSON_IsObject(item)) {
      json_merge(existing, item);
    } else {
      cJSON *dup = cJSON_Duplicate(item, 1);
      if (!dup) {
        continue;
      }
      if (existing) {
        cJSON_ReplaceItemInObjectCaseSensitive(target, item->string, dup);
      } else {
        cJSON_AddItemToObject(target, item->string, dup);
      }
    }
  }
}

static cJSON *load_json_file(const char *path) {
  char *text = read_file(path);
  if (!text) {
    return NULL;
  }
  cJSON *json = cJSON_Parse(text);
  free(text);
  return json;
}

static cJSON *navigate_parent(cJSON *root, const char *path, char *leaf, int leaf_len,
                              int create) {
  if (!root || !path || !leaf || leaf_len <= 0) {
    return NULL;
  }

  char tmp[CFG_PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);

  cJSON *cur = root;
  char *save = NULL;
  char *tok = strtok_r(tmp, ".", &save);
  char *next = NULL;

  while (tok) {
    next = strtok_r(NULL, ".", &save);
    if (!next) {
      snprintf(leaf, (size_t)leaf_len, "%s", tok);
      return cur;
    }

    cJSON *child = cJSON_GetObjectItemCaseSensitive(cur, tok);
    if (!child) {
      if (!create) {
        return NULL;
      }
      child = cJSON_CreateObject();
      if (!child) {
        return NULL;
      }
      cJSON_AddItemToObject(cur, tok, child);
    } else if (!cJSON_IsObject(child)) {
      if (!create) {
        return NULL;
      }
      child = cJSON_CreateObject();
      if (!child) {
        return NULL;
      }
      cJSON_ReplaceItemInObjectCaseSensitive(cur, tok, child);
    }
    cur = child;
    tok = next;
  }
  return NULL;
}

int config_init(const char *default_path, const char *user_path) {
  config_deinit();

  snprintf(g_default_path, sizeof(g_default_path), "%s",
           default_path && default_path[0] ? default_path : CFG_DEFAULT_PATH);
  snprintf(g_user_path, sizeof(g_user_path), "%s",
           user_path && user_path[0] ? user_path : CFG_USER_PATH);

  g_root = load_json_file(g_default_path);
  if (!g_root) {
    log_warn("config", "default missing/invalid (%s), use empty object", g_default_path);
    g_root = cJSON_CreateObject();
    if (!g_root) {
      return -1;
    }
  }

  cJSON *user = load_json_file(g_user_path);
  if (user) {
    if (cJSON_IsObject(user)) {
      json_merge(g_root, user);
      log_info("config", "merged user config %s", g_user_path);
    } else {
      log_warn("config", "user config not object, ignore");
    }
    cJSON_Delete(user);
  } else {
    log_info("config", "no user config yet (%s)", g_user_path);
  }
  return 0;
}

void config_deinit(void) {
  if (g_root) {
    cJSON_Delete(g_root);
    g_root = NULL;
  }
}

int config_get_string(const char *path, char *out, int out_len, const char *def_val) {
  if (!out || out_len <= 0) {
    return -1;
  }
  char leaf[128];
  cJSON *parent = navigate_parent(g_root, path, leaf, (int)sizeof(leaf), 0);
  cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive(parent, leaf) : NULL;
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(out, (size_t)out_len, "%s", item->valuestring);
    return 0;
  }
  snprintf(out, (size_t)out_len, "%s", def_val ? def_val : "");
  return 1;
}

int config_get_int(const char *path, int *out, int def_val) {
  if (!out) {
    return -1;
  }
  char leaf[128];
  cJSON *parent = navigate_parent(g_root, path, leaf, (int)sizeof(leaf), 0);
  cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive(parent, leaf) : NULL;
  if (cJSON_IsNumber(item)) {
    *out = item->valueint;
    return 0;
  }
  *out = def_val;
  return 1;
}

int config_get_bool(const char *path, int *out, int def_val) {
  if (!out) {
    return -1;
  }
  char leaf[128];
  cJSON *parent = navigate_parent(g_root, path, leaf, (int)sizeof(leaf), 0);
  cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive(parent, leaf) : NULL;
  if (cJSON_IsBool(item)) {
    *out = cJSON_IsTrue(item) ? 1 : 0;
    return 0;
  }
  *out = def_val;
  return 1;
}

int config_set_string(const char *path, const char *value) {
  if (!g_root || !value) {
    return -1;
  }
  char leaf[128];
  cJSON *parent = navigate_parent(g_root, path, leaf, (int)sizeof(leaf), 1);
  if (!parent) {
    return -2;
  }
  cJSON *item = cJSON_CreateString(value);
  if (!item) {
    return -3;
  }
  if (cJSON_GetObjectItemCaseSensitive(parent, leaf)) {
    cJSON_ReplaceItemInObjectCaseSensitive(parent, leaf, item);
  } else {
    cJSON_AddItemToObject(parent, leaf, item);
  }
  return 0;
}

int config_set_int(const char *path, int value) {
  if (!g_root) {
    return -1;
  }
  char leaf[128];
  cJSON *parent = navigate_parent(g_root, path, leaf, (int)sizeof(leaf), 1);
  if (!parent) {
    return -2;
  }
  cJSON *item = cJSON_CreateNumber((double)value);
  if (!item) {
    return -3;
  }
  if (cJSON_GetObjectItemCaseSensitive(parent, leaf)) {
    cJSON_ReplaceItemInObjectCaseSensitive(parent, leaf, item);
  } else {
    cJSON_AddItemToObject(parent, leaf, item);
  }
  return 0;
}

int config_set_bool(const char *path, int value) {
  if (!g_root) {
    return -1;
  }
  char leaf[128];
  cJSON *parent = navigate_parent(g_root, path, leaf, (int)sizeof(leaf), 1);
  if (!parent) {
    return -2;
  }
  cJSON *item = cJSON_CreateBool(value ? 1 : 0);
  if (!item) {
    return -3;
  }
  if (cJSON_GetObjectItemCaseSensitive(parent, leaf)) {
    cJSON_ReplaceItemInObjectCaseSensitive(parent, leaf, item);
  } else {
    cJSON_AddItemToObject(parent, leaf, item);
  }
  return 0;
}

int config_save(void) {
  if (!g_root) {
    return -1;
  }

  char *text = cJSON_Print(g_root);
  if (!text) {
    return -2;
  }

  char tmp_path[CFG_PATH_MAX + 8];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_user_path);

  FILE *fp = fopen(tmp_path, "wb");
  if (!fp) {
    log_error("config", "open tmp failed: %s", strerror(errno));
    cJSON_free(text);
    return -3;
  }
  size_t len = strlen(text);
  size_t n = fwrite(text, 1, len, fp);
  fputc('\n', fp);
  fflush(fp);
  fsync(fileno(fp));
  fclose(fp);
  cJSON_free(text);

  if (n != len) {
    unlink(tmp_path);
    return -4;
  }
  if (rename(tmp_path, g_user_path) != 0) {
    log_error("config", "rename failed: %s", strerror(errno));
    unlink(tmp_path);
    return -5;
  }
  log_info("config", "saved %s", g_user_path);
  return 0;
}

int config_reload(void) {
  return config_init(g_default_path, g_user_path);
}

const char *config_user_path(void) {
  return g_user_path;
}
