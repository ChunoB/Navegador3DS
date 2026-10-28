# Navegador 3DS

Navegador leve para Nintendo 3DS (Old e New), feito para sites de texto: Wikipédia, notícias, blogs, fóruns e buscas.
Não roda JavaScript nem CSS: o HTML é lido por um renderizador próprio (`source/html.c`) que mostra títulos, listas,
tabelas, links e formulários. A página continua da tela de cima para a de baixo, onde dá para tocar nos links.

Tem HTTPS com certificados verificados, cookies, redirecionamentos, gzip, sites em UTF-8 e em Latin-1, favoritos,
histórico, voltar/avançar, downloads para o cartão SD e atualização pelo próprio app.

Instalação e uso: veja [LEIA-ME.txt](LEIA-ME.txt). Baixe o pacote em
[Releases](https://github.com/ChunoB/Navegador3DS/releases/latest).

## Compilar

Precisa do devkitPro com `3ds-dev`, `3ds-curl`, `3ds-mbedtls` e `3ds-zlib`. `make` gera o `.3dsx`; `make cia` gera o `.cia`
com `tools/bin/makerom.exe` e `tools/bin/bannertool.exe` (não vão no repositório).

| Arquivo | O que faz |
|---|---|
| `source/main.c` | Telas, histórico, favoritos, páginas internas (`about:`) |
| `source/html.c` | Renderizador de HTML e formulários |
| `source/net.c` | Rede em segundo plano (curl), cookies, downloads |
| `source/url.c` | Links relativos, buscas e conversão de Latin-1 para UTF-8 |
| `source/updater.c` | Atualização pelo próprio app (GitHub releases) |
