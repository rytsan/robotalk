# Discovery do servidor

## Objetivo

Permitir que o Cardputer encontre o Raspberry sem depender de IP fixo no firmware.

O protocolo atual de áudio continua igual. Discovery e handshake são opcionais e compatíveis com o firmware antigo.

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

## UDP beacon compatível

O servidor pode enviar, periodicamente, um beacon JSON de compatibilidade:

```json
{"type":"ROBO_BEACON","server_id":"robo-main","proto":1,"ws_port":8765,"token_hash":""}
```

O firmware v2 atual não depende desse beacon. Ele usa o fluxo `RDISCOVER`.
Por isso, o beacon fica desativado por padrão para evitar erro de broadcast em redes
sem rota padrão, como hotspot local isolado.

## Fluxo no Cardputer

1. conectar no Wi-Fi;
2. gerar um `nonce`;
3. enviar `RDISCOVER <nonce>` por broadcast para a porta `8766`;
4. aguardar `ROBOT <ws_url> <hmac>`;
5. validar `hmac`;
6. usar `ws_url` para conectar no WebSocket.

Se não receber resposta válida, deve manter fallback para o `WS_URL` fixo.

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
export ROBO_DISCOVERY_BEACON_ENABLED="0"
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
- Manter fallback para IP fixo.
- Tratar discovery como etapa anterior ao WebSocket.
- Manter callback WebSocket leve.
- Só considerar o servidor validado depois de `HELLO_ROBO`.
