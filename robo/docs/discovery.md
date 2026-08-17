# Discovery do servidor

## Objetivo

Permitir que o Cardputer encontre o Raspberry sem depender de IP fixo no firmware.

Desde que o Wi-Fi passou a ser configurável pelo teclado do Cardputer, a descoberta deixou de ser opcional: não há como saber o IP do servidor numa rede escolhida em tempo de execução. O `WS_URL` compilado virou último recurso.

O protocolo de áudio continua igual. O handshake continua compatível com o firmware antigo.

## Duas vias complementares

O firmware usa as duas ao mesmo tempo; uma não substitui a outra.

| Via | Quem inicia | Quando resolve |
|---|---|---|
| **Ativa** (`RDISCOVER`) | Cardputer | No boot e a cada ciclo de reconexão |
| **Passiva** (beacon) | Servidor | Quando o servidor sobe depois do Cardputer |

A via ativa é a confiável em hotspot. A passiva cobre o caso de o Raspberry ser ligado depois do Cardputer, sem esperar o ciclo de retry.

## UDP discovery recomendado

O firmware v2 envia um broadcast UDP na porta `8766`:

```text
RDISCOVER <nonce>
```

Exemplo:

```text
RDISCOVER 3fa91c22
```

O servidor responde por unicast:

```text
ROBOT <ws_url> <hmac>
```

Exemplo:

```text
ROBOT ws://192.168.0.50:8765 a1b2c3d4e5f60708
```

O `hmac` é:

```text
HMAC-SHA256(ROBO_DISCOVERY_TOKEN, nonce)[:16]
```

O Cardputer valida o HMAC com o mesmo segredo configurado em `ROBOT_SECRET`.

## UDP beacon

O servidor envia, periodicamente, um beacon JSON em broadcast:

```json
{"type":"ROBO_BEACON","server_id":"robo-main","proto":1,"ws_port":8765,"token_hash":"..."}
```

O `token_hash` é:

```text
sha256(ROBO_DISCOVERY_TOKEN)[:16]
```

Repare que é SHA-256 puro do segredo, e não o HMAC sobre o nonce usado no `RDISCOVER`. São mecanismos diferentes: o HMAC prova frescor de uma resposta específica, o `token_hash` só identifica que o servidor compartilha o segredo.

O beacon não carrega URL. O Cardputer monta o endereço com o **IP de origem do pacote** mais o `ws_port` anunciado.

O beacon vem **ligado por padrão** em `server/run_server.sh`. Para desligar:

```bash
export ROBO_DISCOVERY_BEACON_ENABLED="0"
```

### Para onde o beacon é transmitido

O beacon **não** usa `255.255.255.255`. Esse endereço depende da rota padrão da máquina, que num hotspot isolado simplesmente não existe: o envio falharia a cada intervalo e encheria o log de erro.

Em vez disso, o servidor calcula o **broadcast dirigido de cada interface IPv4 ativa** e transmite para todos. O broadcast da rede em `ROBO_DISCOVERY_HOTSPOT_CIDR` é acrescentado como rede de segurança, caso a enumeração de interfaces falhe mas o hotspot esteja de pé.

No boot, o console mostra os alvos escolhidos:

```text
Beacon UDP transmitindo para: 192.168.0.255, 10.42.0.255
```

A lista é recalculada a cada `ROBO_BEACON_REFRESH_S` segundos (padrão `10`), porque o hotspot pode subir depois do servidor e criar uma interface nova.

Isso tem um efeito colateral necessário: transmitindo para `10.42.0.255`, o kernel escolhe `10.42.0.1` como endereço de origem — que é exatamente o endereço que o Cardputer precisa ver, já que o beacon não carrega URL.

Se um alvo falhar, o erro é registrado **uma vez** e silenciado até voltar a funcionar. Sem isso, uma interface sem rota geraria uma linha de log por segundo, para sempre.

`255.255.255.255` continua existindo como último recurso, usado apenas quando nenhuma interface pôde ser determinada.

A enumeração de interfaces usa `ioctl` e é específica de Linux. Em outro sistema, ela devolve lista vazia e o servidor cai no CIDR do hotspot ou no último recurso.

## Fluxo no Cardputer

1. conectar no Wi-Fi (credencial vinda da NVS; ver `cardputer_setup.md`);
2. se houver URL manual configurada, usar e parar aqui;
3. abrir o socket UDP na porta `8766` e mantê-lo aberto;
4. gerar um `nonce`;
5. enviar `RDISCOVER <nonce>` por broadcast para a porta `8766`;
6. aguardar `ROBOT <ws_url> <hmac>`;
7. validar `hmac`;
8. usar `ws_url` para conectar no WebSocket.

Enquanto não houver WebSocket, o firmware também faz poll não bloqueante do socket a cada volta do loop, aceitando um beacon que chegue nesse meio tempo.

### Porta do socket no Cardputer

O Cardputer escuta na **mesma porta do servidor** (`8766`), não numa porta local separada. Isso é necessário porque é para a `8766` que o beacon em broadcast é enviado; a resposta unicast do `RDISCOVER` volta pela mesma via, então um socket só atende os dois casos.

Como o Cardputer passa a escutar a porta para onde ele mesmo transmite, ele recebe o eco do próprio broadcast. Pacotes que começam com `RDISCOVER` são descartados explicitamente.

## Prioridade de endereço no firmware

```text
URL manual da configuracao   (tela W -> Servidor)
  -> descoberta UDP          (RDISCOVER ou beacon)
    -> gateway DHCP          (ws://<gateway>:8765)
      -> WS_URL compilado    (ultimo recurso)
```

Uma falha de conexão esquece apenas o resultado da descoberta. A URL manual é escolha explícita do usuário e não é descartada sozinha.

## Ligação direta, sem roteador

O Cardputer **não** consegue fazer ad-hoc real (IBSS) com o Raspberry. O driver Wi-Fi do ESP32/ESP32-S3 implementa apenas STA, SoftAP e AP+STA; IBSS e Wi-Fi Direct não existem no stack da Espressif. Não é limitação do firmware deste projeto e não há como contornar por código.

A forma correta de ter os dois conversando sem roteador é o **Raspberry como ponto de acesso**, com o Cardputer entrando como estação normal:

```bash
nmcli device wifi hotspot ssid robo-net password <sua_senha>
```

O NetworkManager cria a rede em `10.42.0.1/24` com DHCP, que é exatamente o padrão de `ROBO_DISCOVERY_HOTSPOT_CIDR` e `ROBO_DISCOVERY_HOTSPOT_HOST`. Depois disso, basta escolher `robo-net` na tela `W` do Cardputer.

Pontos a considerar:

- O Pi 5 tem um rádio só. Enquanto ele for AP, perde o Wi-Fi como cliente; a Ethernet continua funcionando. Para este projeto isso é indiferente, porque Whisper, Piper e Ollama são todos locais.
- Nesse modo as duas vias de descoberta funcionam: o `RDISCOVER` responde por unicast com `10.42.0.1`, e o beacon transmite para `10.42.0.255` com o mesmo endereço de origem.

Alternativas descartadas:

- **Cardputer como AP**: funciona, mas o SoftAP do ESP32 é fraco e inverteria quem distribui o DHCP, exigindo reescrever a descoberta.
- **ESP-NOW**: é direto e dispensa AP, mas o Raspberry não fala ESP-NOW nativamente — precisaria de um segundo ESP32 como ponte. O limite de cerca de 250 bytes por pacote também é ruim para o áudio deste projeto.

## Handshake WebSocket

Depois de conectar no WebSocket, o Cardputer pode enviar:

```text
HELLO_CARDPUTER id=cardputer-01 proto=1 nonce=123456
```

O servidor responde:

```text
HELLO_ROBO id=robo-main proto=1 ok=1 nonce=123456
```

Se `ROBO_DISCOVERY_TOKEN` estiver configurado no servidor, a resposta inclui `proof`:

```text
HELLO_ROBO id=robo-main proto=1 ok=1 nonce=123456 proof=<hash_curto>
```

Nesta versão, o `proof` do handshake WebSocket é:

```text
sha256(nonce + ":" + ROBO_DISCOVERY_TOKEN)[:16]
```

## Variáveis do servidor

```bash
export ROBO_DISCOVERY_ENABLED="1"
export ROBO_DISCOVERY_BEACON_ENABLED="1"
export ROBO_DISCOVERY_PORT="8766"
export ROBO_DISCOVERY_INTERVAL_S="1.0"
export ROBO_DISCOVERY_SERVER_ID="robo-main"
export ROBO_DISCOVERY_TOKEN="TROQUE_ESTE_SEGREDO_COMPARTILHADO"
export ROBO_BEACON_REFRESH_S="10.0"
export ROBO_DISCOVERY_HOTSPOT_CIDR="10.42.0.0/24"
export ROBO_DISCOVERY_HOTSPOT_HOST="10.42.0.1"
```

Os dois últimos valem para as duas vias: o `RDISCOVER` os usa para escolher o endereço anunciado a um cliente do hotspot, e o beacon os usa para calcular um alvo de broadcast. O padrão `10.42.0.0/24` já corresponde à sub-rede que o NetworkManager cria no modo compartilhado.

Para usar o firmware v2 com HMAC, `ROBO_DISCOVERY_TOKEN` precisa ser igual ao `ROBOT_SECRET` do `.ino`. O `server/run_server.sh` já define o valor padrão acima, compatível com os firmwares do repositório. Se trocar o segredo no `.ino`, rode o servidor assim:

```bash
ROBO_DISCOVERY_TOKEN="meu_segredo" bash run_server.sh
```

O script `discovery_beacon.py` na raiz é um responder standalone/legado para testes. Não rode ele junto com `server/run_server.sh`, porque o servidor principal já escuta a porta UDP `8766`.

Para desativar:

```bash
export ROBO_DISCOVERY_ENABLED="0"
```

## Regras para o firmware

- Não fazer discovery durante gravação do microfone.
- Não fazer scan/reconexão agressiva enquanto `gravando == true`.
- Tratar discovery como etapa anterior ao WebSocket.
- Manter callback WebSocket leve.
- Só considerar o servidor validado depois de `HELLO_ROBO`.
- Fechar o socket UDP quando o Wi-Fi cair: ele morre junto com a interface.
- Descartar o eco do próprio broadcast.
