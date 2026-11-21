# AO surface reconstruction comparison

ValoCity, Baiern, identical settled camera and simulation time 13.4, AO High,
strength 108%, radius 151 cm, shadow High / Very soft. Before: ddc633c.
After: projected-depth plane normals and receiver-plane filtering.

The captures are a local comparison, not a reproduction of every reported
pedestrian/curb silhouette. Broad triangular ground material patches also occur
with AO Off and are not evidence of AO. User confirmation of the exact reported
camera remains pending. These changes do not alter the shadow passes.

GPU regression: isolated near and distant planes must have no pixels darkened
by more than 3 RGB levels at either method / any quality. Contact geometry must
still produce AO; preview/capture views and direct light remain unaffected.
