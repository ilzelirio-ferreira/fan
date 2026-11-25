# Ventilador de três velocidades com ESP32-S2 e Matter

Firmware baseado no doorBell. Um toque no botão alterna entre **desligado → V1 → V2 → V3 → desligado**. O dispositivo usa o tipo Matter **Fan** (`0x002B`), com modos baixa, média e alta e ajuste de velocidade de 1 a 3. Os comandos Matter e o botão atuam no mesmo estado. Ao reiniciar, o ventilador começa desligado.

## Ligações no ESP32-S2

| Sinal | GPIO | Lógica |
| --- | --- | --- |
| Botão | 33 | Contato normalmente aberto entre GPIO e GND, com pull-up interno |
| Relé A, bit mais significativo | 35 | Ativo em nível alto |
| Relé B, bit menos significativo | 34 | Ativo em nível alto |

| Velocidade | Relé A | Relé B | Percentual de comando Matter |
| --- | --- | --- | --- |
| Desligado (V0) | 0 | 0 | 0% |
| Baixa (V1) | 0 | 1 | 1–33% |
| Média (V2) | 1 | 0 | 34–66% |
| Alta (V3) | 1 | 1 | 67–100% |

Use entradas de módulos de relé compatíveis com lógica de 3,3 V e mantenha as entradas em nível baixo durante o reset com pull-down externo. A tabela pressupõe um circuito seletor compatível com as quatro combinações e seus estados transitórios. O firmware aguarda pelo menos 30 ms entre desativar um relé e ativar o outro em uma troca como `01 → 10`. Valide a tabela e os tempos com a etapa de potência desconectada do motor.

## Wi-Fi e Alexa

1. Grave o firmware para ESP32-S2, com flash de 4 MB e PSRAM conforme a base doorBell.
2. No primeiro uso, conecte-se à rede de configuração `fan-XXXXXX`, com senha inicial `configurar123`, e abra `http://192.168.4.1`. Use a mesma senha para entrar no painel e informe o Wi-Fi de 2,4 GHz.
3. Depois de conectar a placa ao Wi-Fi, acesse o painel pelo IP da placa na rede local e abra o pareamento Matter. Use o QR code ou código numérico com a Alexa e um Echo compatível na mesma rede. O ESP32-S2 usa descoberta Matter pela rede IP.
4. No app Alexa, verifique se o dispositivo foi identificado como ventilador. Experimente “Alexa, ajuste o ventilador para 33 por cento”, “66 por cento” e “100 por cento”. O nome padrão é **Ventilador**.
5. Verifique se o botão funciona com o Wi-Fi desconectado e se a Alexa acompanha as mudanças locais quando a rede está disponível.

## Compilação e gravação

O GitHub Actions compila para `esp32s2` com a mesma imagem ESP-Matter usada na compilação do doorBell. O artefato **fan-firmware** inclui o aplicativo, bootloader, tabela de partições e argumentos de gravação. Na primeira gravação, extraia o artefato e execute no diretório que contém `flash_args`, substituindo `COM5` pela porta da placa:

```sh
python -m esptool --chip esp32s2 --port COM5 write_flash @flash_args
```

A release disponibiliza `fan.bin` para atualizações OTA pelo painel do ventilador.
