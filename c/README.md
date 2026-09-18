# Downloader em C

Esta pasta contém a migração incremental do Downloader para **C17**. A versão
Rust continua intacta durante a transição para que cada etapa possa ser
comparada e validada antes de substituir a implementação anterior.

## Estado atual

A primeira etapa porta os contratos essenciais do domínio e a máquina de
estados da fila:

- estados e tipos de tarefa;
- regras de tarefa ativa/terminal;
- registros com cópia profunda e destruição explícita;
- fila FIFO com limites separados para downloads e conversões;
- cancelamento, repetição, conclusão e recuperação após reinício;
- testes determinísticos sem rede, GPU, FFmpeg ou yt-dlp.

Ainda **não** foram portados nesta etapa: execução de processos, SQLite, parsing
JSON de yt-dlp/ffprobe, planejamento FFmpeg e interface GTK4.

## Compilar

Os comandos abaixo funcionam normalmente em `fish` porque não dependem de
sintaxe específica de Bash:

```sh
cmake -S c -B build/c -DCMAKE_BUILD_TYPE=Debug
cmake --build build/c
ctest --test-dir build/c --output-on-failure
```

A CLI provisória pode ser executada com:

```sh
./build/c/downloader-c-cli version
./build/c/downloader-c-cli queue-demo
```

## Convenções de legibilidade

O código segue algumas regras deliberadas para continuar fácil de revisar por
humanos:

1. C17 padrão sempre que possível; extensões de compilador não fazem parte da API.
2. Ownership de memória é documentado na fronteira das funções.
3. Nomes usam o prefixo `dld_` para evitar colisões em C.
4. Comentários explicam decisões, invariantes e motivos; não repetem a linha seguinte.
5. Operações que podem falhar reservam/copiam dados antes de alterar o estado.
6. `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` são habilitados no núcleo.
7. Testes espelham regras importantes já cobertas pela implementação Rust.

## Próximas etapas

1. portar `media`: parsing de yt-dlp/ffprobe e planejamento de argumentos;
2. portar `infra/process`: execução segura sem shell;
3. portar SQLite e publicação atômica;
4. conectar a CLI real (`analyze`, `download`, `convert`);
5. portar o desktop para GTK4/Wayland;
6. atualizar AppImage e remover Rust apenas depois de atingir paridade.
