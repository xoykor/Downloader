# Arquitetura

O Downloader é um projeto C17. GTK4 e a CLI compartilham o mesmo núcleo; nenhuma
interface monta comandos de mídia por conta própria.

```text
desktop (GTK4) ─┐
cli -------------┼─> engine
                 │     ├─> application (fila/estados)
                 │     ├─> media (argv + JSON)
                 │     ├─> process (fork/exec/poll)
                 │     ├─> database (SQLite)
                 │     ├─> library (duplicados)
                 │     ├─> hardware (sondagem)
                 │     └─> publish (arquivo final)
                 └─> domain (contratos)
```

## Fronteiras

### domain

Tipos, enums, erros e regras simples de estado. Não acessa o sistema.
Strings pertencentes a registros são copiadas profundamente e liberadas por
funções `*_clear` explícitas.

### application

Mantém tarefas e fila FIFO. Downloads têm limite independente de conversões.
A interface GTK usa dois pools: 2 workers de download e 1 de conversão.

### process

Executa programas com `fork` + `execvp`, com `stdout`/`stderr` em pipes. Não há
shell intermediário. `poll` permite consumir progresso enquanto o filho roda.
Timeout e cancelamento enviam `SIGTERM`; após 2 s sem saída, escalam para
`SIGKILL`.

### media

Monta `argv` para yt-dlp/ffmpeg/ffprobe e interpreta JSON com json-c. Referências
de autenticação continuam sendo argumentos separados. O conteúdo de cookies
não entra em mensagens ou no banco.

### database

SQLite guarda tarefas e o histórico do limitador do YouTube. WAL é ativado para
permitir leitura/escrita concorrente do desktop.

### engine

Orquestra análise, download, conversão, validação e publicação. O download nunca
é publicado diretamente: yt-dlp escreve numa pasta temporária da tarefa,
ffprobe valida cada mídia e `publish` move o resultado. Conversões aceleradas
que falham são repetidas automaticamente em software.

### library

`.downloader-library.json` relaciona identidade da mídia + formato ao basename
do arquivo publicado. Caminhos do índice não podem escapar da pasta de destino.

## Fluxo de download

1. analisar URL com yt-dlp;
2. consultar índice de duplicados para item único;
3. aplicar proteção persistente de YouTube quando habilitada;
4. baixar na área temporária;
5. validar cada arquivo com ffprobe;
6. publicar segundo a política de colisão;
7. atualizar índice e SQLite.

## Fluxo de conversão

1. validar a entrada com ffprobe;
2. resolver aceleração e sondar o dispositivo;
3. executar FFmpeg em temporário;
4. se hardware falhar, repetir em software;
5. validar saída;
6. publicar somente após validação.
