# ESPCars infographic layout drafts

Layout iteration v3, 2026-10-13. Visual direction: technical OEM presentations,
white background, navy typography and blue data paths. High-quality vehicle and
board renders provide the visual base; SVG labels and data paths explain the
system. The board is the main subject. The smaller car is a connection context:
no functions, nodes, storage or feature leaders are placed within the vehicle.
These are editable review drafts, not yet embedded in the main README.

## Current device-centred drafts (v3)

| Draft | Preview | Editable SVG composition |
|---|---|---|
| Device platform and vehicle connection | [PNG](01-device-platform-render-v3.png) | [SVG](01-device-platform-render-v3.svg) |
| Device recording and host collection | [PNG](02-device-data-render-v3.png) | [SVG](02-device-data-render-v3.svg) |

![Device platform draft](01-device-platform-render-v3.png)

![Device recording and host collection draft](02-device-data-render-v3.png)

## Earlier rendering iteration (v2, superseded)

This iteration placed feature markers inside the vehicle. The current direction
removes all such localization and reduces the visual weight of the car.

| Draft | Preview | Editable SVG composition |
|---|---|---|
| In-vehicle platform and hardware detail | [PNG](01-platform-render-v2.png) | [SVG](01-platform-render-v2.svg) |
| Recording and collection workflow | [PNG](02-field-data-render-v2.png) | [SVG](02-field-data-render-v2.svg) |

The SVGs embed their raster assets, so they remain portable without separate image
files. All text, labels, leaders, arrows and timeline elements are editable vector
elements. The original transparent assets remain available separately:
[vehicle](assets/vehicle-render-v2.png), [board](assets/board-render-v2.png).
The [generation prompts](render-prompts.md) document the art direction.

Vehicle and board are illustrative concepts, not a specific OEM vehicle or an
exact depiction of the manufactured reference PCB. Connections and data paths
are schematic. The hardware illustration is not a pinout or wiring guide.

## Original vector drafts (v1)

| Draft | Preview | Editable source |
|---|---|---|
| Platform architecture | [PNG](01-platform.png) | [SVG](01-platform.svg) |
| Recording and collection workflow | [PNG](02-field-data.png) | [SVG](02-field-data.svg) |

All versions use a 1600 × 900 SVG viewBox and 3200 × 1800 PNG previews.
Text uses Arial with Helvetica/sans-serif fallbacks. V1 contains only vector
geometry; v2 and v3 combine embedded raster illustrations and vector annotations.
No version requests remote assets or fonts. V3 reuses the existing render assets;
no new raster generation was needed for this composition change.

The platform diagram distinguishes the on-device firmware components from the
host-side collector, archive, decoder and publisher. Data flows towards the host;
the collection request originates at the host. The workflow illustrates local
recording without WiFi and subsequent collection when the host can reach the
device. The timing and signal sketches are schematic and contain no measurements.

![Platform architecture draft](01-platform.png)

![Recording and collection workflow draft](02-field-data.png)
