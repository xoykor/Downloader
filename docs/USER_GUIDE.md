# Guia de uso

## Interface

Execute `downloader-desktop`. A navegação lateral contém **Downloads**,
**Conversor**, **Fila / Histórico** e **Configurações**.

## Downloads

1. Cole uma URL.
2. Escolha vídeo+áudio, vídeo ou áudio.
3. Escolha formato, altura máxima, bitrate e playlist quando necessário.
4. Opcionalmente informe `cookies.txt` Netscape ou um perfil como
   `firefox:default`.
5. Clique **Analisar** para inspecionar a URL ou **Adicionar à fila** para baixar.

A interface executa até dois downloads ao mesmo tempo. Cada item pode ser
cancelado. Arquivos só aparecem no destino depois de passarem no ffprobe.

## Conversor

Escolha um arquivo e o formato final. **auto** sonda aceleração de hardware e só
usa um backend que respondeu ao teste curto. Se o encoder acelerado não aceitar
o arquivo/codec, o aplicativo repete a conversão por software.

## Evitar duplicados

A pasta de downloads contém `.downloader-library.json`, que associa ID da mídia
e formato ao arquivo publicado. Se um item único já existir e ainda for uma
mídia válida, o download é pulado.

## YouTube

A proteção vem ativada. Ela combina uma pausa de 5 s do yt-dlp com um contador
persistente de até 300 inícios por janela móvel de 90 minutos. Ao atingir o
limite, a tarefa espera até liberar espaço e pode ser cancelada. O toggle fica
em **Configurações**.

## CLI

```text
downloader-cli deps
downloader-cli analyze <url> [--playlist] [--cookies arquivo | --browser perfil]
downloader-cli download <url> [diretório] [--playlist] [--format formato]
                       [--type video+audio|video|audio] [--height N]
                       [--bitrate valor] [--cookies arquivo | --browser perfil]
downloader-cli convert <arquivo> [formato] [diretório]
                       [--acceleration auto|software|vulkan|vaapi|amf|cuda|qsv]
```

Para isolar o banco durante testes, defina `DOWNLOADER_DATA_DIR`.
