# Thin-region test models

Open a `.woby` scene below in Woby. Each scene contains one mesh-check analysis
with **Thin regions**, automatic updates, and the thickness map enabled.
Select the analysis in the scene tree to inspect the detector and its settings.
Select a finding to show its measured thickness line.

Each scene uses the OBJ file with the same name. Paths are relative, so you can
copy this whole folder to another location. You can also import the OBJ files
separately. Treat one coordinate unit as one millimeter; OBJ does not store units.
All models contain surface triangles, not FEM volume elements.

| Scene | Geometry | Saved limit | Expected result |
| --- | --- | --- | --- |
| [01-uniform-plate.woby](01-uniform-plate.woby) | 20 x 20 x 0.5 mm plate | Absolute: 0.6 mm | One region, measured minimum 0.5 mm. |
| [02-thin-web.woby](02-thin-web.woby) | 10 mm thick body joined to a 1 mm web | Relative: 0.2 | One region on the web, measured minimum 1 mm. |
| [03-two-necks.woby](03-two-necks.woby) | Three 10 mm thick blocks joined by 1 mm and 0.5 mm necks | Relative: 0.2 | Two separate regions with minima of 1 mm and 0.5 mm. |
| [04-tapered-plate.woby](04-tapered-plate.woby) | 40 x 20 mm plate; vertical thickness decreases from 6 mm to 0.5 mm | Absolute: 1.5 mm | One region near the thin end, with a gradual change in map color. |
| [05-hollow-box.woby](05-hollow-box.woby) | 20 mm outer cube and 18 mm inner cavity | Absolute: 1.1 mm | One connected wall region, measured minimum 1 mm. |
| [06-close-solids.woby](06-close-solids.woby) | Two 20 x 20 x 4 mm solids, separated by a 0.2 mm air gap | Absolute: 1.1 mm | No thin-region finding. The air gap must not count as solid thickness. |
| [07-open-web.woby](07-open-web.woby) | Model 02 with its thin end cap removed | Relative: 0.2 | Thickness unavailable. Four boundary edges mark the open end. |

Only the stated limit is enabled in each scene. If both absolute and relative
limits are enabled, a face can pass either limit.

## Checks to try

1. Start with **02-thin-web**. The thin web should be highlighted while the body
   stays outside the selected region. Select the finding and inspect its 1 mm
   minimum thickness. With three elements through thickness, the preliminary
   element-size limit is about 0.333 mm.
2. Open **03-two-necks**. Disable the relative limit. Enable the absolute limit
   and set it to **0.75 mm**. Only the 0.5 mm neck should remain.
3. Open **01-uniform-plate**. Reduce the absolute limit from **0.6** to **0.4 mm**.
   The finding should disappear. Restore 0.6 mm to show it again.
4. Open **04-tapered-plate**. Change the absolute limit to move the selected region
   along the taper. Triangles span 2 mm intervals along X, so the selection changes
   in steps. Sampled ray lengths need not equal the vertical thickness, and they
   do not reach the exact 0.5 mm minimum at the end edge.
5. Open **06-close-solids** and **07-open-web** to check the two different outcomes:
   no finding on measured solid regions, and unavailable thickness on an open surface.

## Map and result status

Red means small thickness relative to that model's reference. Blue means reference
thickness or greater. Gray means unknown. Orange edges mark selected regions.
The map uses a separate reference for each analysis batch. The uniform plate and
hollow box have nearly uniform measured thickness, so their maps are blue even
though the absolute limits select their walls.

The plate, web, two-neck model, taper, and close solids report **Partial**. Some
edge or side faces do not have enough supported rays. Their known findings remain
available. The hollow box reports **Complete**. The open web reports **Unavailable**.
These are expected results with the current sampling method.

The scenes were copied to a temporary folder and opened through the Release
application. All seven passed checks for their expected findings and status.
The uniform plate also passed the 0.4 mm limit check. The two-neck model passed
the 0.75 mm limit check. No external models or downloads are required.

See [Thin regions](../../../doc/thin-regions.md) for the measurement method.
