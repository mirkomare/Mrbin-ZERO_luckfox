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

I pad scelti sono GPIO di default (`_d`), con SPI0 spento così C0–C2 restano GPIO per DONE e per l’RTC a bitbang.

## Righe importanti

| Funzione | rv1106 Pro/Max | Pico Zero | Perché si sposta |
|---|---|---|---|
| LED | GPIO1_B2, linux 42, header 1 | GPIO3_C6, linux 118, USER LED | Sulla Zero B2 è la UART di debug. Il LED di scheda è già C6 e non è uno strap. |
| MODE (alto = Config) | GPIO1_C5, linux 53, header 6 | GPIO1_C5, linux 53 | Libero: non è SDIO WiFi né eMMC. |
| D1 PIR (basso = wake) | GPIO1_C4, linux 52, header 7 | GPIO1_C4, linux 52 | Stesso motivo. L’alternativa SDMMC1_M1 non è il bus usato dalla TF. |
| D2 stop | GPIO1_D2, linux 58, header 9 | GPIO1_D2, linux 58 | Pad GPIO, fuori da WiFi e boot. |
| DONE | GPIO1_C0, linux 48, header 12 | GPIO1_C0, linux 48 | SPI0 è disabled nel DTS Zero. |
| SQW RTC | GPIO1_C7, linux 55, header 4 | GPIO1_C7, linux 55 | Invariato, non è uno strap. |
| RTC SDA / SCL | GPIO1_C1 (49) / C2 (50) | uguale, bitbang | I2C4 di CSI è su C7/D0. SPI0 spento. |
| MODEM_PWR | GPIO2_B1, linux 73, header 11 | GPIO2_B1, linux 73 | Il banco SD è GPIO2_A, non B. |
| DEMO | GPIO2_A0, linux 64, header 24 | GPIO1_D3, linux 59 | GPIO2_A0 è SDMMC1_D1 della TF. |

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
