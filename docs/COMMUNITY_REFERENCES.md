# Community resource and modding references

These notes summarize Discord/tutorial material supplied by the user on 11 November 2026. They are community guidance, not independently verified engine contracts. Validate each behavior against stock MWM/vanilla data and differential tests before relying on it.

## RPK identities and common resource types

The tutorials describe IDs as 0xXXXXYYYY: the high 16 bits identify an external pack/namespace and the low 16 bits identify a resource within the current RPK. This is consistent with the host's pack_id/local resource composition in RpkIndex. When adding resources, keep low-word typeid values unique within the RPK and use only hexadecimal digits.

| typeof | Community-described purpose | Typical RSD fields |
| --- | --- | --- |
| 5 | Mesh/SCX resource | sourcefile |
| 7 | Texture resource | sourcefile |
| 8 | Script/part and catalog placement | script, native part |
| 9 | Clickable/tunable shape | shape |
| 14 | Render/material definition | mesh and one or more texture IDs |

A typical render entry connects a type-5 mesh to type-7 textures through a type-14 resource. A type-9 entry can reuse the mesh as the clickable shape. These relationships are directly relevant to runtime texture identity, SDAT embedded-resource remapping, painter tests, and malformed-reference fixtures.

## resdecode / resconvert

The Resource Converter tutorials describe resdecode RPKNAME and resconvert RDBNAME, without the file extension, producing editable RDB text and compiled RPK data respectively. The original Resource Converter is linked by the supplied tutorial at http://vstanced.com/page.php?id=1302.

These are development-time conversion tools, not a replacement for the host's native RPK reader. Do not launch them from slrr_host or use them to bypass parser hardening. If a future fixture-generation script invokes them, run them only in an isolated temporary workspace with explicit paths/arguments, captured output, and a timeout. The supplied ShellExecute example is historical reference, not a recommended process-lifetime or security design.

Use RDB examples to generate small independent fixtures for:

- type-5/type-7/type-14 render references;
- duplicate IDs, missing meshes/textures, and wrong resource kinds;
- type-9 clickable shapes;
- type-8 script/catalog links;
- truncated RES/RSD declarations and invalid hexadecimal IDs.

## Suspension and camera configurations

The suspension tutorial provides examples of wheel, drive-arm, control-arm, axle, and hub slot placement. It emphasizes that suspension correction may require rotating an arm rather than translating it, and gives a front lower-arm example with the last rotation coordinate negative on the right and positive on the left.

Use these as candidate golden configurations for suspension_geometry and physics telemetry tests, not as universal sign rules. Record the source vehicle, side, slot ID, coordinate convention, and stock screenshot/telemetry before accepting a geometry change.

The camera tutorial distinguishes position from yaw/pitch/roll:

- yaw: rotation about the vertical axis;
- pitch: rotation about the longitudinal horizontal axis;
- roll: rotation about the transverse horizontal axis.

The ext_camera field order is described as differing from the inline camera example. Treat that ordering as a compatibility hypothesis to verify against stock CFG files.

## Dealer prevalence

The dealer tutorial identifies the prevalence multiplier in each car's VT script as controlling relative appearance frequency. Lower values should make a car less common and higher values more common. It also warns that reducing stock-car prevalence without alternatives can produce an empty dealer and a crash.

Add differential fixtures for zero prevalence, all-zero dealer pools, one eligible car, multiple weighted cars, and deterministic seeded selection. Do not modify stock VT scripts in the reference tree.

## Provenance

Community authors named in the supplied material: mindeliszz, Holy Shit Man/Alan/KeeJay, Mario, Svander, FJ, BB93, and Jesus Christ. The content includes the original Resource Converter link and examples; no additional external validation is implied by this document.
