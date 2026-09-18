# Implementação C17

Esta é a implementação principal do Downloader.

## Organização

```text
c/
├── include/downloader/   API pública do núcleo
├── src/
│   ├── domain.c          contratos e ownership
│   ├── application.c     máquina de estados da fila
│   ├── process.c         fork/exec, pipes, timeout e cancelamento
│   ├── media.c           yt-dlp, ffprobe, FFmpeg e JSON
│   ├── database.c        SQLite
│   ├── publish.c         publicação atômica e colisões
│   ├── hardware.c        sondagem de aceleração
│   ├── library.c         índice de downloads concluídos
│   ├── engine.c          orquestração real
│   ├── cli/main.c        CLI
│   └── desktop/main.c    GTK4
└── tests/                testes unitários e integração local
```

## Regras de legibilidade

Veja também [`CODING_STYLE.md`](CODING_STYLE.md) para as convenções de comentários, ownership e formatação.

- C17 padrão; POSIX só onde o Linux realmente precisa (`fork`, `exec`, `poll`).
- cada função deixa explícito quem aloca e quem libera memória;
- prefixo `dld_` em símbolos públicos;
- comentários explicam motivo, ownership e invariantes, não repetem sintaxe;
- processos externos recebem `argv` separado e nunca passam por shell;
- operações de publicação só removem o temporário depois que o destino está
  completo;
- warnings fortes são tratados como erro no núcleo.

## Verificação

```sh
cmake -S c -B build/c -DCMAKE_BUILD_TYPE=Debug
cmake --build build/c --parallel
ctest --test-dir build/c --output-on-failure
```

Os testes cobrem domínio, fila, parsing/comandos de mídia, processos,
publicação, SQLite, biblioteca e uma conversão real FFmpeg + ffprobe.
