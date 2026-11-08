# Task Lists (Palm OS) - formerly "Daily Checklist"

Three recurring checklists in one Palm OS app: **Daily**, **Weekly**, **Monthly**.
Started as a port of the "Daily" list in the user's Rabbit r1 to-do creation (HTML/JS).

- Daily items uncheck themselves every day.
- Weekly / monthly items show a **due date** (last day of the current week / month). Once checked they
  show the **date completed**, and they uncheck when the next week / month begins, going back to showing the due date.
- Weeks start on **Monday** by default; Options > Week Starts On... changes it (saved in app preferences).
- A "3/7 done" count for the visible list is drawn at the bottom right.
- Items can be **beamed** one at a time (Beam button in the edit dialog) or a whole list (Item > Beam List).
- The first letter of a new item is capitalized as it is typed (key handler on the edit form, plus a safety net on save).

## Targets and tooling

- Target: **Palm OS 3.0 or newer** (68k). Must run on a Handspring Visor (Palm OS 3.1, 160x160, monochrome)
  and on newer color devices (OS 4.x/5.x, e.g. Zire 22, Tungsten T|X). Only use APIs that exist in Palm OS 3.0;
  check before using anything newer (beaming does: `BeamAvailable()`).
- The user builds with **CodeWarrior for Palm OS (IDE 5.2.1283), installed directly on Windows 11**
  (Palm OS 5 SDK headers in `C:\Program Files (x86)\Metrowerks\CodeWarrior\Palm OS Support\Incs`).
  An earlier Windows 2000 VM and a macOS toolchain attempt are no longer used.
- **Resources are built from `Rsc\tasklists.rcp` with PilRC 3.1** (CodeWarrior's PilRC plug-in; a stale
  `tasklists.rcp.ro` in `TaskLists_Data\Debug\ObjectCode` shows that is what the project does). Earlier notes said
  Constructor `.rsrc` was the source of truth - that is no longer the case. `Makefile` (prc-tools) no longer exists.
- Project file `TaskLists.mcp`, output `Debug\TaskLists.prc`. The project folder is `C:\Users\jakec\Documents\TaskLists`.
- Creator ID `JsDL`, type `appl`, launcher name "Task Lists", database `DailyChecklistDB` (name kept from v1).
- Claude cannot build the app. There is no gcc here, but PilRC is installed and **can be run to check the .rcp**:
  `pilrc.exe -I ..\Src tasklists.rcp <outdir>` from `Rsc` (path above, under `CW for Palm OS Tools\PilRC`).
  Check API symbol names by grepping the SDK `Incs` folder. Do not claim the C code compiles; ask the user to
  build in CodeWarrior and paste errors.

## Files

| File | Purpose |
|---|---|
| `Src\tasklists.c` | All application code |
| `Src\tasklists_res.h` | Resource IDs shared by code and PilRC |
| `Rsc\tasklists.rcp` | PilRC resources: forms, menu, alerts, icon / logo bitmap families |
| `Rsc\icon22*.bmp`, `icon44*.bmp` | Large launcher icon 22x22 and double density 44x44 (1-bit, `_gray` = 2-bit, `_color` = 8-bit) |
| `Rsc\icon15x9*.bmp`, `icon30x18*.bmp` | Small launcher icon 15x9 and double density 30x18, same variants |
| `Rsc\jakesoft*.bmp` | 136x51 About logo (1-bit, 8-bit color, plain alt) |
| `Tools\make_icons.py` | Regenerates the icon bitmaps (needs Python + Pillow). `--all` also rewrites the low-density 1-bit/8-bit files |

Gray (2-bit) BMPs are 4-bit BMP files using gray levels 0/5/10/15, the same trick as the Checklists project.
8-bit icons reuse the palette of `icon22_color.bmp`. After changing bitmaps, force CodeWarrior to recompile
`tasklists.rcp` (touch it or remove object code) because PilRC dependencies on the .bmp files may not be tracked.

## Data model

One record per item, big-endian, same layout as v1 (v1 stored 0 in the `kind` byte, so old items are daily):

| Offset | Size | Field |
|---|---|---|
| 0 | UInt32 | `stamp`: local day number (`TimGetSeconds() / 86400`). Daily: day last checked/reset. Weekly/monthly: day completed |
| 4 | UInt8 | `done` (0 or 1) |
| 5 | UInt8 | `kind`: 0 daily, 1 weekly, 2 monthly |
| 6 | Char[] | item text, NUL-terminated (max 40 chars) |

Optional trailer: a weekly/monthly item with a **custom due date** has a UInt32 day number right after the text's NUL
(absent = automatic due date). Set in the edit dialog (Due selector uses `SelectDay`, "Auto" button clears it); picking
the automatic date stores nothing. It is dropped (`ClearDue`) when the completed item resets at the next week / month.
Overdue unchecked items keep showing their past date. The due date is not beamed.

Limits: 200 items per list. Records are appended in creation order; the main table shows only the records
whose kind matches the selected tab (`gIdx[]` maps table position to record index). No reordering yet.

Reset rules (`ApplyReset`, at launch, once a minute while open, and after changing the week start):
daily resets when `stamp != today`; weekly/monthly reset when `done` and `stamp` is before the start of the current
week / month. Due date = last day of the current week / month. Day 0 (1904-01-01) was a Friday, so
`DayOfWeekOf(days) = (days + 5) % 7`, 0 = Sunday.

Dates are drawn as `M/D` or `D/M` depending on the device date-format preference (no year, to save width).

## Beaming

Payload (extension `.jdl`, target creator `JsDL`): `UInt8 version(1)`, `UInt8 kind`, `UInt8 count`, then `count`
NUL-terminated texts. Received items are added unchecked, to the list of the beamed kind. `ValidBeam()` checks the
whole payload before anything is stored. `ReceiveBeam()` and `RegisterBeaming()` run from launch codes without
globals, so they and their helpers use **no globals and no string literals** (the database is created there by
`CreateDatabase()` with the name built character by character, so beams work even before the app was first run).
Beaming is registered at launch, sync-notify and reset. **Not yet tested on real devices**, nor the trap-based
`BeamAvailable()` check for Palm OS 3.1 ROMs.

## Resource IDs (`tasklists_res.h`)

Main form 1000 (menu 1200): table 1001, scroll bar 1002, New 1003, tab push buttons 1004-1006 (consecutive, ID =
1004 + kind, group 1). Edit form 1100: field 1101, OK/Cancel/Delete/Beam 1102-1105, Due selector 1106, Auto 1107. Menu items 1201-1205.
Alerts 1300 delete, 1302 ROM, 1303 database, 1304 list full, 1305 beam failed, 1306 cannot beam.
About 1400 (OK 1401, logo bitmap 1402). Week form 1500: day buttons 1501-1507 (Sun-Sat, consecutive, group 2),
OK 1510, Cancel 1511. App icon `tAIB` 1000 / 1001.

## Code conventions

- C89 style: declarations at the top of blocks, `/* */` comments, nothing newer than Palm OS 3.0.
- Reads use `DmQueryRecord`. Writes use `DmGetRecord` / `DmReleaseRecord` and `DmWrite`. Call `DmResizeRecord`
  before checking a record out.
- Table rows store the record index via `TblSetRowID`; the draw callback and select handler read it back.

## Status

- Version 1 (daily only) was built and run by the user. Version 2 (this design) has **not been built yet**: only the
  .rcp was verified with PilRC 3.1 and symbol names checked against the SDK headers.
- Still to check on devices: beaming both ways (app running / not running), the Visor's monochrome layout (tabs row,
  10-row table, "Due 11/15" text width), weekly rollover on the chosen day, double-density and 2-bit icons.
- The edit form triggers a PilRC warning about a missing Graffiti State Indicator (harmless).

## Ideas for next steps

- Reorder items (move up/down).
- Delete/clear a whole list; "move to another list" in the edit dialog.
- 4-gray logo bitmaps; double-density About logo.
