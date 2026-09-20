# Auditoria de Config — MobileGlues

Data: 2026-09-20  
Commits:  
  Core:   `31c24c0`  
  Plugin: `1183561`

---

## 1. Tabela completa

| Chave JSON | Tipo | Plugin escreve | Lib lê | Consumer (arquivo:linha) | Status |
|---|---|---|---|---|---|
| `enableANGLE` | int (enum) | ✅ | ✅ `settings.cpp:54` | `gles/loader.cpp:133` (`global_settings.angle`) | **FUNCIONAL** |
| `enableNoError` | int (enum) | ✅ | ✅ `settings.cpp:56` | `gl/framebuffer.cpp:557`, `gl/program.cpp:209`, `gl/shader.cpp:114` (`global_settings.ignore_error`) | **FUNCIONAL** |
| `enableExtTimerQuery` | int→bool | ✅ | ✅ `settings.cpp:58` | `gles/loader.cpp:279` (`global_settings.ext_timer_query`) | **FUNCIONAL** |
| `enableExtComputeShader` | int→bool | ✅ | ✅ `settings.cpp:57` | `gles/loader.cpp:284` (`global_settings.ext_compute_shader`) | **FUNCIONAL** |
| `enableExtDirectStateAccess` | int→bool | ✅ | ✅ `settings.cpp:59` | `gles/loader.cpp:288` (`global_settings.ext_direct_state_access`) | **FUNCIONAL** |
| `enableExtGL43` | int→bool | ✅ | ✅ `settings.cpp:60`¹ | `gl/getter.cpp:82,99` (`global_settings.enable_ext_gl43`) | **FUNCIONAL** |
| `maxGlslCacheSize` | int (MB) | ✅ | ✅ `settings.cpp:132-133` | `gl/glsl/cache.cpp:226,247,277,278` (`global_settings.max_glsl_cache_size`) | **FUNCIONAL** |
| `multidrawEngine` | string (enum) | ✅ | ✅ `settings.cpp:71` | `gl/mg_vmdi.cpp:47,100` (via `multidraw_engine`) | **FUNCIONAL** |
| `multidrawMode` | string (legado) | ✅ remove | ✅ `settings.cpp:72` (fallback) | `settings.cpp:643-645` (aviso de deprecação + fallback) | **LEGADO — plugin apaga, lib aceita como fallback** |
| `multidrawDisableBackends` | string (legado) | ✅ remove | ✅ `settings.cpp:644` (aviso apenas) | `settings.cpp:645` (LOG_W, sem efeito real) | **LEGADO — sem consumer real, lib só imprime aviso** |
| `multidrawOrder` | string (csv) | ✅ | ✅ `settings.cpp:650` | `gl/multidraw.cpp:464` comentário; `bench/multidraw_bench.cpp:1170,1172` (`global_settings.multidraw_order`) | **FUNCIONAL** |
| `enableVMDI` | int→bool | ✅ | ✅ `settings.cpp:90-92` | `gl/mg_vmdi.cpp` (via `multidraw_mode`) | **FUNCIONAL** |
| `enableIMDBI` | int→bool | ✅ | ✅ `settings.cpp:83-85` | `gl/mg_vmdi.cpp` (via `multidraw_mode`) | **FUNCIONAL** |
| `angleDepthClearFixMode` | int (enum) | ✅ | ✅ `settings.cpp:61-62` | `gl/gl.cpp:166,169,173` (`global_settings.angle_depth_clear_fix_mode`) | **FUNCIONAL** |
| `customGLVersion` | int | ✅ | ✅ `settings.cpp:63` | `egl/egl.cpp:270-271,275-276` (`global_settings.custom_gl_version`) | **FUNCIONAL** |
| `fsr1Setting` | int (enum) | ✅ | ✅ `settings.cpp:65` | `egl/egl.cpp:415,433`, `gl/shader.cpp:134`, `gl/texture.cpp:420`, `gl/FSR1/FSR1.cpp:593` (`global_settings.fsr1_setting`) | **FUNCIONAL** |
| `hideMGEnvLevel` | int (enum) | ✅ | ✅ `settings.cpp:67` | `gl/getter.cpp:244,274,376,398,447,493,526,538,549` (`global_settings.hide_mg_env_level`) | **FUNCIONAL** |
| `forceGlGetErrorSkip` | bool | ✅ | ✅ `settings.cpp`² | `gl/getter.cpp:204` (`global_settings.force_gl_get_error_skip`) | **FUNCIONAL** |
| `bufferUploadMode` | int | ✅ | ✅ `settings.cpp`² | `gl/buffer.cpp:1025` (`global_settings.buffer_upload_mode`) | **FUNCIONAL** |
| `textureSwizzleMode` | int | ✅ | ✅ `settings.cpp`² | `gl/texture.cpp:1467` (`global_settings.texture_swizzle_mode`) | **FUNCIONAL** |
| `maxAnisotropyOverride` | int | ✅ | ✅ `settings.cpp`² | `gl/texture.cpp:830-834,1814-1818` (`global_settings.max_anisotropy_override`) | **FUNCIONAL** |
| `forceDepthPrecisionFix` | bool | ✅ | ✅ `settings.cpp`² | `gl/texture.cpp:1369,1392` (`global_settings.force_depth_precision_fix`) | **FUNCIONAL** |
| `imdbiBackend` | string (enum) | ✅ | ✅ `settings.cpp:895` | `settings.cpp:903,915` → passa para `IMDBIDispatcher::initialize()` | **FUNCIONAL** (consumer em settings, via config de IMDBI) |
| `imdbiUnrollFactor` | int | ✅ | ✅ `settings.cpp:904-905` | `settings.cpp:915` → `cfg.unroll_factor` passado ao engine | **FUNCIONAL** |
| `imdbiPersistentMapping` | bool | ✅ | ✅ `settings.cpp:906` | `settings.cpp:916` → `cfg.use_persistent_mapping` | **FUNCIONAL** |
| `imdbiRegisterPinning` | bool | ✅ | ✅ `settings.cpp:907` | `settings.cpp:917` → `cfg.enable_register_pinning` | **FUNCIONAL** |
| `imdbiPrimitiveRestart` | bool | ✅ | ✅ `settings.cpp:908` | `settings.cpp:918` → `cfg.enable_primitive_restart` | **FUNCIONAL** |
| `imdbiRingSize` | int | ✅ | ✅ `settings.cpp:909-910` | `settings.cpp:919` → `cfg.ring_buffer_size` | **FUNCIONAL** |
| `vmdiBackendTier` | string (enum) | ✅ | ✅ `settings.cpp:929` | `settings.cpp:938,941` → `mg_vmdi_set_tier()` | **FUNCIONAL** |
| `vmdiEnableAutotune` | bool | ✅ | ✅ `settings.cpp:939` | `settings.cpp:941` → `mg_vmdi_set_tier(-1 if autotune)` | **FUNCIONAL** |
| `useProgramBinaryCache` | bool | ✅ | ✅ `settings.cpp:863-867` | `settings.cpp:873` → `ProgramBinaryCache::set_enabled()`; `gl/program.cpp:180` (`bin_cache.is_enabled()`) | **FUNCIONAL** |
| `diag` | object | ✅ | ✅ (aninhado, ver abaixo) | — (container JSON) | **FUNCIONAL** (container) |
| `diag.enabled` | bool | ✅ | ✅ `settings.cpp:876` | `global_settings.diag_enabled` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.overlay.frameProfiler` | bool | ✅ | ✅ `settings.cpp:877` | `global_settings.diag_frame_profiler` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.overlay.drawCallCount` | bool | ✅ | ✅ `settings.cpp:878` | `global_settings.diag_draw_call_count` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.overlay.shaderRecompiles` | bool | ✅ | ✅ `settings.cpp:879` | `global_settings.diag_shader_recompiles` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.overlay.backendTier` | bool | ✅ | ✅ `settings.cpp:880` | `global_settings.diag_backend_tier` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.overlay.cpuGpuLoad` | bool | ✅ | ✅ `settings.cpp:881` | `global_settings.diag_cpu_gpu_load` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.logging.backendSelection` | bool | ✅ | ✅ `settings.cpp:882` | `global_settings.diag_log_backend_selection` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.logging.shaderRecompiles` | bool | ✅ | ✅ `settings.cpp:883` | `global_settings.diag_log_shader_recompiles` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.logging.drawCallCount` | bool | ✅ | ✅ `settings.cpp:884` | `global_settings.diag_log_draw_call_count` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.logging.glTrace` | bool | ✅ | ✅ `settings.cpp:885` | `global_settings.diag_log_gl_trace` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.logging.level` | string | ✅ | ✅ `settings.cpp:886-887` | `global_settings.diag_log_level` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.capabilityReport` | bool | ✅ | ✅ `settings.cpp:888` | `global_settings.diag_capability_report` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.perfetto.enabled` | bool | ✅ | ✅ `settings.cpp:889` | `global_settings.diag_perfetto_enabled` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| `diag.perfetto.maxDurationSec` | int | ✅ | ✅ `settings.cpp:890` | `global_settings.diag_perfetto_max_duration` — **sem consumer fora de config/** | **LE-SEM-CONSUMER** |
| *(sem chave JSON)* | float | ❌ | ❌ (não lido do JSON) | `gl/FSR1/FSR1.cpp:521` (`global_settings.fsr1_sharpness`) — só default hardcoded `0.75f` | **SO-LIB** |
| *(sem chave JSON)* | bool | ❌ | ❌ (derivado de `enableANGLE`) | `gl/buffer.cpp:1143,1163,1176` (`global_settings.buffer_coherent_as_flush`) | **SO-LIB** (derivado interno) |

> ¹ `enableExtGL43` é lido como `config_get_int` em settings.cpp (linha ~60) convertido em `global_settings.enable_ext_gl43`.  
> ² Campos da Fase 1 lidos via `config_get_int` / `config_get_bool` com as chaves do plugin — linhas exatas verificadas em CHECK 2.

---

## 2. Chaves no plugin SEM consumidor real

Todas as chaves a seguir são **escritas pelo plugin e lidas pela lib**, mas os campos `global_settings.diag_*` populados **não têm nenhum uso fora de `config/settings.cpp`** — ou seja, o overlay de diagnóstico ainda não está implementado no lado C++.

| Chave JSON | Campo em `global_settings_t` | Sugestão |
|---|---|---|
| `diag.enabled` | `diag_enabled` | Implementar guard central de diag (ex.: `if (!global_settings.diag_enabled) return`) |
| `diag.overlay.frameProfiler` | `diag_frame_profiler` | Implementar overlay HUD ou remover da UI até estar pronto |
| `diag.overlay.drawCallCount` | `diag_draw_call_count` | Implementar contador no hot path ou remover da UI |
| `diag.overlay.shaderRecompiles` | `diag_shader_recompiles` | Hook em `glLinkProgram` / recompile path |
| `diag.overlay.backendTier` | `diag_backend_tier` | Expor via `mg_vmdi_get_tier()` no overlay |
| `diag.overlay.cpuGpuLoad` | `diag_cpu_gpu_load` | Requer polling via `/proc` ou GPU counters |
| `diag.logging.backendSelection` | `diag_log_backend_selection` | Adicionar `LOG_I` em `mg_init_multidraw_subsystem()` condicionado |
| `diag.logging.shaderRecompiles` | `diag_log_shader_recompiles` | Adicionar `LOG_I` em `glLinkProgram` condicionado |
| `diag.logging.drawCallCount` | `diag_log_draw_call_count` | Adicionar contador em `glMultiDraw*` condicionado |
| `diag.logging.glTrace` | `diag_log_gl_trace` | Interceptar chamadas GL (pesado — cuidado com overhead) |
| `diag.logging.level` | `diag_log_level` | Aplicar ao logger global (ex.: `LOG_set_level(diag_log_level)`) |
| `diag.capabilityReport` | `diag_capability_report` | Já há report em `mg_init_multidraw_subsystem()` — condicionar a este flag |
| `diag.perfetto.enabled` | `diag_perfetto_enabled` | Integrar com Android Tracing API (requer JNI bridge) |
| `diag.perfetto.maxDurationSec` | `diag_perfetto_max_duration` | Usado junto com `diag_perfetto_enabled` |
| `multidrawDisableBackends` | *(sem campo)* | Remover da UI/codec — lib só imprime aviso, ignora valor |

---

## 3. Chaves no Core SEM UI (SO-LIB)

| Campo em `global_settings_t` | Chave JSON equivalente | Nota | Sugestão |
|---|---|---|---|
| `fsr1_sharpness` | *(nenhuma)* | Hardcoded `0.75f`; consumer existe em `FSR1.cpp:521` | Expor como `"fsr1Sharpness"` no plugin (float 0.0–1.0) |
| `buffer_coherent_as_flush` | *(nenhuma)* | Derivado interno: `true` quando ANGLE desativado | Documentar como "só editável internamente" — não expor |

---

## 4. Resumo executivo

| Métrica | Valor |
|---|---|
| **Total de chaves analisadas** | 49 |
| **FUNCIONAL** (escreve + lê + consumer real) | 30 |
| **LEGADO** (plugin apaga, lib aceita como fallback/aviso) | 2 (`multidrawMode`, `multidrawDisableBackends`) |
| **LE-SEM-CONSUMER** (escreve + lê, sem consumer fora de config/) | 15 (todos `diag.*`) |
| **SO-LIB** (lib usa, plugin não escreve) | 2 (`fsr1_sharpness`, `buffer_coherent_as_flush`) |
| **FANTASMA** (escreve, lib não lê) | 0 |

> **Conclusão:** O core de configuração está sólido — 30 chaves funcionais e zero fantasmas. O principal gap é o **subsistema de diagnóstico** (`diag.*`): 15 campos são lidos e armazenados na struct mas nenhum é consumido fora de `config/`. A UI do plugin já expõe todas as opções; a implementação C++ do overlay/logging está pendente (Fase 6 natural).

---

## 5. Chaves com ponto suspeito (nível-1 com semântica de aninhamento)

As chaves a seguir são escritas no **nível raiz** do JSON mas têm nomes que sugerem aninhamento — ou vice-versa, há chaves aninhadas que poderiam causar confusão com helpers `config_get_*_path`:

| Chave | Observação |
|---|---|
| `imdbiBackend`, `imdbiUnrollFactor`, … | Estão no nível raiz do JSON, mas `config_get_int_path("imdbiBackend")` os lê sem prefixo — correto, mas contra-intuitivo (o `_path` faz sentido apenas para `diag.*`) |
| `vmdiBackendTier`, `vmdiEnableAutotune` | Idem — nível raiz lido via `_path` helper |
| `useProgramBinaryCache` | Lido via `config_get_bool_path("useProgramBinaryCache", -1)` sem objeto pai — funciona, mas semântica de `_path` é enganosa (sem ponto no nome) |
| `diag` | Único objeto JSON real no nível raiz com filhos aninhados (`diag.overlay.*`, `diag.logging.*`, `diag.perfetto.*`) — uso de `_path` é correto aqui |

**Recomendação:** Em versão futura do schema, considerar mover `imdbi*` e `vmdi*` para subobjetos `"imdbi": { … }` e `"vmdi": { … }`, alinhando o nome da função (`_path`) com a estrutura real do JSON.

---

*Gerado automaticamente — não alterar manualmente. Regenerar com o prompt "Fase 5 catálogo de config" quando houver mudanças nos repos.*
