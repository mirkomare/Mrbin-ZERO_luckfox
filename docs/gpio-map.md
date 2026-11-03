# GPIO — Pico Zero rispetto a Mrbin-rv1106 0.9.6

Sorgente: `C:\CURSOR\Mrbin-rv1106`, versione **0.9.6** (commit `9548eaf`).
Scheda nuova: Luckfox Pico Zero, RV1106G3, CSI IMX415 8MP (`sony,imx415`, I2C `0x37`, modulo CMK-OT2022-PX1).

`pin = bank*32 + (gruppo*8 + X)` con A=0, B=1, C=2, D=3.
I numeri header della Pro/Max non coincidono con il connettore a 40 pin della Zero: qui conta il pad del SoC.

## Scelta

Non si usano:

- il bus WiFi SDIO (`&sdmmc` nel DTS ufficiale `rv1106g-luckfox-pico-zero.dts`): GPIO3_A1–A7, linux 97–103, e il reset `sdio_pwrseq` GPIO1_A2 (linux 34);
- UART Bluetooth GPIO1_A3 / GPIO1_A4 e i wake GPIO0_A2 / GPIO0_A5;
- eMMC (GPIO4_A* e GPIO4_B0/B1): è il boot da 8 GB;
- SARADC_IN0 / GPIO4_C0 (linux 144, 1,8 V): tasto BOOT/recovery, va tenuto in pull-up;
- TF card SDMMC1 su GPIO2_A*: ci passa la SD e il ROM può avviare da lì;
- reset camera GPIO3_C5 e I2C4_M2 GPIO3_C7 / GPIO3_D0 (bus IMX415);
- GPIO1_B2 / GPIO1_B3: console di debug, il DTS lascia `fiq_debugger` acceso.

Un pin del connettore si usa solo se il pinout ufficiale Luckfox Pico Zero non ci stampa I2C, SPI, UART, SDIO, eMMC o BOOT. PWM non è in quella lista. Sul connettore da 40 pin i GPIO così fatti sono cinque: pin 12 (GPIO1_C6, solo PWM10), pin 22 (GPIO1_D3, nessuna funzione), pin 36 (GPIO1_C7, solo PWM11), pin 38 (GPIO0_A3, nessuna funzione), pin 40 (GPIO0_A4, solo PWM1). Sono già tutti occupati: DEMO 22, SQW 36, DONE 40, RTC SDA 12, RTC SCL 38. Non resta un pin il cui pinout stampa solo PWM.

Il LED esterno, quello che sulla Pro/Max era GPIO1_B2 (header 1, linux 42), va quindi sul pin meno scomodo ancora libero: pin 21, GPIO1_C3, linux 51. Il pinout ci stampa SPI0_MISO_M0 e PWM6_M2. SPI0 è `status = "disabled"` nel DTS della Zero. Non è WiFi SDIO, reset radio, UART Bluetooth, eMMC, BOOT, TF, CSI né la UART di debug. Il USER LED di scheda (GPIO3_C6, linux 118) non è questo LED: non esce sul connettore.

## Righe importanti

| Funzione | Prima sulla Zero | Adesso | Perché |
|---|---|---|---|
| LED esterno | GPIO3_C6, linux 118, USER LED di scheda | GPIO1_C3, linux 51, pin 21 | Non resta un GPIO pulito né un pin solo PWM. Il pinout stampa SPI0_MISO e PWM6. SPI0 è spento. Sulla Pro/Max era GPIO1_B2, header 1, linux 42. |
| DONE | GPIO1_C0, linux 48, pin 24, SPI0_CS0 | GPIO0_A4, linux 4, pin 40 | Il pinout stampa solo PWM1_M0. Niente I2C, SPI o UART. |
| RTC SDA | GPIO1_C1, linux 49, pin 23, SPI0_CLK | GPIO1_C6, linux 54, pin 12 | Il pinout stampa solo PWM10_M1. |
| RTC SCL | GPIO1_C2, linux 50, pin 19, SPI0_MOSI | GPIO0_A3, linux 3, pin 38 | Il pinout non stampa nessuna funzione alternativa. |
| SQW RTC | GPIO1_C7, linux 55, pin 36 | invariato | Il pinout stampa solo PWM11_IR_M1. |
| DEMO | GPIO1_D3, linux 59, pin 22 | invariato | Il pinout non stampa I2C, SPI o UART. Sulla Pro/Max era GPIO2_A0, che qui è la TF. |
| MODE (alto = Config) | GPIO1_C5, linux 53, pin 32 | invariato | Non è I2C1: il pinout stampa UART4_TX_M1. I tre GPIO puliti sono già presi. |
| D1 PIR (basso = wake) | GPIO1_C4, linux 52, pin 33 | invariato | Non è I2C1: il pinout stampa UART4_RX_M1. Stesso motivo. |
| D2 stop | GPIO1_D2, linux 58, pin 26 | invariato | Il pinout stampa SPI0_CS1. Non resta un GPIO senza quel bus. |
| MODEM_PWR | GPIO2_B1, linux 73, pin 27 | invariato | Il pinout stampa I2C1_SDA_M1 e UART0_TX_M1. Non resta un GPIO pulito. |

## Pad vietati

| Pad | Linux | Ruolo sulla Zero |
|---|---|---|
| GPIO3_A1–A7 | 97–103 | CLK/CMD/D0–D3/DET del WiFi AIC8800 su SDMMC0 |
| GPIO1_A2 | 34 | Reset radio (`mmc-pwrseq`), attivo basso |
| GPIO1_A3, GPIO1_A4 | 35, 36 | UART Bluetooth |
| GPIO4_A*, GPIO4_B0, GPIO4_B1 | 128+ | eMMC di boot |
| GPIO4_C0 | 144 | BOOT / SARADC_IN0, pull-up obbligatorio |
| GPIO2_A0, A2, A3, A4, A5 | 64, 66–69 | Slot TF (SDMMC1) |
| GPIO3_C4, C5, C7, D0 | 116, 117, 119, 120 | Clock e reset CSI, I2C IMX415 |

Override runtime: `/etc/mrbin/pins.env`.
