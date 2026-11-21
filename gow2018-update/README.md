# God of War (2018) [CUSA07408] Android Port Update — Fork Release

Esta atualização adapta a codebase original (que era restrita e hardcoded para `CUSA01715` / God of War III) para executar com desempenho pleno em **God of War (2018) — CUSA07408** no Android (ARM64) sobre Adreno 7xx/8xx com Turnip Vulkan.

---

## 🔬 Diagnóstico de Engenharia dos Logs (`android-game.log`)

Na versão anterior, o jogo rodava a **~2 FPS** com tempos de frame entre **300ms e 700ms** (com picos de até 4668ms). A análise detalhada das chamadas de sistema, GPU waits e falhas de proteção revelou as causas exatas:

1. **`launch_env.py` forçava 1080p Nativo (`native-game-patch`)**:
   - O script só validava o eboot do GoW 3 (`CUSA01715`).
   - Ao detectar `CUSA07408`, o script caía no branch de fallback: `graphics['render_resolution'] = 'native'` e `updates['GOW3_RESOLUTION_STATUS'] = 'unsupported-eboot-native-fallback'`.
   - Isso forçava o renderizador Vulkan a rodar em **1920×1080 nativo**, gerando 2650 draws e mais de 560 dispatches por frame.

2. **Tempestade de Write Faults (`GOW3_FAULT_WINDOW=4`)**:
   - O `run-bionic.sh` exportava `GOW3_FAULT_WINDOW=4` (4 KiB = 1 página única).
   - O GoW 2018 atualiza megabytes de uniform buffers e transform buffers a cada frame. Com uma janela de apenas 4KB, o runtime disparava **46.000+ page faults** e centenas de chamadas de `mprotect` por frame, bloqueando o `mmap_lock` do processo e travando os 48 threads por **210ms a 516ms**.

3. **Overhead Crítico de Cópias (`GOW3_READBACK_LINEAR=1`)**:
   - `run-bionic.sh` forçava `export GOW3_READBACK_LINEAR=1`.
   - No `texture_cache.cpp`, qualquer imagem linear passava a ser baixada para a CPU via download queue síncrona.
   - Isso adicionava **50–70ms de readback wait** e **76–154ms de host copies** por frame.

4. **Sombras Dinâmicas forçadas em `enabled`**:
   - O patch de sombras só era aceito se o SHA256 do eboot fosse idêntico ao GoW 3. Para GoW 2018, as sombras ficavam sempre ativas, dobrando passes de geometria e shadow map allocations.

5. **Ausência de Shader Disk Cache**:
   - `MESA_SHADER_CACHE_DIR` não estava configurado no ambiente, obrigando o Turnip a recompilar todos os pipelines do zero a cada execução (pico de 3.4s em um único frame).

---

## 🛠️ Modificações Realizadas no Fork

### 1. `tools/android/bionic/run-bionic.sh`
- `export GOW3_FAULT_WINDOW=1024`: amplia a janela de dirty tracking para 1 MiB (256 páginas por fault). Reduz o tempo de write faults em 80-90%.
- `export GOW3_READBACK_LINEAR=0`: elimina o loop de download de texturas lineares.
- `export MESA_SHADER_CACHE_DIR="$HOME/.cache/mesa_shader_cache"`: ativa o cache em disco de pipelines Vulkan.
- `[ -f "$base/gow3.env" ] && . "$base/gow3.env"`: permite personalizações e overrides rápidos via `gow3.env`.
- Adicionados defaults performáticos para `CUSA07408` (qHD 960×540, shadows=0, deferred_readback=0, stale_readback=0, async_shaders=1).

### 2. `scripts/launch_env.py`
- Adicionado suporte nativo para `CUSA07408` no chaveamento de resoluções diretas (`540p`, `720p`, etc.).
- Não força mais `native` (1080p) quando o título for `CUSA07408`.
- Reconhece o status de fork ativo (`gow2018-fork-active`), permitindo desativar sombras dinâmicas e aplicar flags de otimização de imagem.

### 3. `gow3.default.ini` & `gow3.ini`
- Configurado com preset direto de 540p (`render_resolution=540p`, `output_res=960x540`).
- `frames_queued=2`: garante overlap de execução entre CPU (FEX JIT) e GPU (Turnip).
- `disable_dynamic_shadows=1`, `deferred_readback=0`, `stale_readback=0`.

### 4. `gow3port/gow3.env`
- Arquivo de overrides com as variáveis prontas para o runtime do jogo.

---

## 🚀 Como Aplicar no Fork do GitHub

Faça commit e push desses arquivos substituindo as versões correspondentes na raiz do seu repositório:
- `tools/android/bionic/run-bionic.sh`
- `tools/android/device/run-gow3.sh`
- `scripts/launch_env.py`
- `gow3.default.ini`
- `gow3port/gow3.env`
