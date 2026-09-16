# Atalho de dois cliques

Para instalar o executável e o atalho no menu do Linux:

```bash
./packaging/install-desktop.sh
```

Depois, abra **Downloader** no menu de aplicativos. O instalador compila o
binário release se necessário e instala uma cópia em `~/.local/bin`.

O arquivo [`Downloader.desktop`](Downloader.desktop) também pode ser copiado
para a área de trabalho; ele aponta para o binário release deste workspace.

O atalho usa o binário em `rust/target/release/downloader-desktop`, já
compilado após a validação do projeto. Se o gerenciador de arquivos pedir,
selecione **Permitir iniciar** uma vez.
