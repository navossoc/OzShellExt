# OzShellExt

Shell extensions in-proc para os formatos de imagem OZ usados pelo MU Online:
thumbnail provider e preview handler para `.ozb`, `.ozj` e `.ozt`.

*[Read in English](README.md)*

## Os formatos

Cada um é uma imagem comum atrás de um prefixo curto:

| extensão | prefixo | o que vem depois |
| --- | --- | --- |
| `.ozb` | 4 bytes | um BMP |
| `.ozj` | 24 bytes | um JPEG |
| `.ozt` | 4 bytes | um TGA, com alpha |

O prefixo são quatro bytes que o decoder ignora - só o tamanho importa. Num
client completo (704 arquivos `.ozt`), 701 começam com `00 00 02 00`, que é uma
cópia dos quatro primeiros bytes do cabeçalho TGA que vem logo atrás, e três com
`00 00 00 00`. O que *é* conferido é a assinatura da imagem que vem atrás
(`BM`, `FF D8`), o que faz um arquivo com extensão errada falhar cedo.

BMP e JPEG passam pelo WIC, o componente de imagem que o Windows já traz. TGA
não tem codec no WIC, então o `OzImage.cpp` decodifica: image types 1, 2 e 3 e
os gêmeos RLE 9, 10 e 11 - color-mapped, truecolor e grayscale - em 8, 15, 16,
24 e 32 bpp, respeitando os bits de origem e o first-entry index do color map.
O type 0 não carrega imagem e as variantes Huffman do TGA 1.0 (32 e 33) estão
extintas; os dois são rejeitados.

Duas das regras de alpha vêm do que os arquivos reais fazem, não da spec:

- Uma imagem de 32 bpp mantém o canal alpha mesmo quando o descriptor declara
  zero attribute bits. Num client de fábrica, 10 arquivos são folhagem, cabelo
  e pelo recortados guardados exatamente assim; respeitar o descriptor os
  transforma em retângulos opacos.
- Uma imagem com alpha zero em todos os pixels é tratada como opaca. Uma
  miniatura totalmente transparente não mostra nada, enquanto o canal de cor
  muitas vezes tem a textura inteira - outros 13 arquivos, entre eles os tiles
  de grama.

## O que faz

- **Miniaturas** no File Explorer, em qualquer tamanho de ícone.
- **Preview pane**, escalando para caber, com o fundo do próprio painel e um
  xadrez atrás da transparência.

Os dois rodam dentro do host do próprio shell (`dllhost.exe` e `prevhost.exe`).
Não há executável auxiliar para disparar, nem arquivo temporário, nem runtime
gerenciado para subir.

Os dois tipos de handler são independentes: instale um, o outro, ou ambos.

## Medido

| arquivo | tamanho | alvo | tempo |
| --- | --- | --- | --- |
| `Chrome01.OZJ` | 64x64 | thumbnail de 256 px | 0,54 ms |
| `blood.OZT` | 128x128 | thumbnail de 256 px | 0,42 ms |
| `TerrainHeight.OZB` | 256x256 | thumbnail de 256 px | 1,31 ms |

## Como funciona

Uma DLL, seis CLSIDs - um thumbnail provider e um preview handler por
extensão. O CLSID que o shell instancia é o que diz ao handler qual formato ele
está olhando, então nada precisa ser adivinhado a partir do arquivo.

| | thumbnail | preview |
| --- | --- | --- |
| `.ozb` | `{247FC569-1E33-4B8A-9DA1-EDD92F062BFD}` | `{7F42AA44-9ED0-45B3-860B-49800BE1D008}` |
| `.ozj` | `{839DC7DB-0F6A-4EE9-8661-4C081C104CB8}` | `{16D991BD-A3E2-4888-BBA7-B37F422F5FE5}` |
| `.ozt` | `{C3DA4352-C864-4609-A239-7F226ECDC41C}` | `{E864CB5D-539C-4F61-BF5C-147EFA19288F}` |

- `OzImage.cpp` - tratamento do prefixo, os decoders WIC e TGA, e o
  escalonamento: box filter ao reduzir, bilinear ao ampliar, os dois ponderando
  a cor pelo alpha - senão a cor (arbitrária) dos pixels transparentes vaza para
  a borda e vira halo escuro.
- `ThumbnailProvider.cpp` - `IInitializeWithStream` + `IThumbnailProvider`,
  escrevendo direto num DIB section de 32 bpp top-down. Nunca amplia; devolve
  `WTSAT_ARGB` só quando a origem realmente tem alpha. `ThreadingModel = Both`,
  para o host de thumbnails não precisar marshalar.
- `PreviewHandler.cpp` - `IPreviewHandler` + `IPreviewHandlerVisuals` +
  `IOleWindow` + `IObjectWithSite` sobre uma janela filha comum. Escala para
  caber (ampliando quando a imagem é pequena), respeita as cores de fundo e de
  texto do painel, desenha um xadrez atrás da transparência, faz cache do bitmap
  escalado por tamanho de painel e usa double buffering para não piscar ao
  arrastar o divisor. `ThreadingModel = Apartment`, com `AppID` apontando para o
  surrogate `prevhost.exe`.
- `ShellExt.cpp` - a tabela de formatos, class factory, `DllRegisterServer` /
  `DllUnregisterServer` e `DllInstall`, que é o que viabiliza instalar um
  handler de cada vez.

Dependências: só Win32 (`gdi32`, `msimg32`, `ole32`, `advapi32`, `shlwapi`,
`shell32`, `windowscodecs`). Compilado com `/MT`, então não precisa do VC++
redistributable.

## Robustez

O decoder de TGA é escrito à mão e faz parse de um arquivo que o usuário só
precisou baixar, dentro do host do próprio shell. Ele é escrito para entrada
hostil:

- Toda leitura é checada contra o buffer - o color map, os pacotes RLE e o
  bloco de pixels. Um arquivo malformado devolve erro em vez de ler além do
  fim.
- Nada é alocado pelo que o cabeçalho apenas alega. Um pacote RLE rende no
  máximo 128 pixels por byte de header, então o tamanho do payload limita a
  contagem de pixels por cima; um arquivo de 27 bytes declarando 65535x1525 é
  rejeitado direto, em vez de custar meio gigabyte antes. Imagens sem
  compressão precisam trazer os pixels, o que é conferido antes de dimensionar
  o buffer.
- O `test\fuzz.cpp` compila os decoders sob AddressSanitizer e os martela com
  os próprios arquivos do jogo, mutados - campos de cabeçalho reescritos, bytes
  trocados, buffers truncados em pontos aleatórios. 2,07 milhões de execuções
  sobre um client de fábrica, três seeds, nenhum acesso fora dos limites.

BMP e JPEG vão para o WIC, o decoder que o Explorer já roda para `.bmp` e
`.jpg`. Este projeto não acrescenta parse próprio ali, só o offset onde a
imagem começa.

## Build

```bat
build.cmd
```

Gera `build\OzShellExt.dll` (x64). Precisa do Visual Studio 2022 Community no
caminho padrão; se estiver em outro lugar, ajuste `VSDEV` no script.

`build.cmd thumbnail` e `build.cmd preview` compilam uma DLL que carrega só
aquele tipo de handler. Raramente é necessário: o build padrão tem os dois, e
quais serão *registrados* se decide na instalação.

## Instalação

Baixe o zip na página de Releases, extraia e rode `install.cmd` como
administrador - sem compilador, sem build. Os mesmos scripts funcionam direto
da árvore de fontes depois do `build.cmd`.

```bat
install.cmd             (como administrador - os dois tipos)
install.cmd thumbnail   (só miniaturas)
install.cmd preview     (só o preview pane)

uninstall.cmd           (remove tudo)
uninstall.cmd preview   (remove só aquele tipo)
```

Cada opção vale para as três extensões. Instala em
`%ProgramFiles%\navossoc\OzShellExt`; acrescentar o outro handler depois é outro
`install.cmd`, sem rebuild.

Por baixo é `regsvr32 /n /i:<handler>`, que chama `DllInstall` em vez de
`DllRegisterServer`. Um `regsvr32 OzShellExt.dll` puro continua instalando tudo
que a DLL carrega.

Para ver o efeito, limpe o cache de miniaturas:

```bat
del /q "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db"
```

## Teste

```bat
test\build-test.cmd
cd build
test.exe thumb   some.ozt 256 200
test.exe preview some.ozt 500 400
```

O `test.exe` carrega a DLL sem registrar no COM, escolhe o CLSID pela extensão
do arquivo, exercita o handler e grava `out.bmp` para conferência. O modo
preview pinta via `WM_PRINTCLIENT`, então nenhuma janela precisa estar visível.

Aponte para arquivos reais do jogo - `Data\Effect`, `Data\Interface` e
`Data\World*` são cheios dos três formatos.

O fuzzer é um build à parte, porque o AddressSanitizer precisa ser compilado
junto:

```bat
test\build-fuzz.cmd
build\fuzz.exe "D:\MU\Data" 300 1
```

Ele varre o diretório atrás de `.ozb`, `.ozj` e `.ozt`, decodifica cada um
limpo e depois muta cada arquivo o número de vezes pedido. Qualquer acesso fora
dos limites aborta com um relatório do ASan apontando a linha.

## Release

```bat
package.cmd
```

Compila os dois tipos de handler e monta `dist\OzShellExt-<versão>-x64.zip` com
a DLL, os dois scripts de instalação, a licença e os READMEs, e imprime o SHA256
do artefato para as notas da release. A versão sai do `VER_STRING` em
`OzShellExt.rc`, então é o único lugar a incrementar.

## Licença

MIT - veja [LICENSE](LICENSE).
