# Qt OpenVG ellipses collapsed to fragments

Qt 4 applications using the OpenVG graphics system could position text correctly while
rendering filled and stroked ellipses as one- or two-pixel fragments. Replacing an
ellipse with a 32-sided polygon produced the same symptom, so the failure was below
Qt's `drawEllipse` implementation.

Runtime probes showed that Qt submitted floating-point paths with unit scale and that
EKA2L1 calculated the expected 60-by-60 bounding box. The geometry was therefore valid
before the draw. The actual size loss was in `Context::render_elements`: it uploaded a
hard-coded six indices (two triangles), then issued the draw with the tessellator's full
index count. All later indices read unrelated buffer contents, which collapsed most of
the path around arbitrary vertices. The fix uploads `nr_indices * sizeof(uint32_t)` and
rejects empty indexed draws.

Three adjacent state and curve defects became visible once the full geometry was
submitted. They explained why a single screenshot could look correct while later
frames grew tails or lost parts of a circle:

- `Context::setup_buffers` left its input descriptors uninitialized. Descriptor flags
  could therefore vary between draws even though the caller set every field it used.
  Descriptors are now value-initialized before those fields are populated.
- A reused `Path` retained its previous libtess2 contours. The fill tessellator is now
  recreated for each tessellation.
- Bevel joins used a vertex from the opposite side of the stroke as the third triangle
  point. Joins now use the path point at the center, while valid miter joins retain
  their additional outer tip and fall back to the corrected bevel at the miter limit.
- Qt represents an ellipse with exact-diameter arc segments. A few ULPs of rounding
  could make the unit-circle discriminant slightly negative and enter the fallback.
  Paths whose endpoints genuinely exceeded the supplied radii also entered a fallback
  that could not reach the requested endpoint. Radii are now uniformly enlarged as
  required by the OpenVG arc algorithm, near-zero discriminants are clamped, and the
  last-resort approximation advances through both quadrants before ending exactly at
  the requested point.

The audit also found that stored path coordinates were copied without applying the
OpenVG `stored * scale + bias` decode required for every path datatype. Append and
modify operations now share that decoding path; this did not cause the unit-scale Qt
test case, but leaving it unfixed would reproduce the same class of size error for
compact integer paths.

The regression case is a Qt 4 widget drawing several changing approach circles and
filled hit circles in one frame. Validation sampled 12 frames at 0.7-second intervals
and another 24 frames at 0.25-second intervals across the complete animation; all
filled circles, strokes, and approach circles remained closed. The fix is deliberately
in the common OpenVG path renderer, so ellipses, polygons, curves, and non-Qt clients
use the same corrected submission and state handling.
