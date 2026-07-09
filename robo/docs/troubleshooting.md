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

## Regra prática

Se algo estranho acontecer no fluxo de áudio, primeiro verificar:

1. versão do pacote M5Stack;
2. tamanho dos frames WebSocket;
3. estado do SD;
4. modo half-duplex;
5. posição do sensor da tampa;
6. chave física em `ON`.
