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

Technische Präzision mit einer starken Bildidee pro Grafik. Große Typografie,
wenige Labels, klare Hierarchie und viel Freiraum. Graphit und gebrochenes Weiß
bilden die Basis; Türkis markiert Datenflüsse, Amber markiert Hinweise und
Unterbrechungen. Farben haben eine durchgängige Bedeutung.

Diagramme mit editierbarem Text als SVG anlegen, PNG für Einbettungen exportieren.
Querformat für README und Präsentationen, separat komponierte Hochformate für
Social Media. Die folgenden Motive sind Vorschläge, noch keine fertigen Assets.

### 1. The infrastructure around your application

**Aussage:** Eine Anwendung bekommt eine gemeinsame Infrastruktur.

**Bild:** Ein großer zentraler Block `Your application` auf einem ESPHome-Fundament.
Links kommen CAN und LIN an; rechts verlassen Daten das Gerät in Richtung
Collector, Archiv und Auswertung. Diagnose, Logger und lokales Portal sitzen als
klar getrennte Funktionsbereiche am Gerät. Eine kleine Legende trennt Device
und Host. Typografischer Fokus auf der Anwendung, keine gleich großen Feature-Kacheln.

**Headline:** `Build the application. Reuse the infrastructure.`

**Einsatz:** Hero-Grafik im README. Zeigt sofort Umfang und Zusammenspiel.

### 2. From test drive to usable data

**Aussage:** Aufzeichnung und spätere automatische Abholung bilden einen Workflow.

**Bild:** Drei große Szenen entlang einer Zeitachse: Bench, Vehicle, Archive.
Ein türkisfarbener Datenpfad läuft durch alle Szenen. Unter der Fahrt liegt ein
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
