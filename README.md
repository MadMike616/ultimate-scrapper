# Ultimate Scrapper

Ultimate Scrapper adds configurable Horadric Cube recipes to D2RLoader. Put the required input items in the cube and use the configured transmute button.

Edit the active mod's **d2rloader/config/ultimate-scrapper.toml** to change settings. Restart D2R for TOML changes to take effect.

## Default behavior

With the shipped TOML unchanged, Ultimate Scrapper behaves like Unique/Set Scrapper:

- Accepts one Unique or Set item.
- Has a 30% chance to consume the item and produce nothing.
- On success, produces 1 rune 70% of the time, 2 runes 25% of the time, or 3 runes 5% of the time.
- Selects each rune independently from El through Zod, with weights that decrease by 20% per rune.
- Starts in **Runes** mode, with batch scrapping off.

Custom inputs, Custom and Both modes, batch scrapping, and separate button routing are optional. They only change behavior when enabled in the TOML.

## Input recipes

The **[ultimate-scrapper.input]** settings apply to every output mode.

A flat **inputs** array defines one recipe and requires every listed item. Nested arrays define alternatives: each inner array is a complete recipe. The number of entries in a recipe determines how many items it needs.

~~~toml
[ultimate-scrapper.input]
inputs = ["amu"]

# Or one amulet OR one ring and one belt
# inputs = [["amu"], ["rin", "belt"]]

# Or one normal armor item and one misc item
# inputs = [["type:armo,normal", "type:misc"]]

# Or the default: one Unique or Set item
# inputs = [""]
~~~

Entries in one recipe must match distinct cube items. Each recipe must have 1–12 valid entries. An invalid input list disables input matching until it is corrected. Any **input-count** setting left over from an older config is ignored.

### Matching items

- **Item code:** Use the item's 3- or 4-character code, such as **amu**.
- **Any item:** Use **any**. Add a quality to restrict it, such as **any,rare**.
- **Item type:** Use **type:** followed by a 1–4-character itemtype code. For example, **type:armo,normal** matches normal armor.
- **Quality:** Qualities are **inferior**, **normal**, **superior**, **magic**, **set**, **rare**, **unique**, and **crafted**. If omitted, any quality matches.

Itemtype rules follow the active mod's itemtype hierarchy, including **Equiv1** and **Equiv2** parent types. They read **itemtypes.txt**, **armor.txt**, **weapons.txt**, and **misc.txt** from the active mod's **data/global/excel** folder.

Set **inputs = [""]** to use the default recipe: exactly one Unique or Set item of any code.

## Output modes

Set **active-output-mode** in the **[ultimate-scrapper]** section.

| Mode | Behavior |
| --- | --- |
| **Runes** | Produces random runes using the failure chance, output-count odds, and rune weights in the root section. |
| **Custom** | Selects outputs from the item pool in **[ultimate-scrapper.custom]**. |
| **Both** | Chooses Runes or Custom with equal 50% odds for each transmute. The selected mode uses its own settings. |

In Both mode, the mode is chosen before the failure roll.

### Runes

By default, each transmute has a 30% failure chance. On success, output counts are:

| Rune count | Chance |
| --- | ---: |
| 1 | 70% |
| 2 | 25% |
| 3 | 5% |

Each rune is rolled independently. The plugin normalizes the configured weights across all 33 runes. By default, each successive rune's weight is 20% lower than the previous rune's.

Configure Runes in the root **[ultimate-scrapper]** section:

- **failure-chance-percent**
- **[ultimate-scrapper.output-count-weights]**
- **[ultimate-scrapper.rune-weights]**

### Custom

Custom mode uses settings in **[ultimate-scrapper.custom]**:

- **failure-chance-percent** sets the chance to consume the input and socket contents without producing output.
- **item-level** sets the item level of every output, from 1 to 99.
- **[ultimate-scrapper.custom.output-count-weights]** chooses 1–12 random outputs after a successful roll. The success-only weights for **one** through **twelve** must total 100%.

Every random output is selected independently from **[[ultimate-scrapper.custom.items]]**, so an entry may be selected more than once. The shipped weights always select one random item.

Each item entry requires a D2 item **code** or a special selector. Its **quantity** defaults to the game's normal amount. The **weight** controls how often that entry is selected relative to other pool entries.

#### Gem and rune selectors

Selectors count as one entry for the configured **weight**. If a selector wins, the plugin makes a second, equal-chance roll among the eligible item codes in that selector:

~~~toml
[[ultimate-scrapper.custom.items]]
code = "gem:chipped"
quality = ""
weight = 1
guaranteed = false

[[ultimate-scrapper.custom.items]]
code = "rune:r01-r10"
quality = ""
weight = 1
guaranteed = false
~~~

- **gem** includes all five sizes: chipped, flawed, normal, flawless, and perfect. It includes the seven standard types at each size, including skulls. A size suffix, such as **gem:chipped**, limits the pool to that size. The seven eligible codes within a size have equal odds.
- **rune** includes all runes from El (**r01**) through Zod (**r33**). Use an inclusive range such as **rune:r01-r10** to limit the pool; a single rune such as **rune:r10** is also accepted. Runes within the selected range have equal odds.
- Selector names and size names are case-insensitive. The **quality** field applies to whichever code the selector chooses. Blank quality uses the item's valid quality; gems and runes use Normal quality.

#### Full configuration example

**examples/advanced-gem-pool.toml** is a complete example with weighted gem-size pools, always-on batch scrapping, and Separate button routing. It leaves Runes active by default; change **active-output-mode** to **Custom** or **Both** to use its gem pool. Copy settings you want into the active mod's TOML.

#### Output quality

Set **quality** to one quality or a comma-separated list, such as **magic,rare**. A list rolls uniformly among only the named qualities.

If **quality** is blank or omitted, the plugin rolls among every quality valid for that item code. Items with only one valid quality, such as runes and gems, use that quality. Set and Unique outputs use a random compatible game record.

Supported qualities are **inferior**, **normal**, **superior**, **magic**, **set**, **rare**, **unique**, and **crafted**. Quality selection uses the active mod's item tables, available Set and Unique records, and enabled crafting recipes.

#### Guaranteed outputs

Set **guaranteed = true** on an item entry to add one copy of it to every successful recipe. Guaranteed items are added in addition to the random outputs and are excluded from the random pool. Their weight has no effect.

The sample TOML includes **r01**, **gpv**, and **amu** with equal weights and blank qualities. Edit or replace those entries to make your output pool. The plugin validates the active mode's settings at startup and reports invalid settings in the D2RLoader log.

## Batch scrapping

The cube and stash panels can show a **Batch scrap** checkbox. It starts unchecked each time the plugin starts. The checkbox controls plugin recipes from either panel.

- Set **show-batch-checkboxes = false** to hide the checkbox and label. Their saved widgets remain in the layout variants, so setting it back to true restores them at their saved positions.
- Set **batch-scrap-always-on = true** to keep batch processing enabled and hide the checkbox and label. This setting takes precedence over **show-batch-checkboxes**. It defaults to false.

When batch scrapping is enabled, one click can process several cube items if every item matches a configured one-item recipe. Each item receives its own mode, failure, output-count, and output-item rolls.

If the cube instead matches a multi-item recipe, the plugin processes that recipe as one transaction. If the transaction cannot complete or the outputs exceed ItemService's capacity, the inputs remain untouched.

## Transmute buttons and layouts

Set **transmute-button-mode** to **Native** or **Separate**.

| Mode | Native Transmute button | Added plugin button |
| --- | --- | --- |
| **Native** | Checks for a plugin recipe first. If none matches, passes the contents to D2R for native recipes. A matching plugin recipe takes priority over an overlapping native recipe. | Hidden |
| **Separate** | Always passes the click through to native D2R recipes, including when a plugin recipe also matches. | Handles plugin recipes only |

In Separate mode, the added button uses the Repair All button widget. On first initialization, the plugin adds that widget and its initial coordinates while preserving the player's other layout content. Later starts keep the saved coordinates.

### Layout setup

On first load, the plugin copies the active cube and stash layouts into per-install Native and Separate variants under **d2rloader/config/ultimate-scrapper-layouts/**.

- Native variants omit the plugin button; Separate variants include it.
- The Batch scrap checkbox is added to both cube variants and both stash variants.
- The checkbox label is nested under the checkbox, so moving the checkbox also moves the label. The label's coordinates are offsets from its parent.
- Existing flat checkbox and label entries are migrated while preserving the label's on-screen position.
- Other widgets, text, styles, and layout values are preserved.
- A one-time **.ultimate-scrapper.bak** backup is kept for each active layout.

The four per-install files are:

- **horadriccubelayouthd.native.json**
- **horadriccubelayouthd.separate.json**
- **bankexpansionlayouthd.native.json**
- **bankexpansionlayouthd.separate.json**

The plugin copies the selected variants to the active layout filenames at startup. Once the variants exist, it does not reseed them or reset button positions. Edit the per-install variants to reposition the controls. Optional layout examples are under **assets/layout-examples/**; they are never copied over an active mod's layouts.

Default checkbox positions:

| Layout | Cube | Stash |
| --- | --- | --- |
| Native | (545, 1088) | (825, 1402) |
| Separate | (663, 1088) | (825, 1530) |

## Item handling

Plugin actions run as one D2RLoader **ItemService** transaction on the game thread.

- A failed recipe consumes its input and any socket contents.
- Quantity-bearing inputs are consumed in full, removing the item rather than reducing its quantity by one.
- Runes and gems are always identified.
- Other outputs with the **misc** itemtype are identified, except amulets, rings, quivers, charms, and torches.
- If an input changes before the transaction, or the game cannot create and place every output, the transaction leaves the input untouched.

The plugin does not edit save files or **cubemain.txt**. Successful outputs are added to the inventory normally.

Ultimate Scrapper requires an authoritative local game or TCP/IP host. Remote clients cannot perform the item transaction.

## Install

Place **d2rl-ultimate-scrapper.dll** in the active mod's **d2rloader/plugins/** folder, then restart D2R through D2RLoader.

On first load, the plugin creates **ultimate-scrapper.toml** and the per-install layout variants if they do not already exist. The optional layout examples may need adjustments for a mod's own UI and are not installed automatically.

## Build

Build requirements: Windows x64, CMake 3.29 or newer, Visual Studio 2022 or newer, and the Windows SDK. CMake fetches the pinned D2RLoader PluginSDK commit and TOML++ v3.4.0. To use a local PluginSDK checkout:

~~~powershell
cmake -S . -B ..\ultimate-scrapper-build -A x64 -DULTIMATE_SCRAPPER_LOCAL_SDK_DIR="<PluginSDK checkout>"
cmake --build ..\ultimate-scrapper-build --config Release --target ultimate_scrapper
~~~

The DLL is written to **../ultimate-scrapper-build/Release/d2rl-ultimate-scrapper.dll**. Manual builds also need the generated **default_config.hpp** and TOML++ include directory.

## Compatibility and license

Ultimate Scrapper targets Windows x64, D2R 3.3.93847, and D2RLoader 1.3.1-beta (PluginSDK ABI 4). Version 0.2.0 adds weighted gem and rune selectors for Custom mode.

The project is licensed under the MIT License. See **LICENSE** and **THIRD_PARTY_NOTICES.md** for third-party notices for TOML++ and the D2RLoader PluginSDK.

