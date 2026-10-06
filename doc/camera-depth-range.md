# Scene-aware camera depth range

Viewport drawing, CPU/GPU picking, annotation projection, and screenshot exports
share the camera depth-range calculation. It derives clip distances from the
camera orientation and the scene's bounding sphere, including the offset between
the camera target and the scene center.

**Edit > Settings > Camera** contains field of view, automatic near clipping,
the retained manual near distance, and read-only effective near/far distances in
model units. New scenes enable automatic near clipping. The effective near plane
can increase to 1% of the scene center's forward depth, limited to half the distance
to the front of the bounding sphere. This improves depth precision on large
distant models while leaving space before their nearest possible geometry.
Inside or behind that sphere, automatic clipping retreats to `0.0001` model units
so close views do not inherit an oversized manual cutoff. Geometry closer than
that numerical minimum is still clipped.

Manual mode retains `SceneCamera.nearPlane` as the minimum clipping distance,
allowing deliberate cuts into nearby geometry. Switching to automatic mode does
not overwrite that value. Scene format 24 saves `automatic_near_plane` in live
cameras and named views. Files without that key retain manual behavior; enable
automatic clipping in Settings to use it on an older scene. Setting a near distance
through `camera set --near-plane N` selects manual mode unless the same command
also supplies `--automatic-near-plane true`.

The far plane retains the previous generous range for helpers and expands when
necessary to contain the scene beyond the camera target. Moving the target close
to the eye while looking toward a distant model no longer clips that model at
the old target-distance-based far plane.

`camera get` reports `automaticNearPlane`, the configured `nearPlane`, the derived
`effectiveNearPlane`, and the derived `farPlane`. The constant-time calculation
uses cached scene bounds; it does not scan triangles, edit camera or scene state,
change field of view, or construct a spatial index.

Scene rendering uses a reversed-Z projection with the native 32-bit floating-point
depth attachment: near maps to 1, far to 0, and nearer fragments pass a greater
depth test. The projection is constructed directly with reversed clip distances,
not by subtracting an already rounded forward depth from 1. This resolves distant,
closely spaced surfaces even when the camera is inside a large scene's bounds and
the configured near plane must stay small. Window, screenshot, and marker-ID
passes use the same convention. Annotation strokes use that projection too, with
a relative depth bias toward the eye so they do not jump through distant occluders.

CPU picking and persisted annotation projectors retain the forward convention,
with the same field of view and clip distances. Saved cameras and `.woby` files
therefore do not need conversion. Exactly coplanar geometry still has no unique
front surface and is not resolved by increasing depth precision.

CPU surface picking defines its ray with an interior depth sample and clips
each hit against its projection. This avoids an infinite ray endpoint when
float precision cannot represent the far plane at extreme far/near ratios.
