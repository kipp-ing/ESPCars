# ESPCars marketing direction

Working brief, 2026-10-13. Public-facing copy is English; discussion and design
notes here are German. The README contains the platform introduction.

## Positionierung

**The open platform for connected vehicle electronics.**

**Build your application. Connect it to the vehicle. Understand it in the field.**

ESPCars stellt die gemeinsame Infrastruktur für die Entwicklung und den Betrieb
vernetzter Fahrzeugelektronik bereit: ESPHome als Firmware-Framework,
Fahrzeugkommunikation, Diagnose, lokale Prüfung, Aufzeichnung und Datenverarbeitung.
Die Anwendung ist projektspezifisch; die Infrastruktur ist wiederverwendbar.

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
16:9-Raster, sachliche Sans-Serif-Typografie und viel Freiraum. Hochwertige
Fahrzeug- und Board-Renderings bilden die visuelle Basis; editierbare SVG-Labels,
Leader-Lines und Datenpfade erklären die Funktionen darüber. Das Fahrzeug darf
attraktiv und hochwertig wirken; der Board-Ausschnitt zeigt ESP32 und Peripherie.
Dunkles Navy trägt Titel und Anwendungsebene; ein klares technisches
Blau markiert Datenflüsse und aktive Elemente. Grau zeigt Systemgrenzen und
Anfragen; Amber ist für fehlende Konnektivität oder Hinweise reserviert.
Gerät und Host bleiben als getrennte Bereiche erkennbar. Farben und Linienstile
haben eine durchgängige Bedeutung. Eigenständige ESPCars-Gestaltung ohne
Übernahme einer OEM-Marke.

Renderings und SVG-Overlay getrennt halten; für portable SVGs werden die Raster-
Assets eingebettet. Texte, Verbindungslinien und Diagramme bleiben editierbar.
PNG für Einbettungen exportieren. Fahrzeug und Board sind konzeptionelle
Illustrationen, keine Abbildungen eines bestimmten OEM-Modells oder des exakten
Referenz-PCBs.
Querformat für README und Präsentationen, separat komponierte Hochformate für
Social Media. Erste Layoutentwürfe für Motive 1 und 2 liegen unter
[`marketing/figures/`](marketing/figures/README.md); sie werden vor einer
Einbettung in den öffentlichen README gemeinsam überarbeitet. Motive 3–5
bleiben Vorschläge.

### 1. The infrastructure around your application

**Aussage:** Eine Anwendung bekommt eine gemeinsame Infrastruktur.

**Bild:** Ein hochwertiges Fahrzeug-Rendering mit dezentem Cutaway bildet den
Anwendungskontext. SVG-Callouts zeigen CAN/LIN, Diagnose, lokales Logging und
Browser-Zugriff. Ein Detail-Leader führt zu einem vergrößerten ESP32-Board-
Rendering mit Transceivern, GPIO und SD. Die ESPHome-Grundlage wird als ruhiges
gemeinsames Fundament darunter sichtbar. Die v1-Blockarchitektur bleibt als
ergänzendes technisches Diagramm erhalten; die Host-Datenkette folgt in Motiv 2.

**Headline:** `Build the application. Reuse the infrastructure.`

**Einsatz:** Hero-Grafik im README. Zeigt sofort Umfang und Zusammenspiel.

### 2. From test drive to usable data

**Aussage:** Aufzeichnung und spätere automatische Abholung bilden einen Workflow.

**Bild:** Board-Rendering, Fahrzeug-Rendering und Host-Datenkette bilden die drei
Stationen Konfiguration, Fahrzeugbetrieb und Auswertung. Ein blauer Datenpfad
verbindet die Stationen; das Fahrzeug trägt ein SVG-SD-Overlay. Unter der Fahrt liegt ein
Abschnitt `WiFi unavailable`, während die SD-Aufzeichnung weiterläuft. Zurück im
erreichbaren Netz zieht der Host die versiegelten Dateien; danach folgen
Verifikation, Archiv und Dekodierung. Der Collector wird ausdrücklich als Host
dargestellt, nicht als automatischer Cloud-Push aus dem Gerät.

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
Layoutentwürfe gemeinsam verbessern, danach final ausarbeiten und im README einbetten.

## Demo-Storyboard

Ein fiktives Signal entsteht am Bus, wird lokal sichtbar und bei einer
Verbindungsunterbrechung aufgezeichnet. Sobald der Host das Gerät wieder
erreicht, wird ein versiegelter Chunk automatisch abgeholt, überprüft und
archiviert. Das dekodierte Signal erscheint in der Auswertung. Ein Diagnosewert
und eine gezielt ausgelöste Störung zeigen Zustand, Datenalter und Fehlergrund.

Hardware, Simulation und Wiedergabe aufgezeichneter Daten werden im Demo-Auftritt
klar bezeichnet. Ausschließlich fiktive Definitionen und synthetische Daten
werden als öffentliche Beispiele verwendet.
