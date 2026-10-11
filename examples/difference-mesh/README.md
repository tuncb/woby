# Difference mesh examples

Open a `.woby` scene below with the new build. Select its comparison in the scene
tree to see the difference controls. The source OBJ files are in this folder.

| Scene | Expected result |
| --- | --- |
| `01-shifted-solids.woby` | Two cubes shifted by 2 units. Blue A-only volume: 200. Orange B-only volume: 200. Total: 400. Net: 0. |
| `02-distorted-solid.woby` | The top of B tilts through the top of A. A-only and B-only volumes: about 33.3333 each. Total: about 66.6667. |
| `03-identical-solids.woby` | A and B use the same source. No geometric difference; total volume is zero. |
| `04-open-surfaces.woby` | A flat sheet and a raised sheet with different mesh densities. Automatic walls close the gap. Gold marks the generated walls. |
| `05-hole-problem.woby` | A has an inner hole. Creation stops and reports two boundary loops on A. |
| `06-crossing-problem.woby` | The sheets cross. Creation stops and reports a self-intersection. |

The comparison is placed beside its sources. Use the eye controls to show or hide
A-only, B-only, or closure walls. Source changes make the result out of date; use
**Rebuild difference mesh** after a change.

The problem scenes intentionally fail. Their inputs remain visible so you can
inspect them. No boundary controls are required.

Run `python generate.py` from this folder to regenerate the OBJ inputs. This does
not change the saved scene settings.
