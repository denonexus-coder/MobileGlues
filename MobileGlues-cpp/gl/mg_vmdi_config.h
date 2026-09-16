// MobileGlues - gl/mg_vmdi_config.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#pragma once
#include <cstdint>

// Modos de Operação do MultiDraw no MobileGlues
enum class MG_MultiDrawMode : int32_t {
    LEGACY_MOBILEGLUES = 0, // Utiliza o fallback padrão (multidraw.cpp legado)
    MG_VMDI_OPTIMIZED  = 1  // Utiliza nosso Virtual MDI com Ring-Buffer, Fusion e NEON
};

#ifdef __cplusplus
extern "C" {
#endif

// Funções para controle via menu/config do MobileGlues
void mg_vmdi_set_mode(MG_MultiDrawMode mode);
MG_MultiDrawMode mg_vmdi_get_mode();
void mg_vmdi_toggle_mode();

#ifdef __cplusplus
}
#endif
