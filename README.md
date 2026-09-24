# Bierkühler ESP32-S3 – V6 mit DRV8871

Diese Fassung ersetzt ausschließlich das bisherige Rührwerk-Relais durch einen
DRV8871-Motortreiber. Die Relais für Kompressor und Verflüssigerlüfter bleiben
unverändert.

## Projektstruktur

```text
.
├── src/             Firmware und zentrale Konfiguration
├── cad/             Hinweise und später die mechanischen CAD-Daten
├── platformio.ini   PlatformIO-Umgebung und Bibliotheken
└── README.md        Anschluss-, Bedien- und Sicherheitshinweise
```

Das Verzeichnis kann direkt als PlatformIO-Projekt geöffnet werden. Die
benötigten Bibliotheken und die ESP32-S3-Umgebung sind in `platformio.ini`
festgelegt. Hardwareoptionen wie Demo-Modus, Pinbelegung und die Schaltlogik
der Relais stehen zentral in `src/config.h`.

CAD-Dateien werden nach dem Schema in [`cad/README.md`](cad/README.md)
einsortiert. Dadurch bleiben bearbeitbare Originale, neutrale STEP-Exporte und
druckfertige Dateien voneinander getrennt.

## Rührwerk-Verhalten

| Zustand | Kompressor | Rührwerk |
|---|---:|---:|
| `OFF` | aus | 0 % |
| `WARTET` | aus | 50 % |
| `KÜHLT` | ein | 100 % |
| `BEREIT` | aus | 50 % |
| `FEHLER` bei aktivierter Kühlung | aus | 50 % |

Beim Übergang von Stillstand auf 50 % läuft der Motor zuerst 800 ms mit 100 %
an. Das verhindert, dass das Rührwerk unter Last bei kleiner Tastweite stehen
bleibt. Die PWM-Frequenz beträgt 20 kHz und liegt damit oberhalb des üblichen
Hörbereichs.

Die Werte stehen in `src/config.h`:

```cpp
STIRRER_IDLE_PERCENT = 50;
STIRRER_COOLING_PERCENT = 100;
STIRRER_START_BOOST_MS = 800;
STIRRER_PWM_FREQUENCY_HZ = 20000;
```

50 % PWM bedeutet nicht bei jedem Motor exakt die halbe Drehzahl. Last,
Versorgungsspannung und Motorbauart beeinflussen die tatsächliche Drehzahl.

## Anschluss des DRV8871-Moduls

| DRV8871-Modul | Anschluss |
|---|---|
| `IN1` | ESP32-S3 GPIO 16 |
| `IN2` | ESP32-S3 GPIO 8 (PWM) |
| `GND` | gemeinsamer GND von ESP32 und Motorversorgung |
| `VM` | Plus der zum Motor passenden Versorgung |
| `OUT1`, `OUT2` | Rührwerksmotor |

`VM` darf nicht mit dem 3,3-V-Pin des ESP32 verwechselt werden. Der Motor wird
aus seiner eigenen passenden Versorgung gespeist; nur die Logikeingänge kommen
vom ESP32. Dreht der Motor falsch herum, `OUT1` und `OUT2` tauschen.

Der DRV8871 ist laut TI für Bürsten-Gleichstrommotoren mit 6,5 bis 45 V und bis
zu 3,6 A Spitzenstrom vorgesehen. Der praktisch zulässige Dauerstrom hängt stark
vom Modul, dessen Kupferfläche/Kühlung, der Umgebungstemperatur und der
PWM-Frequenz ab. Anlauf- und Blockierstrom des Rührwerkmotors müssen deshalb zum
Modul passen.

Die Stromgrenze wird mit dem Widerstand an `ILIM` bestimmt. Näherungsweise gilt:

```text
I_TRIP [A] = 64 / R_ILIM [kΩ]
```

Beispiel: 32 kΩ ergeben ungefähr 2 A. Bei fertigen Modulen ist dieser Widerstand
oft bereits bestückt. Vor dem Anschluss den Wert beziehungsweise die
Modulbeschreibung prüfen. Direkt an `VM` gehören außerdem eine gute lokale
Abblockung und ausreichend Pufferkapazität; TI zeigt in der typischen Schaltung
0,1 µF plus 47 µF.

## Gesamte Pinbelegung

| Funktion | GPIO |
|---|---:|
| Encoder CLK | 4 |
| Encoder DT | 5 |
| Encoder SW | 6 |
| DS18B20 DATA | 7 |
| DRV8871 IN2 / PWM | 8 |
| Display SCL | 9 |
| Display CS | 10 |
| Display SDA / IO0 | 11 |
| Display IO1 | 12 |
| Display IO2 | 13 |
| Display IO3 | 14 |
| Display BL | 15 |
| DRV8871 IN1 | 16 |
| Relais Kompressor | 17 |
| Relais Lüfter | 18 |
| Display RST | 47 |

Am DS18B20 wird normalerweise ein 4,7-kΩ-Pull-up zwischen DATA und 3,3 V
benötigt. Die vorhandenen Relais sind weiterhin als HIGH-aktiv eingestellt.

## Schnellere Bedienung und Farbring

Der Drehgeber und sein Taster werden jetzt flankengesteuert erfasst. Dadurch
gehen Eingaben nicht verloren, während das Display ein Bild überträgt. Auch ein
kompletter kurzer Tastendruck wird zwischengespeichert und nach dem Zeichnen
verarbeitet.

Der lückenlose Farbring bleibt erhalten. Seine Pixelradien werden jedoch nur
noch einmal beim Start vorausberechnet. Während des Kühlens braucht jedes Bild
dadurch keine zehntausenden Quadratwurzelberechnungen mehr. Die Pulsbreite bleibt
gleitend und die Innen- und Außenkante bleiben weich überblendet. Die Animation
wird beim Kühlen alle 100 ms aktualisiert.

## Bedienung

Hauptanzeige:

- Drehen: Solltemperatur ändern
- Kurz drücken: Kühlmodus ein/aus
- Lang drücken und loslassen: Einstellungen öffnen

Einstellungen:

- Drehen: Zeile wählen oder Wert ändern
- Kurz drücken: Bearbeiten / speichern
- Lang drücken und loslassen: Abbrechen beziehungsweise zurück

## Sicher testen

Beim ersten Test Motor, Kompressor und Lüfter noch nicht gleichzeitig
anschließen:

1. Zuerst ohne Motor an `OUT1/OUT2` prüfen, ob `IN1/IN2` und die Statusausgaben
   plausibel reagieren.
2. Danach nur das Rührwerk anschließen und kontrollieren, ob es beim Aktivieren
   kurz mit 100 % anläuft und anschließend auf 50 % wechselt.
3. Motorstrom bei 50 %, 100 % und beim blockierten beziehungsweise gebremsten
   Rührwerk beurteilen; Blockiertest nur sehr kurz durchführen.
4. Erst danach Lüfter und Kompressorsteuerung wieder zuschalten.

Der Demo-Modus lässt alle echten Ausgänge sicher aus. Für den realen Betrieb
muss in `src/config.h` stehen:

```cpp
DEMO_MODE = false;
```
