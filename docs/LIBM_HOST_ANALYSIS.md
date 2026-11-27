# Análise: o que roda como código convidado no Android e o que dá para trazer para o host

Escopo: só código (`cpu/`, `src/runtime*.c`, `gpu/shim`, `scripts/link_libc.py`, docs de desempenho).
Nenhum binário foi analisado e nenhuma medição foi feita. As conclusões abaixo são de código e de
perfis já registrados nos docs do repositório.

## O que já roda nativo (ARM64, fora do JIT)

- Renderização: o emulador de GPU (`gpu/shadps4`, `gpu/shim`) é C++ nativo. É ele que aparece nos perfis
  de `docs/parallel_gpu.md`: thread da GPU ocupada 91% do tempo, `Draw` 74% em CPU, `BindResources` 32%.
- Funções de memória da libc: `memcpy`, `memmove`, `memset`, `memcmp` e `strlen` já são funções do host
  (`guest_memcpy` etc. em `src/runtime.c`, ligadas em `runtime_resolve`).
- Kernel, threads, mutex, semáforo, áudio, pad, arquivos e tempo: `runtime_*_resolve` em `src/`.

## O que roda como código convidado (FEX JIT)

- O código do próprio `eboot.bin` (x86-64). Não há como trazê-lo para o host sem recompilar ou reescrever função
  por função.
- As funções da `libc` do jogo. `scripts/link_libc.py` liga `sce_module/libc.prx` como um módulo convidado. As
  importações que não estão em `runtime_resolve` caem nas exportações desse módulo, que rodam sob o JIT.
  Por isso, matemática como `pow`, `exp` ou `atan2` executa como x86 convidado mesmo sendo simples.

## Hipótese testada no código: funções matemáticas da libc

Motivo: são chamadas frequentes em física, animação e câmera, e cada uma roda como código convidado. O mesmo
mecanismo que já traz `memcpy` para o host pode trazer as funções da libm padrão C.

Mudança (`src/runtime.c`, branch `analysis/libm-host`):

- Wrappers nativos para `pow`, `exp`, `atan2`, `fmod`, `tan`, `atan`, `acos`, `asin` e as versões `f`.
- Ligação por NID exato em `libm_resolve`, chamada antes de `runtime_mutex_resolve`.
- Ativação por `GOW3_LIBM_HOST=1`. Padrão desligado, para permitir A/B.
- Ficam de fora `_Sin` e `_FSin`, que são helpers da Dinkumware com argumentos extras e semântica própria.

Por que é um knob e não o padrão: a diferença de resultado entre a libm do jogo e a do Android pode ser de último
bit. Isso não quebra nada por si só, mas pode alterar física ou cutscenes que dependem de determinismo.

## Não comprovado

- Que essas funções estejam entre as mais quentes em um frame do God of War 2018 no Poco F6. Não há contagem de
  chamadas de guest libc nos docs. As contagens existentes (`guest_cpu_stub_top`) só cobrem stubs do host.
- Que a chamada de host, com o custo de troca de ABI pelo FEX, seja mais barata que a função em JIT. Para funções
  pequenas isso pode não compensar.
- Que o `GOW3_LIBM_HOST=1` compile e rode no alvo Android. Só houve checagem de sintaxe no host x86-64 com
  `gcc -fsyntax-only`. Não houve build NDK nem teste no aparelho.

## Como validar (conforme CLAUDE.md)

1. Compilar pelo workflow `android.yml` ou `android-dev-apk.yml`.
2. No Poco F6, mesma cena, mesma temperatura e mesmo estado de salvamento. Comparar `GOW3_LIBM_HOST=0` e `=1`,
   mudando só essa variável. Medir FPS mediano e 1% lows, não o tempo da cena.
3. Conferir se o jogo não muda de comportamento visível em cutscene e na física dos objetos.
4. Só depois de comprovar ganho, considerar tornar o padrão `1`.

## Próximo passo que traria evidência de verdade

Medir quais funções convidadas são quentes no aparelho, antes de mexer em mais nenhuma. Por exemplo, um contador
de chamadas por endereço no caminho de retorno do JIT (`guest_cpu_return_address` já existe para diagnóstico),
ativado só por knob e desligado por padrão. Com esse ranking, dá para decidir quais funções do próprio jogo valem
a reescrita nativa, que é o trabalho grande e não cabe em um diff pequeno.
