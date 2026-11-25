# Stock racer re-spec: current vs real (draft for validation)

Goal: give each stock racer the engine and weight of its real counterpart
(owner's mapping), in the Java part classes and the CFG masses, with the
#407 fuel model. This page is the table to validate **before** any class is
changed.

## How the current figures were measured

A throwaway VM probe assembled each of the 24 models with
`vehicle_spawn_stock` on a private copy of the stock MWM game plus the full
`patch/` (branch `bug/407-fuel-knock-stoich`), then read the block's
`DynoData` after `Chassis.updatevariables` (host dyno, net crank power) and
summed `Part.getMass` over the installed tree. Power is shown in PS (ch);
the host reports mechanical hp (× 1.014).

**Weight is total assembled mass** (what the physics uses). The game UI
(`CarInfo` "Mass", Garage, Catalog delivery price) calls `Chassis.getMass()`,
which the host answers with the chassis CFG alone (838.5 kg for the
CoupeSport instead of 1,524 kg). That display gap is a separate host issue.

Real figures are manufacturer data from memory (DIN curb weight, PS); the
ones marked `?` need a source check.

## Base game calculations vs current

Same probe, same 24 stock cars, four configurations. Peak power (PS) and
torque (Nm) up to the rev limit, and the rpm below which the net torque is
negative (500–750 rpm is normal: idle is not reached yet).

- **Game display**: stock MWM classes and the stock dyno output scales, as the
  stock dyno screen shows them: torque = cyl·W/8 (1.57 × the real cycle
  torque, no friction) and power = cyl·W·rpm/60 − 2e-8·rpm³ (2 × the real cycle
  power). These two figures disagree with each other (power ≠ T·ω), and
  driving used 2 × that torque.
- **Stock Java, SI**: the same stock classes and the same combustion cycle,
  converted to SI net crank torque and P = T·ω (#203 host math), with nothing
  else changed. This is the physical output of the stock data.
- **main**: plus the #203 Java patch (port-flow law, fuel heat × 1.73).
- **#407**: plus ethanol blends and knock.

No step adds a power coefficient: the host change is a unit conversion, the
Java changes are the port-flow law and the fuel heat.

| Car | Game display | Stock Java, SI | main | #407 | Negative torque below (rpm): stock → #407 |
| --- | --- | --- | --- | --- | --- |
| Baiern CoupeSport | 198 / 206 | 96 / 128 | 198 / 243 | 195 / 240 | 750 → 750 |
| Baiern CoupeSport GT III | 464 / 379 | 226 / 235 | 347 / 398 | 340 / 390 | 750 → 750 |
| Baiern DevilSport | 316 / 308 | 155 / 192 | 275 / 334 | 271 / 329 | 750 → 750 |
| Duhen SunStrip 1.5 DVC | 144 / 142 | 67 / 89 | 107 / 139 | 107 / 138 | – |
| Duhen SunStrip 1.8 DVC | 165 / 160 | 78 / 98 | 117 / 151 | 117 / 151 | 750 → 750 |
| Duhen SunStrip 2.2 DVC | 205 / 233 | 99 / 147 | 125 / 220 | 125 / 219 | 500 → 500 |
| Duhen Racing 2.0 CDVC | 301 / 227 | 144 / 135 | 169 / 165 | 165 / 163 | 500 → 500 |
| Einvagen 110 GT | 130 / 163 | 63 / 101 | 96 / 158 | 96 / 157 | 500 → 500 |
| Einvagen 110 GTK | 171 / 215 | 84 / 134 | 127 / 206 | 126 / 205 | 500 → 500 |
| Einvagen 140 GTA | 238 / 266 | 117 / 166 | 147 / 227 | 146 / 227 | 500 → 500 |
| Ishima Enula WRY | 180 / 192 | 86 / 120 | 109 / 180 | 109 / 180 | – |
| Ishima Enula WRZ | 223 / 227 | 107 / 142 | 139 / 221 | 139 / 219 | – |
| Ishima Enula WR SuperTurizmo | 371 / 347 | 181 / 216 | 219 / 283 | 205 / 265 | – |
| Shimutshibu Focer RC 200 | 190 / 205 | 91 / 128 | 125 / 195 | 125 / 194 | – |
| Shimutshibu Focer RC 300 | 256 / 263 | 124 / 163 | 173 / 241 | 172 / 238 | – |
| Shimutshibu Focer WRC | 383 / 410 | 188 / 258 | 257 / 392 | 245 / 375 | – |
| MC GT | 364 / 359 | 178 / 225 | 236 / 368 | 235 / 366 | 500 → 500 |
| MC GTB | 374 / 366 | 183 / 229 | 250 / 375 | 249 / 374 | 500 → 500 |
| **MC GTLE** | 553 / 628 | 273 / 396 | 273 / 310 | 272 / 309 | **500 → 2,000** |
| Emer Nonus Street GT | 271 / 267 | 132 / 166 | 173 / 230 | 171 / 228 | – → 500 |
| Emer MotorSport Nonus GT II | 628 / 483 | 304 / 302 | 429 / 477 | 407 / 457 | – |
| **Prime DLH 500** | 499 / 423 | 243 / 264 | 223 / 240 | 211 / 227 | **500 → 2,250** |
| **Hauler's SuperDuty 500** | 388 / 480 | 193 / 302 | 128 / 200 | 128 / 199 | **500 → 2,500** |
| **Hauler's SuperDuty Extra 750** | 529 / 733 | 263 / 465 | 180 / 281 | 179 / 279 | **500 → 2,500** |

Hauler's SuperDuty 500 torque (Nm) by rpm:

| rpm | 1,000 | 1,500 | 2,000 | 2,500 | 3,000 | 3,500 | 4,000 | 4,500 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Stock Java, SI | 72 | 132 | 172 | 203 | 247 | 279 | 302 | 301 |
| #407 | −101 | −90 | −56 | −11 | 63 | 127 | 180 | 199 |

### Diagnosis

- 20 of the 24 cars keep the stock curve shape; the #203 fuel heat raises
  their output by 30–90 % over the stock data in SI. #407 changes peaks by at
  most −6 % (knock on the boosted and 98-octane engines).
- The four broken curves share one cause. Eight camshafts
  (`MC_Prime_SuperDuty_{L,R}_DOHC_{intake,exhaust}_camshaft`,
  `SuperDuty_{L,R}_{intake,exhaust}_camshaft`) declare a closed-valve lift of
  1.27 mm (0.050 in, the lift at which cam duration is measured); the 24
  other cams declare 0. The stock law squared the opening (X²/V), so that
  leak was about 0.5 % of the open flow; the #203 linear curtain law makes it
  12 %. Both valves stay ajar through compression and combustion, and the
  engine loses torque below 2,500 rpm. These cams are on the Hauler, Prime and
  MC GTLE stock engines.
- So the curves are ugly because of **data** that the new law reads
  literally, not because of a coefficient: re-spec those eight cams (closed
  lift 0, a seated valve, as every other cam), then compare again.
- Done in #409. Only those four cars change; their curves are positive from
  1,000 rpm again (main + #409, PS / Nm peak): Hauler 500 342 / 561, Extra 750
  503 / 901, Prime 298 / 450, MC GTLE 361 / 613. They now exceed their real
  targets below, like most cars after the #203 fuel heat; the engine data
  re-spec brings them back.

## Outcome (#411)

The owner chose realistic targets over the base game's displayed power, kept
the #203 flow law and the stock weights, and did not ask for real
displacements. #411 re-specs performance data only; results per car are in
[patch](../../patch/README.md#stock-engine-re-spec-411). The real-car table
below remains the reference for targets.

## Engines and weight

| In game | Real target | Engine now | Engine target | PS now → target | Nm now → target | Mass now → target |
| --- | --- | --- | --- | --- | --- | --- |
| Baiern CoupeSport | BMW E36 325i (M50B25) | I6 2,714 cc, 80×90, 10.4:1, 98, limit 7,000 | I6 2,494 cc, 84×75, 10.0:1, 95, ~6,500 | 195 @6,250 → 192 @5,900 | 240 @4,750 → 245 @4,700 | 1,524 → 1,325 |
| Baiern DevilSport | BMW E36 M3 3.2 (S50B32) | I6 3,587 cc, 93×88, 10.0:1, 98, 7,250 | I6 3,201 cc, 86.4×91, 11.3:1, 98, 7,600 | 271 @6,250 → 321 @7,400 | 329 @5,000 → 350 @3,250 | 1,479 → 1,460 |
| Baiern CoupeSport GT III | race E36 M3 (no road car) | I6 3,587 cc, 10.0:1, 98, 8,500 | decide | 340 @7,250 → ? | 391 → ? | 1,167 → ? |
| Duhen SunStrip 1.5 DVC | CRX del Sol S (D15B7) | I4 1,497 cc, 80.8×73, 10.2:1, 95, 7,500 | I4 1,493 cc, 75×84.5, 9.2:1, 95, ~6,500 | 107 @7,000 → 103 @5,900 | 138 @3,000 → 133 @5,000 | 1,222 → 1,045 |
| Duhen SunStrip 1.8 DVC | del Sol Si/ESi (D16Z6) | I4 1,796 cc, 88.5×73, 9.8:1, 95, 6,750 | I4 1,590 cc, 75×90, 9.2:1, 95, ~7,000 | 117 @6,500 → 127 @6,600 | 151 @4,500 → 144 @5,200 | 1,243 → 1,080 ? |
| Duhen SunStrip 2.2 DVC | del Sol VTi/VTEC (B16A2) | I4 2,174 cc, 93×80, 9.9:1, 95, 7,500 | I4 1,595 cc, 81×77.4, 10.2:1, 95, 8,200 | 125 @5,250 → 160 @7,600 | 219 @2,500 → 150 @7,000 | 1,226 → 1,110 ? |
| Duhen Racing SunStrip 2.0 CDVC | race turbo (no road car) | I4 1,984 cc, 9.5:1, 2.0 bar, 9,000 | decide | 165 @7,750 → ? | 163 → ? | 993 → ? |
| Einvagen 110 GT | Golf III 1.8 (ADZ) | I4 1,810 cc, 80×90, 10.3:1, 95, 6,500 | I4 1,781 cc, 81×86.4, 10.0:1, 95, ~6,300 | 96 @4,750 → 90 @5,500 | 157 @3,750 → 145 @2,500 | 1,324 → 1,060 ? |
| Einvagen 110 GTK | decide: Golf III VR6 (AAA) power, or GTI 8v (ABA) | I4 1,810 cc, 7.5:1, supercharger 3.1, 6,500 | VR6: 2,792 cc, 174 PS (block stays I4 + SC) | 126 @5,000 → 174 @5,800 | 205 @3,750 → 235 @4,200 | 1,373 → 1,220 ? |
| Einvagen 140 GTA | Golf III GTI 16V (ABF) | I4 2,445 cc, 93×90, 10.3:1, 95, 6,500 | I4 1,984 cc, 82.5×92.8, 10.4:1, 98, ~6,800 | 146 @5,000 → 150 @6,000 | 227 @4,000 → 180 @4,800 | 1,360 → 1,180 ? |
| Ishima Enula WRY | Impreza 2.0 (EJ20 NA) | flat-4 2,038 cc, 93×75, 9.3:1, 95, 7,500 | flat-4 1,994 cc, 92×75, 9.5:1 ?, 95 | 109 @5,500 → 115 @5,600 ? | 180 @2,500 → 172 @4,000 ? | 1,546 → 1,200 ? |
| Ishima Enula WRZ | Impreza GT/WRX (EJ20G) | 2,038 cc, 9.3:1, turbo 1.15, 7,500 | 1,994 cc, 8.0:1, turbo, 98 | 139 @6,000 → 218 @5,600 | 219 @3,500 → 290 @4,000 ? | 1,560 → 1,250 |
| Ishima Enula WR SuperTurizmo | Impreza 22B STi (EJ22G) | 2,445 cc, 93×90, 9.0:1, turbo 2.0, 7,500 | 2,212 cc, 96.9×75, 8.0:1, turbo, 98 | 205 @6,500 → 280 @6,000 | 265 @5,250 → 363 @3,200 | 1,470 → 1,270 |
| Shimutshibu Focer RC 200 | decide: no naturally aspirated Evo | I4 1,999 cc, 94×72, 10.3:1, 95, 7,500 | decide | 125 @5,750 → ? | 194 → ? | 1,475 → ? |
| Shimutshibu Focer RC 300 | Lancer Evo VI GSR (4G63T) | I4 1,999 cc, 94×72, 8.8:1, turbo 1.5, 7,500 | I4 1,997 cc, 85×88, 8.8:1, turbo, 98, ~7,500 | 172 @6,000 → 280 @6,500 | 238 @4,750 → 373 @2,750 | 1,493 → 1,360 |
| Shimutshibu Focer WRC | decide: Evo VI TME (road) or Group A WRC | 1,999 cc, 8.8:1, turbo 4.0 | decide | 245 @5,750 → 280 (TME) / ~300 (WRC) | 375 → 373 / ~500 | 1,395 → 1,365 (TME) / 1,230 (WRC) |
| MC GT | Camaro Z28 LT1 (1993–97) | V8 6,474 cc, 107×90, 9.1:1, 95, 6,500 | V8 5,735 cc, 101.6×88.4, 10.4:1, 95, ~5,700 | 235 @5,750 → 279 @5,000 | 366 @3,250 → 441 @2,400 | 1,903 → 1,560 |
| MC GTB | Camaro Z28 LS1 (1998–2002) | V8 6,474 cc, 9.8:1, 95, 6,500 | V8 5,665 cc, 99×92, 10.1:1, 95, ~6,000 | 249 @6,000 → 309 @5,200 | 374 @3,750 → 454 @4,000 | 1,905 → 1,570 |
| MC GTLE | Camaro SS LS1 | V8 8,129 cc, 107×113, 9.3:1, 95, 6,500 | V8 5,665 cc, 10.1:1, 95 | 272 @6,750 → 324 @5,200 | 309 @5,500 → 461 @4,000 ? | 1,905 → 1,600 ? |
| Emer Nonus Street GT | Skyline R34 GT-R (RB26DETT) | I6 2,597 cc, 86×74.5, 10.0:1, 98, NA, 8,000 | I6 2,568 cc, 86×73.7, 8.5:1, twin turbo, 98, ~8,000 | 171 @6,000 → 280 @6,800 | 228 @4,500 → 392 @4,400 | 1,670 → 1,560 |
| Emer MotorSport Nonus GT II | decide: JGTC GT500 R34 (race) | I6 3,199 cc, 10.0:1, 100, NA, 10,000 | decide | 407 @7,750 → ? | 457 → ? | 1,490 → ? |
| Prime DLH 500 | engine = Hauler's | V8 7,000 cc, 104×103, 9.7:1, 98, 8,000 | follows the Hauler engine | 211 @7,500 | 227 @6,250 | 1,840 → no real target given |
| Hauler's SuperDuty 500 | Dodge Ram 1500 5.9 Magnum (2nd gen) | V8 5,395 cc, 90×106, 7.9:1, 95, 4,500 | V8 5,899 cc, 101.6×90.9, 8.9:1, 95, ~5,000 | 151 @5,000 → 248 @4,000 | 212 @5,000 → 454 @3,200 | 2,431 → 2,000 ? |
| Hauler's SuperDuty Extra 750 | decide: Ram 2500 8.0 V10 power (block stays V8 + SC) | V8 5,395 cc, 7.9:1, supercharger 1.15, 4,500 | V10 7,997 cc, 300 hp | 210 @5,000 → 304 @4,000 | 295 @5,000 → 610 @2,800 | 2,477 → 2,500 ? |

## What the re-spec changes

- **Engine classes** (`parts/engines/*/scripts`): block bore and cylinder
  layout, crankshaft stroke, rod length and piston crown (compression), head
  chamber volume, valves and cams (where power and torque peak), redline and
  idle, fuel rail fuel type and mixture, chargers. Several heads, cams and
  rails are shared between models and catalog parts, so changing one moves
  every car that uses it.
- **Weights**: the chassis `_main_*.cfg` body masses (already shipped by the
  patch), scaled so the assembled car reaches the target. Engine trees weigh
  150–280 kg today and stay as they are unless a target needs it.
- **Names and descriptions**: a real displacement no longer matches names like
  "SunStrip 2.2" or "KraftWerk 2.5"; either rename or keep the fictional name.

## Decisions needed

1. The trims marked "decide": the race cars (GT III, Racing CDVC, Focer WRC,
   Nonus GT II), the Focer RC 200 (there is no naturally aspirated Evo) and the
   charged GTK / Extra 750 (real VR6 / V10, but the blocks are I4 / V8).
2. Nonus Street GT: the R34 GT-R is twin turbo, but the stock car has no
   turbo. Add stock turbos, or reach 280 PS naturally aspirated?
3. Weight reference: DIN curb weight (no driver) or EU (with 75 kg driver)?
4. Rename the engines and cars to their real displacements, or keep the names?
5. The Prime has no real-car weight target.
