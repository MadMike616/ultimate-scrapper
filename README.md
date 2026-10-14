# Ultimate Scrapper

Ultimate Scrapper is a standalone D2RLoader client plugin with a Horadric Cube transmutation. Place the configured number of matching input items in the cube and press Transmute.

Choose an active output mode in `d2rloader/config/ultimate-scrapper.toml` under the active mod. Changes take effect after restarting D2R.

## Default behavior

With the shipped TOML unchanged, Ultimate Scrapper behaves like Unique/Set Scrapper: it accepts exactly one Unique or Set item by itself and transmutes it into 0, 1, 2, or 3 random runes using the same default failure chance, output-count odds, and rune weights. The default output mode is `Runes`, and batch scrapping is off. Ultimate Scrapper adds optional input rules, Custom and Both output modes, batch scrapping, and button-routing settings; these only change behavior when the user chooses to edit the TOML.

The Horadric Cube and stash panels can show a **Batch scrap** checkbox, unchecked each time the plugin starts. Set `batch-scrap-always-on = true` in the root `[ultimate-scrapper]` table to keep batch processing on and omit the checkbox and label from the panels; this takes precedence over `show-batch-checkboxes`. It defaults to `false`. When it is false, `show-batch-checkboxes = false` hides the controls from the active layouts while retaining existing widgets in saved Native/Separate variants, so setting it back to `true` restores them at their saved positions. When batch processing is on, the plugin can process several cube items in one click if every item matches a configured one-item input rule. Each item gets its own output-mode, failure, output-count, and output-item rolls. Multi-item input recipes still run together as one recipe when the cube does not match a batch of one-item rules. The batch transaction consumes or creates all items together; if the output would exceed ItemService's capacity or the transaction cannot complete, all inputs are left untouched. The checkbox state applies to plugin recipes triggered from either panel.

## Inputs

The `[ultimate-scrapper.input]` settings apply to Runes, Custom, and Both modes. A flat `inputs` array describes one recipe and requires every listed item. Nested arrays describe OR alternatives, with each inner array forming a complete recipe; for example, `inputs = [["amu"], ["rin", "belt"]]` accepts one amulet or one ring and one belt. Each recipe's length sets its required input count, so no separate count setting is needed. All entries within a selected recipe must match distinct cube items. Use a 3- or 4-character item code for an exact base-item match, or prefix an itemtype code with `type:`, such as `"type:armo,normal"` for any normal armor. Itemtype codes contain 1 through 4 characters and match the active mod's item type hierarchy, including `Equiv1` and `Equiv2` parents. Type matching reads `itemtypes.txt`, `armor.txt`, `weapons.txt`, and `misc.txt` from the active mod's `data/global/excel` folder. Use `"any"` to match any item code, as in `"any,rare"` for any rare item. An entry without a quality accepts any quality; supported qualities are `inferior`, `normal`, `superior`, `magic`, `set`, `rare`, `unique`, and `crafted`. Set `inputs = [""]` to use the default recipe, which accepts exactly one Unique or Set item of any code. Each recipe must contain 1 through 12 valid entries; an invalid list disables the recipe until corrected. Any leftover `input-count` setting in an older config is ignored.

## Output modes

### Runes

`active-output-mode = "Runes"` preserves the original recipe: each transmutation has a 30% failure chance by default. On success it produces 1 rune with 70% probability, 2 runes with 25%, or 3 runes with 5%. Each rune is selected independently from El through Zod; rune weights step down 20% per rune by default and are normalized across all 33 runes. These settings remain in the root `[ultimate-scrapper]` table: `failure-chance-percent`, `output-count-weights`, and `rune-weights`.

### Custom

Set `active-output-mode = "Custom"` to use the separate `[ultimate-scrapper.custom]` settings. `failure-chance-percent` controls the chance that the input and its socket contents are consumed without output. `item-level` sets the level for every output, from 1 through 99. The nested `[ultimate-scrapper.custom.output-count-weights]` table controls how many random items are selected after a successful roll. Set the success-only weights for `one` through `twelve`; they must sum to 100. This allows up to 12 random outputs, in addition to any guaranteed outputs. The defaults preserve the previous behavior of always selecting one random item. Each output is selected independently from `[[ultimate-scrapper.custom.items]]`, so the same entry can be selected more than once. Set `guaranteed = true` on any item entry to add one copy of it to every successful recipe in addition to the random outputs. Guaranteed entries are excluded from random selection; their `weight` does not affect the random pool. Entries require a D2 item `code`; `quality` may be one supported name, a comma-separated list such as `"magic,rare"`, blank (`quality = ""`), or omitted. A list chooses uniformly from only its named qualities, excluding all others. A blank or omitted quality chooses uniformly from every quality valid for that item code, so a rune or gem resolves to Normal while jewelry, equipment, and other multi-quality items can roll among their eligible qualities. `quantity` defaults to the game's normal amount for the item.

Supported custom qualities are `inferior`, `normal`, `superior`, `magic`, `set`, `rare`, `unique`, and `crafted`. The blank-quality selection uses the active mod's item tables, available set and unique records, and enabled crafting recipes. Set and unique outputs use a random compatible game record. If the game cannot create and place every requested output, the transaction rolls back and leaves the input untouched.

### Both

Set `active-output-mode = "Both"` to choose between Runes and Custom with equal 50% probability on each transmute. The chosen mode uses its own failure chance and output settings. The mode is selected before the failure roll, so each attempt first chooses which mode's recipe settings to use.

The shipped TOML includes a sample custom output pool using item codes `r01`, `gpv`, and `amu`, each with equal weight and blank quality. Edit or replace those entries to configure your output pool. The plugin validates the active mode's settings during load and reports invalid settings in the D2RLoader log.

## Behavior and compatibility

`transmute-button-mode` selects button layout and routing. `"Native"` removes the plugin-owned button from the active layout, checks configured inputs when native Transmute is pressed, and passes unmatched contents to D2R for native `cubemain.txt` recipes. When both a plugin input and native recipe match, the plugin recipe takes priority. `"Separate"` keeps the plugin-owned button visible, always passes native Transmute clicks through (including recipe overlaps), and reserves the added button for plugin recipes. On first initialization, the plugin adds its button with default coordinates without replacing other widgets, text, styles, or layout fields. Later starts preserve the saved coordinates.

On first load, the plugin copies the active mod's cube and stash layouts into per-install Native and Separate variants under `d2rloader/config/ultimate-scrapper-layouts/`. It leaves the Native variants without the plugin button and injects the Repair All button widget into the Separate variants. The Batch scrap checkbox is injected into both cube variants and both stash variants. Default checkbox positions are Native cube `(545, 1088)` and stash `(825, 1402)`, and Separate cube `(663, 1088)` and stash `(825, 1530)`. Its text label is a child of the checkbox, so changing the checkbox's `rect` coordinates moves both together; the label's own `rect` coordinates are offsets from the checkbox. Existing layout variants are migrated to this nested structure while preserving the label's current on-screen position. All other JSON content is preserved. The four files are named `horadriccubelayouthd.native.json`, `horadriccubelayouthd.separate.json`, `bankexpansionlayouthd.native.json`, and `bankexpansionlayouthd.separate.json`. Once those variants exist, the plugin does not reseed them from the active mod files or reset button coordinates. If a checkbox is missing when its layout is first initialized, it is added once to that layout's variants; separate markers prevent later reinjection if a user removes it. At startup it copies the selected mode's variants into the active mod's same-named layout files; this is how a changed Native/Separate setting takes effect. Edit the per-install variants to customize button positions and the label offset. Players can use the same active filenames while keeping their own contents and appearance; the repository's example layouts are not installed over their files. The plugin preserves a one-time `.ultimate-scrapper.bak` copy of each active layout as a recovery backup.

Accepted plugin actions perform the exchange as one D2RLoader `ItemService` transaction on the game thread. All rune and gem outputs are identified. Other outputs with the `misc` itemtype are identified except amulets, rings, quivers, charms, and torches. A failed recipe consumes the input and any socket contents. Quantity-bearing inputs are consumed at their full current quantity, removing the whole item. If the input changes before the transaction, or an output transaction fails, the input remains untouched. The plugin does not edit save files or `cubemain.txt` directly; D2R records successful outputs in the inventory normally.

This is a client plugin and requires an authoritative local game or TCP/IP host. Remote clients cannot perform the item transaction.

## Install

Build the DLL and copy `d2rl-ultimate-scrapper.dll` to the active mod's `d2rloader/plugins/` directory. Restart D2R through D2RLoader. The plugin creates `ultimate-scrapper.toml` and per-install layout variants on first load. The JSON files under `assets/grailerrl-ui/` are example layouts for the GrailerRL setup; do not copy them over a player's existing layouts unless they specifically want those layouts.

## Build

Requires Windows x64, CMake 3.29 or newer, Visual Studio 2022 or newer with the Windows SDK, and D2RLoader PluginSDK ABI 4. CMake fetches the pinned SDK commit `6eb8f8b6192868214706bd6d528c5294f2f551b7` on the first configure. To use a local SDK checkout:

```powershell
cmake -S . -B ..\ultimate-scrapper-build -A x64 `
  -DULTIMATE_SCRAPPER_LOCAL_SDK_DIR="<PluginSDK checkout>"
cmake --build ..\ultimate-scrapper-build --config Release --target ultimate_scrapper
```

The output is `../ultimate-scrapper-build/Release/d2rl-ultimate-scrapper.dll`. The build fetches TOML++ v3.4.0. Manual builds need the generated `default_config.hpp` from the CMake configure step and the TOML++ include directory in addition to the SDK.

## Compatibility

Targeted at Windows x64, D2R 3.3.93847, and D2RLoader 1.3.1-beta (PluginSDK ABI 4). The source is based on the established Unique/Set Scrapper implementation pattern; this separately identified build has not yet been verified in-game.

See `THIRD_PARTY_NOTICES.md` for notices covering TOML++ and D2RLoader PluginSDK. This project is licensed under the MIT License; see `LICENSE`.

