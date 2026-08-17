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

### Limitação do beacon em hotspot

O beacon é enviado para `255.255.255.255`, que sai pela **rota padrão** da máquina. Se o Raspberry estiver como hotspot e também conectado a uma LAN, o beacon pode sair pela interface errada, ou chegar com o IP de origem da LAN em vez do `10.42.0.1`.

O caminho `RDISCOVER` não tem esse problema: ele responde por unicast e escolhe o endereço anunciado com `client_in_hotspot()` / `advertised_host_for()`, olhando de onde veio a requisição.

Conclusão prática: **em hotspot, quem resolve é o `RDISCOVER`**. O beacon é um complemento para rede plana comum. Se for necessário beacon confiável em hotspot, ele precisa passar a transmitir por interface, usando o endereço de broadcast de cada uma.

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
```

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
