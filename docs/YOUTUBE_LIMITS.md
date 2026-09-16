# Limites e novas tentativas do YouTube

## Política aplicada pelo aplicativo

- **5 segundos de pausa antes de cada download**, sem aleatoriedade. Processamento, extração e esperas por limite podem aumentar o intervalo efetivo.
- **Até 300 inícios de vídeo em uma janela móvel de 90 minutos**. Cada início ocupa uma vaga por 90 minutos; a fila aguarda automaticamente a liberação de vagas e permite cancelar durante a espera.
- Vídeos pulados pelo arquivo de downloads já realizados não consomem vagas. Uma tentativa que começou a baixar e falhou consome uma vaga.
- A contagem e os intervalos de segurança sobrevivem ao reinício, em `youtube-network.json` no diretório de dados (normalmente `~/.downloader`). GUI e CLI compartilham o controle quando usam o mesmo diretório. Um bloqueio de arquivo mantém apenas uma operação do YouTube ativa nesse diretório.
- Extração usa `--sleep-requests 5.4`. Isso espaça as requisições cobertas pelo extrator do yt-dlp; não é um limitador universal de todas as requisições HTTP, fragmentos ou redirecionamentos.
- Downloads de fragmentos usam uma conexão por vez. Configurações externas do yt-dlp são ignoradas para preservar a política do aplicativo.

O controle se aplica às URLs reconhecidas do YouTube. Não contabiliza navegação, outros aplicativos ou instâncias com diretórios de dados diferentes.

## Recusas e timeouts

Erros temporários reconhecidos (429, 500/502/503/504, timeout e interrupções de conexão) recebem até **três novas tentativas**, após **90, 180 e 360 segundos**. A playlist interrompe o avanço durante a recusa e reutiliza o arquivo de downloads concluídos ao retomar. Erros de login, conteúdo privado e 401/403 não são repetidos automaticamente.

O timeout de socket é de 30 segundos. Cada tentativa de análise tem limite total de 180 segundos para vídeo individual e 300 segundos para playlist. Cancelar interrompe tanto o processo quanto a espera.

Se os diagnósticos apresentarem explicitamente um `Retry-After` numérico, o aplicativo considera esse valor acrescido de 50%, sem reduzir a espera progressiva. O yt-dlp normalmente não expõe esse cabeçalho ao aplicativo; datas HTTP não são interpretadas. Portanto, não há garantia de leitura do tempo solicitado pelo servidor em toda recusa.

## O que o YouTube publica

As [cotas oficiais da YouTube Data API](https://developers.google.com/youtube/v3/getting-started) se referem à API, não aos downloads do yt-dlp. Não foi identificada uma cota oficial pública fixa de downloads por hora/dia nem um tempo universal de desbloqueio para este uso.

A [documentação dos mantenedores do yt-dlp](https://github.com/yt-dlp/yt-dlp/wiki/Extractors#this-content-isnt-available-try-again-later) registra observações aproximadas de 300 vídeos/hora para visitantes e recomenda pausas de 5 a 10 segundos. São observações, não garantias do YouTube.

Os 300 vídeos por **90 minutos** são a política local solicitada: a janela é 50% maior que uma hora. Os 5 segundos foram mantidos conforme a instrução final. As esperas de 90/180/360 segundos são uma escolha conservadora do aplicativo (base de 60 segundos, duplicada e acrescida de 50%), não um prazo oficial exigido pelo YouTube. Nenhum teto diário fictício foi acrescentado. Estes limites reduzem a frequência, mas não garantem ausência de bloqueios.
