// MobileGlues - gl/mg_vmdi_config.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#pragma once
#include <cstdint>

// Modos de Operacao do MultiDraw no MobileGlues
enum class MG_MultiDrawMode : int32_t {
    LEGACY_MOBILEGLUES = 0, // Utiliza o multidraw original (legacy)
    MG_VMDI_OPTIMIZED  = 1, // Virtual MultiDraw Indirect (VMDI)
    MG_IMDBI_OPTIMIZED = 2  // Infinity MultiDraw Bi-Indirect (IMDBI)
};

#ifdef __cplusplus
extern "C" {
#endif

// Funcoes para controle via menu/config do MobileGlues
void mg_vmdi_set_mode(MG_MultiDrawMode mode);
MG_MultiDrawMode mg_vmdi_get_mode();
void mg_vmdi_toggle_mode();

// Expor informacoes do backend ativo e profiler em tempo real para F3 / Debug
const char* mg_get_multidraw_engine_name();
const char* mg_get_multidraw_profiler_string();

#ifdef __cplusplus
}
#endif
