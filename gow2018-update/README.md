# gow2018-update – Tentativa de correção de tela preta

## Problema
Log mostrava milhares de erros:
```
BindBuffers: Clamped size from 4294967280 to ...
```
Isso indica tamanho de buffer inválido (0xFFFFFFF0) sendo passado para o Vulkan/Turnip → geometria não renderiza → tela preta.

## O que este update faz
Cria/atualiza o arquivo de configuração:

```
gow3port/gow3.ini
```

Com as seguintes mudanças principais:

| Flag / Setting              | Valor | Motivo                                      |
|----------------------------|-------|---------------------------------------------|
| deferred_readback          | 0     | Desliga path de readback que pode corromper tamanho de buffer |
| readback_linear            | 0     | Simplifica path de textura                  |
| shadows                    | 0     | Remove path pesado e problemático           |
| object_motion              | 0     | Já estava off, reforça                      |
| draw_workers               | 0     | Mais estável em Adreno                      |
| dmem_mb                    | 4096  | Limite mais conservador                     |
| frame_limit                | 30    | Reduz pressão                               |
| safe_bind_buffers          | 1     | (se o build implementar) trata tamanho inválido |
| clamp_invalid_buffer_size  | 1     | (se o build implementar)                    |

Também inclui `gow3.env` com variáveis de ambiente equivalentes.

## Como a automação deve instalar
O zip deve ser extraído de forma que o conteúdo de `gow3port/` vá para:

```
/data/user/0/com.gow3.android.bionic/files/gow3port/
```

Ou seja, o arquivo final deve ficar em:

```
.../files/gow3port/gow3.ini
```

## Depois de instalar
1. Force close do app
2. Limpe cache se possível
3. Abra o jogo novamente
4. Colete novo log se ainda ficar preto

## Observação
Estas flags são uma tentativa empírica. O erro de BindBuffers com 4294967280 provavelmente precisa de correção no código do rasterizer (vk_rasterizer.cpp). Estas configs apenas tentam desviar dos paths mais quebrados.
