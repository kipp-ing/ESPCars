# Vehicle and board render prompts

Generated with the built-in image-generation tool for the second infographic
layout iteration. Both source assets request transparent backgrounds. SVG text,
leaders and data paths are composed separately; the render prompts intentionally
contain no labels or diagram arrows.

The board is an illustrative hardware concept, not a reproduction of the
manufactured ESPCars reference PCB. The vehicle is a generic conceptual design.

## Vehicle asset

```text
Use case: product-mockup.
Asset type: photorealistic 3D automotive engineering illustration, isolated asset for an OEM-style technical presentation with separate SVG annotations.
Primary request: A beautiful generic modern sporty electric fastback car, premium proportions, restrained industrial design, brushed metallic silver body with dark graphite wheels. A subtle technical cutaway on the visible side opens a window into the cabin and lower chassis, showing plausible wiring harness routes, a few small generic ECU enclosures and the underfloor battery pack. Most of the sleek exterior stays intact and polished. The subject should look like a high-end automotive CAD marketing render, credible and attractive, rather than a toy or a sketch.
Composition: wide landscape isolated full vehicle in front-left three-quarter view, nose points toward the left, visible side faces the viewer, camera slightly elevated. Entire car and all wheels fit comfortably with transparent margins. Car fills the frame; no other objects.
Lighting: soft bright studio light designed for placement on a white presentation slide, subtle ambient occlusion and delicate contact shadow, precise material detail, silver, graphite and muted cool tones.
Constraints: genuinely transparent background. No scene or road, no labels, no typography, no arrows, no callout lines, no logos, no brand marks, no watermark. Conceptual generic vehicle, not a depiction of any manufacturer's real model. The cutaway should be restrained and readable, not an explosion of parts.
```

## Board asset

```text
Use case: product-mockup.
Asset type: photorealistic 3D embedded hardware concept render, isolated asset for an OEM-style technical presentation with separate SVG annotations.
Primary request: A single compact, plausible ESP32 automotive development PCB with peripheral interfaces. Dark navy solder mask, detailed copper traces, silver metal shield on a WiFi microcontroller module with a PCB antenna zone, two CAN transceiver sections, a LIN transceiver section, practical pluggable screw terminal connectors, a microSD socket with card inserted, USB-C, small power regulation components, capacitors, pin headers and four mounting holes. Components arranged neatly as on a professional prototype board, with believable small-package proportions.
Composition: elevated isometric three-quarter view so the board top and all major peripherals are visible; square isolated composition, board fills the frame with transparent margins. Flat assembled PCB, not an exploded view.
Lighting/materials: premium realistic product CAD render, bright soft studio lighting for a white presentation slide, navy PCB, natural silver components, restrained blue connector accents, fine solder and subtle ambient occlusion.
Constraints: genuinely transparent background, no desk or other objects, no labels, no typography, no arrows, no wiring callouts, no logos, no brand marks, no watermark, no glowing science-fiction parts. This is a conceptual illustration, not an exact manufactured ESPCars reference board.
```
