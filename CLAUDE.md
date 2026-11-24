# CLAUDE.md — fork gow2018 (God of War 2018 / CUSA07408 no Android)

Contexto do projeto para o Claude Code. Leia antes de qualquer alteração.

## Objetivo

Fazer o God of War 2018 (CUSA07408) rodar no Poco F6 (Snapdragon 8s Gen 3, Adreno 735, driver Turnip) com FPS jogável e sem bugs visuais:
- FPS atual: 6–9 com picos de frame de 130 a 1500 ms.
- Sintomas visuais: texturas trocadas/invertidas, chão sumindo, partes do personagem pretas, faixas e blocos corrompidos.

## Base de trabalho

- Branch base: `android`. Referência atual: `c1add599` ("Apply Android diagnostics update"), que já inclui o ajuste de `Phi` em `flatten_extended_userdata_pass.cpp`.
- Não trabalhe direto em `android`. Crie um branch por hipótese, por exemplo `fix/srt-<assunto>` ou `diag/<assunto>`.
- Antes de cada trabalho, sincronize com `origin/android`.
- Há muitos branches `audit/`, `diag/`, `analysis/`, `feat/` e `experimental-*`. Eles são experimentos antigos. Não os use como base nem os misture sem pedido explícito.
- Não faça push, não reescreva histórico e não apague branches sem pedir.

## Regras de alteração

1. Uma hipótese por vez. Não misture correções de SRT, memória, shader e texture cache no mesmo diff.
2. Mudança mínima. Não refatore, não renomeie e não reformate código que não faz parte do problema.
3. Toda mudança deve citar a evidência: linha de log, contador, hash de shader ou knob testado.
4. Não altere `tests/`, `scripts/` ou workflows sem pedido explícito.
5. Não crie novas variáveis de ambiente `GOW3_*` sem necessidade clara. Antes de criar, procure as existentes (há cerca de 260).
6. Não reativar comportamentos que estão desligados por padrão (por exemplo `GOW3_UFFD`, `GOW3_TEXTURE_HELPER`).
7. Se algo já existe no código (knob, exclusão de shader, throttle), explique o que ele faz antes de propor outro.
8. Entregue o resultado como diff, com a lista de arquivos alterados e uma explicação curta de cada um.

## Validação obrigatória antes de propor algo como correto

- Compilar o alvo afetado. Se não houver NDK/clang local, diga explicitamente que a compilação não foi feita e use o workflow `android.yml` ou `android-dev-apk.yml` para validar.
- Rodar os testes relacionados em `tests/` (ex.: `test_android_fsr_integration.py`, `test_android_patch_validation3.py`, `test_android_readback_affinity.py`, `test_graphics_options.py`) e reportar o resultado real, sem arredondar.
- Não declare que um bug foi corrigido com base só em teste unitário Python. Só o teste no aparelho confirma.

## Áreas do código relevantes

- SRT / flatten de descritores: `gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp` (`ComputeOffset`, `VisitPointer`, `GenerateSrtProgram`, `MaxFlattenPasses`). Caminho ARM64 é a variante `#else` do arquivo. O código ARM64 nativo está em `srt_arm64.h`.
- Erros de SRT: `LogSrtError` imprime só nas 4 primeiras ocorrências de cada mensagem e depois a cada 1024. Os números no log são amostrados, não totais.
- Recurso de shader: `resource_patching_pass.cpp` (ex.: `Sharp source was not flatenned`, `CalculateBufferAddress`).
- Metadados de imagem: `buffer_cache/buffer_cache.cpp` (`Unhandled metadata type CMask`, modo de write-fault sync).
- Emissão de imagem: `backend/spirv/emit_spirv_image.cpp` (`Fallback for ImageWrite with LOD`).
- Texture cache: `video_core/texture_cache/texture_cache.cpp` (`SyncPairedSibling`, `FindImage`).
- Rasterizer e throttles: `video_core/renderer_vulkan/vk_rasterizer.cpp` (`ShouldSkipShadowAtlas`, VolFog, `BindHelperWanted`).
- Memória e rastreamento de páginas: `video_core/page_manager.cpp` (por padrão usa sinais; `userfaultfd` só com `GOW3_UFFD=1`, e falha neste kernel).
- Configuração: `gpu/shim/gow3_settings.cpp` (mapeia env → `gow3.ini`) e `scripts/launch_env.py` (ex.: `GOW3_SHADOWS` é tratado só aqui, não em C++).

## Knobs de runtime para isolar hipóteses (sem recompilar)

- `GOW3_SHADOW_RATE` (1 = sem throttle; padrão 4 com `GOW3_ANDROID_PORT`)
- `GOW3_VOLFOG_RATE` (padrão 1 = sem throttle)
- `GOW3_WRITE_FAULT_SYNC` (0 ou 2; padrão 0 com `GOW3_ANDROID_PORT`, senão 2)
- `GOW3_WRITE_FAULT_SYNC_PROTECT_SRT` (0 desliga a proteção da arena do SRT)
- `GOW3_SRT_NATIVE` (0 desliga o SRT nativo ARM64)
- `GOW3_DEFERRED_READBACK`, `GOW3_STALE_READBACK` (podem gerar artefatos visuais; testar por último)

## Formato de resposta esperado

- Hipótese, com a evidência usada.
- Onde no código e por quê.
- Diff mínimo.
- O que foi validado e o que não foi.
- Como testar no aparelho (qual knob ou qual cena comparar).
