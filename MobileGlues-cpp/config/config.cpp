// MobileGlues - config/config.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
#include "config.h"

#include "../gl/log.h"
#include "../gl/mg.h"
#include "cJSON.h"
#include "stats.h"
#include <cmath>
#include <cerrno>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>

#define DEBUG 0

char* DEFAULT_MG_DIRECTORY_PATH = "/sdcard/MG";

bool is_custom_mg_dir = false;
char* mg_directory_path = nullptr;
char* config_file_path = nullptr;
char* log_file_path = nullptr;
char* glsl_cache_file_path = nullptr;

static cJSON* config_json = nullptr;

int initialized = 0;

char* concatenate(char* str1, char* str2) {
    std::string str = std::string(str1) + str2;
    char* result = new char[str.size() + 1];
    strcpy(result, str.c_str());
    return result;
}

int check_path() {
    if (!mg_directory_path) {
        char* var = getenv("MG_DIR_PATH");
        is_custom_mg_dir = var ? true : false;
        mg_directory_path = var ? strdup(var) : DEFAULT_MG_DIRECTORY_PATH;
    }
    config_file_path = concatenate(mg_directory_path, "/config.json");
    log_file_path = concatenate(mg_directory_path, "/latest.log");
    glsl_cache_file_path = concatenate(mg_directory_path, "/glsl_cache.tmp");
    stats_file_path = concatenate(mg_directory_path, "/stats.json");

    if (mkdir(mg_directory_path, 0755) != 0 && errno != EEXIST) {
        LOG_E("Error creating MG directory.\n")
        return 0;
    }
    return 1;
}

int config_refresh() {
    LOG_D("MG_DIRECTORY_PATH=%s", mg_directory_path)
    LOG_D("CONFIG_FILE_PATH=%s", config_file_path)
    LOG_D("LOG_FILE_PATH=%s", log_file_path)
    LOG_D("GLSL_CACHE_FILE_PATH=%s", glsl_cache_file_path)

    FILE* file = fopen(config_file_path, "r");
    if (file == NULL) {
        LOG_E("Unable to open config file %s", config_file_path);
        return 0;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* file_content = (char*)malloc(file_size + 1);
    if (file_content == NULL) {
        LOG_E("Unable to allocate memory for file content");
        fclose(file);
        return 0;
    }

    fread(file_content, 1, file_size, file);
    fclose(file);
    file_content[file_size] = '\0';

    config_json = cJSON_Parse(file_content);
    free(file_content);

    if (config_json == NULL) {
        LOG_E("Error parsing config JSON: %s\n", cJSON_GetErrorPtr());
        return 0;
    }

    initialized = 1;
    return 1;
}

int config_get_int(char* name) {
    if (config_json == NULL) {
        return -1;
    }

    cJSON* item = cJSON_GetObjectItem(config_json, name);
    if (item == NULL || !cJSON_IsNumber(item)) {
        LOG_D("Config item '%s' not found or not an integer.\n", name);
        return -1;
    }

    return item->valueint;
}

char* config_get_string(char* name) {
    if (config_json == NULL) {
        return NULL;
    }

    cJSON* item = cJSON_GetObjectItem(config_json, name);
    if (item == NULL || !cJSON_IsString(item)) {
        LOG_D("Config item '%s' not found or not a string.\n", name);
        return "";
    }

    return item->valuestring;
}

void config_cleanup() {
    if (config_json != NULL) {
        cJSON_Delete(config_json);
        config_json = NULL;
    }
}

// ---------------------------------------------------------------------------
// Path-navigating helpers (Fase 2)
//
// Split `path` on '.' and walk the cJSON tree one segment at a time.
// If any intermediate segment is missing or is not an object the function
// returns the appropriate "absent" sentinel (-1 / nullptr / default_val).
// When the path contains no '.', we delegate to the original flat functions
// so that callers do not have to distinguish between the two cases.
// ---------------------------------------------------------------------------

// Walk config_json following `path` (dot-separated segments).
// Returns the final cJSON node, or nullptr if any segment is missing.
static cJSON* config_navigate(const char* path) {
    if (config_json == NULL || path == NULL) return nullptr;

    // Fast path: no dot → root lookup, same as config_get_int/string.
    if (strchr(path, '.') == NULL) {
        return cJSON_GetObjectItem(config_json, path);
    }

    // Copy the path so we can tokenise in place.
    char buf[256];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    cJSON* node = config_json;
    char* token = strtok(buf, ".");
    while (token != NULL) {
        if (!cJSON_IsObject(node) && node != config_json) {
            // Intermediate segment is not an object; path does not resolve.
            return nullptr;
        }
        node = cJSON_GetObjectItem(node, token);
        if (node == NULL) return nullptr;
        token = strtok(NULL, ".");
    }
    return node;
}

int config_get_int_path(const char* path) {
    if (path == NULL) return -1;

    // No dot: delegate to the original function (keeps compat for callers that
    // do not know whether the key is nested or not).
    if (strchr(path, '.') == NULL) {
        return config_get_int(const_cast<char*>(path));
    }

    cJSON* item = config_navigate(path);
    if (item == NULL) {
        LOG_D("Config path '%s' not found.\n", path);
        return -1;
    }
    if (cJSON_IsNumber(item))  return item->valueint;
    if (cJSON_IsBool(item))    return cJSON_IsTrue(item) ? 1 : 0;
    if (cJSON_IsString(item))  return atoi(item->valuestring);
    return -1;
}

float config_get_float_path(const char* path) {
    if (path == NULL) return std::nanf("");

    cJSON* item = config_navigate(path);
    if (item == NULL) return std::nanf("");

    if (cJSON_IsNumber(item)) return static_cast<float>(item->valuedouble);
    if (cJSON_IsString(item) && item->valuestring) return static_cast<float>(std::atof(item->valuestring));
    return std::nanf("");
}

char* config_get_string_path(const char* path) {
    if (path == NULL) return nullptr;

    if (strchr(path, '.') == NULL) {
        return config_get_string(const_cast<char*>(path));
    }

    cJSON* item = config_navigate(path);
    if (item == NULL || !cJSON_IsString(item)) {
        LOG_D("Config path '%s' not found or not a string.\n", path);
        return nullptr;
    }
    return item->valuestring;
}

int config_get_bool_path(const char* path, int default_val) {
    int v = config_get_int_path(path);
    if (v < 0) return default_val;
    return v > 0 ? 1 : 0;
}
