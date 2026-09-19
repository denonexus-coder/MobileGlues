// MobileGlues - config/config.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
#ifndef _MOBILEGLUES_CONFIG_H_
#define _MOBILEGLUES_CONFIG_H_

#ifdef __cplusplus
extern "C"
{
#endif

    extern char* mg_directory_path;
    extern char* config_file_path;
    extern char* log_file_path;
    extern char* glsl_cache_file_path;

    extern int initialized;

    char* concatenate(char* str1, char* str2);

    int check_path();

    int config_refresh();
    int config_get_int(char* name);
    char* config_get_string(char* name);
    void config_cleanup();

    // Path-navigating variants (Fase 2): split `path` on '.' and walk the
    // cJSON tree — e.g. "diag.overlay.frameProfiler" resolves to
    // config_json["diag"]["overlay"]["frameProfiler"].
    // Fall back to the flat originals when there is no '.' in the path.
    int   config_get_int_path(const char* path);
    char* config_get_string_path(const char* path);
    // config_get_bool_path: returns default_val when the path is absent or
    // config_get_int_path returns < 0; otherwise 1 if value > 0, else 0.
    int   config_get_bool_path(const char* path, int default_val);

#ifdef __cplusplus
}
#endif

extern bool is_custom_mg_dir;

#endif // _MOBILEGLUES_CONFIG_H_
