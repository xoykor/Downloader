# Handoff — estado real e roteiro de conclusão

Data: 15/09/2026. Projeto: `/home/x/Documentos/Estudo/Downloader`.

## Execução da reconstrução Rust — MVP executável entregue

Em 15/09/2026 foram despachados três agentes Luna para começar a reconstrução em paralelo:

- **rust-infra:** processos, SQLite, autenticação e publicação segura.
- **rust-media:** parsers, planejamento, argumentos de yt-dlp/FFmpeg e validação de mídia.
- **rust-ui:** interface egui/eframe e empacotamento Linux.

O agente principal prepara `domain` e `application`, coordena os contratos e integra os crates. O trabalho paralelo usa `rust/crates/{infra,media,desktop}`; nenhum agente deve apagar ou descartar mudanças dos demais. O primeiro marco é M0: workspace compilável, contratos fechados e UI mínima abrindo no Linux alvo.

Estado do primeiro disparo: `domain` e `application` foram criados pelo agente principal; `infra` entregou processos/SQLite/autenticação/publicação; `media` entregou parsers/planos/comandos/progresso; `desktop` entregou UI egui/eframe. A integração dos contratos foi concluída e o runtime passou a executar processos reais em thread Tokio.

Após a continuação, `infra` passou a expor adapters para `domain`, `media` passou a converter seus modelos para `domain` e `application` passou a persistir transições através de `SqliteTaskRepository`. O conjunto offline sem o desktop agora passa com 22 testes (2 `domain`, 3 `application`, 7 `infra`, 10 `media`).

Depois que as dependências do `eframe` foram disponibilizadas, `cargo test --workspace --locked` passou com 46 testes (2 `domain`, 6 `application`, 13 `infra`, 17 `media`, 8 `desktop`); o teste de integração do executor cria uma mídia local, converte para MP4 e verifica a publicação. Na continuação, a autenticação foi integrada ao engine, CLI e UI: `cookies.txt` Netscape é validado antes do uso e perfis locais podem ser importados via `--cookies-from-browser`. A consulta de playlist agora usa `--flat-playlist` e `--lazy-playlist`, com limite de 45 segundos (consultas simples usam 30 segundos), evitando extrair formatos de cada vídeo na prévia. Quando a UI não recebe um caminho, o engine procura automaticamente o primeiro perfil local compatível entre Firefox, Chrome, Chromium, Brave, Edge, Vivaldi e Opera. O conjunto passou a 52 testes (2 `domain`, 9 `application`, 13 `infra`, 19 `media`, 9 `desktop`). `cargo clippy --workspace --all-targets --locked -- -D warnings` e `cargo fmt --all` também passam. O binário release foi compilado em `rust/target/release/downloader-desktop` e a CLI em `rust/target/release/downloader-cli`.

O MVP entregue executa análise de URL, download com yt-dlp, seleção de tipo/qualidade/formato/bitrate, download de playlist, autenticação por arquivo Netscape ou perfil de navegador, conversão local com ffprobe/FFmpeg, fila persistida em SQLite, cancelamento por token, política de colisão e publicação segura. A GUI usa o mesmo runtime; a CLI permite validar os fluxos sem compositor gráfico. O instalador registra um atalho de dois cliques no menu Linux. A abertura visual não pôde ser exercitada neste sandbox porque não há compositor Wayland nem servidor X acessível, mas a compilação, a instalação de teste e os testes do crate desktop foram concluídos.

### Correção aplicada após o relato de aplicativo quebrado

O executável release foi recompilado depois das últimas alterações de código. A prévia de playlist deixou de fazer a extração completa de cada item: usa `--flat-playlist` e `--lazy-playlist`, com timeout dedicado. A UI envia a opção de playlist escolhida, e campos de cookies vazios acionam a detecção automática de um perfil local compatível. A CLI e a UI foram testadas com um `yt-dlp` simulado; a validação confirmou os argumentos rápidos e a ausência de conteúdo de cookie em mensagens de erro.

## 1. Contexto e limites desta entrega

Pedido atendido: avaliar o estado atual, gerar um novo passo a passo de conclusão e avaliar migração para linguagem compilada. Atualização solicitada: incluir a reconstrução em Rust e organizar a implementação para quatro agentes. A reconstrução foi iniciada com três agentes Luna; este documento registra o plano, os contratos e o estado do trabalho paralelo.

O documento `/home/x/Downloads/HANDOFF-download-conversao.md` foi usado como referência de escopo, não como ordem para executar sua implementação. Sua afirmação de que o aplicativo ainda não foi desenvolvido estava ligada ao protótipo Python/PySide6 auditado no início. Os requisitos detalhados daquele documento continuam sendo referência para os critérios de aceite abaixo.

**Diagnóstico atualizado:** o protótipo Python estava incompleto e foi retirado na limpeza final, enquanto a reconstrução Rust entrega um MVP funcional para análise, download, conversão local, playlists e autenticação por arquivo/perfil. Permanecem fora do MVP os recursos avançados de lote e progresso granular; eles seguem como próximos incrementos abaixo.

## 2. O que existe

| Área | Implementação encontrada | Limite atual |
|---|---|---|
| Inicialização e UI (histórico) | `main.py`, `src/app/ui/main.py`; janela em português, botões e formulário de conversão | Protótipo removido após a migração; a UI ativa está em `rust/crates/desktop` |
| Dependências | `services/dependencies.py`: versões, encoders, caminhos por API | Sem configuração gráfica; consultas síncronas na abertura; erros no executor |
| Metadados | `services/metadata.py`: JSON sem baixar, título, canal, duração e quantidade de formatos | Não entrega formatos detalhados à UI; usa `--no-playlist` |
| Downloads | `services/worker.py`: executa yt-dlp e registra resultado | Ignora opções de download, não planeja destino; sucesso só pelo código de saída |
| Conversão | Worker, `models/output_plan.py`, sete presets em `presets/library.py` | Argumentos incorretos, compatibilidade incompleta, sem validação final |
| Cookies (histórico) | Seleção de arquivo e caminho salvo em configuração | Protótipo removido; a implementação Rust valida Netscape e aceita perfil de navegador |
| Persistência | `persistence/store.py`, `models/task.py`: CRUD, estados e histórico | Opções com JSON duplamente serializado; sem recuperação ou escalonador |
| Testes | Cinco scripts avulsos na raiz | `tests/` vazio; cobertura insuficiente e algumas premissas incorretas |
| Entrega | Workspace Rust, README, guia de uso e atalho `.desktop` | Não há pacote autocontido; yt-dlp, FFmpeg e ffprobe continuam sendo dependências externas |

Os diretórios `tests/` e `assets/` eram vazios e foram removidos na limpeza final. O repositório Git ainda não possui commits; o código aparece como não rastreado. O banco configurado pela aplicação é `~/.downloader/tasks.sqlite`; o banco local gerado durante os testes foi removido. Uma cópia reversível dos artefatos legados ficou temporariamente em `/tmp/downloader-legacy.*` durante a operação e não faz parte da entrega.

## 3. Evidências e verificações realizadas

Executados com o ambiente Python temporário antes da remoção do protótipo:

- `_mtest.py`: passou; builder e parser de metadados.
- `_tmeta.py`: passou; metadados e dependência ausente simulada.
- `_ptest.py`: passou; sete presets e parte da validação.
- `_e2e.py`: passou; criação/conversão de mídia temporária, existência da saída e persistência de estado.

O `_e2e.py` não comprova cópia de streams, codecs finais ou qualidade: confere sobretudo retorno e existência do arquivo. Seu bloqueio fixo de VP9 também não comprova falta real de encoder. `_wtest.py` foi lido, mas não executado nesta auditoria.

Reproduções isoladas adicionais:

| Caso | Resultado observado |
|---|---|
| `QThread(target=lambda: None)` no PySide6 6.11.2 | `AttributeError`: `target` não é propriedade ou sinal Qt |
| `DependencyManager._run()` com executável inexistente | `TypeError`: `CompletedProcess` não aceita `code=` |
| Gravar e ler `options_json` no banco temporário | String JSON codificada novamente; um parse retorna string, não objeto |
| `inspect_media()` com resposta simulada sem duração | `TypeError` ao executar `float(None)` |

Não foram validados nesta sessão: download público real (rede externa), autenticação real com uma conta, abertura visual ponta a ponta neste sandbox sem compositor, retomada automática e progresso granular. O instalador foi executado com um HOME temporário; o cancelamento e a conversão local são cobertos pelo runtime/testes; os testes não usam cookies reais.

## 4. Defeitos prioritários a corrigir

Registro do protótipo Python: as referências a Qt/Python abaixo explicam as falhas encontradas. Na reconstrução Rust, resolver os problemas equivalentes de comportamento; não é necessário consertar e completar a base antiga antes de reescrever.

### P0 — execução e integridade

1. **Thread inválida:** `MainWindow._run_in_background` usa `QThread(target=job)`. Substituir por worker QObject com sinais e `moveToThread`, ou execução assíncrona com QProcess. Tratar exceções e fechamento da janela; sempre liberar o estado ocupado.
2. **Navegação:** `show_details()` muda o QStackedWidget para a página de detalhes, sem ação para voltar. Usar abas visíveis ou botão de retorno; manter cancelamento acessível.
3. **Executor de dependências:** trocar `code=` por `returncode=`; cobrir timeout e erros de permissão. `_which` procura `ytdlp` por usar `kind.name.lower()`; usar `kind.value`. Preservar o ambiente necessário, pois hoje o padrão é `env={}`. Download deve usar o caminho configurado, e não o literal `yt-dlp`.
4. **FFmpeg:** codecs colocados antes de `-i` são opções da entrada e podem selecionar decoders. Posicionar encoders após a entrada, usar `-map` explícito e `-c copy` para cópia. Remover a conversão fixa `libx264 → h264`, a proibição global de VP9 e a inclusão artificial de encoders disponíveis. Os comentários sobre comportamento especial do “FFmpeg n9” não justificam essas regras: a ordem atual dos argumentos explica erros de decoder. Conferir capacidades reais e validar a saída.
5. **Publicação:** hoje usa `-y` e escreve diretamente no destino. Implementar temporário exclusivo, política Renomear/Substituir/Pular e validação antes da publicação. Preservar o original e arquivos existentes.
6. **Inspeção:** pedir `-show_format` além de streams; tratar duração ausente como desconhecida. Falhas de ffprobe não podem virar suposição de áudio e vídeo presentes nem sucesso.
7. **SQLite:** estabelecer representação única das opções e serializar uma vez. Migrar registros antigos, incluindo casos duplamente codificados por atualizações repetidas, com backup e validação do objeto resultante.

### P1 — correção funcional

8. `OutputPlan.is_valid()` pode lançar KeyError para codec digitado e retorna cedo, sem validar os dois streams. Separar codec de encoder, validar todas as faixas e contêineres, rejeitar desconhecidos com mensagem clara.
9. `build_convert_options()` sobrescreve flags do preset com False por padrão. Usar override opcional e sincronizar controles; plano exibido e comando executado devem concordar.
10. `build_download_args()` ignora `options_json`. Implementar seleção de formato, resolução, áudio, destino e nome; vincular a análise à URL para invalidá-la quando o usuário mudar o campo.
11. O download extrai caminho de texto `Destination:` e marca sucesso pelo retorno. Usar saída estruturada que indique o arquivo final após pós-processamento e validar com ffprobe.
12. Cookies são reutilizados globalmente sem escolha por tarefa. Implementar consentimento explícito na UI, validação Netscape, cópias privadas isoladas e limpeza. Sanitizar logs, erros, URLs e histórico; comentários de privacidade no código não implementam essa proteção.

## 5. Decisão de linguagem antes de ampliar o produto

### É possível migrar totalmente?

**Todo o código próprio do aplicativo pode ser reescrito em uma linguagem compilada.** Isso abrange interface, fila, modelos, planejamento, persistência e gerenciamento de processos.

É preciso distinguir esse objetivo de eliminar Python de todas as dependências. yt-dlp é escrito em Python, inclusive com releases independentes que embutem o interpretador. Chamá-lo de um aplicativo C++ ou Rust elimina a necessidade de Python para o código próprio, mas mantém Python dentro dessa ferramenta. Também é necessário escolher o artefato correto: o executável Unix genérico não é necessariamente um pacote com interpretador embutido.

Eliminar Python de toda a solução exigiria substituir ou reimplementar a extração de sites. É tecnicamente possível com escopo definido, mas reproduzir a cobertura e acompanhar mudanças dos sites constitui outro projeto, de manutenção contínua. Não pressupor equivalência com yt-dlp. Empacotar Python em um executável também não equivale a reescrever em C++/Rust.

### Direção adotada para este handoff: Rust

Reconstruir todo o código próprio em Rust, incluindo a interface. O protótipo
Python foi mantido durante a transição para comparação e testes e retirado após
a validação do MVP. yt-dlp, FFmpeg e ffprobe continuam externos; não há
reimplementação de extratores ou codecs.

Arquitetura proposta para iniciar a implementação:

- **egui/eframe:** interface desktop escrita em Rust, com Downloads, Conversor, Fila/Histórico e Configurações. A escolha é uma proposta técnica deste plano; verificar navegação por teclado, acessibilidade e funcionamento no Linux alvo logo no primeiro marco.
- **Tokio:** execução assíncrona, canais de comandos/eventos e processos externos; nunca bloquear a thread gráfica com rede, SQLite ou subprocessos.
- **SQLite com rusqlite:** persistência em worker dedicado; migrações versionadas, backup e importação dos registros antigos.
- **Serde/serde_json:** contratos serializáveis e interpretação das respostas estruturadas das ferramentas.
- **Cargo workspace:** separar domínio, infraestrutura, mídia e desktop em crates; fixar toolchain e dependências após validar uma compilação limpa, versionando Cargo.lock.

Não há ganho de velocidade de conversão demonstrado: esse trabalho já ocorre no FFmpeg. Ganhos de memória ou inicialização deverão ser medidos. Compilado não significa necessariamente binário estático ou ausência de dependências do sistema.

### Aceleração por hardware e Vulkan

O FFmpeg instalado nesta máquina anuncia `vulkan`, `vaapi`, `amf`, `cuda`, `qsv`, `drm` e `opencl`; também possui os encoders `h264_vulkan`, `hevc_vulkan` e `av1_vulkan` e filtros `scale_vulkan`/`tonemap_vulkan`. Isso confirma capacidade no build do FFmpeg, mas não confirma que uma GPU pode ser usada.

Na verificação de runtime, `ffmpeg -init_hw_device vulkan=list` e `vulkaninfo --summary` não encontraram dispositivos (`VK_ERROR_INITIALIZATION_FAILED`). O sistema expõe uma AMD Navi 22, portanto o próximo diagnóstico é driver Mesa/RADV, permissões e acesso ao dispositivo `/dev/dri` no ambiente de execução. A aplicação não deve habilitar Vulkan apenas porque o nome aparece em `-hwaccels`.

Os pacotes `vulkan-radeon`, `mesa` e `vulkan-tools` estão instalados. O bloqueio observado nesta sessão é mais específico: `/dev/dri` não existe no namespace disponível, embora `amdgpu` esteja carregado e `/sys/class/drm/renderD128` esteja visível. Isso não é corrigível por código Rust nem por reinstalação de pacotes dentro do workspace; a validação final precisa ocorrer no host com o dispositivo DRM exposto ao processo.

Implementação prevista: detectar métodos anunciados, enumerar dispositivos reais, executar um encode/resize curto de prova por backend e guardar o resultado por tarefa. Oferecer Automático, Software e backends validados (Vulkan, VAAPI ou AMF); se a prova falhar, informar a causa e retornar ao software. Só construir `-hwaccel`/`-hwaccel_output_format`, filtros Vulkan e encoder Vulkan depois da prova correspondente. Registrar o backend escolhido sem registrar dados sensíveis. A aceleração muda o caminho de processamento, mas não substitui a validação final por ffprobe.

Implementação entregue: `rust/crates/infra/src/hardware.rs` separa backends anunciados de backends utilizáveis e executa uma sondagem curta com `-init_hw_device <backend>=list`. `media` só acrescenta flags/encoders de aceleração quando o chamador informa que o backend foi validado; `application::engine` faz essa sondagem por tarefa e recua para software em caso de falha; `desktop` expõe Automático, Software, Vulkan, VAAPI, AMF, CUDA/NVENC e QSV nas opções de conversão. O teste local confirma que Vulkan sem dispositivo não é usado; neste ambiente a conversão permanece em software.

## 6. Passo a passo para terminar

### Passo 1 — estabelecer uma base reproduzível

- Registrar o estado atual no controle de versão, excluindo caches, cookies, banco, mídia gerada e artefatos de compilação; revisar antes do primeiro commit.
- Registrar versões reais do sistema e ferramentas, dependências e instruções de execução.
- Rust: criar Cargo workspace, rust-toolchain.toml, Cargo.lock e configuração de testes; manter o código Python preservado durante a transição.
- Manter a especificação original como referência externa e esta auditoria como registro histórico versionado.

**Aceite:** outra instalação Linux prepara o ambiente e abre a janela seguindo o README, inclusive com ferramentas externas ausentes.

### Passo 2 — fechar os bloqueios P0

- Corrigir execução assíncrona, navegação, ambiente e localização de ferramentas.
- Implementar resultado de processo tipado: código, stdout, stderr, timeout e cancelamento.
- Corrigir inspeção, argumentos FFmpeg, persistência de opções e publicação segura.
- Incluir testes de regressão para as quatro falhas reproduzidas e para preservação de arquivos.

**Aceite:** analisar, inspecionar e converter não bloqueiam a interface; exceções geram falha recuperável e nunca sucesso falso.

### Passo 3 — criar o planejador de saída

- Normalizar streams e formatos: ID, codec, contêiner, dimensões, FPS, idioma, bitrate e tamanho/estimativa.
- Criar matriz de compatibilidade e resolução de encoder a partir dos recursos detectados.
- Definir cópia versus recodificação por stream, faixas mantidas/removidas e saída esperada.
- Corrigir presets, seus overrides e controles; oferecer prévia legível do plano.

**Aceite:** cada preset gera o codec e contêiner prometidos; combinações inválidas são tratadas sem exceção; não há aumento de resolução implícito.

### Passo 4 — construir fila e ciclo de vida

- Separar o escalonador da UI e persistir transições com datas, erro, destino e temporário.
- Implementar limites de downloads e conversões, incluindo pós-processamento de downloads no limite de conversão.
- Adicionar cancelamento da árvore de processos, escalada após prazo e limpeza conforme política.
- Recuperar tarefas ativas como Interrompido na abertura; permitir nova tentativa sem iniciar automaticamente trabalho indevido.

**Aceite:** concorrência respeitada; cancelar/fechar não deixa processo órfão; reinício conserva opções e não apresenta conclusão falsa.

### Passo 5 — concluir download e análise

- Adicionar seletores de modo, qualidade, resolução real e faixa; invalidar análise ao mudar URL.
- Implementar seleção prévia de itens de playlists; distinguir consulta de download.
- Usar opções estruturadas de progresso e caminho final do yt-dlp, com arquivos temporários por tarefa.
- Executar união/recodificação conforme plano; validar saída e publicar.

**Aceite:** vídeo com áudio, somente áudio e somente vídeo respeitam seleção/destino; 100% global aparece apenas após validação e publicação.

### Passo 6 — autenticação (base implementada; hardening pendente)

- A base agora aceita Sem cookies, arquivo Netscape (`cookies.txt`) e
  navegador/perfil (`--cookies-from-browser`) na análise e no download.
- A UI oferece campos para arquivo e perfil; a CLI expõe `--cookies` e
  `--browser`. A referência é persistida sem o conteúdo do cookie.
- Próximo hardening: criar cópia privada isolada por tarefa, remover arquivos
  derivados e cobrir permissões e expiração sem alterar o arquivo original.
- Diferenciar falhas de rede, perfil, permissão e autenticação; sanitizar diagnósticos.

**Aceite:** uso autenticado e repetição funcionam sem modificar o arquivo original; tarefas concorrentes não compartilham cookie gravável; logs não expõem credenciais.

### Passo 7 — concluir conversão individual e em lote

- Adicionar múltiplos arquivos e arrastar/soltar; inspecionar antes de enfileirar.
- Oferecer preset por lote e ajuste por item, destino e tratamento de colisões.
- Implementar extração de áudio com seleção explícita; opções de resolução, FPS, qualidade, sample rate, canais, faixas, metadados e cortes validados.
- Tratar legendas e streams incompatíveis com aviso antes de descarte; hardware pode ficar opcional.
- Ler `-progress`, usar duração esperada e mostrar progresso indeterminado quando ausente.

**Aceite:** originais intactos, cancelamento sem arquivo final incompleto e saídas conferidas por ffprobe; corte e duração respeitam tolerâncias documentadas.

### Passo 8 — acabamento da interface

- Separar Downloads, Conversor, Fila/Histórico e Configurações.
- Expor versões/caminhos de ferramentas, destino padrão, concorrência, parciais e autenticação.
- Mostrar progresso por etapa, velocidade/tempo quando disponíveis, repetir, abrir arquivo/pasta e detalhes expansíveis.
- Revisar mensagens em português, foco por teclado, redimensionamento e tema claro/escuro.

**Aceite:** todos os fluxos obrigatórios funcionam sem digitar comandos e sem depender de campos técnicos para escolhas comuns.

### Passo 9 — validação e entrega Linux

- Testes determinísticos: parsers, planos, argumentos, transições, serialização e migração do banco.
- Integração local: fixtures pequenas com áudio/vídeo/legenda, formatos diferentes, codecs finais, duração e arquivos inválidos.
- Exercitar caminhos com espaços/acentos, colisão, permissões, falta de espaço, encoder ausente, timeout, cancelamento e reinício.
- Separar testes reais de rede e autenticação dos offline; conferir seleção de playlists e streams separados.
- Validar visualmente o aplicativo e reproduzir amostras da mídia gerada.
- Empacotar para Linux, documentar instalação/atualização e limitações, revisar as condições de redistribuição das dependências escolhidas e testar em ambiente limpo.

**Definição de pronto do MVP:** analisar URL, escolher saída/qualidade,
autenticar opcionalmente, baixar playlist ou item, converter um arquivo e
acompanhar/recuperar tarefas pela UI, com saídas verificadas e instalador
reproduzível. Conversão em lote e progresso granular permanecem extensões
planejadas.

## 7. Reconstrução Rust dividida entre quatro agentes

Esta divisão foi usada na implementação, com o agente 1 também responsável pela integração. Não requer um quinto agente. Os itens avançados que não fazem parte do MVP continuam organizados pelos mesmos papéis.

### Estrutura e propriedade de arquivos

```text
rust/
  Cargo.toml                    # agente 1
  Cargo.lock                    # agente 1 integra atualizações
  rust-toolchain.toml           # agente 1
  crates/
    domain/                     # agente 1: contratos e estados
    infra/                      # agente 2: processos, banco, cookies, publicação
    media/                      # agente 3: análise, planos, download e conversão
    desktop/                    # agente 4: UI e inicialização gráfica
    application/                # agente 1: fila e coordenação
  tests/                        # agente 1: integração entre componentes
  packaging/                    # agente 4: distribuição Linux
```

Cada agente mantém os testes de seu próprio crate e seu manifesto. O agente 1 cria os esqueletos de todos os crates e gerencia manifestos compartilhados, lockfile e testes transversais. Não editar arquivos de outro responsável sem combinar a alteração. Em um checkout compartilhado, evitar comandos Git que descartem trabalho; se houver branches isoladas, usar `codex/rust-core`, `codex/rust-infra`, `codex/rust-media` e `codex/rust-ui`.

### Contratos compartilhados — fechar antes do trabalho paralelo

O agente 1 publica os tipos e interfaces compiláveis em `domain`, incluindo exemplos e testes de serialização. Os demais revisam a parte que consomem antes de fechar o marco inicial.

| Contrato | Conteúdo mínimo |
|---|---|
| TaskSpec / TaskRecord | ID, operação, entrada, opções tipadas (incluindo `AccelerationMode`), destino, política de colisão, AuthRef, estado e datas |
| MediaInfo / FormatInfo | Streams e IDs, codecs, resolução/FPS, idiomas, duração opcional, tamanho exato ou estimado |
| OutputPlan | Etapas, streams selecionados, cópia/recodificação, encoders e propriedades esperadas |
| ProcessSpec / ProcessResult | Executável e argumentos estruturados, ambiente/diretório, código, término por timeout/cancelamento e erro sanitizado |
| TaskEvent | ID e sequência, etapa, progresso opcional, velocidade/ETA opcionais, erro ou destino validado |
| AppCommand | Analyze, Enqueue, Cancel, Retry, UpdateSettings, ListTasks e resolução de colisão/autenticação |
| AppError | Categoria recuperável, mensagem em português, etapa, código e detalhes sanitizados |

Usar PathBuf/OsString para caminhos e argumentos, evitando pressupor UTF-8 no sistema de arquivos. Valores de cookies não fazem parte dos eventos nem registros persistidos. O serviço de autenticação resolve AuthRef apenas durante a execução. Persistir opções como objeto JSON uma única vez, com versão do esquema.

Dependências permitidas: `infra → domain`, `media → domain + infra`, `application → domain + infra + media`, `desktop → application + domain`. Não criar ciclos. UI envia comandos e consome eventos/snapshots; somente a aplicação controla o estado das tarefas e chama os serviços. ProcessRunner pertence à infraestrutura; o componente de mídia usa esse executor, sem implementar outro.

Definir interfaces para ProcessRunner, TaskRepository, AuthStore, OutputPublisher e MediaService. Usar implementações falsas nos testes para que UI, infraestrutura e mídia avancem independentemente. O plano representa fases explícitas de download e processamento: o escalonador adquire o limite de conversão antes de união/recodificação, inclusive quando iniciadas por yt-dlp.

### Agente 1 — domínio, fila e integração

**Missão:** estabelecer a base Rust e conectar os quatro componentes.

**Entregas:**

1. Criar workspace, contratos, eventos, estados e esqueletos compiláveis.
2. Implementar application: comandos, fila, limites separados para download e conversão e transições persistidas.
3. Implementar recuperação de tarefas interrompidas e coordenação de novas tentativas; autenticação e colisões que dependem do usuário viram estados explícitos.
4. Encaminhar cancelamento ao executor, impedir transições tardias para sucesso e só concluir após validação/publicação.
5. Integrar os crates, manter documentação arquitetural e executar testes transversais.

**Aceite:** testes com serviços simulados demonstram limites, ordenação de eventos, reinício, cancelamento e ausência de sucesso prematuro. As opções de uma tarefa sobrevivem à gravação/leitura sem alterações.

**Depende de:** contratos primeiro; depois repositório e executor do agente 2 e MediaService do agente 3. Pode desenvolver a fila contra implementações falsas.

### Agente 2 — infraestrutura, persistência e privacidade

**Missão:** oferecer execução e armazenamento confiáveis aos serviços.

**Entregas:**

1. Detectar executáveis e versões, respeitar caminhos configurados e preservar ambiente necessário.
2. Implementar ProcessRunner assíncrono com stdout/stderr consumidos continuamente, limites de buffers, timeout, término e coleta do processo.
3. Gerenciar grupos de processos no Linux: encerramento gracioso, prazo, encerramento forçado e espera; `kill_on_drop` sozinho não é prova de cancelamento de descendentes.
4. Implementar SQLite, preferências/histórico, migração do esquema e importação testada de JSON antigo com codificações repetidas. Usar cópia do banco na migração e preservar backup.
5. Implementar AuthStore: arquivo Netscape, navegador/perfil explícitos, cópia privada por tarefa, referência persistente opcional e limpeza.
6. Implementar OutputPublisher: temporários, colisões, publicação entre sistemas de arquivos e limpeza conforme política. Só publicar artefato que o serviço de mídia já validou.
7. Sanitizar diagnósticos e erros, com testes usando segredos sintéticos.

**Aceite:** ausência de ferramenta, timeout e permissão geram erros tipados; cancelamento não deixa descendentes no teste; migração preserva registros; falha/cancelamento não sobrescreve arquivos existentes nem deixa saída incompleta publicada.

**Depende de:** contratos do agente 1. Não assume decisões de compatibilidade de codecs, responsabilidade do agente 3.

### Agente 3 — mídia, yt-dlp e FFmpeg

**Missão:** implementar os fluxos corretos de análise, planejamento, download e conversão.

**Entregas:**

1. Normalizar JSON de yt-dlp/ffprobe com formatos completos, playlists, streams e duração desconhecida.
2. Implementar presets, matriz codec/contêiner e resolução de encoders disponíveis, sem aliases ou bloqueios globais herdados do protótipo.
3. Gerar comandos com argumentos estruturados: posição correta das opções FFmpeg, mapeamento de faixas, cópia explícita, extração de áudio e cortes.
4. Aplicar seleção real de resolução, faixa, qualidade, itens da playlist e formato final ao download.
5. Emitir progresso por fase e caminho final estruturado; disponibilizar etapas de processamento ao escalonador.
6. Validar arquivos com ffprobe antes de solicitar publicação: streams, codec, contêiner, duração e dimensões conforme plano.
7. Cobrir todos os presets com fixtures locais e manter testes de rede separados.

**Aceite:** cópia é verificada como cópia de streams; recodificação entrega propriedades esperadas; fonte inválida e encoder ausente falham claramente; somente áudio não publica vídeo; resultados de downloads apontam ao arquivo final.

**Depende de:** contratos do agente 1 e ProcessRunner/AuthStore/OutputPublisher do agente 2. Parsers e planejamento começam com respostas simuladas, sem aguardar infraestrutura real.

### Agente 4 — interface Rust e entrega Linux

**Missão:** reconstruir a experiência gráfica e preparar a distribuição.

**Entregas:**

1. Implementar desktop com egui/eframe; iniciar a UI consumindo snapshots e eventos simulados.
2. Criar telas de Downloads, Conversor, Fila/Histórico e Configurações, com navegação permanente.
3. Implementar seletores baseados em metadados, preview do plano, playlists, seleção múltipla/arrastar arquivos e ajustes de lote.
4. Expor autenticação por tarefa, ferramentas, destinos, colisões e concorrência.
5. Mostrar progresso/estados sem travar, permitir cancelar/repetir e abrir destino; invalidar análise quando a URL mudar.
6. Validar teclado, foco, legibilidade, tema e redimensionamento; comunicar cedo limitações de acessibilidade da solução escolhida.
7. Criar pacote Linux, README de instalação/atualização, requisitos externos e teste de abertura em ambiente limpo. Documentar versões testadas e condições de redistribuição do pacote escolhido.

**Aceite:** todos os fluxos do escopo são acessíveis sem terminal, interface permanece responsiva e o pacote inicia com mensagens úteis mesmo sem ferramentas externas.

**Depende de:** contratos/eventos do agente 1; integração real após application estar disponível. Não implementa fila, SQL ou subprocessos dentro da UI.

## 8. Ordem de execução e marcos de integração

| Marco | Trabalho e paralelismo | Condição para avançar |
|---|---|---|
| M0 — base e contratos | Agente 1 cria workspace/esqueletos; 2 revisa contratos de processos/banco; 3 prepara fixtures e matriz; 4 valida janela e navegação | Workspace compila, tipos/interfaces acordados, UI mínima abre no Linux alvo |
| M1 — componentes isolados | 1 implementa fila simulada; 2 infraestrutura; 3 parsers/planos; 4 telas com eventos simulados | Testes por componente passam sem rede; sem contratos duplicados |
| M2 — primeira conversão completa | 1 integra; 2 fornece executor/banco/publicação; 3 inspeção/conversão/validação; 4 conecta o formulário | Arquivo local atravessa UI → fila → conversão → validação → publicação; cancelar e colisão testados |
| M3 — downloads e autenticação | 1 coordena fases/recuperação; 2 cookies; 3 yt-dlp/playlist; 4 seletores e autenticação | Download público e autenticado, quando houver credenciais de teste autorizadas, respeita opções; erros e repetição testados |
| M4 — lote e recuperação | Cada agente fecha seus recursos pendentes dos passos 1–9 | Lote, concorrência, reinício e cancelamento de descendentes passam |
| M5 — entrega | 1 coordena regressão integrada; 2 verifica dados/privacidade; 3 verifica mídia; 4 pacote e QA visual | Critérios da definição de pronto satisfeitos em ambiente limpo |

Não iniciar todos alterando os mesmos arquivos. O paralelismo principal começa após M0; decisões de contrato passam pelo agente 1. Cada entrega informa arquivos alterados, interfaces afetadas, testes executados/resultados e pendências. Não declarar um componente pronto apenas porque compila ou porque a ferramenta retornou código zero.

### Verificações compartilhadas

- Executar a partir de `rust/`: `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets -- -D warnings` e `cargo test --workspace --locked`.
- Separar testes offline, integração com executáveis locais e rede/autenticação; registrar explicitamente testes não executados e por quê.
- Evitar testes que apenas repetem tabelas da implementação. Priorizar preservação de arquivos, propriedades da mídia, cancelamento, recuperação, migração e contratos.
- Nenhum agente deve usar cookies reais em fixtures, commits, logs ou relatórios.
- Só substituir o ponto de entrada Python após equivalência funcional validada; não excluir a base antiga nesta etapa de planejamento.

### Instruções de encaminhamento para cada agente

**Agente 1:** “Implemente a base Rust, contratos e aplicação conforme as seções 7 e 8. Coordene a integração dos agentes 2–4. Feche M0 antes de liberar trabalho dependente. Preserve a base Python durante a transição e não considere a reconstrução pronta sem os critérios de aceite.”

**Agente 2:** “Implemente somente infraestrutura, persistência, autenticação, processos e publicação nos diretórios atribuídos. Consuma os contratos do agente 1, entregue testes dos riscos de integridade e comunique alterações de interface antes de aplicá-las.”

**Agente 3:** “Implemente somente parsers, planejador, presets e serviços de mídia nos diretórios atribuídos. Use o executor do agente 2; exponha fases ao escalonador do agente 1. Verifique propriedades reais das saídas com fixtures pequenas.”

**Agente 4:** “Reconstrua a UI em Rust com a arquitetura proposta, consumindo comandos/eventos. Comece com dados simulados, depois integre a aplicação. Prepare pacote Linux, documentação e validação visual, sem duplicar lógica dos demais agentes.”

## 9. Referências técnicas

- [FFmpeg: ordem das opções, mapeamento e cópia de streams](https://ffmpeg.org/ffmpeg.html).
- [yt-dlp: dependências, formatos e executáveis com interpretador](https://github.com/yt-dlp/yt-dlp).
- [egui: interface gráfica em Rust e integração com eframe](https://github.com/emilk/egui).
- [Tokio: execução e ciclo de vida de processos](https://docs.rs/tokio/latest/tokio/process/struct.Command.html).

**Estado final desta etapa:** M0, o MVP executável, autenticação básica, playlist, documentação e o atalho de dois cliques estão concluídos. Os incrementos opcionais restantes são lote, progresso granular, cópia privada de cookies por tarefa e QA visual no host com compositor; o protótipo legado foi removido na limpeza final.

## Atualização: Opus, capa, títulos e progresso individual

Implementados na interface e no runtime: saída Opus, capa da thumbnail,
publicação com título e progresso de transferência em tempo real por faixa.
O executor drena stdout/stderr concorrentemente; registros JSON do yt-dlp
identificam cada faixa. A lista de arquivos finais vem de `after_move`, evitando
confundir thumbnails ou arquivos intermediários com mídia pronta.

Validação: 54 testes; teste real de Opus com capa verificada por ffprobe;
playlist local de duas entradas executada com yt-dlp e FFmpeg reais, com
progressos separados, nomes por título e capa incorporada. Não foi utilizado
um serviço externo neste teste. Detalhes por faixa ainda não são persistidos.

### Correção da capa separada

A publicação agora consome eventos `after_move` durante o download, em paralelo
com o restante da playlist. Antes, aguardava a saída do yt-dlp e uma falha
impedia a incorporação das capas das faixas anteriores. Desabilitada a imagem
geral da playlist. 55 testes passaram, incluindo publicação antes do fim do
processo e preservação da faixa com capa quando a próxima falha. Release recompilado.

### Evitar duplicação de playlists

`application::library` verifica a pasta de destino, valida mídia com ffprobe e
reconhece identidades por índice local ou URL de origem do YouTube. O arquivo
`--download-archive` é recriado por tarefa somente com arquivos existentes e
compatíveis; removê-los permite novo download. A publicação confere o índice
novamente para evitar cópia duplicada entre tarefas concorrentes.

57 testes passaram, além de clippy e release. Teste de integração local executou
a mesma playlist duas vezes com yt-dlp real; a segunda execução pulou ambas as
faixas mesmo com o arquivo-fonte temporariamente indisponível. Permaneceram
somente dois Opus com capa e o índice na pasta de saída. Limites: varredura direta
(não recursiva), identificação conservadora e equivalência por vídeo/formato.

## Atualização de 16/09/2026 — limites do YouTube

Implementado em `application/src/network.rs`: pausa fixa de 5 segundos,
300 inícios de vídeo por janela móvel de 90 minutos, persistência e bloqueio
entre processos, espera cancelável e retomada por arquivo de downloads.
Falhas temporárias recebem até três retries (90/180/360 segundos). Análises
passaram a executar em jobs canceláveis, com timeout de 180/300 segundos por
tentativa; o executor de stdout em streaming agora respeita timeout.
As configurações mostram a política e oferecem o toggle **Ativar limite de
segurança**, ligado por padrão. A alteração é enviada como `UpdateSettings` e
vale para as próximas operações do runtime; ao desligar, os argumentos de
limitação não são adicionados às URLs do YouTube. Consulte [limites e fontes](YOUTUBE_LIMITS.md).

Validação final: 66 testes passaram, clippy sem avisos e build release concluído.
Integração local com yt-dlp e FFmpeg reais: estado inicial com 299 vagas ocupadas,
primeiro vídeo preencheu a cota, espera de 20 segundos até expirar o histórico
artificial, retomada pulando o primeiro item e conclusão do segundo. Dois arquivos
Opus com capas incorporadas confirmadas pelo ffprobe, sem imagens separadas ou
duplicação; estado final com dois inícios persistidos. Esse teste não acessou
o YouTube e não valida uma recusa real do serviço; recusas, retries, persistência
e cancelamento são cobertos pelos testes automatizados.
