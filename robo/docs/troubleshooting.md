# Troubleshooting

## Problemas já descobertos

### O microfone para de funcionar com o pacote 3.3.7

- **Sintoma**: o áudio do microfone não entra corretamente ou fica quebrado.
- **Solução**: usar o pacote de placas M5Stack na versão `3.2.2`.
- **Observação**: não atualizar para `3.3.7` enquanto o microfone estiver sendo usado neste projeto.

### O notebook pode suspender quando o ímã encosta no sensor da tampa

- **Sintoma**: o sistema suspende ou entra em estado inesperado quando o Cardputer é fechado ou o ímã se aproxima do sensor.
- **Solução**: evitar que o ímã encoste no sensor durante testes e gravações.

### WebSocket com frame grande reinicia o Cardputer

- **Sintoma**: conexão cai ou o Cardputer reinicia ao receber áudio grande de uma vez.
- **Solução**: enviar áudio em chunks pequenos, de preferência `1024` bytes.

### SD não deve ser usado durante captura do microfone

- **Sintoma**: travamento, perda de amostras ou captura instável.
- **Solução**: durante a gravação, não escrever no SD. Salvar em arquivo somente depois da captura terminar.

### Microfone e speaker são half-duplex

- **Sintoma**: áudio de gravação e reprodução ao mesmo tempo causa falhas ou comportamento inconsistente.
- **Solução**: tratar o sistema como half-duplex; gravar e reproduzir em momentos separados.

### Cardputer precisa da chave em ON para carregar

- **Sintoma**: o dispositivo não carrega ou parece desligado durante o processo.
- **Solução**: deixar a chave física em `ON` para carregar normalmente.

### O Cardputer não acha o servidor

- **Sintoma**: rodapé mostra `Descoberta falhou` ou o WebSocket nunca conecta.
- **Causa mais comum**: `ROBOT_SECRET` no `.ino` diferente do `ROBO_DISCOVERY_TOKEN` do servidor. O HMAC é rejeitado em silêncio e o firmware cai no fallback.
- **Solução**: conferir os dois valores. Para testar sem regravar o firmware, rodar `ROBO_DISCOVERY_TOKEN="<segredo_do_ino>" bash run_server.sh`.
- **Outra causa**: rede com isolamento de cliente, que bloqueia broadcast entre dispositivos. Nesse caso usar `W` → `Servidor` e digitar a URL manualmente.

### O beacon não chega, mas o RDISCOVER funciona

- **Sintoma**: o servidor só é encontrado quando o Cardputer inicia a busca, nunca sozinho.
- **Causa**: o beacon sai para `255.255.255.255` pela rota padrão. Num Raspberry que é hotspot e também está numa LAN, ele pode sair pela interface errada.
- **Solução**: não há problema real; o `RDISCOVER` cobre o caso. Ver `discovery.md`.

### A senha do Wi-Fi não é aceita

- **Sintoma**: a tela mostra `Falhou. Senha errada?` e volta ao menu.
- **Observação**: nada é salvo quando a conexão falha, então a configuração anterior continua intacta.
- **Solução**: a senha aparece em texto claro na tela justamente para conferência. Verificar maiúsculas e o uso da tecla `Shift`.

### Não consigo voltar na tela de configuração

- **Sintoma**: `Ctrl` não sai da tela.
- **Solução**: apagar tudo com `Del`; com o campo vazio, `Del` volta ao menu. É a rota de fuga garantida.

### A boca não acompanha a fala

- **Sintoma**: a boca abre e fecha, mas sem relação com o áudio.
- **Solução**: calibrar `VIS_ZCR_PCT` e os cortes de nível em `visemeFromWindow`. Eles dependem do volume real de saída da voz Piper em uso. Ver `rosto.md`.

### O rosto fica sempre com a mesma emoção

- **Sintoma**: o humor não muda entre interações.
- **Verificar**: o console do servidor imprime `Sentimento: <HUMOR> (val=... aro=... conf=... hits=...)` a cada fala. Se `hits=0`, nenhuma palavra do léxico foi reconhecida e o resultado é `NEUTRAL` por padrão. Ver `sentimento.md`.

## Regra prática

Se algo estranho acontecer no fluxo de áudio, primeiro verificar:

1. versão do pacote M5Stack;
2. tamanho dos frames WebSocket;
3. estado do SD;
4. modo half-duplex;
5. posição do sensor da tampa;
6. chave física em `ON`.

Se o problema for de rede, verificar:

1. segredo compartilhado igual nos dois lados;
2. isolamento de cliente na rede;
3. credencial salva na NVS (tecla `W` mostra a rede atual);
4. URL manual do servidor sobrepondo a descoberta.
