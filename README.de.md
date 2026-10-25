# OpenSL1

*English: [README.md](README.md)* – die englische Fassung ist ausführlicher (Build, Tests, Aufbau).

Ein freier Treiber für die **Rane SL1** (Serato Scratch Live), damit das Interface mit Timecode-Vinyl in
[Mixxx](https://mixxx.org) weiterläuft – vor allem auf Macs mit Apple Silicon, für die es keinen Herstellertreiber
mehr gibt.

OpenSL1 ist ein unabhängiges Projekt und **nicht mit Rane, Serato oder inMusic verbunden, von ihnen unterstützt oder
empfohlen**. Rane, SL1, Serato und Scratch Live sind Marken ihrer jeweiligen Inhaber und werden hier nur genannt, um
die Hardware zu bezeichnen. Das Projekt enthält keinen Code und keine Dateien von Rane oder Serato; alles beruht auf
eigenen Messungen am Gerät (siehe [`docs/PROTOCOL.md`](docs/PROTOCOL.md)).

## Stand

- **macOS:** Gerät „Rane SL1“ mit 4 Eingängen (Input 1/2) und 4 Ausgängen (Line 1/2) bei 44,1 kHz. Mit Serato-CV02-
  Timecode auf beiden Decks in Mixxx getestet (MacBook Air M4, macOS 27): keine USB-Fehler, keine Lücken im Timecode;
  Ruhezustand und Ab-/Anstecken während der Wiedergabe fängt der Treiber selbst ab.
- **Linux:** braucht OpenSL1 nicht – der Kernel (`snd-usb-audio`, ab Linux 6.0) unterstützt die SL1, Mixxx nutzt sie
  über ALSA.
- Latenz: Hardware-Schleife (USB + Wandler) 2,9 ms; vom Timecode bis zum Line-Ausgang etwa 35 ms bei 5,8 ms Puffer.
- Mikrofon-Eingang: nicht freigegeben.

## Getestet mit

- Rane SL1 (`13e5:0001`) an einem **MacBook Air 13 Zoll, M4, 2025** (`Mac16,12`, 10 Kerne, 16 GB) mit
  **macOS 27.0.1** (26A434), angeschlossen über einen passiven USB-A-auf-USB-C-Adapter; Mixxx 2.6.0-beta,
  Serato-CV02-Timecode, externer Pioneer-Mixer.
- Linux (ohne diesen Treiber): CachyOS, Kernel 7.2, Mixxx 2.5.6.

## Installation (macOS)

`OpenSL1-<Version>.pkg` von der Release-Seite laden und doppelklicken. Der Installer kopiert `OpenSL1.driver` nach
`/Library/Audio/Plug-Ins/HAL/` und startet den Audiodienst neu. Ab macOS 12, Apple Silicon und Intel.

**Nicht notariell signiert:** Die Pakete sind nur ad hoc signiert (keine Apple-Developer-ID). macOS blockiert sie beim
ersten Öffnen; unter *Systemeinstellungen → Datenschutz & Sicherheit → Trotzdem öffnen* freigeben.

Deinstallieren: `OpenSL1-Uninstall-<Version>.pkg` öffnen oder `sudo packaging/uninstall.sh` ausführen.

## Mixxx einrichten

1. SL1 direkt an den Mac anschließen, **bevor** Mixxx startet. Fragt macOS, ob Mixxx das Mikrofon nutzen darf:
   **erlauben** – sonst bekommt Mixxx keinen Eingang und keinen Timecode.
2. *Einstellungen → Soundhardware*: Sound-API **CoreAudio**, Abtastrate **44100 Hz**, Puffer 5,8 ms.
   - Ausgänge: *Deck 1* → „Rane SL1“ Kanäle 1–2, *Deck 2* → Kanäle 3–4. *Main* und *Kopfhörer* auf **Keine**
     (externer Mixer). Keine zweite Soundkarte – auch nicht die eingebauten Lautsprecher –, sonst gleicht Mixxx zwei
     Uhren ab und es kann knacken oder springen.
   - Eingänge: *Vinyl Control 1* → Kanäle 1–2, *Vinyl Control 2* → Kanäle 3–4.
3. *Einstellungen → Vinyl-Steuerung*: *Serato CV02 Vinyl, Side A*, 33⅓, Modus **REL**, zum Scratchen den
   **Alpha-Beta-Filter** (mit dem Kalman-Filter läuft der Ton nach einem Handstopp kurz in Zeitlupe nach).
4. Nach dem Start von Mixxx ein paar Sekunden warten, bevor die Nadel aufsetzt: Direkt nach dem Start liefern die
   Eingänge der SL1 kurz Störgeräusche.

## Für Auftritte

- Ladekabel des Macs **vor** dem Set einstecken und währenddessen nicht anfassen: Beim Wechsel der Stromquelle fällt
  die SL1 manchmal kurz vom USB (meist ≤ 3 s Stille, selten muss man sie neu einstecken). Details:
  [docs/MACOS.md](docs/MACOS.md#known-issue-the-sl1-drops-off-usb-when-the-macs-power-source-changes).
- Abgezogen? Mixxx laufen lassen und die SL1 wieder einstecken (jeder Port geht) – der Ton kommt von selbst wieder.
- Soundeinstellungen nicht während des Sets ändern: Mixxx 2.6.0-beta kann dabei hängen bleiben (Fehler in Mixxx).

## Clean Room

Beiträge müssen auf öffentlicher Dokumentation und Messungen an der Hardware beruhen. Kein Code, keine Binärdateien
und keine Disassemblierung von Rane-/Serato-Software. OpenSL1 gibt die SL1 nicht als andere Hardware aus und umgeht
keine Lizenzprüfung.

## Lizenz

GPL-3.0-or-later, siehe [`LICENSE`](LICENSE).
