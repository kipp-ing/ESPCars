# ESPCars infographic layout drafts

Initial layouts, 2026-10-13. Visual direction: technical OEM presentations,
white background, navy typography, blue data paths, explicit device/host boundaries.
These are editable review drafts, not yet embedded in the main README.

| Draft | Preview | Editable source |
|---|---|---|
| Platform architecture | [PNG](01-platform.png) | [SVG](01-platform.svg) |
| Recording and collection workflow | [PNG](02-field-data.png) | [SVG](02-field-data.svg) |

Each SVG has a 1600 × 900 viewBox. PNG previews are exported at 3200 × 1800.
Text uses Arial with Helvetica/sans-serif fallbacks; all diagram elements are
vector geometry and remain editable. There are no remote assets or font requests.

The platform diagram distinguishes the on-device firmware components from the
host-side collector, archive, decoder and publisher. Data flows towards the host;
the collection request originates at the host. The workflow illustrates local
recording without WiFi and subsequent collection when the host can reach the
device. The timing and signal sketches are schematic and contain no measurements.

![Platform architecture draft](01-platform.png)

![Recording and collection workflow draft](02-field-data.png)
