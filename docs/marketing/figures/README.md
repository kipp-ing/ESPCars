# ESPCars architecture illustrations

README edition, 2026-10-13. Visual direction: technical OEM presentations,
white background, navy typography and blue data paths. The original SVG block
diagrams provide the layout: application, firmware, components and host tools
are logical modules. A small vehicle render is the only raster illustration.
There are no PCB renderings in the current README images. The car is a connection context:
no functions, nodes, storage or feature leaders are placed within the vehicle.
The current three illustrations are embedded in the main README. They use the
requested replacement vehicle render with an intact exterior and restore the
first platform-architecture and three-stage workflow layouts.

## README illustrations

| Illustration | PNG | Editable SVG |
|---|---|---|
| Platform overview | [PNG](platform-overview.png) | [SVG](platform-overview.svg) |
| Local recording and host data pipeline | [PNG](data-pipeline.png) | [SVG](data-pipeline.svg) |
| Inline CAN gateway and calibration | [PNG](inline-gateway.png) | [SVG](inline-gateway.svg) |

![Platform overview](platform-overview.png)

![Local recording and host data pipeline](data-pipeline.png)

![Inline CAN gateway and calibration](inline-gateway.png)

The replacement vehicle source is [vehicle-render-v5.png](assets/vehicle-render-v5.png).
Connections remain schematic and do not identify installation positions,
wiring locations or a hardware pinout. The gateway illustration uses application,
bridge, filter, rewrite and runtime-value blocks in place of the previous board render.

## Earlier board-render drafts (v3/v4, superseded)

| Draft | Preview | Editable SVG composition |
|---|---|---|
| Active device platform: gateway, calibration and recording (v4) | [PNG](01-active-platform-render-v4.png) | [SVG](01-active-platform-render-v4.svg) |
| Device recording and host collection | [PNG](02-device-data-render-v3.png) | [SVG](02-device-data-render-v3.svg) |
| Inline CAN gateway, signal manipulation and calibration (v4) | [PNG](03-inline-gateway-render-v4.png) | [SVG](03-inline-gateway-render-v4.svg) |

The gateway illustration shows the two CAN segments and independent forwarding
directions. Signal rewriting is distinct from CCP calibration access. Runtime
signal values can be supplied by automations or declared portal controls; CCP
memory writes require opt-in and supported access on the target ECU. The original
device overview remains available as [v3 PNG](01-device-platform-render-v3.png)
and [v3 SVG](01-device-platform-render-v3.svg).

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

The earlier vehicle and board renders are illustrative concepts. The board is
not an exact depiction of the manufactured reference PCB. Connections and data
paths are schematic. The hardware illustration is not a pinout or wiring guide.

## Original vector drafts (v1)

| Draft | Preview | Editable source |
|---|---|---|
| Platform architecture | [PNG](01-platform.png) | [SVG](01-platform.svg) |
| Recording and collection workflow | [PNG](02-field-data.png) | [SVG](02-field-data.svg) |

All versions use a 1600 × 900 SVG viewBox and 3200 × 1800 PNG previews.
Text uses Arial with Helvetica/sans-serif fallbacks. V1 contains only vector
geometry; current README diagrams combine vector geometry with only the vehicle
illustration. Earlier rendering experiments also contained PCB illustrations.
No version requests remote assets or fonts. V3 and v4 reuse the existing render assets;
no new raster generation was needed for this composition change.

The platform diagram distinguishes the on-device firmware components from the
host-side collector, archive, decoder and publisher. Data flows towards the host;
the collection request originates at the host. The workflow illustrates local
recording without WiFi and subsequent collection when the host can reach the
device. The timing and signal sketches are schematic and contain no measurements.

![Platform architecture draft](01-platform.png)

![Recording and collection workflow draft](02-field-data.png)
