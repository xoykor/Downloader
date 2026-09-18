# Limites do YouTube

## Política aplicada pelo Downloader

Quando **Proteção YouTube** está ativada, o Downloader aplica duas camadas:

- passa `--sleep-interval 5` ao yt-dlp, introduzindo uma pausa de 5 segundos;
- registra inícios de download em SQLite e permite no máximo **300 inícios**
  dentro de uma janela móvel de **90 minutos**.

O histórico do limitador fica no mesmo banco `tasks.sqlite` usado pelas tarefas,
por isso sobrevive ao reinício do aplicativo. GUI e CLI compartilham o contador
quando usam o mesmo diretório de dados.

Ao atingir o limite, a tarefa aguarda até a entrada mais antiga sair da janela.
O cancelamento continua sendo consultado durante essa espera.

## Tentativas

O comando de download passa ao yt-dlp:

- `--retries 3`;
- `--fragment-retries 3`.

A política de repetição específica de cada site continua sendo responsabilidade
do yt-dlp. O Downloader não inventa um tempo de desbloqueio do YouTube e não
interpreta `Retry-After` por conta própria.

## O que esse limite significa

Os 300 inícios por 90 minutos são uma política local conservadora do aplicativo;
não representam uma cota oficial publicada pelo YouTube. A pausa reduz a
frequência de acessos, mas não garante que um serviço externo nunca imponha
limites adicionais.

A proteção pode ser ativada ou desativada na tela **Configurações**. Desativá-la
remove o contador local e os argumentos de pausa das próximas tarefas.
