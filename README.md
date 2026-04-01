# AmiiboBox 🎮

Emulador de Amiibo para M5StickC Plus2.

- **IR → 3DS Old / 2DS / 3DS XL** via IrDA no G19
- **NFC → Switch / Wii U** via PN532 na Porta A (G32/G33)
- **WebUI** via WiFi AP para upload de arquivos `.bin`

## Como compilar

1. Faça fork ou upload deste repositório no GitHub
2. Vá em **Actions** → **Build AmiiboBox Firmware** → **Run workflow**
3. Aguarde ~3 minutos
4. Baixe o artifact `amiibobox-firmware` com o `.bin` pronto

## Como gravar

Com o M5StickC Plus2 conectado ao PC via USB:

```bash
pip install esptool
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 1500000 \
  write_flash 0x0 amiibobox_flash.bin
```

Ou use o **M5Burner** (Windows/Mac/Linux) para gravar o `.bin` graficamente.

## Pinagem PN532

| PN532 | M5StickC Plus2 |
|-------|----------------|
| SDA   | G32 (Porta A)  |
| SCL   | G33 (Porta A)  |
| VCC   | 3.3V           |
| GND   | GND            |

## Uso

1. Conecte ao WiFi **AmiiboBox** (senha: `amiibo123`)
2. Abra o browser em **192.168.4.1**
3. Faça upload do arquivo `.bin` do amiibo (dump NTAG215, 540 bytes)
4. Selecione o amiibo na lista
5. No M5Stick:
   - **Botão A** → modo IR para 3DS Old
   - **Botão B** → modo NFC para Switch/Wii U
