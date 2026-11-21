# God of War (2018) — Android Port Performance Update

## 📦 Conteúdo do Update

```
gow2018-update/
├── gow3.ini                    # Config principal com tuning de performance
├── CHANGELOG.md                # Log detalhado das mudanças
├── README.md                   # Este arquivo
└── scripts/
    ├── setup-governor.sh       # Script para forçar CPU/GPU governor (opcional)
    └── revert-governor.sh      # Script para reverter governor ao padrão
```

## 🚀 Instalação

### Config principal
Copie `gow3.ini` para o diretório de configuração do port:
```
/data/user/0/com.gow3.android.bionic/files/gow3port/gow3.ini
```

### Scripts de Governor (Opcional)
Requer ADB shell ou Shizuku:
```bash
# Antes de jogar:
rish -c "bash setup-governor.sh"

# Depois de jogar (economizar bateria):
rish -c "bash revert-governor.sh"
```

## 📊 Resumo das Mudanças

| Configuração | Antes | Depois | Impacto |
|-------------|-------|--------|---------|
| Resolução | 1280×720 | 960×540 | -120 a -180ms/frame |
| Shadows | ON | OFF | -100 a -150ms/frame |
| Deferred | ON | OFF | -50 a -100ms/frame |
| Stale Textures | ON | OFF | -20 a -40ms/frame |
| FEX L1 Cache | OFF | ON | -30 a -60ms/frame |
| Shader Disk Cache | OFF | ON | Elimina hitch de 4.7s |
| Motion Blur | OFF | OFF | (já desabilitado) |

**FPS esperado: ~2 FPS → 15–25 FPS** no Adreno 735 + FEXCore JIT

## 🔧 Dispositivos Alvo
- Snapdragon 8 Gen 2 / 8 Gen 3 / 8 Elite
- Adreno 730 / 735 / 740 / 750 (driver Turnip/Mesa)
- RAM: 8GB+ recomendado (dmem=7104MiB)

## ⚠️ Notas
- Ajuste `GOW3_SCREEN_W` / `GOW3_SCREEN_H` conforme a resolução do seu display
- Se quiser shadows de volta (com FPS mais baixo): `GOW3_SHADOWS=1`
- Veja `CHANGELOG.md` para análise técnica completa
