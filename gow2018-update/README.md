# Correção do build – Kotlin Compose Plugin

## Problema
O build falhava com:

```
Plugin [id: 'org.jetbrains.kotlin.plugin.compose', version: '2.2.10', apply: false] was not found
```

## Solução
Atualizei a versão do plugin `org.jetbrains.kotlin.plugin.compose` de **2.2.10** para **2.2.21** (versão estável da mesma linha 2.2).

A versão do Compose Compiler Gradle plugin **deve ser a mesma** da versão do Kotlin.

## Como aplicar
1. Substitua o arquivo `android-launcher/build.gradle.kts` pelo que está neste zip  
   **ou**
2. Altere manualmente a linha do plugin para:

```kotlin
id("org.jetbrains.kotlin.plugin.compose") version "2.2.21" apply false
```

Se o projeto usar Version Catalog (`libs.versions.toml`), atualize também a versão do Kotlin/Compose lá.

## Observação
Se o restante do projeto já estiver em Kotlin 2.3.x ou 2.4.x, use a versão correspondente (ex: 2.3.21 ou 2.4.21).
