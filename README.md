# Followup

O Followup é um lugar para guardar seus pensamentos: uma ideia, uma tarefa ou só uma anotação. Grave o que está na sua cabeça no momento em que a ideia surge, antes que ela escape, e o Followup ajuda a organizar depois. Com o Gemini, as gravações são transcritas e resumidas automaticamente. Tudo fica guardado no seu cartão SD.

Ele roda na [Waveshare ESP32-S3-ePaper-3.97](https://docs.waveshare.com/ESP32-S3-ePaper-3.97). Assim, seus pensamentos ficam numa tela silenciosa e sempre ligada, que você pode deixar em qualquer lugar: um lembrete constante e discreto, em vez de mais uma notificação perdida no celular. A mesma tela e-paper também serve para **ler livros** em EPUB.

## Em uma frase

**O Followup é um caderno de voz numa tela e-paper sempre ligada: grave ideias, tarefas e notas na hora, deixe o Gemini transcrever e resumir, mantenha à vista o que importa como lembretes fixos e leia seus livros na mesma tela.**

## Para que serve

- Registrar uma ideia por voz no instante em que ela aparece, antes de esquecer
- Anotar tarefas e lembretes rápidos sem usar as mãos, no meio de outra atividade
- Manter à vista, na mesa, na geladeira ou na parede, um pequeno conjunto de coisas para acompanhar
- Rever ideias antigas depois e decidir quais ainda valem a pena
- Ler livros numa tela confortável, sem brilho e sem distrações
- Para quem quer seus pensamentos organizados sem viver dentro de mais um aplicativo no celular

## Principais recursos

### 1. Grave no momento da ideia

Segure o BOOT e fale. Cada registro começa como uma gravação de voz (até 60 segundos), marcada como **Ideia**, **Tarefa** ou **Nota**, para você guardar o pensamento na hora sem parar para digitar.

### 2. Transcrição e resumo com o Gemini

Depois que a gravação é salva, o Gemini transcreve o áudio no idioma falado e resume o conteúdo em português. Um áudio solto vira um texto legível e um resumo curto, fácil de bater o olho.

É preciso uma chave de API do Gemini, que você cria no [Google AI Studio](https://aistudio.google.com/apikey). Dá para começar no plano gratuito, dentro dos limites dele, ou usar uma conta paga para transcrever sem esses limites. Quando o plano gratuito devolve "limite excedido" por alguns segundos, o Followup tenta de novo sozinho.

### 3. Tudo guardado no seu cartão SD

Gravações, transcrições e resumos ficam no cartão SD do próprio aparelho. Seus pensamentos continuam com você, no seu armazenamento.

### 4. Checar a vibe das ideias

Nem toda ideia envelhece bem. Revise cada uma e decida se ela ainda faz sentido ou se é melhor descartar e seguir em frente com a cabeça leve.

### 5. Acompanhe tarefas e notas

Marque uma tarefa ou nota para acompanhar e ela não sai do seu radar. O Followup ajuda você a manter o foco no que realmente precisa ser feito.

### 6. Seus acompanhamentos como lembretes fixos

Fixe seus acompanhamentos na tela e-paper. Como a tela fica sempre ligada e gasta pouca energia, eles ficam à sua frente como um lembrete constante e discreto.

### 7. Livros

Copie arquivos `.epub` para a pasta `books` do cartão SD e abra **Livros** no menu. A biblioteca mostra capa, título, autor e quanto você já leu de cada livro. No leitor:

- três tamanhos de letra;
- pular para o próximo capítulo ou para o anterior;
- a página em que você parou fica salva em cada livro e continua certa mesmo se você mudar o tamanho da letra.

## Aplicações típicas

| Aplicação | Descrição |
| --- | --- |
| Ideia | Registre uma faísca por voz e revise depois com o "Checar vibe" |
| Tarefa | Grave uma tarefa sem usar as mãos e acompanhe até concluir |
| Nota | Guarde um pensamento ou lembrete rápido, transcrito e resumido |
| Acompanhamento | Marque o que importa para não sair da sua cabeça |
| Lembretes fixos | Mostre seus acompanhamentos na tela e-paper, sempre visíveis |
| Resumos | Deixe o Gemini condensar gravações longas num resumo rápido de ler |
| Livros | Leia EPUBs na tela e-paper, retomando de onde parou |

## Especificações

O Followup roda na [Waveshare ESP32-S3-ePaper-3.97](https://docs.waveshare.com/ESP32-S3-ePaper-3.97).

| Item | Informação |
| --- | --- |
| Nome do produto | Followup (na ESP32-S3-ePaper-3.97) |
| Tipo | Caderno de voz e leitor de livros num terminal e-paper |
| MCU | ESP32-S3R8, Xtensa LX7 dual-core até 240 MHz |
| Memória | 8 MB de PSRAM, 16 MB de flash |
| Tela | E-paper preto e branco de 3,97", 800 × 480, controlador SSD1677 |
| Interação | Só botões; esta placa não tem tela sensível ao toque (veja [Controles](#controles)) |
| Conectividade | Wi-Fi 2,4 GHz (802.11 b/g/n), Bluetooth 5 (LE) |
| Áudio | Codec ES8311, microfone integrado, amplificador NS4150B, conector para alto-falante |
| Sensores | IMU de 6 eixos QMI8658, relógio de tempo real PCF85063 |
| Energia | PMIC AXP2101, bateria de lítio de 3,7 V (conector MX1.25), carregamento por USB-C |
| Armazenamento | Cartão microSD (gravações, transcrições, resumos e livros) |
| IA | Gemini (na nuvem) para transcrição e resumo, via Wi-Fi |

A placa também tem um sensor de temperatura e umidade SHTC3 no mesmo barramento I2C. O Followup ainda não usa esse sensor.

## Controles

O Followup é controlado inteiramente pelos três controles físicos: a alavanca, o botão BOOT e o botão PWR.

| Controle | Ação |
| --- | --- |
| Alavanca para cima / para baixo | Move a seleção; segure para repetir |
| Alavanca para baixo, segurada | Sai de uma lista ou de um cartão em que você entrou |
| Apertar a alavanca | Seleciona / confirma |
| BOOT, toque | Seleciona / confirma |
| BOOT, segurado | Grava: começa quando você segura e para quando solta |
| PWR, toque | Bloqueia ou desbloqueia a tela |
| PWR, segurado ~1 s | Abre a confirmação de desligar |
| PWR, segurado 6 s | Desliga direto pelo PMIC (desligamento forçado) |

Gravar é exclusivo do BOOT, então nenhum outro controle começa ou para uma gravação por acidente. Segurar o PWR por 6 segundos passa por cima do firmware e sempre corta a energia.

No leitor de livros:

| Controle | Ação |
| --- | --- |
| Alavanca para baixo / para cima, toque curto | Próxima página / página anterior |
| Alavanca para baixo, segurada | Volta para a lista de livros |
| Apertar a alavanca ou tocar no BOOT | Abre o menu: tamanho da letra, capítulos, voltar |

## Primeiros passos

1. **Cartão SD:** use um microSD formatado em FAT32. O Followup cria as pastas de que precisa (`recordings`, `todos`, `summaries`, `books`...). Também dá para formatar pelo próprio aparelho em **Configurações → Formatar cartão SD**.
2. **Wi-Fi e Gemini:** no primeiro uso, o aparelho cria uma rede Wi-Fi chamada `Followup-XXXXXX`. Conecte o celular nela e abra **http://192.168.4.1** para escolher o seu Wi-Fi, colar a chave do Gemini e ajustar o fuso horário. O padrão é Brasília; também há Amazonas, Acre e Fernando de Noronha, e a hora sincroniza pelo `pool.ntp.br`. Para voltar ao portal depois, ative **Configurações → Ponto de acesso**.
3. **Livros:** ative **Configurações → Ativar OTG** com o cabo USB ligado no computador. O cartão SD aparece como um pendrive; copie os arquivos `.epub` para a pasta `books` e desative o OTG. Também dá para usar um leitor de cartão no computador.

Sobre os livros:
- Na primeira vez que você abre a biblioteca, o Followup lê cada livro e prepara a capa, o que leva alguns segundos por livro. Depois, fica guardado em cache e abre na hora.
- EPUBs com DRM (protegidos contra cópia, como os comprados em algumas lojas) não abrem.
- As capas precisam ser JPEG. Quando a capa é PNG, aparece uma capa genérica no lugar.
- Livros de domínio público em português você encontra, por exemplo, no [Project Gutenberg](https://www.gutenberg.org/browse/languages/pt).

## Compilar e gravar

O firmware usa o **ESP-IDF v5.5.4**. Com o ambiente do ESP-IDF ativado:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash
```

No Windows, a placa aparece como uma porta COM ("USB JTAG/serial debug unit"); no Linux e no macOS, como `/dev/ttyACM0` ou `/dev/cu.usbmodem*`. Se a porta não aparecer, segure o **BOOT** enquanto conecta o cabo USB para entrar no modo de gravação.

Outros detalhes úteis para quem mexe no firmware:

- **Configuração de compilação:** fica em `sdkconfig.defaults`. O `sdkconfig` gerado não vai para o git, porque pode conter segredos como a chave do Gemini.
- **Duração da gravação:** o limite fica no Kconfig, em `FOLLOWUP_MAX_RECORDING_SECONDS`.
- **RAM interna:** o recurso mais apertado da placa, com cerca de 20 KB livres no pico de uso. O log serial mostra a cada minuto a memória livre e o uso de pilha de cada tarefa; confira depois de qualquer mudança que acrescente tarefas ou buffers.
- **Largura dos textos:** `scripts/measure_text.py` mede a largura de um texto em pixels com as fontes do próprio firmware. Use antes de trocar um texto que fica num botão ou rótulo de largura fixa.
- **Fontes e imagens:** para regerar, use `scripts/generate_epaper_fonts.py` e `scripts/generate_epaper_project_assets.py` (veja `docs/asset-generation.md`).

## Em resumo

O valor do Followup é ter um lugar silencioso e sempre visível para guardar seus pensamentos e manter à frente o que importa. Em vez de perder uma ideia num aplicativo de notas esquecido ou enterrar uma tarefa no meio das notificações, você fala na hora. O Gemini transforma a fala em texto limpo e num resumo, e tudo fica guardado, com privacidade, no seu cartão SD.

As ideias passam pelo "Checar vibe", e você só leva adiante o que ainda faz sentido. Tarefas e notas viram acompanhamentos, para você não perder o rumo. O que mais importa fica fixo na tela e-paper: um lembrete constante e discreto do que vem a seguir. E, quando sobrar um tempo, a mesma tela vira um leitor de livros.
