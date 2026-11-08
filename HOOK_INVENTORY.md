# Hook Inventory — Ultimate Scrapper

## Plugin metadata

| Field | Value |
|---|---|
| Plugin name | Ultimate Scrapper |
| Plugin ID | ultimate-scrapper |
| Release version | 0.2.4 |
| Inventory updated | 2026-11-08 |
| Source revision | Tag v0.2.4 |
| D2R build(s) audited | D2R 3.3.93847 is the documented compatibility target; no in-game validation performed for this inventory |
| D2RLoader SDK / Plugin ABI | PluginSDK 0.4.0 / Plugin ABI 4 |
| Audit status | Complete |

## Scope and method

Inspected the complete plugin source tree, including src/plugin.cpp, src/plugin.rc, src/default_config.hpp.in, CMakeLists.txt, the shipped TOML files, example layouts, and dependency notices. Searched for direct native hook and patch installation, including inline hooks, relative CALL/JMP patches, byte patches, vtable/shared-slot replacements, MinHook, and Win32 input/message hooks. Cube and stash messages use D2RLoader shared-event and item services.

## Hook sites

| Site ID | Type | Target | Address / identifier | Operation | Guard / validation | Source reference | Notes |
|---|---|---|---|---|---|---|---|

No direct hook sites found in the audit.

## Known collision overlaps

| Other plugin | Shared target | Relationship | Evidence / notes |
|---|---|---|---|

None identified in the current inventory; this is not a compatibility guarantee.

## Audit notes

The shared-event listener and item transaction calls are SDK callbacks/services, not native or operating-system hooks. No direct hook-site changes were made from v0.2.3. No in-game validation was performed for this inventory.
