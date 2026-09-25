# Configuração do canal ThingSpeak

Passo a passo para criar o canal que recebe os dados do ESP32 e deixá-lo com **visualização pública**.

## 1. Criar o canal

1. Entre em <https://thingspeak.mathworks.com> (conta MathWorks gratuita).
2. **Channels → My Channels → New Channel**.
3. Preencha:
   - **Name:** `Pelé Academia · Monitor Ambiental de Treino`
   - **Description:** `Temperatura e umidade do local de treino, processadas na borda por um ESP32 (média móvel, índice de calor, WBGT estimado e classificação de risco).`
   - Marque os 8 campos com estes nomes:

| Campo | Nome |
|---|---|
| Field 1 | Temperatura (°C) |
| Field 2 | Umidade (%) |
| Field 3 | Temperatura média (°C) |
| Field 4 | Umidade média (%) |
| Field 5 | Índice de calor (°C) |
| Field 6 | WBGT estimado (°C) |
| Field 7 | Estado (0 Normal, 1 Atenção, 2 Alerta, 3 Crítico) |
| Field 8 | Leituras fora do limite |

   - Marque **Show Status** para que a mensagem de status de cada envio apareça no canal.
4. **Save Channel**.

## 2. Ligar o ESP32 ao canal

1. Aba **API Keys** → copie a **Write API Key**.
2. No `esp32/sketch.ino`, cole a chave em `TS_WRITE_API_KEY`.
3. Rode a simulação no Wokwi. O Monitor Serial deve mostrar `[NUVEM] Enviado ao ThingSpeak — registro #N` a cada 20 s.

## 3. Configurar os gráficos

Em **Private View**, clique no lápis ✏️ de cada gráfico:

| Gráfico | Title | Type | Results | Y-axis |
|---|---|---|---|---|
| Field 1 | Temperatura instantânea | line | 100 | °C |
| Field 2 | Umidade instantânea | line | 100 | % |
| Field 3 | Temperatura média (borda) | spline | 100 | °C |
| Field 4 | Umidade média (borda) | spline | 100 | % |
| Field 5 | Índice de calor | line | 100 | °C |
| Field 6 | WBGT estimado | line | 100 | °C |
| Field 7 | Estado do ambiente | step | 100 | Y-min 0 · Y-max 3 |
| Field 8 | Excedências na janela | column | 100 | Y-min 0 · Y-max 10 |

**Widgets recomendados** (**Add Widgets**):

- **Gauge** no Field 6 (WBGT), de 15 a 40, com faixas: verde 15–25, amarelo 25–28, laranja 28–32, vermelho 32–40.
- **Numeric Display** no Field 1 (temperatura atual).
- **Lamp Indicator** no Field 7, acendendo quando `> 1` (ALERTA ou CRÍTICO).

## 4. Deixar o canal público

1. Aba **Sharing** → selecione **Share channel view with everyone**.
2. Aba **Public View** → repita os gráficos e widgets (a visão pública tem layout próprio).
3. Copie a URL do canal (`https://thingspeak.mathworks.com/channels/<ID>`) e teste numa **janela anônima**.
4. Cole o link na tabela de links do `README.md`.

## 5. Ler os dados de fora (integração)

Os dados ficam acessíveis pela API REST, útil para o front-end e para o script Python:

```
https://api.thingspeak.com/channels/<ID>/feeds.json?results=20
https://api.thingspeak.com/channels/<ID>/fields/6/last.json
https://api.thingspeak.com/channels/<ID>/feeds.csv
```
