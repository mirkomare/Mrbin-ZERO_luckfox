# Mrbin-ZERO_luckfox

Port di **Mrbin-rv1106 0.9.6** sulla Luckfox Pico Zero (RV1106G3) con camera **IMX415**.

La mappa pin è in `docs/gpio-map.md`. Il WiFi di scheda (AIC8800, `wlan0`) si accende solo in **Config** e **RTC**. In **PIR** l'interfaccia resta giù: niente scan e niente associazione. La pagina è `http://<ip>:1510/wifi`.

Le credenziali stanno in NVS e, se il filesystem è scrivibile, in `/userdata/mrbin/wifi.env` (modo 0600). Non vanno committate.

## Build

Sintassi su Windows, senza CSI:

```text
cmake -S . -B build -DHOST_SIM=ON
cmake --build build
```

L'immagine NAND chiede l'SDK Luckfox (`LUCKFOX_SDK`) e il toolchain `arm-rockchip830-linux-uclibcgnueabihf`, che su questa macchina non sono nel PATH. Il DTS è `kernel/rv1106g-pico-zero-mrbin.dts`.
