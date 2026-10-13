# ESPCars marketing direction

Working brief, 2026-10-13. Public-facing copy is English; discussion and design
notes here are German. The README contains the platform introduction.

## Positionierung

**The open platform for connected vehicle electronics.**

**Build your application. Connect it to the vehicle. Observe, modify and validate.**

ESPCars stellt die gemeinsame Infrastruktur für die Entwicklung und den Betrieb
vernetzter Fahrzeugelektronik bereit: ESPHome als Firmware-Framework,
Fahrzeugkommunikation, Diagnose, lokale Prüfung, Aufzeichnung und Datenverarbeitung.
Die Anwendung ist projektspezifisch; die Infrastruktur ist wiederverwendbar.
Aktive Anwendungen stehen gleichberechtigt neben Beobachtung und Datenanalyse:
Inline-CAN-Gateway / Man-in-the-Middle, Filter und Signaländerungen während der
Weiterleitung, Signaladaption, Integrations- und Fehlertests sowie Tuning und
Kalibrierung. CAN-Signalmanipulation und CCP-Kalibrierzugriff sind unterschiedliche
Fähigkeiten. CCP bietet DAQ, Speicherzugriff, Kalibrierseitenwahl und opt-in
Schreibzugriff an ECUs mit unterstütztem CCP-Zugang; projektspezifische Signale
und Kalibrieradressen bleiben beim Anwenderprojekt.

Primäre Leser: Embedded-, Automotive-, Integrations- und Testingenieure.
Home Assistant ist eine mögliche Integration, kein vorausgesetzter Einstieg.

Ton: selbstbewusst, sachlich, konkret. Der Umfang und eine nachvollziehbare
Demonstration tragen den Anspruch. Keine unbelegten Alleinstellungs-, Leistungs-
oder Vollständigkeitsbehauptungen. Fehlende Teile des Zielbilds werden als
Entwicklungsaufgaben geführt und nach Umsetzung als Fähigkeiten beschrieben.

## Produktversprechen und nächste Bausteine

1. **Schneller zur Funktion.** Eine dokumentierte Referenzanwendung führt
   Buskommunikation, Diagnose, Portal, Logger und Datenabholung zusammen.
2. **Vom Prüfstand ins Fahrzeug.** Hardwareauswahl, Anschlussplan und Konfiguration
   geben einen reproduzierbaren Einstieg; eine gemeinsame Zustandsübersicht zeigt
   Bus, Diagnose, Aufzeichnung, Datenalter und Fehler.
3. **Vom Frame zum nutzbaren Signal.** Automatische Abholung, überprüftes Archiv,
   Dekodierung und eine unmittelbar nutzbare Auswertung bilden eine demonstrierte
   Kette. Messbedingungen und Belastungstests belegen die getesteten Konfigurationen.

Dies ist ein Ausbauplan, keine Aussage, dass die durchgängige Referenzanwendung
oder öffentliche Demo bereits existiert.

## Visuelle Richtung

Stil technischer Präsentationen großer OEMs: weißer Hintergrund, präzises
16:9-Raster, sachliche Sans-Serif-Typografie und viel Freiraum. Die ersten
klaren SVG-Blockdiagramme bestimmen den Aufbau: Anwendung, Komponenten,
Firmware-Grundlage und Host-Datenkette sind als logische Module dargestellt.
Platinen-Renderings werden in den README-Grafiken nicht verwendet.
Das Fahrzeug bleibt als einziges Rendering ein kleines Kontextmotiv,
das über eine schematische CAN/LIN-Verbindung an das Gerät angebunden ist.
**Keine Funktionen, Geräte, SD-Marker oder Anschlüsse im Fahrzeug lokalisieren.**
Verbindungslinien enden außerhalb der Fahrzeugsilhouette; sie stellen keine
konkrete Einbauposition oder Fahrzeugverkabelung dar. Die Geräteblöcke zeigen
Softwarefähigkeiten und Datenflüsse, keine Leiterplatte oder Pinbelegung.
Dunkles Navy trägt Titel und Anwendungsebene; ein klares technisches
Blau markiert Datenflüsse und aktive Elemente. Grau zeigt Systemgrenzen und
Anfragen; Amber ist für fehlende Konnektivität oder Hinweise reserviert.
Gerät und Host bleiben als getrennte Bereiche erkennbar. Farben und Linienstile
haben eine durchgängige Bedeutung. Eigenständige ESPCars-Gestaltung ohne
Übernahme einer OEM-Marke.

Renderings und SVG-Overlay getrennt halten; für portable SVGs werden die Raster-
Assets eingebettet. Texte, Verbindungslinien und Diagramme bleiben editierbar.
PNG für Einbettungen exportieren. Das Fahrzeug-Rendering folgt dem ausdrücklich
gewünschten Fahrzeugmotiv. Das Fahrzeug zeigt eine geschlossene
Karosserie und bekommt weiterhin keine Funktions- oder Einbauortmarker.
Querformat für README und Präsentationen, separat komponierte Hochformate für
Social Media. Plattformüberblick, Datenkette und Inline-Gateway sind mit dem
aktualisierten Fahrzeugmotiv im README eingebettet. Quellen und frühere
Layoutentwürfe liegen unter [`marketing/figures/`](marketing/figures/README.md).
Die weiteren Motive bleiben Vorschläge.

### 1. The infrastructure around your application

**Aussage:** Eine Anwendung bekommt eine gemeinsame Infrastruktur.

**Bild:** Der zentrale Gerätebereich enthält einen großen Block `Your application`.
Darunter liegen CAN/LIN mit Gateway/MITM und Signal-Rewrite, Diagnose mit
Kalibrierzugriff, Recording, Portal und die ESPHome-Grundlage. Links verbinden
sich ein kleiner Fahrzeugkontext und Phone/Laptop mit den logischen Modulen;
rechts liegen Collector, überprüftes Archiv und Decode/Publish. Das entspricht
dem ersten Architekturbild, ergänzt um das gewünschte Fahrzeugmotiv und die
aktiven Fähigkeiten. Keine Leiterplatte und keine Feature-Marker im Fahrzeug.

**Headline:** `Build the application. Reuse the infrastructure.`

**Einsatz:** Hero-Grafik im README. Zeigt sofort Umfang und Zusammenspiel.

### 2. From test drive to usable data

**Aussage:** Aufzeichnung und spätere automatische Abholung bilden einen Workflow.

**Bild:** Die drei Phasen des ersten Ablaufbilds bleiben erhalten: Configure/Verify
am Prüfstand, lokale Aufzeichnung während der Testfahrt, anschließende Abholung
und Auswertung. Das ursprüngliche Auto-Symbol wird durch das kleine Fahrzeug-
Rendering ersetzt. Bus-Recording und SD stehen als eigene Blöcke unter dem Auto,
ohne Funktionen darin zu lokalisieren. Die Host-Phase zeigt Pull, Prüfung, Archiv,
Bestätigung und Dekodierung. Keine Platinenabbildung.

**Headline:** `Record on the road. Analyse when you return.`

**Einsatz:** Workflow-Grafik und Storyboard für die spätere Demo.

### 3. Inside the recorder

**Aussage:** Der Logger puffert Aufnahme und SD-Schreibvorgang getrennt.

**Bild:** Ein vergrößerter Ausschnitt des Geräts. CAN-Tap und LIN-Automation
führen zum RAM-Ring; ein separater Writer führt zur SD-Karte. Neben der
SD-Spur ist eine Schreibpause sichtbar, während weitere Records in den Ring
kommen. Puffergrenze und Drop-Zähler bleiben sichtbar. Die Grafik erklärt
SD-Latenzpufferung, ohne unbegrenzte Kapazität oder allgemeine Timing-Garantien
für WLAN und Busse zu suggerieren.

**Headline:** `Capture. Buffer. Write.`

**Einsatz:** Technischer Beleg für Ingenieure nach dem Plattformüberblick.

### 4. Know what happened

**Aussage:** Ein Messwert bekommt Herkunft, Zeitbezug und interpretierbaren Zustand.

**Bild:** Ein großes fiktives Signaldiagramm mit einem markierten Punkt. Wenige
präzise Verbindungen zeigen auf Originalframe, Geräte-/Firmwareidentität,
Zeitbasis und Signaldefinition. Eine erkennbare Lücke bleibt als Lücke stehen;
unbekannte Zeit oder veraltete Werte werden nicht als aktuelle Daten gezeigt.
Die Zustände werden nur so visualisiert, wie sie durch vorhandene Metadaten
belegt werden können. Beispielwerte sind ausdrücklich synthetisch.

**Headline:** `A value is useful when you can trace it.`

**Einsatz:** Datenqualität und Diagnose statt einer weiteren Featureliste.

### 5. Check it where it runs

**Aussage:** Die lokale Bedienoberfläche ist beim Gerät verfügbar.

**Bild:** Eine ruhige Fahrzeug-/Prüfstandsillustration mit einem großen Telefon
im Vordergrund. Der Bildschirm zeigt eine echte oder originalgetreu aus einer
fiktiven Portal-Konfiguration gerenderte Zustandsansicht. Eine kurze direkte
Verbindung führt zum ESP32. Wenige Callouts erklären Zustand, Werte und
deklarierte Aktionen. Keine erfundenen automatischen Busdiagnosen im Screenshot.

**Headline:** `Check the system where it runs.`

**Einsatz:** Zugängliches Motiv für Einstieg, Social Media und Portal-Dokumentation.

## Reihenfolge

Zuerst Motive 1 und 2 für Produktverständnis und Nutzen entwickeln. Motiv 3
liefert anschließend technische Tiefe. Motive 4 und 5 ergänzen die Serie.
Plattformüberblick, Datenkette und das ergänzende Gateway-Motiv sind ausgearbeitet
und im README eingebettet. Weitere Motive und die Demo folgen als eigener Ausbau.

## Zusätzliches Motiv: inline gateway and tuning

Ein schematisches CAN-Segment A (kleines Fahrzeug-/Prüfstandsmotiv) und CAN-Segment B
(ECU-/Subsystem-Block) liegen auf den beiden Seiten des großen ESPCars-Geräts.
Beide Kommunikationsrichtungen sind sichtbar. Bridge, Filter, Signal-Rewrite und
optionale CRC-8-Korrektur stehen innerhalb des Gerätebereichs. Runtime-Signalwerte
kommen aus Automationen oder deklarierten Portal-Controls. CCP-Messung und opt-in
Kalibrierschreiben werden als eigener Zugriff ausgewiesen. Keine Lokalisierung
von Funktionen im Fahrzeug.

Headline: `Observe. Modify. Validate.`

## Demo-Storyboard

Ein fiktives Signal entsteht am Bus, wird lokal sichtbar und bei einer
Verbindungsunterbrechung aufgezeichnet. Sobald der Host das Gerät wieder
erreicht, wird ein versiegelter Chunk automatisch abgeholt, überprüft und
archiviert. Das dekodierte Signal erscheint in der Auswertung. Ein Diagnosewert
und eine gezielt ausgelöste Störung zeigen Zustand, Datenalter und Fehlergrund.

Hardware, Simulation und Wiedergabe aufgezeichneter Daten werden im Demo-Auftritt
klar bezeichnet. Ausschließlich fiktive Definitionen und synthetische Daten
werden als öffentliche Beispiele verwendet.

Ein aktiver Demo-Abschnitt zeigt einen synthetischen Frame auf CAN-Segment A,
einen gezielt umgeschriebenen Signalwert auf CAN-Segment B und die Änderung
über einen deklarierten Portal-Control. Beide Seiten werden aufgezeichnet,
damit die Änderung nachvollziehbar ist. Ein eigener CCP-Demo-Target kann DAQ
und opt-in Schreiben auf einen fiktiven Kalibrierparameter zeigen. Diese
Demo-Erweiterungen sind Ausbauaufgaben, keine bereits veröffentlichte Demo.
