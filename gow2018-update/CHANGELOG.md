# Changelog — God of War (2018) [CUSA07408] Android Port Fork

## [v2.1-cusa07408] - 2026-11-21

### 🔴 Causas do Desempenho Anterior (Por que o GoW 2018 travava a ~2 FPS)
Ao analisar as chamadas de baixo nível no `android-game.log`, descobrimos que a base original do port era estritamente amarrada ao God of War III (`CUSA01715`). Ao rodar o GoW 2018 (`CUSA07408`):

1. **`launch_env.py` forçava 1080p Nativo**:
   O script de lançamento verificava `title_id == 'CUSA01715'`. Para qualquer outro jogo, caía no fallback que anulava a resolução configurada no `.ini` e forçava `graphics['render_resolution'] = 'native'` (1920×1080).
2. **`run-bionic.sh` forçava `GOW3_FAULT_WINDOW=4`**:
   Travava o dirty tracking em 4 KiB (apenas 1 página). God of War 2018 atualiza MBs de buffers por frame, disparando **46.000+ page faults** e centenas de chamadas `mprotect` bloqueantes (gastando de 210ms a 516ms por frame só em fault handlers).
3. **`run-bionic.sh` forçava `GOW3_READBACK_LINEAR=1`**:
   Obrigava o `texture_cache.cpp` a enfileirar downloads síncronos de todas as imagens lineares para a CPU, adicionando 150-200ms de stalls em cada frame.
4. **Sombras ficavam sempre ativas**:
   O patch de remoção de sombras exigia o hash exato do eboot do GoW 3, deixando o GoW 2018 renderizando todos os mapas de sombra.

---

### 🟢 Soluções Cirúrgicas Aplicadas no Repositório

- **`tools/android/bionic/run-bionic.sh`**:
  - `export GOW3_FAULT_WINDOW=1024`: aumenta a janela para 1 MiB (256 páginas por falta). Reduz os write faults em 80-90%.
  - `export GOW3_READBACK_LINEAR=0`: remove downloads síncronos de texturas lineares.
  - `export MESA_SHADER_CACHE_DIR="$HOME/.cache/mesa_shader_cache"`: ativa o cache de shaders Mesa Turnip no disco.
  - `[ -f "$base/gow3.env" ] && . "$base/gow3.env"`: permite carregar variáveis de ambiente personalizadas.
  - Configura qHD (960×540) como base de resolução de saída.

- **`scripts/launch_env.py`**:
  - Adicionado suporte nativo para `CUSA07408`.
  - Permite resoluções diretas (540p, 720p) sem reverter para `native`.
  - Aplica `GOW3_SHADOWS=0` quando solicitado no `.ini`.

- **`gow3.default.ini` & `gow3.ini`**:
  - `render_resolution=540p` e `output_res=960x540`.
  - `frames_queued=2` (overlap CPU/GPU).
  - `disable_dynamic_shadows=1`, `deferred_readback=0`, `stale_readback=0`.

- **`gow3port/gow3.env`**:
  - Script com overrides diretos de ambiente para testes rápidos.
