# bancada

Codinome provisório de um jogo original sobre construir um computador, do LED aos programas em C.

## Como jogar

Não precisa instalar nem compilar nada. Os executáveis prontos ficam na pasta [`jogar`](jogar).

**Windows 10 ou 11 (64 bits):** baixe [`jogar/windows/bancada.exe`](jogar/windows/bancada.exe) e dê dois cliques.
O programa ainda não tem assinatura digital, então o Windows pode avisar "O Windows protegeu o computador". Nesse caso, clique em **Mais informações** e depois em **Executar assim mesmo**. Em PCs com o Controle Inteligente de Aplicativos ligado, o Windows bloqueia o programa até ele ser assinado.

**Linux (64 bits, Ubuntu 22.04 ou mais novo, Debian 12, Fedora 36, Mint 21 ou equivalente):** baixe [`jogar/linux/bancada`](jogar/linux/bancada) e, na pasta onde ele estiver, rode:

```bash
chmod +x bancada
./bancada
```

O `chmod` só é preciso na primeira vez, porque o GitHub não guarda a permissão de execução.

Para levar o jogo num pendrive com os seus circuitos, crie uma pasta `dados` ao lado do executável: tudo o que o jogo salvar fica nela.

## Estado atual

- **Núcleo lógico headless** (`src/sim`): netlist, compilação com detecção de laços, interpretador de referência e simulador orientado a eventos.
- **Editor headless** (`src/editor`): placa, fios, roteamento, desfazer e ferramentas, testáveis sem janela.
- **Sandbox** (`src/app`): janela SDL3 que desenha a sessão e traduz eventos.
- **Arquivos** (`src/io`): projetos, autosave, migrações e perfis locais.
- **Biblioteca** (`src/editor/boxes.cpp`): portas e caixas prontas de 1 a 64 bits, de somador a ULA, registrador, contador, RAM e ROM, e também buffer, paridade par e ímpar, seletor de bits, codificador de prioridade, multiplicador, divisor, negador, contador de bits, localizador de bit, decodificador para 7 segmentos e gerador aleatório. Entre as entradas e saídas, há também o botão, que vale 1 só enquanto está apertado, o display de 7 segmentos, o teclado (que recebe as teclas do teclado de verdade enquanto está selecionado), a matriz de LEDs 8×8 e o terminal de texto.
- **Barramentos**: fios de 1 a 64 bits, com separador, juntador e constante.
- **Clock**: rodar, pausar, passo a passo e velocidade ajustável.
- **Chips**: qualquer circuito do projeto vira uma caixa, inclusive dentro de outros chips.
- **Tabela-verdade** (`src/editor/truth_table.cpp`): recalculada a cada mudança no circuito.
- **Rótulos**: dois rótulos com o mesmo nome são o mesmo fio, sem fio entre eles.
- **Computador de 16 bits** (`src/editor/computer16.cpp`, `src/asm`): CPU feita só de peças do editor, em cinco blocos que se abrem, com assembler, editor de código e painel que mostra o programa, o caminho de dados e os registradores.
- **Eletricidade** (`src/electric`, `src/editor/analog.cpp`): bateria (desenhada como uma bateria de verdade, e também como Fonte, o símbolo), terra, resistor, diodo, LED (desenhado como um LED de verdade, e também como Diodo LED, o símbolo), chave, capacitor e transistores NPN, PNP, NMOS e PMOS, resolvidos como no SPICE. O valor de resistores, baterias e capacitores é digitado só como número, com a unidade fixa ao lado. Corrente e tensão aparecem nos fios e no mouse. Transistores dentro da lógica viram tabelas exatas, inclusive circuitos que lembram.
- **Visual**: fonte própria embutida (Ubuntu, com acentos), linhas suaves, ícones e biblioteca em pastas, com um desenho de cada peça. A tela inicial tem um logotipo de chip e, ao fundo, circuitos de todos os atos desenhados ao vivo, meio transparentes e flutuando devagar.
- **Raio-X** (`src/editor/computer_view.cpp`): a instrução do computador de 16 bits passando pelo caminho de dados em seis etapas, com o valor de cada fio. Cada tipo de instrução tem uma cor, e blocos com os valores andam pelas trilhas. O Painel mostra o mesmo diagrama. Cada peça tem a forma do que é: a ULA e os dois somadores (PC + 1 e Alvo do desvio, com a conta PC+1+imm) no formato de ULA, e os quatro multiplexadores (A e B na frente da ULA, o que volta para os registradores e o Próximo PC) no formato de MUX, cada um com as duas entradas e a linha de seleção vinda do Controle. O Z da ULA desce até o Controle, que manda o MUX do Próximo PC desviar ou não. Os testes conferem cada valor contra o circuito.
- **Depurador**: à esquerda do Painel e do Raio-X, como num IDE: o programa com a linha atual e pontos de parada, os registradores (os que mudaram ficam amarelos) e a memória, com um botão que alterna entre Rodar e Pausar (e vira De novo quando o programa chega ao HLT), Instrução, Etapa e Zerar; o ▶ da barra de cima e a barra de espaço fazem o mesmo. Comentários longos quebram em várias linhas.
- **Exemplos** (`src/editor/gallery.cpp`, `src/editor/gallery_circuits.cpp`): cada peça da biblioteca com o seu nome (portas lógicas, contas de 1 e de N bits, memória), em versões que se trocam acima da placa: a peça pronta, feita de portas lógicas, feita de transistores (lógica resistor-transistor) ou de blocos menores. Cada exemplo explica o que é e como cada versão funciona, e um teste confere que todas as versões de um exemplo dão a mesma tabela-verdade. As portas NÃO, E, OU e XOR têm também a versão Só NAND. Também há circuitos de eletrônica (LED e resistor, fonte e terra, transistor como chave, chaves em série e em paralelo, pisca-pisca), multiplexadores 2:1 e 4:1, latch D, flip-flop D feito de dois latches, dez programas para o computador (soma, multiplicação, Fibonacci, potências de 2, contagem regressiva, soma de uma lista, sub-rotina, contador, um programa escrito em binário e um que espera pelas chaves) e dois com um sistema operacional pequeno (chamadas de sistema e duas tarefas). Toda aula tem pelo menos um exemplo do mesmo assunto.
- **Programar**: um computador só seu, separado dos exemplos, para escrever e testar os próprios programas. Ele fica salvo e reabre como estava.
- **Idiomas** (`src/i18n`): português e inglês, escolhidos no canto da tela inicial. O código é escrito em português, e o catálogo em inglês traduz cada texto; os testes conferem que as aulas, os exemplos e as peças estão todos traduzidos.
- **Aulas** (`src/editor/lessons.cpp`): 50 exercícios em dez atos: eletricidade, transistores, portas lógicas, aritmética, memória (latch, flip-flop, registrador, contador), linguagem de máquina (três programas escritos à mão em binário, só com .word), programação no computador de 16 bits (do primeiro programa às sub-rotinas) sistema operacional (entrada e saída, espera ocupada, chamadas de sistema, duas tarefas revezando a CPU e, por fim, o jogador escreve a troca de tarefas), mais peças (paridade, multiplexador, codificador de prioridade, números negativos e multiplicação) e entrada e saída (display de 7 segmentos, contador com botão, números aleatórios e teclado com terminal). Também há aulas próprias para o decodificador, o multiplexador 4:1, o subtrator, o comparador e a ULA de 1 bit. As saídas são LEDs de verdade, e toda aula oferece na biblioteca a bateria e a fonte, terra, resistor, LED e diodo LED. Cada aula se verifica sozinha (as de programação rodam o programa no próprio circuito), e os testes resolvem todas. O progresso pode ser zerado na lista de aulas.
- **Curso** (`src/editor/course.cpp`): cada ato se divide em aulas teóricas, com texto, tabelas e circuitos de verdade desenhados ao vivo, e cada aula é seguida dos seus exercícios. O ato seguinte fica com cadeado até os exercícios do atual estarem feitos. Os dez atos têm 25 aulas teóricas: corrente, bateria e LED; resistor, lei de Ohm, chave e capacitor; chaves em série e em paralelo; o transistor; portas feitas de transistores; as portas lógicas e a álgebra booleana; NAND universal e XOR; decodificador e multiplexador; números binários e a soma; subtração, complemento de dois, comparador e ULA; latches; clock, flip-flop, registrador e contador; a CPU por dentro e o ciclo de instrução; os formatos das instruções e o hexadecimal; desvios, saltos e o montador. Nas aulas, um bloco mostra os 16 bits de uma instrução nas cores do Raio-X, e o montador aceita _ entre os dígitos (0b0010_001_000000111). Na programação e no sistema operacional: assembly, laços e memória, sub-rotinas, entrada e saída com espera, chamadas de sistema e troca de tarefas; nas peças finais: paridade, multiplexador e prioridade, negativos e multiplicação, display de 7 segmentos, botões e números ao acaso, teclado e terminal. As aulas de programação trazem trechos de código coloridos como no editor.
- **Raio-X e Ajuda do assembly**: o Raio-X mostra os 16 bits da instrução, campo por campo e em cores, e os valores que andam nos fios usam as cores dos campos de onde vêm. O editor de programas tem um botão Ajuda com todas as instruções, o que cada uma faz, um exemplo e como seus 16 bits se dividem.
- Ainda não existem puzzles, laboratório nem JIT.

## Usar o editor

| Ação | Como |
|---|---|
| Colocar peça | Arraste da biblioteca para a placa, ou clique nela e depois na placa; `R` gira; `Esc` termina |
| Ajustar peça | Selecione e use o painel da direita, ou `+` e `-` para entradas e `[` e `]` para tamanho |
| Ligar fio | Clique em um pino e depois no destino, ou arraste de pino a pino |
| Fixar o caminho | Durante o fio, clique no vazio para fixar um ponto; `Shift` força um trajeto em L |
| Derivar um fio | Arraste a partir do meio de um fio, ou termine um fio em cima de outro |
| Ponta solta | `Enter` encerra o fio no cursor; clique na ponta para continuar depois |
| Selecionar | Clique na peça ou no fio, ou arraste no vazio; `Shift` adiciona; `Ctrl+A` seleciona tudo |
| Mover e girar | Arraste a peça; `R` ou o botão Girar gira a seleção |
| Apagar | Botão Apagar, `Delete` ou `Backspace` |
| Copiar e colar | `Ctrl+C`, `Ctrl+X`, `Ctrl+V` ou os botões; a cópia segue o cursor e um clique a posiciona |
| Desfazer | `Ctrl+Z`, e `Ctrl+Y` ou `Ctrl+Shift+Z` para refazer |
| Navegar | Roda do mouse dá zoom; botão direito ou do meio move a vista; `F` enquadra |
| Simular | Clique em uma entrada para ligar ou desligar; em entradas de vários bits, clique no bit; `T` mostra ou esconde a tabela |
| Bits | Selecione a peça e use Bits no painel, ou `,` e `.`; um fio entre larguras diferentes fica vermelho |
| Clock | Coloque um Clock e use Rodar, Passo e Zerar na barra; `Espaco` roda ou pausa, `N` dá meio ciclo |
| Chips | Em Circuitos do projeto, clique em + Novo chip; suas Entradas e saídas viram os pinos; arraste o nome para a placa; clique no nome ou dê duplo clique no chip para abrir |
| ROM e constante | Selecione e use Editar conteúdo ou Editar valor, em hexadecimal |
| Programa | Selecione uma ROM de 16 bits e use Editar programa; `Ctrl+Enter` monta e grava |
| Computador | Em Exemplos, abra Computador pronto ou um dos programas e use Painel na barra para ver a CPU trabalhando |
| Seu programa | Na tela inicial, clique em Programar: o editor de código abre num computador só seu |
| Depurador | No Painel ou no Raio-X, clique numa linha do programa para parar nela; Continuar, Instrução e Etapa avançam |
| Idioma | Na tela inicial, Português ou English, no canto de cima |
| Rótulos | Clique num rótulo: todos os rótulos com o mesmo nome ficam em laranja, com os fios do sinal |
| Versões de um exemplo | Os botões à direita, acima da placa: Peça pronta, Portas lógicas, Transistores ou Blocos |
| Raio-X | Com o computador na placa, use Raio-X na barra: as etapas avançam sozinhas; clique numa etapa para ficar nela; Próxima instrução avança |
| Aulas | Na tela inicial, clique em Aulas, escolha o ato à esquerda e leia a aula antes dos exercícios; Verificar diz o que falta, Dica ajuda, e o exercício se completa sozinho quando o circuito funciona |
| Tela inicial | O botão da casinha, no começo da barra; o circuito fica salvo e Continuar o traz de volta |
| Eletrônica | Arraste bateria, resistores e transistores; clique na chave para fechá-la; passe o mouse para medir; Editar valor aceita `4k7`, `220`, `10u` |
| Salvar | `Ctrl+S`; `Ctrl+Shift+S` salva com outro nome |
| Abrir | `Ctrl+O`, ou solte um arquivo `.bancada` na janela |
| Novo projeto | `Ctrl+N` |
| Perfis | `P` abre a lista; digite um nome e tecle `Enter` para criar um novo |
| Atalhos | `F1` |

O jogo salva sozinho a cada 30 segundos e ao fechar, em um arquivo separado do projeto, e reabre de onde parou.

## Compilar

### Windows (terminal MSYS2 UCRT64)

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-xxhash mingw-w64-ucrt-x86_64-nlohmann-json
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

O `.exe` sai com ícone, nome e versão. O nome que aparece como autor nas Propriedades vem de `-DBANCADA_AUTHOR="Seu Nome"` (sem acentos). O ícone é gerado por `python3 tools/make_icon.py`.

#### Assinatura e bloqueio do Windows

O Controle Inteligente de Aplicativos do Windows 11 bloqueia todo programa sem assinatura que a Microsoft ainda não conhece. Ele não acusa vírus: o bloqueio aparece no log `Microsoft-Windows-CodeIntegrity/Operational`, eventos 3033 e 3077. O SmartScreen faz algo parecido com programas baixados da internet.

A solução é assinar o `.exe` com um certificado de assinatura de código de uma autoridade reconhecida pela Microsoft. Certificado autoassinado não serve. Com o certificado em mãos:

```powershell
powershell -ExecutionPolicy Bypass -File tools\assinar_windows.ps1 -Impressao <SHA-1 do certificado>
powershell -ExecutionPolicy Bypass -File tools\assinar_windows.ps1 -Conferir
```

O script também aceita o Artifact Signing da Microsoft (`-Metadados metadata.json -Dlib Azure.CodeSigning.Dlib.dll`) e precisa do `signtool.exe` do Windows SDK. Mesmo assinado, um programa novo pode receber aviso do SmartScreen até ganhar reputação.

### Linux (Ubuntu 22.04)

Sem SDL3 instalado, o CMake baixa e compila a versão fixada em `CMakeLists.txt`, que precisa destes pacotes:

```bash
sudo apt install build-essential cmake ninja-build git pkg-config libzstd-dev libxxhash-dev nlohmann-json3-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libgbm-dev libdrm-dev libasound2-dev libpulse-dev libdbus-1-dev libudev-dev
```

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Para compilar só o núcleo, sem janela, use `-DBANCADA_APP=OFF`.
Para conferir o simulador elétrico contra o ngspice: `python3 tools/compare_spice.py build/bancada_electric_cli`.
Para detectar erros de memória no Linux, use `-DBANCADA_SANITIZE=ON`.

## Modo portátil

Crie uma pasta `dados` ao lado do executável. Se ela existir, tudo o que o jogo salvar fica nela.
Sem essa pasta, ele usa a pasta do usuário no sistema.

## Formato dos projetos

Um `.bancada` tem um cabeçalho e blocos independentes, cada um comprimido com zstd e verificado com XXH3.
O conteúdo é JSON e guarda só o que o jogador fez. Netlists e código compilado nunca são salvos.

- Todo arquivo é tratado como entrada não confiável e validado por inteiro antes de virar uma placa.
- Blocos desconhecidos são ignorados, então versões antigas do jogo abrem arquivos com blocos novos.
- Mudar o formato exige subir `kFormatVersion` e acrescentar um passo de migração em `src/io/project.cpp`.
- Os arquivos em `tests/fixtures` (da versão 1 à 10 do formato) precisam abrir para sempre. Nunca regenere um deles para um teste passar.
- As hashes detectam danos, não adulteração: isto não é uma medida de segurança.

## Regras do editor

- O desenho de um fio nunca decide o que ele liga: só pinos e juntas ligam.
- Cruzar ou passar por cima de outro fio nunca cria conexão. Um ponto marca toda ligação real.
- O roteador só sugere caminhos. Cliques do jogador têm prioridade e sobrevivem na placa.
- A placa é a fonte da verdade. Netlist e simulador são derivados e descartáveis.
- Ligar uma entrada não é uma edição e não entra no histórico.

## Regras do simulador

- NAND é a única porta primitiva. As peças AND, OR, XOR, NOR, XNOR e NOT viram NANDs ao entrar no simulador (`src/sim/gates.hpp`).
- Flip-flops prontos e palavras de memória são nós próprios do simulador, porque uma RAM real tem milhões de bits.
- Chips são achatados ao compilar: um chip custa o que seu conteúdo custa, sem nada a mais.
- Editar o circuito recompila tudo, o que zera registradores e RAM.
- Portas não têm atraso. `settle()` leva o circuito ao repouso, e laços que não param são relatados.
- Barramentos têm de 1 a 64 bits.
- Com as entradas iguais, o resultado é sempre o mesmo, inclusive em condições de corrida.
- Flip-flops feitos de NAND acordam em um estado não escolhido: circuitos precisam de reset.
- O jogo roda no simulador orientado a eventos, que só reavalia o que pode ter mudado.
- O interpretador é a referência: `tests/differential.cpp` exige que todo backend dê o mesmo resultado em circuitos aleatórios.
