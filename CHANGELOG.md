# Changelog

Formato basato su [Keep a Changelog](https://keepachangelog.com/it/1.0.0/).

**Versioning:** se non indicato diversamente, incrementare sempre l’ultimo numero (`0.9.6-zero` → `0.9.7-zero`) e il contatore in `VERSION`. Minor/major solo su richiesta esplicita. **Policy:** saltare sempre numeri di versione e subversione **13** e **17**. Albero Luckfox Pico Zero, indipendente da Mrbin-rv1106.

## [0.9.8-zero] - 2026-11-04 — DEMO spenta, LED, file senza RTSP, video al tetto dell'ISP

**Data/ora release:** 2026-11-04 19:37:02 +0100
**Subversion:** `3` (serie **0.9.8-zero**, da `2` / 0.9.7-zero)
**Commit:** `3d84086221e50541199dcca8e80ae7798cac0d4a`
**Stato:** Stable (patch). Sorgente. `out/` e l’immagine non sono nel commit. La scheda in sessione ha già questo core e i moduli ISP, senza reflash.

### Aggiunto

- Casella **DEMO** in Impostazioni, spenta di default (`demo_en`). Il GPIO 59 da solo non avvia la demo. D1, D2, MODE e DEMO hanno il pull-up: a riposo sono HIGH e il filo a GND è il fronte (`CoreSettings.cpp`, `CoreDemo.cpp`, `hal_gpio.cpp`).
- LED spento a riposo. Acceso fisso solo in registrazione, lampeggio veloce mentre chiude il file, tre lampeggi da 1 s al primo IP, lampeggio continuo finché resta in AP (`CoreStatusLed.cpp`).
- `tools/gpio-watch.c`: stampa numero Linux, stato vecchio e stato nuovo a ogni fronte dei pin configurati.
- Patch del kernel Luckfox in `kernel/pico-zero-isp.patch` (si applica dalla root dell’SDK). I `.ko` non sono nel commit.

### Modificato

- Se all’avvio non c’è un PIR vero e non c’è la sveglia RTC (A1F), entra in CONFIG con la GUI accesa. SQW basso per il pull-down non conta come sveglia. MODE che sale e poi scende esce e dà il DONE (`mrbin_core_main.cpp`).
- RTSP e registrazione locale non stanno insieme. Con il canale RTSP su, il record locale è rifiutato; accendere RTSP chiude la clip in corso. Il pulsante è «Spegni RTSP» solo se il canale è davvero aperto (`CoreRecorder.cpp`, `CoreWeb.cpp`).
- Il preset 3840×2160 registra **3072×1728**. Sopra i 5 MP il tetto resta 15 fps. D2 chiude anche la clip partita dalla GUI (`CoreRecorder.cpp`, `hal_capture.cpp`).
- La SD fa `fsck.vfat -a` una sola volta, a volume smontato e senza domanda interattiva (`hal_sd.cpp`).

### Corretto

- «Capture 3840x2160@15 failed»: l’encoder si apriva e l’ISP non consegnava un frame, perché oltre 3072×1728 scarta il frame MIPI. Ora VI e VENC usano 3072×1728. Una clip di prova ha scritto 45 frame in circa 3 s, un frame ogni 66 ms, nessuno scartato.
- Il dts con `rockchip,unite=1` leggeva ogni frame due volte (circa 4 fps a 2880, circa 1 fps a 8 MP). Il modulo ignora quel flag e resta su ISP V32: V32_L faceva fallire `EnableChn`. Il primo modo IMX415 3864×2192 è 30 fps / 891M al posto dei 20 fps / 594M. Con il preset a 15 fps il sensore resta a un frame ogni 66 ms.

## [0.9.7-zero] - 2026-11-04 — live sempre acceso, due canali, movimento senza blocchi

**Data/ora release:** 2026-11-04 13:49:31 +0100
**Subversion:** `2` (serie **0.9.7-zero**, da `1` / 0.9.6-zero)
**Commit:** `27e253742ee9f2c58a4bf6d73992a8b61819da44`
**Stato:** Stable (patch). Sorgente. `out/` e l’immagine non sono nel commit. La scheda in sessione ha già questo core, senza reflash.

### Aggiunto

- Pagina **Canali Video**. Il canale 1 è la stessa risoluzione di Reg. video; un avviso dice che salvarlo sovrascrive Reg. video, e salvare Reg. video aggiorna il canale 1. Il canale 2 (default 1280×720@15) serve al movimento e a RTSP, non alla registrazione (`CoreWeb.cpp`, `CoreSettings.cpp`).
- RTSP su `rtsp://ip:554/ch1` e `/ch2`. `/live` resta l’alias del canale 1. Un solo pulsante accende o spegne sempre entrambi i canali e lo ricorda in NVS (`CoreRtsp.cpp`).
- Anteprima locale MJPEG su `rkisp_bypasspath` (VI canale 2, VENC canale 3), così il selfpath resta al movimento o al canale 2 (`hal_capture.cpp`).
- Calibrazione separata per camera: `cal_imx415`, `cal_imx708`, `cal_sc3336`, `cal_ov5647`. In config e con l’RTC il sensore viene riconosciuto e carica la propria chiave. L’IMX415 non eredita il vecchio blob `isp_cal` (`CoreIspCal.cpp`, `mrbin_core_main.cpp`).

### Modificato

- Il video locale non dipende da Attiva RTSP. In DEMO la pagina Live parte anche con lo streamer spento (`CoreLive.cpp`, `CoreWeb.cpp`).
- RTSP in DEMO parte spento. Un client non accende l’encoder. PLAY con lo streamer spento risponde 453 (`CoreRtsp.cpp`, `CoreDemo.cpp`).
- Il canale 2 parte solo dopo che il canale 1 ha preso la memoria. Se 1280×720 non entra, riprova a 640×360 senza chiudere il canale 1.
- Sopra il 1080p il bitrate di movimento sale fino al tetto di 28 Mbps (prima 16 Mbps), il buffer di stream è 2,5 MB e il GOP è circa mezzo secondo (`CoreConfig.h`, `CoreSettings.cpp`, `hal_capture.cpp`).
- Capovolgi e specchia restano sul sensore. L’encoder non li riapplica, così non si annullano (`CoreVideo.cpp`).
- La SD di questa Pico Zero eMMC è `mmcblk2`, non `mmcblk0` (quello è la flash interna). Se il sistema l’ha già montata, viene collegata su `/sdcard` (`hal_sd.cpp`).

### Corretto

- Sul movimento l’H.264 non sale più fino al QP 51: canale 1 (sopra il 1080p) resta tra 22 e 34, il canale 2 tra 18 e 32. I blocchi grandi che scorrevano fino al fotogramma fermo si chiudono al quantizzatore e al frame successivo (`hal_capture.cpp`).
- Accendere i due canali insieme non uccide più la GUI. Il canale 2 non parte prima dell’8 MP, e `DisableChn` sul mainpath non viene chiamato mentre selfpath o il live sono ancora in streaming (`hal_capture.cpp`, `CoreRtsp.cpp`).
- L’H.264 sopra il 1080p non usa più il wrap online: su questa IMX415 non consegnava frame.
- In PIR non si carica la calibrazione e non si interroga il 3A, così il primo frame resta nel budget di 500 ms (`mrbin_core_main.cpp`).
- Il nome del sensore IMX415 si legge anche da `/sys/bus/i2c/devices/4-0037/name` (`CoreVideoSensor.cpp`).
