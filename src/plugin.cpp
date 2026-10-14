#include <D2RLPlugin/api.h>

#include "default_config.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <toml++/toml.hpp>

namespace {

constexpr char PluginId[] = "ultimate-scrapper";
constexpr char ConvertTarget[] = "HoradricCubePanelMessage";
constexpr char StashConvertTarget[] = "BankPanelMessage";
constexpr char ConvertCommand[] = "Convert";
constexpr char ScrapperButtonMarker[] = "UltimateScrapper";
constexpr char BatchToggleMarker[] = "UltimateScrapperBatch";
constexpr std::size_t MaximumCubeItems = 12;
constexpr std::size_t MaximumInputLists = 64;
constexpr std::size_t MaximumItemTypeRows = 2048;
constexpr std::size_t MaximumItemRows = 16384;
constexpr std::size_t MaximumCustomPoolEntries = 64;
constexpr std::size_t MaximumItemQualities = 8;
constexpr std::uint32_t RuneCount = 33;
constexpr std::uint32_t MaximumRuneOutputs = 3;
constexpr std::uint32_t MaximumCustomRandomOutputs = 12;
constexpr std::size_t MaximumCustomOutputs = D2RL::Items::MaxTransactionOutputs;
constexpr std::uint32_t RuneRarityStepPercent = 80;
constexpr wchar_t ConfigFileName[] = L"ultimate-scrapper.toml";
constexpr std::array<std::string_view, RuneCount> RuneNames{
    "el", "eld", "tir", "nef", "eth", "ith", "tal", "ral", "ort",
    "thul", "amn", "sol", "shael", "dol", "hel", "io", "lum", "ko",
    "fal", "lem", "pul", "um", "mal", "ist", "gul", "vex", "ohm",
    "lo", "sur", "ber", "jah", "cham", "zod",
};

auto MakeDefaultRuneWeights() -> std::array<double, RuneCount> {
    std::array<double, RuneCount> weights{};
    double weight = 100.0;
    for (double& runeWeight : weights) {
        runeWeight = weight;
        weight *= static_cast<double>(RuneRarityStepPercent) / 100.0;
    }
    return weights;
}

struct RuneSettings {
    std::uint32_t failureChancePercent{30};
    std::array<std::uint32_t, MaximumRuneOutputs> outputCountWeights{70, 25, 5};
    std::array<double, RuneCount> runeWeights = MakeDefaultRuneWeights();
    double totalRuneWeight{};
};

enum class OutputMode {
    Runes,
    Custom,
    Both,
};

enum class TransmuteButtonMode {
    Native,
    Separate,
};

struct CustomOutputEntry {
    std::uint32_t code{};
    std::array<D2RL::Items::Quality, MaximumItemQualities> qualities{};
    std::size_t qualityCount{};
    std::uint32_t quantity{D2RL::Items::DefaultValue};
    double weight{1.0};
    bool guaranteed{};
    bool randomQuality{};
};

struct CustomSettings {
    std::uint32_t failureChancePercent{30};
    std::array<std::uint32_t, MaximumCustomRandomOutputs> outputCountWeights{100};
    std::uint32_t itemLevel{1};
    std::array<CustomOutputEntry, MaximumCustomPoolEntries> items{};
    std::size_t itemCount{};
    std::size_t guaranteedCount{};
    double totalWeight{};
};

struct InputRule {
    std::uint32_t code{};
    std::uint32_t itemTypeCode{};
    D2RL::Items::Quality quality{D2RL::Items::Quality::Unknown};
    bool anyCode{};
    bool matchesItemType{};
    bool hasQuality{};
};

struct InputRuleList {
    std::array<InputRule, MaximumCubeItems> items{};
    std::size_t itemCount{};
};

struct InputSettings {
    std::array<InputRuleList, MaximumInputLists> lists{};
    std::size_t listCount{};
    bool usingDefaultRules{true};
    bool valid{true};
};

struct ItemTypeCatalog {
    std::unordered_set<std::uint32_t> knownTypeCodes;
    std::unordered_map<std::uint32_t, std::array<std::uint32_t, 2>> parentTypes;
    struct QualityFlags {
        bool magic{};
        bool rare{};
        bool normal{};
    };
    std::unordered_map<std::uint32_t, QualityFlags> qualityFlagsByType;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> directItemTypesByItemCode;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> itemTypesByItemCode;
    std::unordered_set<std::uint32_t> equipmentItemCodes;
    std::unordered_map<std::uint32_t, std::uint32_t> itemFamilyByItemCode;
    std::unordered_set<std::uint32_t> uniqueItemCodes;
    std::unordered_set<std::uint32_t> setItemCodes;
    std::unordered_set<std::uint32_t> craftedItemCodes;
    std::unordered_set<std::uint32_t> craftedItemTypeCodes;
    std::unordered_set<std::uint32_t> craftedItemFamilies;
    std::unordered_map<std::uint32_t, std::vector<D2RL::Items::Quality>> validQualitiesByItemCode;
    bool available{};
};

struct Settings {
    OutputMode activeOutputMode{OutputMode::Runes};
    TransmuteButtonMode transmuteButtonMode{TransmuteButtonMode::Separate};
    bool showBatchCheckboxes{true};
    bool batchScrapAlwaysOn{};
    InputSettings input;
    RuneSettings runes;
    CustomSettings custom;
};

struct PendingExchange {
    D2RL::PlayerHandle player{D2RL::InvalidPlayerHandle};
    std::array<D2RL::Items::ItemInfo, MaximumCubeItems> items{};
    std::size_t itemCount{};
    bool batchModeRequested{};
    bool processIndividually{};
};

struct CubeCollector {
    std::array<D2RL::Items::ItemInfo, MaximumCubeItems> items{};
    std::size_t count{};
    bool overflow{};
};

Settings Config;
ItemTypeCatalog ItemTypeData;
bool TypeRuleRequested{};
std::atomic_bool BatchModeEnabled{};

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .apiVersion = D2RL_PLUGIN_API_VERSION,
    .id = PluginId,
    .name = "Ultimate Scrapper",
    .version = "0.1.0",
    .author = "MadMike",
    .description = "Transmutes configured input items into runes or configurable custom items.",
    .flags = D2RL::PluginFlags::Client,
};

const D2RL::PluginContext* Context{};
const D2RL::InventoryServiceV1* InventoryService{};
const D2RL::ItemServiceV1* ItemService{};
const D2RL::ThreadServiceV1* ThreadService{};
const D2RL::SharedEventServiceV1* EventService{};
D2RL::SharedEvents::ListenerHandle MessageHandle{D2RL::SharedEvents::InvalidHandle};
std::atomic<bool> Active{};
std::atomic<bool> ExchangePending{};
std::atomic<bool> ConvertMessageLogged{};
std::atomic<bool> ScrapperButtonMessageLogged{};
std::atomic<bool> MismatchMessageLogged{};
std::mutex PendingMutex;
PendingExchange Pending;
std::mt19937 RandomEngine;

auto __cdecl CollectCubeItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item,
        void* userData) noexcept -> D2RL::Inventory::IterationAction {
    auto* collector = static_cast<CubeCollector*>(userData);
    if (collector == nullptr || item == nullptr
            || item->structSize < D2RL::Items::ItemInfoRequiredSize) {
        return D2RL::Inventory::IterationAction::Continue;
    }
    if (collector->count >= collector->items.size()) {
        collector->overflow = true;
        return D2RL::Inventory::IterationAction::Stop;
    }
    collector->items[collector->count++] = *item;
    return D2RL::Inventory::IterationAction::Continue;
}

auto ReadCubeItems(D2RL::PlayerHandle player, CubeCollector& cube) noexcept
        -> D2RL::Inventory::Result {
    const D2RL::Inventory::ItemFilter filter{
        .structSize = D2RL::Inventory::ItemFilterSize,
        .flags = 0,
        .containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Cube),
        .reserved = 0,
    };
    return InventoryService->forEachInventoryItem(Context, player, &filter,
        CollectCubeItem, &cube);
}

auto PackItemTypeCode(D2RL::Inventory::ItemTypeFourCC code) noexcept -> std::uint32_t {
    std::uint32_t packed{};
    for (std::size_t index = 0; index < 4; ++index) {
        packed |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(code.value[index])) << (index * 8U);
    }
    return packed;
}

auto ItemHasType(std::uint32_t itemCode, std::uint32_t typeCode) noexcept -> bool {
    const auto item = ItemTypeData.itemTypesByItemCode.find(itemCode);
    return item != ItemTypeData.itemTypesByItemCode.end()
        && std::find(item->second.begin(), item->second.end(), typeCode) != item->second.end();
}

auto InputRuleMatches(const InputRule& rule,
        const D2RL::Items::ItemInfo& item) noexcept -> bool {
    const bool codeMatches = rule.anyCode
        || (rule.matchesItemType
            ? ItemHasType(item.code, rule.itemTypeCode)
            : item.code == rule.code);
    return codeMatches
        && (!rule.hasQuality || item.quality == rule.quality);
}

auto MatchRemainingInputRules(const InputRuleList& input,
        const CubeCollector& cube, std::uint16_t assignedRules,
        std::uint16_t usedItems) noexcept -> bool {
    const std::uint16_t allRules = static_cast<std::uint16_t>(
        (1U << input.itemCount) - 1U);
    if (assignedRules == allRules) return true;

    std::size_t selectedRule = input.itemCount;
    std::size_t selectedCandidateCount = MaximumCubeItems + 1;
    for (std::size_t ruleIndex = 0; ruleIndex < input.itemCount; ++ruleIndex) {
        if ((assignedRules & (1U << ruleIndex)) != 0) continue;
        std::size_t candidates{};
        for (std::size_t itemIndex = 0; itemIndex < cube.count; ++itemIndex) {
            if ((usedItems & (1U << itemIndex)) == 0
                    && InputRuleMatches(input.items[ruleIndex], cube.items[itemIndex])) {
                ++candidates;
            }
        }
        if (candidates < selectedCandidateCount) {
            selectedRule = ruleIndex;
            selectedCandidateCount = candidates;
        }
    }
    if (selectedRule == input.itemCount || selectedCandidateCount == 0) return false;

    for (std::size_t itemIndex = 0; itemIndex < cube.count; ++itemIndex) {
        if ((usedItems & (1U << itemIndex)) != 0
                || !InputRuleMatches(input.items[selectedRule], cube.items[itemIndex])) {
            continue;
        }
        if (MatchRemainingInputRules(input, cube,
                static_cast<std::uint16_t>(assignedRules | (1U << selectedRule)),
                static_cast<std::uint16_t>(usedItems | (1U << itemIndex)))) {
            return true;
        }
    }
    return false;
}

auto MatchesConfiguredInputItem(const D2RL::Items::ItemInfo& item) noexcept
        -> bool {
    if (!Config.input.valid) return false;
    if (Config.input.usingDefaultRules) {
        return item.quality == D2RL::Items::Quality::Unique
            || item.quality == D2RL::Items::Quality::Set;
    }

    CubeCollector singleItemCube;
    singleItemCube.items[0] = item;
    singleItemCube.count = 1;
    for (std::size_t listIndex = 0; listIndex < Config.input.listCount;
            ++listIndex) {
        const InputRuleList& input = Config.input.lists[listIndex];
        if (input.itemCount == 1
                && MatchRemainingInputRules(input, singleItemCube, 0, 0)) {
            return true;
        }
    }
    return false;
}

auto MatchesBatchInputs(const CubeCollector& cube) noexcept -> bool {
    if (cube.count == 0 || cube.count > MaximumCubeItems || cube.overflow) {
        return false;
    }
    for (std::size_t index = 0; index < cube.count; ++index) {
        if (!MatchesConfiguredInputItem(cube.items[index])) return false;
    }
    return true;
}

auto MatchesConfiguredInputs(const CubeCollector& cube,
        bool batchModeRequested, bool& processIndividually) noexcept -> bool {
    processIndividually = false;
    if (!Config.input.valid || cube.overflow) return false;
    if (batchModeRequested && MatchesBatchInputs(cube)) {
        processIndividually = true;
        return true;
    }
    if (Config.input.usingDefaultRules) {
        return cube.count == 1 && MatchesConfiguredInputItem(cube.items[0]);
    }
    if (cube.count > MaximumCubeItems) {
        return false;
    }
    for (std::size_t listIndex = 0; listIndex < Config.input.listCount; ++listIndex) {
        const InputRuleList& input = Config.input.lists[listIndex];
        if (cube.count == input.itemCount
                && MatchRemainingInputRules(input, cube, 0, 0)) {
            return true;
        }
    }
    return false;
}

auto MatchesPending(const CubeCollector& cube,
        const PendingExchange& pending) noexcept -> bool {
    bool processIndividually{};
    if (!MatchesConfiguredInputs(cube, pending.batchModeRequested,
                processIndividually)
            || processIndividually != pending.processIndividually
            || cube.count != pending.itemCount) return false;
    std::uint16_t matchedItems{};
    for (std::size_t pendingIndex = 0; pendingIndex < pending.itemCount; ++pendingIndex) {
        const auto& expected = pending.items[pendingIndex];
        bool found{};
        for (std::size_t itemIndex = 0; itemIndex < cube.count; ++itemIndex) {
            const auto& actual = cube.items[itemIndex];
            if ((matchedItems & (1U << itemIndex)) == 0
                    && actual.code == expected.code
                    && actual.quality == expected.quality
                    && actual.qualityRecordId == expected.qualityRecordId) {
                found = true;
                matchedItems = static_cast<std::uint16_t>(matchedItems | (1U << itemIndex));
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

constexpr auto MakeRuneCode(std::uint32_t runeNumber) noexcept -> std::uint32_t {
    return D2RL::Items::MakeItemCode('r',
        static_cast<char>('0' + runeNumber / 10U),
        static_cast<char>('0' + runeNumber % 10U));
}

auto OutputIdentifiedStateFlags(std::uint32_t itemCode) noexcept
        -> std::uint32_t {
    bool isNativeRune{};
    for (std::uint32_t runeNumber = 1; runeNumber <= RuneCount; ++runeNumber) {
        if (itemCode == MakeRuneCode(runeNumber)) {
            isNativeRune = true;
            break;
        }
    }
    if (isNativeRune
            || ItemHasType(itemCode, D2RL::Items::MakeItemCode("rune"))
            || ItemHasType(itemCode, D2RL::Items::MakeItemCode("gem"))) {
        return D2RL::Items::ItemStateIdentified;
    }
    if (!ItemHasType(itemCode, D2RL::Items::MakeItemCode("misc"))) {
        return 0;
    }

    constexpr std::array<std::uint32_t, 6> excludedMiscTypes{
        D2RL::Items::MakeItemCode("amul"),
        D2RL::Items::MakeItemCode("ring"),
        D2RL::Items::MakeItemCode("bowq"),
        D2RL::Items::MakeItemCode("xboq"),
        D2RL::Items::MakeItemCode("char"),
        D2RL::Items::MakeItemCode("torc"),
    };
    const bool excluded = std::any_of(excludedMiscTypes.begin(),
        excludedMiscTypes.end(), [itemCode](std::uint32_t typeCode) noexcept {
            return ItemHasType(itemCode, typeCode);
        });
    return excluded ? 0 : D2RL::Items::ItemStateIdentified;
}

template <typename WeightAt>
auto ChooseWeightedIndex(std::size_t count, double totalWeight,
        WeightAt weightAt) noexcept -> std::size_t {
    const double roll = std::uniform_real_distribution<double>(0.0, totalWeight)(RandomEngine);
    double cumulative{};
    std::size_t lastPositive{};
    for (std::size_t index = 0; index < count; ++index) {
        const double weight = weightAt(index);
        if (weight <= 0.0) continue;
        lastPositive = index;
        cumulative += weight;
        if (roll < cumulative) return index;
    }
    return lastPositive;
}

auto ChooseRuneIndex() noexcept -> std::size_t {
    return ChooseWeightedIndex(Config.runes.runeWeights.size(),
        Config.runes.totalRuneWeight, [](std::size_t index) noexcept {
            return Config.runes.runeWeights[index];
        });
}

auto ChooseCustomItemIndex() noexcept -> std::size_t {
    return ChooseWeightedIndex(Config.custom.itemCount,
        Config.custom.totalWeight, [](std::size_t index) noexcept {
            const CustomOutputEntry& entry = Config.custom.items[index];
            return entry.guaranteed ? 0.0 : entry.weight;
        });
}

auto ChooseRandomQuality(std::uint32_t itemCode) noexcept
        -> D2RL::Items::Quality {
    const auto found = ItemTypeData.validQualitiesByItemCode.find(itemCode);
    if (found == ItemTypeData.validQualitiesByItemCode.end()
            || found->second.empty()) {
        return D2RL::Items::Quality::Normal;
    }
    const std::size_t index = std::uniform_int_distribution<std::size_t>(
        0, found->second.size() - 1)(RandomEngine);
    return found->second[index];
}

auto ChooseCustomOutputQuality(const CustomOutputEntry& entry) noexcept
        -> D2RL::Items::Quality {
    if (entry.randomQuality) return ChooseRandomQuality(entry.code);
    if (entry.qualityCount == 0) return D2RL::Items::Quality::Normal;
    const std::size_t index = std::uniform_int_distribution<std::size_t>(
        0, entry.qualityCount - 1)(RandomEngine);
    return entry.qualities[index];
}

auto ChooseOutputMode() noexcept -> OutputMode {
    if (Config.activeOutputMode != OutputMode::Both) {
        return Config.activeOutputMode;
    }
    return std::uniform_int_distribution<std::uint32_t>(0, 1)(RandomEngine) == 0
        ? OutputMode::Runes : OutputMode::Custom;
}

auto MakeOutputSpec(std::uint32_t code, D2RL::Items::Quality quality,
        std::uint32_t itemLevel, std::uint32_t quantity) noexcept
        -> D2RL::Items::ItemCreateSpec {
    D2RL::Items::ItemCreateSpec output{};
    output.structSize = D2RL::Items::ItemCreateSpecSize;
    output.code = code;
    output.quality = quality;
    output.qualityRecordId = D2RL::Items::RandomQualityRecord;
    output.itemLevel = itemLevel;
    output.seedMode = D2RL::Items::SeedMode::Random;
    output.quantity = quantity;
    output.durability = D2RL::Items::DefaultValue;
    output.socketCount = 0;
    output.stateFlags = OutputIdentifiedStateFlags(code);
    output.destination = {
        .structSize = D2RL::Items::ItemDestinationSize,
        .flags = 0,
        .container = D2RL::Items::ItemContainer::Cube,
        .placement = D2RL::Items::Placement::Automatic,
        .customPageHandle = 0,
        .x = 0,
        .y = 0,
    };
    return output;
}

auto AppendRuneOutputs(
        std::array<D2RL::Items::ItemCreateSpec, MaximumCustomOutputs>& outputs,
        std::size_t& outputCount) noexcept -> bool {
    const std::uint32_t countRoll =
        std::uniform_int_distribution<std::uint32_t>(1, 100)(RandomEngine);
    std::uint32_t cumulative{};
    std::uint32_t count{};
    for (std::size_t index = 0;
            index < Config.runes.outputCountWeights.size(); ++index) {
        cumulative += Config.runes.outputCountWeights[index];
        if (countRoll <= cumulative) {
            count = static_cast<std::uint32_t>(index + 1);
            break;
        }
    }
    if (count == 0 || outputCount > MaximumCustomOutputs
            || count > MaximumCustomOutputs - outputCount) {
        return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t runeIndex = ChooseRuneIndex();
        outputs[outputCount++] = MakeOutputSpec(MakeRuneCode(
            static_cast<std::uint32_t>(runeIndex + 1)),
            D2RL::Items::Quality::Normal, 1, D2RL::Items::DefaultValue);
    }
    return true;
}

auto AppendCustomOutputs(
        std::array<D2RL::Items::ItemCreateSpec, MaximumCustomOutputs>& outputs,
        std::size_t& outputCount) noexcept -> bool {
    const std::uint32_t countRoll =
        std::uniform_int_distribution<std::uint32_t>(1, 100)(RandomEngine);
    std::uint32_t cumulative{};
    std::uint32_t randomOutputCount{};
    for (std::size_t index = 0;
            index < Config.custom.outputCountWeights.size(); ++index) {
        cumulative += Config.custom.outputCountWeights[index];
        if (countRoll <= cumulative) {
            randomOutputCount = static_cast<std::uint32_t>(index + 1);
            break;
        }
    }
    const std::size_t additionalCount = Config.custom.guaranteedCount
        + randomOutputCount;
    if (randomOutputCount == 0 || outputCount > MaximumCustomOutputs
            || additionalCount > MaximumCustomOutputs - outputCount) {
        return false;
    }

    for (std::size_t index = 0; index < Config.custom.itemCount; ++index) {
        const CustomOutputEntry& entry = Config.custom.items[index];
        if (!entry.guaranteed) continue;
        const auto quality = ChooseCustomOutputQuality(entry);
        outputs[outputCount++] = MakeOutputSpec(entry.code, quality,
            Config.custom.itemLevel, entry.quantity);
    }
    for (std::uint32_t index = 0; index < randomOutputCount; ++index) {
        const std::size_t itemIndex = ChooseCustomItemIndex();
        const CustomOutputEntry& entry = Config.custom.items[itemIndex];
        const auto quality = ChooseCustomOutputQuality(entry);
        outputs[outputCount++] = MakeOutputSpec(entry.code, quality,
            Config.custom.itemLevel, entry.quantity);
    }
    return true;
}

auto AppendOutputsForMode(OutputMode mode,
        std::array<D2RL::Items::ItemCreateSpec, MaximumCustomOutputs>& outputs,
        std::size_t& outputCount) noexcept -> bool {
    return mode == OutputMode::Runes
        ? AppendRuneOutputs(outputs, outputCount)
        : AppendCustomOutputs(outputs, outputCount);
}

auto ConfigPath() -> std::filesystem::path {
    if (Context != nullptr && Context->pluginConfigPath != nullptr
            && Context->pluginConfigPath[0] != L'\0') {
        return std::filesystem::path(Context->pluginConfigPath);
    }
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    if (error) return {};
    return current / L"d2rloader" / L"config" / ConfigFileName;
}

struct JsonObjectRange {
    std::size_t begin{};
    std::size_t end{};
};

struct JsonArrayRange {
    std::size_t begin{};
    std::size_t end{};
};

struct JsonMemberLocation {
    std::size_t key{};
    std::size_t value{};
};

void SkipJsonWhitespace(std::string_view json, std::size_t& position,
        std::size_t limit) noexcept {
    while (position < limit && (json[position] == ' ' || json[position] == '\t'
            || json[position] == '\r' || json[position] == '\n')) {
        ++position;
    }
}

auto ReadJsonString(std::string_view json, std::size_t quotePosition,
        std::size_t limit, std::size_t& after) noexcept
        -> std::optional<std::string_view> {
    if (quotePosition >= limit || json[quotePosition] != '"') return std::nullopt;
    for (std::size_t position = quotePosition + 1; position < limit; ++position) {
        if (json[position] == '\\') {
            ++position;
        } else if (json[position] == '"') {
            after = position + 1;
            return json.substr(quotePosition + 1,
                position - quotePosition - 1);
        }
    }
    return std::nullopt;
}

auto FindMatchingJsonBrace(std::string_view json, std::size_t opening,
        std::size_t limit) noexcept -> std::optional<std::size_t> {
    if (opening >= limit || json[opening] != '{') return std::nullopt;
    std::size_t depth{};
    bool inString{};
    bool escaped{};
    for (std::size_t position = opening; position < limit; ++position) {
        const char current = json[position];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '"') {
                inString = false;
            }
            continue;
        }
        if (current == '"') {
            inString = true;
        } else if (current == '{') {
            ++depth;
        } else if (current == '}' && --depth == 0) {
            return position;
        }
    }
    return std::nullopt;
}

auto FindNamedJsonObject(std::string_view json, std::string_view name)
        -> std::optional<JsonObjectRange> {
    std::vector<std::size_t> objectStack;
    for (std::size_t position = 0; position < json.size();) {
        if (json[position] == '"') {
            std::size_t afterKey{};
            const auto stringValue = ReadJsonString(json, position,
                json.size(), afterKey);
            if (!stringValue) return std::nullopt;
            if (*stringValue == "name" && !objectStack.empty()) {
                std::size_t valuePosition = afterKey;
                SkipJsonWhitespace(json, valuePosition, json.size());
                if (valuePosition < json.size()
                        && json[valuePosition] == ':') {
                    ++valuePosition;
                    SkipJsonWhitespace(json, valuePosition, json.size());
                    std::size_t afterValue{};
                    const auto value = ReadJsonString(json, valuePosition,
                        json.size(), afterValue);
                    if (value && *value == name) {
                        const std::size_t objectStart = objectStack.back();
                        const auto objectEnd = FindMatchingJsonBrace(json,
                            objectStart, json.size());
                        if (!objectEnd) return std::nullopt;
                        return JsonObjectRange{objectStart, *objectEnd};
                    }
                }
            }
            position = afterKey;
            continue;
        }
        if (json[position] == '{') {
            objectStack.push_back(position);
        } else if (json[position] == '}' && !objectStack.empty()) {
            objectStack.pop_back();
        }
        ++position;
    }
    return std::nullopt;
}

auto FindDirectJsonMember(std::string_view json, JsonObjectRange object,
        std::string_view key) noexcept -> std::optional<JsonMemberLocation> {
    std::size_t nestedDepth{};
    for (std::size_t position = object.begin + 1; position < object.end;) {
        if (json[position] == '"') {
            std::size_t afterKey{};
            const auto stringValue = ReadJsonString(json, position,
                object.end, afterKey);
            if (!stringValue) return std::nullopt;
            if (nestedDepth == 0 && *stringValue == key) {
                std::size_t valuePosition = afterKey;
                SkipJsonWhitespace(json, valuePosition, object.end);
                if (valuePosition >= object.end || json[valuePosition] != ':') {
                    return std::nullopt;
                }
                ++valuePosition;
                SkipJsonWhitespace(json, valuePosition, object.end);
                return JsonMemberLocation{position, valuePosition};
            }
            position = afterKey;
            continue;
        }
        if (json[position] == '{' || json[position] == '[') {
            ++nestedDepth;
        } else if ((json[position] == '}' || json[position] == ']')
                && nestedDepth > 0) {
            --nestedDepth;
        }
        ++position;
    }
    return std::nullopt;
}

auto FindJsonObjectMember(std::string_view json, JsonObjectRange object,
        std::string_view key) noexcept -> std::optional<JsonObjectRange> {
    const auto member = FindDirectJsonMember(json, object, key);
    if (!member || member->value >= object.end || json[member->value] != '{') {
        return std::nullopt;
    }
    const auto end = FindMatchingJsonBrace(json, member->value, object.end + 1);
    if (!end) return std::nullopt;
    return JsonObjectRange{member->value, *end};
}

auto ReadDirectJsonString(std::string_view json, JsonObjectRange object,
        std::string_view key) -> std::optional<std::string> {
    const auto member = FindDirectJsonMember(json, object, key);
    if (!member) return std::nullopt;
    std::size_t afterValue{};
    const auto value = ReadJsonString(json, member->value, object.end,
        afterValue);
    return value ? std::optional<std::string>(std::string(*value))
        : std::nullopt;
}

auto FindConvertButton(std::string_view json, std::string_view clickMessage)
        -> std::optional<JsonObjectRange> {
    std::vector<std::size_t> objectStack;
    for (std::size_t position = 0; position < json.size();) {
        if (json[position] == '"') {
            std::size_t afterKey{};
            const auto stringValue = ReadJsonString(json, position,
                json.size(), afterKey);
            if (!stringValue) return std::nullopt;
            if (*stringValue == "onClickMessage" && objectStack.size() >= 2) {
                std::size_t valuePosition = afterKey;
                SkipJsonWhitespace(json, valuePosition, json.size());
                if (valuePosition < json.size()
                        && json[valuePosition] == ':') {
                    ++valuePosition;
                    SkipJsonWhitespace(json, valuePosition, json.size());
                    std::size_t afterValue{};
                    const auto value = ReadJsonString(json, valuePosition,
                        json.size(), afterValue);
                    if (value && *value == clickMessage) {
                        const std::size_t widgetStart =
                            objectStack[objectStack.size() - 2];
                        const auto widgetEnd = FindMatchingJsonBrace(json,
                            widgetStart, json.size());
                        if (!widgetEnd) return std::nullopt;
                        const JsonObjectRange widget{widgetStart, *widgetEnd};
                        const auto type = ReadDirectJsonString(json, widget,
                            "type");
                        if (type && *type == "ButtonWidget") {
                            return widget;
                        }
                    }
                }
            }
            position = afterKey;
            continue;
        }
        if (json[position] == '{') {
            objectStack.push_back(position);
        } else if (json[position] == '}' && !objectStack.empty()) {
            objectStack.pop_back();
        }
        ++position;
    }
    return std::nullopt;
}

auto FindInnermostArrayBefore(std::string_view json, std::size_t position)
        -> std::optional<std::size_t> {
    std::vector<std::size_t> arrayStack;
    for (std::size_t cursor = 0; cursor < position;) {
        if (json[cursor] == '"') {
            std::size_t afterString{};
            if (!ReadJsonString(json, cursor, position, afterString)) {
                return std::nullopt;
            }
            cursor = afterString;
            continue;
        }
        if (json[cursor] == '[') {
            arrayStack.push_back(cursor);
        } else if (json[cursor] == ']' && !arrayStack.empty()) {
            arrayStack.pop_back();
        }
        ++cursor;
    }
    if (arrayStack.empty()) return std::nullopt;
    return arrayStack.back();
}

auto FindMatchingJsonBracket(std::string_view json, std::size_t opening,
        std::size_t limit) -> std::optional<std::size_t> {
    if (opening >= limit || json[opening] != '[') return std::nullopt;
    std::size_t depth{};
    for (std::size_t position = opening; position < limit;) {
        if (json[position] == '"') {
            std::size_t afterString{};
            if (!ReadJsonString(json, position, limit, afterString)) {
                return std::nullopt;
            }
            position = afterString;
            continue;
        }
        if (json[position] == '[') {
            ++depth;
        } else if (json[position] == ']' && --depth == 0) {
            return position;
        }
        ++position;
    }
    return std::nullopt;
}

auto FindJsonArrayMember(std::string_view json, JsonObjectRange object,
        std::string_view key) -> std::optional<JsonArrayRange> {
    const auto member = FindDirectJsonMember(json, object, key);
    if (!member || member->value >= object.end || json[member->value] != '[') {
        return std::nullopt;
    }
    const auto end = FindMatchingJsonBracket(json, member->value,
        object.end + 1);
    if (!end) return std::nullopt;
    return JsonArrayRange{member->value, *end};
}

auto IsJsonWhitespace(char value) noexcept -> bool {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

auto JsonIndentationAt(std::string_view json, std::size_t position)
        -> std::string {
    const std::size_t lineStart = json.rfind('\n', position);
    const std::size_t indentationStart = lineStart == std::string_view::npos
        ? 0 : lineStart + 1;
    std::size_t indentationEnd = indentationStart;
    while (indentationEnd < position
            && (json[indentationEnd] == ' ' || json[indentationEnd] == '\t')) {
        ++indentationEnd;
    }
    return std::string(json.substr(indentationStart,
        indentationEnd - indentationStart));
}

auto ReindentJsonObject(std::string_view object,
        std::string_view sourceIndent, std::string_view targetIndent,
        std::string_view newline) -> std::string {
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start <= object.size();) {
        const std::size_t end = object.find('\n', start);
        const std::size_t lineEnd = end == std::string_view::npos
            ? object.size() : end;
        std::size_t contentEnd = lineEnd;
        if (contentEnd > start && object[contentEnd - 1] == '\r') {
            --contentEnd;
        }
        lines.push_back(object.substr(start, contentEnd - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (lines.empty()) return {};

    std::string formatted;
    formatted.reserve(object.size() + targetIndent.size() * lines.size());
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) formatted.append(newline);
        formatted.append(targetIndent);
        auto line = lines[index];
        if (index != 0 && !sourceIndent.empty()
                && line.starts_with(sourceIndent)) {
            line.remove_prefix(sourceIndent.size());
        }
        formatted.append(line);
    }
    return formatted;
}

auto AppendJsonArrayObject(std::string& json, JsonArrayRange array,
        std::string_view serializedObject,
        std::string_view sourceIndent = {}) -> bool {
    if (array.begin >= array.end || array.end >= json.size()
            || json[array.begin] != '[' || json[array.end] != ']') {
        return false;
    }
    std::size_t firstContent = array.begin + 1;
    SkipJsonWhitespace(json, firstContent, array.end);
    const std::string newline = json.find("\r\n") != std::string::npos
        ? "\r\n" : "\n";
    const std::string arrayIndent = JsonIndentationAt(json, array.begin);
    const std::string itemIndent = arrayIndent + "    ";
    const std::string indentedObject = ReindentJsonObject(serializedObject,
        sourceIndent, itemIndent, newline);
    if (indentedObject.empty()) return false;

    if (firstContent == array.end) {
        json.insert(array.begin + 1, newline + indentedObject + newline
            + arrayIndent);
        return true;
    }

    std::size_t lastContent = array.end;
    while (lastContent > array.begin + 1
            && IsJsonWhitespace(json[lastContent - 1])) {
        --lastContent;
    }
    if (lastContent <= array.begin + 1) return false;
    json.insert(lastContent, "," + newline + indentedObject);
    return true;
}

auto AppendJsonArrayMember(std::string& json, JsonObjectRange object,
        std::string_view memberName, std::string_view serializedObject,
        std::string_view sourceIndent = {})
        -> bool {
    if (FindDirectJsonMember(json, object, memberName)) {
        const auto array = FindJsonArrayMember(json, object, memberName);
        return array && AppendJsonArrayObject(json, *array, serializedObject,
            sourceIndent);
    }
    if (object.begin >= object.end || object.end >= json.size()
            || json[object.end] != '}') {
        return false;
    }

    std::size_t insertion = object.end;
    while (insertion > object.begin + 1
            && IsJsonWhitespace(json[insertion - 1])) {
        --insertion;
    }
    const bool hasMembers = insertion > object.begin + 1
        && json[insertion - 1] != '{';
    const std::string newline = json.find("\r\n") != std::string::npos
        ? "\r\n" : "\n";
    const std::string objectIndent = JsonIndentationAt(json, object.begin);
    const std::string memberIndent = objectIndent + "    ";
    const std::string childIndent = memberIndent + "    ";
    const std::string indentedObject = ReindentJsonObject(serializedObject,
        sourceIndent, childIndent, newline);
    if (indentedObject.empty()) return false;

    std::string addition = hasMembers ? "," : "";
    addition += newline + memberIndent + "\"" + std::string(memberName)
        + "\": [" + newline + indentedObject
        + newline + memberIndent + "]";
    json.insert(insertion, addition);
    return true;
}

auto InsertJsonArrayObjectAfter(std::string& json, JsonObjectRange object,
        std::string_view serializedObject) -> bool {
    const auto arrayOpening = FindInnermostArrayBefore(json, object.begin);
    if (!arrayOpening) return false;
    const auto arrayEnd = FindMatchingJsonBracket(json, *arrayOpening,
        json.size());
    if (!arrayEnd || object.end >= *arrayEnd) return false;

    const std::size_t lineStart = json.rfind('\n', object.begin);
    const std::size_t indentationStart = lineStart == std::string::npos
        ? 0 : lineStart + 1;
    std::string indentation = json.substr(indentationStart,
        object.begin - indentationStart);
    if (indentation.find_first_not_of(" \t") != std::string::npos) {
        indentation.clear();
    }
    const std::string newline = json.find("\r\n") != std::string::npos
        ? "\r\n" : "\n";

    std::size_t afterObject = object.end + 1;
    SkipJsonWhitespace(json, afterObject, *arrayEnd);
    if (afterObject < *arrayEnd && json[afterObject] == ',') {
        json.insert(afterObject + 1, newline + indentation
            + std::string(serializedObject) + ",");
        return true;
    }
    if (afterObject != *arrayEnd) return false;
    json.insert(object.end + 1, "," + newline + indentation
        + std::string(serializedObject));
    return true;
}

auto JsonScalarEnd(std::string_view json, std::size_t start,
        std::size_t limit) noexcept -> std::optional<std::size_t> {
    if (start >= limit) return std::nullopt;
    if (json[start] == '"') {
        std::size_t after{};
        if (!ReadJsonString(json, start, limit, after)) return std::nullopt;
        return after;
    }
    std::size_t end = start;
    while (end < limit && json[end] != ',' && json[end] != '}'
            && json[end] != ']') {
        ++end;
    }
    while (end > start && (json[end - 1] == ' ' || json[end - 1] == '\t'
            || json[end - 1] == '\r' || json[end - 1] == '\n')) {
        --end;
    }
    return end > start ? std::optional<std::size_t>(end) : std::nullopt;
}

auto ReadNamedWidgetRectCoordinate(std::string_view json,
        std::string_view widgetName, std::string_view coordinate)
        -> std::optional<std::int32_t> {
    const auto widget = FindNamedJsonObject(json, widgetName);
    if (!widget) return std::nullopt;
    const auto fields = FindJsonObjectMember(json, *widget, "fields");
    if (!fields) return std::nullopt;
    const auto rect = FindJsonObjectMember(json, *fields, "rect");
    if (!rect) return std::nullopt;
    const auto member = FindDirectJsonMember(json, *rect, coordinate);
    if (!member) return std::nullopt;
    const auto end = JsonScalarEnd(json, member->value, rect->end);
    if (!end) return std::nullopt;
    std::int32_t value{};
    const auto parsed = std::from_chars(json.data() + member->value,
        json.data() + *end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != json.data() + *end) {
        return std::nullopt;
    }
    return value;
}

auto SetNamedWidgetRectPosition(std::string& json,
        std::string_view widgetName, std::int32_t x,
        std::int32_t y) -> bool {
    const auto setCoordinate = [&](std::string_view coordinate,
            std::int32_t value) -> bool {
        const auto widget = FindNamedJsonObject(json, widgetName);
        if (!widget) return false;
        const auto fields = FindJsonObjectMember(json, *widget, "fields");
        if (!fields) return false;
        const auto rect = FindJsonObjectMember(json, *fields, "rect");
        if (!rect) return false;
        const auto member = FindDirectJsonMember(json, *rect, coordinate);
        if (!member) return false;
        const auto end = JsonScalarEnd(json, member->value, rect->end);
        if (!end) return false;
        const std::string replacement = std::to_string(value);
        if (std::string_view(json).substr(member->value,
                *end - member->value) != replacement) {
            json.replace(member->value, *end - member->value, replacement);
        }
        return true;
    };
    return setCoordinate("x", x) && setCoordinate("y", y);
}

auto RemoveNamedJsonObject(std::string& json, std::string_view name) -> bool;

auto NestBatchLabelUnderCheckbox(std::string& json) -> bool {
    constexpr std::string_view CheckboxName =
        "ultimate_scrapper_batch_toggle";
    constexpr std::string_view LabelName = "ultimate_scrapper_batch_label";
    const auto checkbox = FindNamedJsonObject(json, CheckboxName);
    const auto label = FindNamedJsonObject(json, LabelName);
    if (!checkbox || !label) return true;

    const auto children = FindJsonArrayMember(json, *checkbox, "children");
    if (children && label->begin > children->begin
            && label->end < children->end) {
        return true;
    }
    if (FindDirectJsonMember(json, *checkbox, "children") && !children) {
        return false;
    }

    const auto checkboxX = ReadNamedWidgetRectCoordinate(json,
        CheckboxName, "x");
    const auto checkboxY = ReadNamedWidgetRectCoordinate(json,
        CheckboxName, "y");
    const auto labelX = ReadNamedWidgetRectCoordinate(json, LabelName, "x");
    const auto labelY = ReadNamedWidgetRectCoordinate(json, LabelName, "y");
    if (!checkboxX || !checkboxY || !labelX || !labelY) return false;

    const std::int64_t relativeX = static_cast<std::int64_t>(*labelX)
        - *checkboxX;
    const std::int64_t relativeY = static_cast<std::int64_t>(*labelY)
        - *checkboxY;
    if (relativeX < std::numeric_limits<std::int32_t>::min()
            || relativeX > std::numeric_limits<std::int32_t>::max()
            || relativeY < std::numeric_limits<std::int32_t>::min()
            || relativeY > std::numeric_limits<std::int32_t>::max()
            || !SetNamedWidgetRectPosition(json, LabelName,
                static_cast<std::int32_t>(relativeX),
                static_cast<std::int32_t>(relativeY))) {
        return false;
    }

    const auto relativeLabel = FindNamedJsonObject(json, LabelName);
    if (!relativeLabel) return false;
    const std::string labelIndent = JsonIndentationAt(json,
        relativeLabel->begin);
    const std::string serializedLabel(json.substr(relativeLabel->begin,
        relativeLabel->end - relativeLabel->begin + 1));
    if (!RemoveNamedJsonObject(json, LabelName)) return false;
    const auto updatedCheckbox = FindNamedJsonObject(json, CheckboxName);
    return updatedCheckbox && AppendJsonArrayMember(json, *updatedCheckbox,
        "children", serializedLabel, labelIndent);
}

auto SetNamedWidgetVisible(std::string& json, std::string_view widgetName,
        bool visible) -> bool {
    const auto widget = FindNamedJsonObject(json, widgetName);
    if (!widget) return false;
    const auto fields = FindJsonObjectMember(json, *widget, "fields");
    if (!fields) return false;
    const auto visibility = FindDirectJsonMember(json, *fields, "visible");
    const std::string replacement = visible ? "true" : "false";
    if (visibility) {
        const auto tokenEnd = JsonScalarEnd(json, visibility->value, fields->end);
        if (!tokenEnd) return false;
        if (std::string_view(json).substr(visibility->value,
                *tokenEnd - visibility->value) != replacement) {
            json.replace(visibility->value, *tokenEnd - visibility->value,
                replacement);
        }
        return true;
    }
    if (visible) return true;

    const auto firstMember = FindDirectJsonMember(json, *fields, "rect");
    if (!firstMember) return false;
    const std::size_t lineStart = json.rfind('\n', firstMember->key);
    const std::size_t indentationStart = lineStart == std::string::npos
        ? fields->begin + 1 : lineStart + 1;
    const std::string prefix = json.substr(indentationStart,
        firstMember->key - indentationStart);
    const bool memberStartsLine = std::all_of(prefix.begin(), prefix.end(),
        [](char character) {
            return character == ' ' || character == '\t' || character == '\r';
        });
    const std::string newline = lineStart != std::string::npos
            && lineStart > 0 && json[lineStart - 1] == '\r'
        ? "\r\n" : "\n";
    std::string visibilityMember = "\"visible\": " + replacement + ",";
    if (memberStartsLine) {
        visibilityMember += newline + prefix;
    } else {
        // Some user layouts keep a label's fields object on one line. Keep
        // valid JSON by separating the injected member with a space there.
        visibilityMember += ' ';
    }
    json.insert(firstMember->key, visibilityMember);
    return true;
}

auto SetBatchCheckboxVisibility(std::string& json, bool visible) -> bool {
    constexpr std::array<std::string_view, 2> widgetNames{
        "ultimate_scrapper_batch_toggle",
        "ultimate_scrapper_batch_label"
    };
    for (const auto widgetName : widgetNames) {
        // Keep user layouts that intentionally omit either widget untouched.
        if (!FindNamedJsonObject(json, widgetName)) continue;
        if (!SetNamedWidgetVisible(json, widgetName, visible)) return false;
    }
    return true;
}

auto SetNamedWidgetString(std::string& json, std::string_view widgetName,
        std::string_view memberName, std::string_view value) -> bool {
    const auto widget = FindNamedJsonObject(json, widgetName);
    if (!widget) return false;
    const auto fields = FindJsonObjectMember(json, *widget, "fields");
    if (!fields) return false;
    const auto member = FindDirectJsonMember(json, *fields, memberName);
    if (!member) return false;
    std::size_t afterValue{};
    if (!ReadJsonString(json, member->value, fields->end, afterValue)) {
        return false;
    }
    const std::string replacement = "\"" + std::string(value) + "\"";
    if (std::string_view(json).substr(member->value,
            afterValue - member->value) != replacement) {
        json.replace(member->value, afterValue - member->value, replacement);
    }
    return true;
}

auto RemoveNamedJsonObject(std::string& json, std::string_view name) -> bool {
    const auto object = FindNamedJsonObject(json, name);
    if (!object) return true;

    std::size_t afterObject = object->end + 1;
    SkipJsonWhitespace(json, afterObject, json.size());
    if (afterObject < json.size() && json[afterObject] == ',') {
        json.erase(object->begin, afterObject - object->begin + 1);
    } else {
        std::size_t beforeObject = object->begin;
        while (beforeObject > 0 && (json[beforeObject - 1] == ' '
                || json[beforeObject - 1] == '\t'
                || json[beforeObject - 1] == '\r'
                || json[beforeObject - 1] == '\n')) {
            --beforeObject;
        }
        if (beforeObject > 0 && json[beforeObject - 1] == ',') {
            const std::size_t eraseBegin = beforeObject - 1;
            json.erase(eraseBegin, object->end + 1 - eraseBegin);
        } else {
            json.erase(object->begin, object->end + 1 - object->begin);
        }
    }
    return !FindNamedJsonObject(json, name).has_value();
}

auto InjectSeparateButton(std::string& json, bool cubeLayout) -> bool {
    if (FindNamedJsonObject(json, "ultimate_scrapper_transmute")) return true;

    constexpr std::string_view CubeButton = R"json({
    "type": "ButtonWidget", "name": "ultimate_scrapper_transmute",
    "fields": {
        "rect": { "x": 641, "y": 975 },
        "filename": "PANEL\\Vendors\\RepairAll_Button",
        "tooltipString": "Salvage",
        "hoveredFrame": 2,
        "sound": "cursor_convert_item",
        "onClickMessage": "HoradricCubePanelMessage:Convert:UltimateScrapper"
    }
})json";
    constexpr std::string_view StashButton = R"json({
    "type": "ButtonWidget", "name": "ultimate_scrapper_transmute",
    "fields": {
        "rect": { "x": 801, "y": 1410 },
        "filename": "PANEL\\Vendors\\RepairAll_Button",
        "tooltipString": "Salvage",
        "hoveredFrame": 2,
        "sound": "cursor_convert_item",
        "onClickMessage": "BankPanelMessage:Convert:UltimateScrapper"
    }
})json";
    const auto convertButton = FindConvertButton(json,
        cubeLayout ? "HoradricCubePanelMessage:Convert"
            : "BankPanelMessage:Convert");
    if (!convertButton) return false;
    return InsertJsonArrayObjectAfter(json, *convertButton,
        cubeLayout ? CubeButton : StashButton);
}

auto SyncExistingBatchCheckbox(std::string& json, bool cubeLayout) -> bool {
    if (!FindNamedJsonObject(json, "ultimate_scrapper_batch_toggle")) {
        return true;
    }
    // Convert is a native, registered UI message. A made-up command name
    // causes D2RLoader's message parser to assert before listeners run.
    const std::string target = cubeLayout
        ? "HoradricCubePanelMessage" : "BankPanelMessage";
    return SetNamedWidgetString(json, "ultimate_scrapper_batch_toggle",
            "onClickMessage",
            target + ":Convert:UltimateScrapperBatch")
        && NestBatchLabelUnderCheckbox(json);
}

auto InjectBatchCheckbox(std::string& json, bool cubeLayout,
        bool nativeButtonMode) -> bool {
    constexpr std::string_view BatchCheckbox = R"json({
    "type": "ToggleButtonWidget", "name": "ultimate_scrapper_batch_toggle",
    "fields": {
        "rect": { "x": 545, "y": 1088, "width": 64, "height": 64 },
        "sound": "cursor_menu_select_hd",
        "filename": "Lobby\\CreateGame\\CreateGame_AdvancedCheckbox",
        "untoggledFrame": 0,
        "untoggledPressedFrame": 1,
        "untoggledHoveredFrame": 3,
        "untoggledDisabledFrame": 2,
        "toggledFrame": 4,
        "toggledPressedFrame": 5,
        "toggledHoveredFrame": 6,
        "tooltipString": "Process each matching input item separately",
        "onClickMessage": "HoradricCubePanelMessage:Convert:UltimateScrapperBatch"
    }
})json";
    constexpr std::string_view BatchLabel = R"json({"type":"TextBoxWidget","name":"ultimate_scrapper_batch_label",
	"fields":{
		"visible": true, "rect":{"x":-55,"y":42,"width":180,"height":64},
		"text":"Batch scrap",
		"style":{"fontColor":"$PanelTitleColor",
		"pointSize":"$SmallPanelFontSize",
		"alignment":{"v":"center","h":"left"},
		"dropShadow":"$DefaultDropShadow"}}}
)json";

    auto checkbox = FindNamedJsonObject(json,
        "ultimate_scrapper_batch_toggle");
    if (!checkbox) {
        const auto existingButton = FindNamedJsonObject(json,
            "ultimate_scrapper_transmute");
        const std::string convertMessage = cubeLayout
            ? "HoradricCubePanelMessage:Convert"
            : "BankPanelMessage:Convert";
        const auto anchor = existingButton
            ? existingButton : FindConvertButton(json, convertMessage);
        if (!anchor || !InsertJsonArrayObjectAfter(json, *anchor,
                BatchCheckbox)) {
            return false;
        }
        checkbox = FindNamedJsonObject(json,
            "ultimate_scrapper_batch_toggle");
        if (!checkbox) return false;
        const std::int32_t defaultX = cubeLayout
            ? nativeButtonMode ? 545 : 663 : 825;
        const std::int32_t defaultY = cubeLayout ? 1088
            : nativeButtonMode ? 1402 : 1530;
        if (!SetNamedWidgetRectPosition(json,
                "ultimate_scrapper_batch_toggle", defaultX, defaultY)) {
            return false;
        }
    }
    if (!FindNamedJsonObject(json, "ultimate_scrapper_batch_label")) {
        if (!AppendJsonArrayMember(json, *checkbox, "children", BatchLabel)) {
            return false;
        }
    }
    return SyncExistingBatchCheckbox(json, cubeLayout);
}

auto FindActiveLayoutFile(const wchar_t* fileName) -> std::filesystem::path {
    if (Context == nullptr || Context->modDirectory == nullptr
            || Context->modDirectory[0] == L'\0') {
        return {};
    }
    std::vector<std::filesystem::path> roots{
        std::filesystem::path(Context->modDirectory)};
    const auto configPath = ConfigPath();
    if (!configPath.empty()) {
        const auto modRoot = configPath.parent_path().parent_path().parent_path();
        if (std::find(roots.begin(), roots.end(), modRoot) == roots.end()) {
            roots.push_back(modRoot);
        }
    }

    std::filesystem::path modFolder;
    if (Context->activeMod != nullptr && Context->activeMod[0] != '\0') {
        std::string modName(Context->activeMod);
        if (modName.find_first_of("/\\:") != std::string::npos) {
            modName.clear();
        }
        if (!modName.empty()
                && (modName.size() < 4
                    || modName.substr(modName.size() - 4) != ".mpq")) {
            modName += ".mpq";
        }
        if (!modName.empty()) {
            std::u8string modNameUtf8;
            modNameUtf8.reserve(modName.size());
            for (const unsigned char character : modName) {
                modNameUtf8.push_back(static_cast<char8_t>(character));
            }
            modFolder = std::filesystem::path(modNameUtf8);
        }
    }

    for (const auto& root : roots) {
        std::vector<std::filesystem::path> candidates;
        if (!modFolder.empty()) {
            candidates.push_back(root / modFolder / L"data" / L"global"
                / L"ui" / L"layouts" / fileName);
        }
        candidates.push_back(root / L"data" / L"global" / L"ui"
            / L"layouts" / fileName);
        for (const auto& candidate : candidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                return candidate;
            }
        }
    }
    return {};
}

auto ReadTextFile(const std::filesystem::path& path)
        -> std::optional<std::string> {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) return std::nullopt;
        std::string contents(std::istreambuf_iterator<char>(input), {});
        if (input.bad()) return std::nullopt;
        return contents;
    } catch (...) {
        return std::nullopt;
    }
}

auto StoreTextAtomically(const std::filesystem::path& path,
        const std::string& contents) noexcept -> bool {
    try {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
        std::filesystem::path temporaryPath = path;
        temporaryPath += L".ultimate-scrapper.tmp";
        error.clear();
        (void)std::filesystem::remove(temporaryPath, error);
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) return false;
        output.write(contents.data(),
            static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output) {
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
        if (!MoveFileExW(temporaryPath.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

auto EnsureOriginalLayoutBackup(const std::filesystem::path& path) noexcept
        -> bool {
    try {
        std::filesystem::path backupPath = path;
        backupPath += L".ultimate-scrapper.bak";
        std::error_code error;
        if (std::filesystem::exists(backupPath, error)) return !error;
        error.clear();
        std::filesystem::copy_file(path, backupPath,
            std::filesystem::copy_options::none, error);
        return !error;
    } catch (...) {
        return false;
    }
}

struct LayoutVariantPaths {
    std::filesystem::path native;
    std::filesystem::path separate;
};

auto GetLayoutVariantPaths(const std::filesystem::path& layoutPath)
        -> LayoutVariantPaths {
    const auto configPath = ConfigPath();
    if (configPath.empty() || configPath.parent_path().empty()) return {};
    const auto variantDirectory = configPath.parent_path()
        / L"ultimate-scrapper-layouts";
    const auto baseName = layoutPath.stem().wstring();
    const auto extension = layoutPath.extension().wstring();
    return {
        variantDirectory / (baseName + L".native" + extension),
        variantDirectory / (baseName + L".separate" + extension),
    };
}

auto HasAnyLayoutVariantJson(const std::filesystem::path& layoutPath) noexcept
        -> bool {
    try {
        const auto variantPaths = GetLayoutVariantPaths(layoutPath);
        if (variantPaths.native.empty()) return true;
        std::error_code error;
        if (!std::filesystem::exists(variantPaths.native.parent_path(), error)) {
            return static_cast<bool>(error);
        }
        if (error) return true;
        std::filesystem::directory_iterator entry(
            variantPaths.native.parent_path(), error);
        if (error) return true;
        const std::filesystem::directory_iterator end;
        for (; entry != end; entry.increment(error)) {
            if (error) return true;
            if (entry->is_regular_file(error)
                    && entry->path().extension() == L".json") {
                return true;
            }
            if (error) return true;
        }
        return static_cast<bool>(error);
    } catch (...) {
        return true;
    }
}

auto NormalizeLayoutVariant(std::string& json, bool nativeButtonMode,
        bool cubeLayout, bool addBatchCheckbox) -> bool {
    if (nativeButtonMode) {
        if (!RemoveNamedJsonObject(json, "ultimate_scrapper_transmute")) {
            return false;
        }
        return !addBatchCheckbox
            || InjectBatchCheckbox(json, cubeLayout, nativeButtonMode);
    }
    if (!InjectSeparateButton(json, cubeLayout)) return false;
    return SetNamedWidgetVisible(json, "ultimate_scrapper_transmute", true)
        && SetNamedWidgetString(json, "ultimate_scrapper_transmute",
            "tooltipString", "Salvage")
        && (!addBatchCheckbox
            || InjectBatchCheckbox(json, cubeLayout, nativeButtonMode));
}

auto PrepareLayoutVariants(const std::filesystem::path& layoutPath,
        LayoutVariantPaths& variantPaths, bool cubeLayout,
        bool seedWhenEmpty, bool showBatchCheckboxes,
        bool addBatchCheckboxes) noexcept -> bool {
    if (layoutPath.empty()) return false;
    try {
        variantPaths = GetLayoutVariantPaths(layoutPath);
        if (variantPaths.native.empty() || variantPaths.separate.empty()) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(
            variantPaths.native.parent_path(), error);
        if (error) return false;

        const bool nativeVariantExists = std::filesystem::exists(
            variantPaths.native, error);
        if (error) return false;
        const bool separateVariantExists = std::filesystem::exists(
            variantPaths.separate, error);
        if (error || nativeVariantExists != separateVariantExists) {
            return false;
        }

        std::optional<std::string> nativeJson;
        std::optional<std::string> separateJson;
        if (nativeVariantExists) {
            nativeJson = ReadTextFile(variantPaths.native);
            separateJson = ReadTextFile(variantPaths.separate);
            if (!nativeJson || !separateJson) return false;
        }

        if (!nativeVariantExists) {
            if (!seedWhenEmpty) return false;
            auto activeJson = ReadTextFile(layoutPath);
            if (!activeJson || activeJson->empty()) return false;

            std::string nativeBase = *activeJson;
            std::string separateBase = *activeJson;
            if (!NormalizeLayoutVariant(nativeBase, true, cubeLayout,
                        addBatchCheckboxes)
                    || !NormalizeLayoutVariant(separateBase, false,
                        cubeLayout, addBatchCheckboxes)) {
                return false;
            }
            if (!StoreTextAtomically(variantPaths.native, nativeBase)) {
                return false;
            }
            if (!StoreTextAtomically(variantPaths.separate, separateBase)) {
                std::filesystem::remove(variantPaths.native, error);
                return false;
            }
            nativeJson = std::move(nativeBase);
            separateJson = std::move(separateBase);
        }

        if (!nativeJson || nativeJson->empty() || !separateJson
                || separateJson->empty()) {
            return false;
        }
        const auto injectionMarker = variantPaths.native.parent_path()
            / (cubeLayout ? L"batch-checkbox-injected.marker"
                : L"stash-batch-checkbox-injected.marker");
        const bool markerExists = std::filesystem::exists(
            injectionMarker, error);
        if (error) return false;
        const std::string nativeBefore = *nativeJson;
        const std::string separateBefore = *separateJson;
        if (!markerExists && addBatchCheckboxes) {
            if (!InjectBatchCheckbox(*nativeJson, cubeLayout, true)
                    || !InjectBatchCheckbox(*separateJson, cubeLayout,
                        false)) {
                return false;
            }
        } else if (!SyncExistingBatchCheckbox(*nativeJson, cubeLayout)
                || !SyncExistingBatchCheckbox(*separateJson, cubeLayout)) {
            return false;
        }
        if (!SetBatchCheckboxVisibility(*nativeJson, showBatchCheckboxes)
                || !SetBatchCheckboxVisibility(*separateJson,
                    showBatchCheckboxes)) {
            return false;
        }

        if (*nativeJson != nativeBefore
                && !StoreTextAtomically(variantPaths.native, *nativeJson)) {
            return false;
        }
        if (*separateJson != separateBefore
                && !StoreTextAtomically(variantPaths.separate, *separateJson)) {
            return false;
        }
        if (!markerExists && addBatchCheckboxes
                && !StoreTextAtomically(injectionMarker,
                "Batch checkbox was added to the layout variants.\n")) {
            return false;
        }
        if (FindNamedJsonObject(*nativeJson,
                    "ultimate_scrapper_transmute")
                || !FindNamedJsonObject(*separateJson,
                    "ultimate_scrapper_transmute")) {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

auto ApplyLayoutFile(const std::filesystem::path& path,
        bool nativeButtonMode, bool cubeLayout,
        bool seedWhenEmpty, bool showBatchCheckboxes,
        bool addBatchCheckboxes) noexcept -> bool {
    if (path.empty()) return false;
    try {
        LayoutVariantPaths variantPaths;
        if (!EnsureOriginalLayoutBackup(path)
                || !PrepareLayoutVariants(path, variantPaths, cubeLayout,
                    seedWhenEmpty, showBatchCheckboxes,
                    addBatchCheckboxes)) {
            return false;
        }
        const auto& selectedPath = nativeButtonMode
            ? variantPaths.native : variantPaths.separate;
        auto selectedJson = ReadTextFile(selectedPath);
        if (!selectedJson || selectedJson->empty()) return false;
        if (!showBatchCheckboxes
                && (!RemoveNamedJsonObject(*selectedJson,
                        "ultimate_scrapper_batch_toggle")
                    || !RemoveNamedJsonObject(*selectedJson,
                        "ultimate_scrapper_batch_label"))) {
            return false;
        }
        const auto activeJson = ReadTextFile(path);
        if (activeJson && *activeJson == *selectedJson) return true;
        return StoreTextAtomically(path, *selectedJson);
    } catch (...) {
        return false;
    }
}

void ApplyButtonLayout() noexcept {
    if (Context == nullptr) return;
    const bool nativeButtonMode = Config.transmuteButtonMode
        == TransmuteButtonMode::Native;
    const bool showBatchCheckboxes = Config.showBatchCheckboxes
        && !Config.batchScrapAlwaysOn;
    const bool addBatchCheckboxes = !Config.batchScrapAlwaysOn;
    try {
        const auto cubeLayout = FindActiveLayoutFile(
            L"horadriccubelayouthd.json");
        const auto bankLayout = FindActiveLayoutFile(
            L"bankexpansionlayouthd.json");
        const auto seedProbe = !cubeLayout.empty() ? cubeLayout : bankLayout;
        const bool seedWhenEmpty = !seedProbe.empty()
            && !HasAnyLayoutVariantJson(seedProbe);
        if (!cubeLayout.empty()) {
            if (!ApplyLayoutFile(cubeLayout, nativeButtonMode, true,
                    seedWhenEmpty, showBatchCheckboxes,
                    addBatchCheckboxes)) {
                Context->LogWarn("UltimateScrapper: could not select the configured cube button layout variant; check d2rloader/config/ultimate-scrapper-layouts and file permissions.");
            } else {
                Context->LogInfo(nativeButtonMode
                    ? "UltimateScrapper: Native cube button layout variant is active."
                    : "UltimateScrapper: Separate cube button layout variant is active.");
            }
        }
        if (!bankLayout.empty()) {
            if (!ApplyLayoutFile(bankLayout, nativeButtonMode, false,
                    seedWhenEmpty, showBatchCheckboxes,
                    addBatchCheckboxes)) {
                Context->LogWarn("UltimateScrapper: could not select the configured stash button layout variant; check d2rloader/config/ultimate-scrapper-layouts and file permissions.");
            } else {
                Context->LogInfo(nativeButtonMode
                    ? "UltimateScrapper: Native stash button layout variant is active."
                    : "UltimateScrapper: Separate stash button layout variant is active.");
            }
        }
    } catch (...) {
        Context->LogWarn("UltimateScrapper: could not inspect active layout JSON files for the selected button mode.");
    }
}

auto ReadInteger(const toml::table& table, std::string_view key,
        std::uint32_t minimum, std::uint32_t maximum,
        std::uint32_t& destination, std::string& error) -> bool {
    const auto* node = table.get(key);
    if (node == nullptr) return true;
    const auto value = node->value<std::int64_t>();
    if (!value || *value < minimum || *value > maximum) {
        error = std::string(key) + " must be an integer from "
            + std::to_string(minimum) + " through " + std::to_string(maximum);
        return false;
    }
    destination = static_cast<std::uint32_t>(*value);
    return true;
}

auto ReadCustomItemLevel(const toml::table& table,
        CustomSettings& destination, std::string& error) -> bool {
    const auto* node = table.get("item-level");
    if (node == nullptr) return true;
    if (const auto level = node->value<std::int64_t>()) {
        if (*level < 1 || *level > 99) {
            error = "ultimate-scrapper.custom.item-level must be an integer from 1 through 99";
            return false;
        }
        destination.itemLevel = static_cast<std::uint32_t>(*level);
        return true;
    }
    error = "ultimate-scrapper.custom.item-level must be an integer from 1 through 99";
    return false;
}

auto ReadWeight(const toml::table& table, std::string_view key,
        double& destination, std::string& error) -> bool {
    const auto* node = table.get(key);
    if (node == nullptr) return true;
    double value{};
    if (const auto floating = node->value<double>()) {
        value = *floating;
    } else if (const auto integer = node->value<std::int64_t>()) {
        value = static_cast<double>(*integer);
    } else {
        error = std::string(key) + " must be a non-negative number";
        return false;
    }
    if (!std::isfinite(value) || value < 0.0 || value > 1.0e12) {
        error = std::string(key) + " must be finite and between 0 and 1e12";
        return false;
    }
    destination = value;
    return true;
}

auto ParseItemCode(std::string_view text, std::uint32_t& destination) noexcept -> bool {
    if (text.size() < 3 || text.size() > 4) return false;
    std::array<char, 4> codeChars{' ', ' ', ' ', ' '};
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        if (character < 0x21 || character > 0x7E) return false;
        codeChars[index] = static_cast<char>(character);
    }
    destination = D2RL::Items::MakeItemCode(
        codeChars[0], codeChars[1], codeChars[2], codeChars[3]);
    return true;
}

auto ParseItemTypeCode(std::string_view text,
        std::uint32_t& destination) noexcept -> bool {
    if (text.empty() || text.size() > 4) return false;
    D2RL::Inventory::ItemTypeFourCC itemType{};
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        if (character < 0x21 || character > 0x7E) return false;
        itemType.value[index] = static_cast<char>(character);
    }
    destination = PackItemTypeCode(itemType);
    return true;
}

auto ParseItemQuality(std::string_view text,
        D2RL::Items::Quality& destination) noexcept -> bool {
    if (text == "inferior") destination = D2RL::Items::Quality::Inferior;
    else if (text == "normal") destination = D2RL::Items::Quality::Normal;
    else if (text == "superior") destination = D2RL::Items::Quality::Superior;
    else if (text == "magic") destination = D2RL::Items::Quality::Magic;
    else if (text == "set") destination = D2RL::Items::Quality::Set;
    else if (text == "rare") destination = D2RL::Items::Quality::Rare;
    else if (text == "unique") destination = D2RL::Items::Quality::Unique;
    else if (text == "crafted") destination = D2RL::Items::Quality::Crafted;
    else return false;
    return true;
}

auto TrimInputText(std::string_view text) noexcept -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

auto ParseInputRuleList(const toml::array& inputArray,
        InputRuleList& destination) -> bool {
    if (inputArray.empty() || inputArray.size() > MaximumCubeItems) {
        return false;
    }
    destination.itemCount = inputArray.size();
    for (std::size_t index = 0; index < destination.itemCount; ++index) {
        const auto inputText = inputArray[index].value<std::string>();
        if (!inputText) return false;

        const std::string_view text = TrimInputText(*inputText);
        const std::size_t comma = text.find(',');
        const std::string_view code = TrimInputText(text.substr(0, comma));
        InputRule& rule = destination.items[index];
        if (code == "any") {
            rule.anyCode = true;
        } else if (code.starts_with("type:")) {
            TypeRuleRequested = true;
            const std::string_view typeCode = TrimInputText(code.substr(5));
            if (!ParseItemTypeCode(typeCode, rule.itemTypeCode)
                    || !ItemTypeData.available
                    || !ItemTypeData.knownTypeCodes.contains(rule.itemTypeCode)) {
                return false;
            }
            rule.matchesItemType = true;
        } else if (!ParseItemCode(code, destination.items[index].code)) {
            return false;
        }

        if (comma != std::string_view::npos) {
            if (text.find(',', comma + 1) != std::string_view::npos) return false;
            const std::string_view qualityText = TrimInputText(text.substr(comma + 1));
            if (!ParseItemQuality(qualityText, rule.quality)) {
                return false;
            }
            rule.hasQuality = true;
        }
    }
    return true;
}

auto ParseInputRules(const toml::table& section,
        InputSettings& destination) -> bool {
    destination = InputSettings{};
    const auto* inputNode = section.get("input");
    const auto* input = inputNode == nullptr ? nullptr : inputNode->as_table();
    if (input == nullptr) return false;

    const auto* inputsNode = input->get("inputs");
    if (inputsNode == nullptr) return false;
    destination.usingDefaultRules = false;
    destination.valid = false;

    const auto* inputs = inputsNode->as_array();
    if (inputs == nullptr || inputs->size() > MaximumInputLists) {
        return false;
    }
    const bool hasNestedLists = !inputs->empty()
        && (*inputs)[0].as_array() != nullptr;

    // A single empty string is the TOML sentinel for the default one
    // Unique/Set input recipe. Other empty or malformed entries stay invalid.
    if (!hasNestedLists && inputs->size() == 1) {
        const auto placeholder = (*inputs)[0].value<std::string>();
        if (placeholder && TrimInputText(*placeholder).empty()) {
            destination = InputSettings{};
            return true;
        }
    }

    InputSettings parsed;
    parsed.usingDefaultRules = false;
    parsed.valid = true;
    if (hasNestedLists) {
        parsed.listCount = inputs->size();
        for (std::size_t listIndex = 0; listIndex < parsed.listCount; ++listIndex) {
            const auto* inputArray = (*inputs)[listIndex].as_array();
            if (inputArray == nullptr
                    || !ParseInputRuleList(*inputArray, parsed.lists[listIndex])) {
                return false;
            }
        }
    } else {
        parsed.listCount = 1;
        if (!ParseInputRuleList(*inputs, parsed.lists[0])) {
            return false;
        }
    }

    destination = parsed;
    return true;
}

auto TrimTableField(std::string_view field) noexcept -> std::string_view {
    while (!field.empty()
            && (field.front() == ' ' || field.front() == '\t'
                || field.front() == '\r' || field.front() == '\n')) {
        field.remove_prefix(1);
    }
    while (!field.empty()
            && (field.back() == ' ' || field.back() == '\t'
                || field.back() == '\r' || field.back() == '\n')) {
        field.remove_suffix(1);
    }
    return field;
}

auto SplitTableRow(std::string_view line)
        -> std::vector<std::string_view> {
    std::vector<std::string_view> fields;
    std::size_t start{};
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos) {
            fields.push_back(TrimTableField(line.substr(start)));
            break;
        }
        fields.push_back(TrimTableField(line.substr(start, tab - start)));
        start = tab + 1;
    }
    return fields;
}

auto ReadTableHeaders(std::ifstream& input,
        std::vector<std::string>& headers) -> bool {
    std::string line;
    if (!std::getline(input, line)) return false;
    std::string_view headerLine(line);
    if (headerLine.starts_with("\xEF\xBB\xBF")) headerLine.remove_prefix(3);
    const auto fields = SplitTableRow(headerLine);
    headers.reserve(fields.size());
    for (std::string_view field : fields) {
        std::string name(field);
        for (char& character : name) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        headers.push_back(std::move(name));
    }
    return !headers.empty();
}

auto FindTableColumn(const std::vector<std::string>& headers,
        std::string_view name) -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (headers[index] == name) return index;
    }
    return std::nullopt;
}

auto TableFieldAt(const std::vector<std::string_view>& fields,
        std::size_t index) noexcept -> std::string_view {
    return index < fields.size() ? fields[index] : std::string_view{};
}

auto IsEnabledTableFlag(std::string_view value) noexcept -> bool {
    return value == "1" || value == "true" || value == "yes";
}

auto CleanTableToken(std::string_view value) noexcept -> std::string_view {
    value = TrimTableField(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return TrimTableField(value);
}

auto LoadItemTypeDefinitions(const std::filesystem::path& path,
        ItemTypeCatalog& catalog) -> bool {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    std::vector<std::string> headers;
    if (!ReadTableHeaders(input, headers)) return false;
    const auto codeColumn = FindTableColumn(headers, "code");
    const auto equiv1Column = FindTableColumn(headers, "equiv1");
    const auto equiv2Column = FindTableColumn(headers, "equiv2");
    const auto magicColumn = FindTableColumn(headers, "magic");
    const auto rareColumn = FindTableColumn(headers, "rare");
    const auto normalColumn = FindTableColumn(headers, "normal");
    if (!codeColumn || !equiv1Column || !equiv2Column
            || !magicColumn || !rareColumn || !normalColumn) return false;

    std::size_t rowCount{};
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = SplitTableRow(line);
        const std::string_view codeText = TableFieldAt(fields, *codeColumn);
        std::uint32_t code{};
        if (codeText == "xxx" || !ParseItemTypeCode(codeText, code)) continue;
        if (++rowCount > MaximumItemTypeRows) return false;

        std::array<std::uint32_t, 2> parents{};
        const std::array<std::size_t, 2> parentColumns{
            *equiv1Column, *equiv2Column};
        for (std::size_t index = 0; index < parentColumns.size(); ++index) {
            const std::string_view parentText = TableFieldAt(fields, parentColumns[index]);
            if (parentText != "xxx") {
                (void)ParseItemTypeCode(parentText, parents[index]);
            }
        }
        catalog.knownTypeCodes.insert(code);
        catalog.parentTypes[code] = parents;
        catalog.qualityFlagsByType[code] = {
            .magic = IsEnabledTableFlag(TableFieldAt(fields, *magicColumn)),
            .rare = IsEnabledTableFlag(TableFieldAt(fields, *rareColumn)),
            .normal = IsEnabledTableFlag(TableFieldAt(fields, *normalColumn)),
        };
    }
    return !input.bad() && rowCount != 0;
}

auto LoadItemTypeMemberships(const std::filesystem::path& path,
        ItemTypeCatalog& catalog, std::size_t& totalRows,
        bool equipmentTable) -> bool {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    std::vector<std::string> headers;
    if (!ReadTableHeaders(input, headers)) return false;
    const auto codeColumn = FindTableColumn(headers, "code");
    const auto typeColumn = FindTableColumn(headers, "type");
    const auto type2Column = FindTableColumn(headers, "type2");
    if (!codeColumn || !typeColumn) return false;
    const auto normCodeColumn = FindTableColumn(headers, "normcode");
    const auto uberCodeColumn = FindTableColumn(headers, "ubercode");
    const auto ultraCodeColumn = FindTableColumn(headers, "ultracode");

    std::size_t tableRows{};
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = SplitTableRow(line);
        std::uint32_t itemCode{};
        if (!ParseItemCode(TableFieldAt(fields, *codeColumn), itemCode)) continue;
        if (++tableRows > MaximumItemRows || ++totalRows > MaximumItemRows) {
            return false;
        }

        auto& memberships = catalog.itemTypesByItemCode[itemCode];
        const std::array<std::optional<std::size_t>, 2> typeColumns{
            typeColumn, type2Column};
        for (const auto column : typeColumns) {
            if (!column) continue;
            std::uint32_t typeCode{};
            const std::string_view typeText = TableFieldAt(fields, *column);
            if (ParseItemTypeCode(typeText, typeCode)
                    && catalog.knownTypeCodes.contains(typeCode)
                    && std::find(memberships.begin(), memberships.end(), typeCode)
                        == memberships.end()) {
                memberships.push_back(typeCode);
                catalog.directItemTypesByItemCode[itemCode].push_back(typeCode);
            }
        }
        if (equipmentTable) {
            catalog.equipmentItemCodes.insert(itemCode);
            std::uint32_t family = itemCode;
            std::vector<std::uint32_t> familyCodes{itemCode};
            const std::array<std::optional<std::size_t>, 3> familyColumns{
                normCodeColumn, uberCodeColumn, ultraCodeColumn};
            for (const auto column : familyColumns) {
                if (!column) continue;
                std::uint32_t familyCode{};
                if (ParseItemCode(TableFieldAt(fields, *column), familyCode)) {
                    familyCodes.push_back(familyCode);
                    if (column == normCodeColumn) family = familyCode;
                }
            }
            for (const std::uint32_t familyCode : familyCodes) {
                catalog.itemFamilyByItemCode[familyCode] = family;
            }
        }
    }
    return !input.bad() && tableRows != 0;
}

auto LoadQualityRecordCodes(const std::filesystem::path& path,
        std::string_view codeColumnName,
        std::unordered_set<std::uint32_t>& destination) -> bool {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    std::vector<std::string> headers;
    if (!ReadTableHeaders(input, headers)) return false;
    const auto codeColumn = FindTableColumn(headers, codeColumnName);
    const auto disabledColumn = FindTableColumn(headers, "disabled");
    const auto spawnableColumn = FindTableColumn(headers, "spawnable");
    if (!codeColumn) return false;

    std::size_t rowCount{};
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = SplitTableRow(line);
        const std::string_view codeText = TableFieldAt(fields, *codeColumn);
        std::uint32_t code{};
        if (!ParseItemCode(codeText, code)) continue;
        if (++rowCount > MaximumItemRows) return false;
        if (disabledColumn && IsEnabledTableFlag(
                TableFieldAt(fields, *disabledColumn))) {
            continue;
        }
        const std::string_view spawnable = spawnableColumn
            ? TableFieldAt(fields, *spawnableColumn) : std::string_view{};
        if (!spawnable.empty() && !IsEnabledTableFlag(spawnable)) continue;
        destination.insert(code);
    }
    return !input.bad() && rowCount != 0;
}

auto LoadCraftQualityRules(const std::filesystem::path& path,
        ItemTypeCatalog& catalog) -> bool {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    std::vector<std::string> headers;
    if (!ReadTableHeaders(input, headers)) return false;
    const auto enabledColumn = FindTableColumn(headers, "enabled");
    const auto outputColumn = FindTableColumn(headers, "output");
    const auto inputColumn = FindTableColumn(headers, "input 1");
    if (!outputColumn || !inputColumn) return false;

    std::size_t rowCount{};
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = SplitTableRow(line);
        const std::string_view output = CleanTableToken(
            TableFieldAt(fields, *outputColumn));
        bool createsCrafted{};
        std::size_t outputStart{};
        for (;;) {
            const std::size_t comma = output.find(',', outputStart);
            if (CleanTableToken(output.substr(outputStart,
                    comma == std::string_view::npos
                        ? output.size() - outputStart : comma - outputStart)) == "crf") {
                createsCrafted = true;
                break;
            }
            if (comma == std::string_view::npos) break;
            outputStart = comma + 1;
        }
        if (!createsCrafted) continue;
        if (++rowCount > MaximumItemRows) return false;
        if (enabledColumn) {
            const std::string_view enabled = TableFieldAt(fields, *enabledColumn);
            if (!enabled.empty() && !IsEnabledTableFlag(enabled)) continue;
        }

        const std::string_view inputSpec = CleanTableToken(
            TableFieldAt(fields, *inputColumn));
        const std::size_t comma = inputSpec.find(',');
        const std::string_view selector = CleanTableToken(
            inputSpec.substr(0, comma));
        bool upgraded{};
        std::size_t optionStart = comma == std::string_view::npos
            ? inputSpec.size() : comma + 1;
        while (optionStart < inputSpec.size()) {
            const std::size_t optionEnd = inputSpec.find(',', optionStart);
            const std::string_view option = CleanTableToken(inputSpec.substr(
                optionStart, optionEnd == std::string_view::npos
                    ? inputSpec.size() - optionStart : optionEnd - optionStart));
            upgraded = upgraded || option == "upg";
            if (optionEnd == std::string_view::npos) break;
            optionStart = optionEnd + 1;
        }

        std::uint32_t code{};
        if (ParseItemTypeCode(selector, code)
                && catalog.knownTypeCodes.contains(code)) {
            catalog.craftedItemTypeCodes.insert(code);
        } else if (ParseItemCode(selector, code)) {
            catalog.craftedItemCodes.insert(code);
            if (upgraded) {
                const auto family = catalog.itemFamilyByItemCode.find(code);
                catalog.craftedItemFamilies.insert(family == catalog.itemFamilyByItemCode.end()
                    ? code : family->second);
            }
        }
    }
    return !input.bad() && rowCount != 0;
}

auto AddQualityIfMissing(std::vector<D2RL::Items::Quality>& qualities,
        D2RL::Items::Quality quality) -> void {
    if (std::find(qualities.begin(), qualities.end(), quality) == qualities.end()) {
        qualities.push_back(quality);
    }
}

auto HasCraftedQuality(const ItemTypeCatalog& catalog,
        std::uint32_t itemCode,
        const std::vector<std::uint32_t>& itemTypes) noexcept -> bool {
    if (catalog.craftedItemCodes.contains(itemCode)) return true;
    for (const std::uint32_t typeCode : itemTypes) {
        if (catalog.craftedItemTypeCodes.contains(typeCode)) return true;
    }
    const auto family = catalog.itemFamilyByItemCode.find(itemCode);
    return family != catalog.itemFamilyByItemCode.end()
        && catalog.craftedItemFamilies.contains(family->second);
}

auto BuildOutputQualityCatalog(ItemTypeCatalog& catalog) -> void {
    for (const auto& [itemCode, itemTypes] : catalog.itemTypesByItemCode) {
        std::vector<D2RL::Items::Quality> qualities;
        if (catalog.equipmentItemCodes.contains(itemCode)) {
            AddQualityIfMissing(qualities, D2RL::Items::Quality::Inferior);
            AddQualityIfMissing(qualities, D2RL::Items::Quality::Normal);
            AddQualityIfMissing(qualities, D2RL::Items::Quality::Superior);
            AddQualityIfMissing(qualities, D2RL::Items::Quality::Magic);
            AddQualityIfMissing(qualities, D2RL::Items::Quality::Rare);
        } else {
            const auto directTypes = catalog.directItemTypesByItemCode.find(itemCode);
            if (directTypes != catalog.directItemTypesByItemCode.end()) {
                for (const std::uint32_t typeCode : directTypes->second) {
                    const auto flags = catalog.qualityFlagsByType.find(typeCode);
                    if (flags == catalog.qualityFlagsByType.end()) continue;
                    if (flags->second.normal) {
                        AddQualityIfMissing(qualities, D2RL::Items::Quality::Normal);
                    }
                    if (flags->second.magic) {
                        AddQualityIfMissing(qualities, D2RL::Items::Quality::Magic);
                    }
                    if (flags->second.rare) {
                        AddQualityIfMissing(qualities, D2RL::Items::Quality::Rare);
                    }
                }
            }
            // Most miscellaneous items have one fixed Normal quality. ItemTypes
            // explicitly marks jewelry and charms that can instead be Magic/Rare.
            if (qualities.empty()) {
                AddQualityIfMissing(qualities, D2RL::Items::Quality::Normal);
            }
        }
        if (catalog.setItemCodes.contains(itemCode)) {
            AddQualityIfMissing(qualities, D2RL::Items::Quality::Set);
        }
        if (catalog.uniqueItemCodes.contains(itemCode)) {
            AddQualityIfMissing(qualities, D2RL::Items::Quality::Unique);
        }
        if (HasCraftedQuality(catalog, itemCode, itemTypes)) {
            AddQualityIfMissing(qualities, D2RL::Items::Quality::Crafted);
        }
        catalog.validQualitiesByItemCode.emplace(itemCode, std::move(qualities));
    }
}

auto FindItemTypeExcelDirectory() -> std::optional<std::filesystem::path> {
    std::vector<std::filesystem::path> roots;
    if (Context != nullptr && Context->modDirectory != nullptr
            && Context->modDirectory[0] != L'\0') {
        roots.emplace_back(Context->modDirectory);
    }
    const auto configPath = ConfigPath();
    if (!configPath.empty()) {
        roots.push_back(configPath.parent_path().parent_path());
    }

    std::vector<std::filesystem::path> candidates;
    std::string modName;
    if (Context != nullptr && Context->activeMod != nullptr) {
        modName = Context->activeMod;
        if (modName.find_first_of("/\\:") != std::string::npos) {
            modName.clear();
        }
        if (!modName.empty()
                && (modName.size() < 4
                    || modName.substr(modName.size() - 4) != ".mpq")) {
            modName += ".mpq";
        }
    }

    for (const auto& root : roots) {
        if (!modName.empty()) {
            std::u8string modNameUtf8;
            modNameUtf8.reserve(modName.size());
            for (const unsigned char character : modName) {
                modNameUtf8.push_back(static_cast<char8_t>(character));
            }
            candidates.push_back(root / std::filesystem::path(modNameUtf8)
                / L"data" / L"global" / L"excel");
        }
        candidates.push_back(root / L"data" / L"global" / L"excel");
    }

    constexpr std::array<const wchar_t*, 4> RequiredFiles{
        L"itemtypes.txt", L"armor.txt", L"weapons.txt", L"misc.txt"};
    for (const auto& candidate : candidates) {
        bool complete = true;
        for (const wchar_t* filename : RequiredFiles) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(candidate / filename, error)
                    || error) {
                complete = false;
                break;
            }
        }
        if (complete) return candidate;
    }
    return std::nullopt;
}

auto LoadItemTypeCatalog() noexcept -> bool {
    ItemTypeData = {};
    try {
        const auto excelDirectory = FindItemTypeExcelDirectory();
        if (!excelDirectory) return false;

        ItemTypeCatalog catalog;
        if (!LoadItemTypeDefinitions(*excelDirectory / L"itemtypes.txt", catalog)) {
            return false;
        }
        std::size_t totalItemRows{};
        if (!LoadItemTypeMemberships(*excelDirectory / L"armor.txt",
                    catalog, totalItemRows, true)
                || !LoadItemTypeMemberships(*excelDirectory / L"weapons.txt",
                    catalog, totalItemRows, true)
                || !LoadItemTypeMemberships(*excelDirectory / L"misc.txt",
                    catalog, totalItemRows, false)) {
            return false;
        }

        (void)LoadQualityRecordCodes(*excelDirectory / L"uniqueitems.txt",
            "code", catalog.uniqueItemCodes);
        (void)LoadQualityRecordCodes(*excelDirectory / L"setitems.txt",
            "item", catalog.setItemCodes);
        (void)LoadCraftQualityRules(*excelDirectory / L"cubemain.txt", catalog);

        for (auto& [itemCode, memberships] : catalog.itemTypesByItemCode) {
            (void)itemCode;
            std::vector<std::uint32_t> expanded;
            for (std::uint32_t typeCode : memberships) {
                if (std::find(expanded.begin(), expanded.end(), typeCode)
                        == expanded.end()) {
                    expanded.push_back(typeCode);
                }
            }
            for (std::size_t index = 0; index < expanded.size(); ++index) {
                const auto parents = catalog.parentTypes.find(expanded[index]);
                if (parents == catalog.parentTypes.end()) continue;
                for (std::uint32_t parent : parents->second) {
                    if (parent != 0 && catalog.knownTypeCodes.contains(parent)
                            && std::find(expanded.begin(), expanded.end(), parent)
                                == expanded.end()) {
                        expanded.push_back(parent);
                    }
                }
            }
            memberships = std::move(expanded);
        }
        catalog.available = true;
        BuildOutputQualityCatalog(catalog);
        ItemTypeData = std::move(catalog);
        return true;
    } catch (...) {
        ItemTypeData = {};
        return false;
    }
}

auto ParseConfig(std::string_view text, Settings& destination,
        std::string& error) -> bool {
    try {
        const auto root = toml::parse(text);
        const auto* sectionNode = root.get("ultimate-scrapper");
        if (sectionNode == nullptr) return true;
        const auto* section = sectionNode->as_table();
        if (section == nullptr) {
            error = "ultimate-scrapper must be a TOML table";
            return false;
        }
        (void)ParseInputRules(*section, destination.input);
        if (const auto* modeNode = section->get("active-output-mode")) {
            const auto mode = modeNode->value<std::string>();
            if (!mode) {
                error = "ultimate-scrapper.active-output-mode must be \"Runes\", \"Custom\", or \"Both\"";
                return false;
            }
            if (*mode == "Runes") {
                destination.activeOutputMode = OutputMode::Runes;
            } else if (*mode == "Custom") {
                destination.activeOutputMode = OutputMode::Custom;
            } else if (*mode == "Both") {
                destination.activeOutputMode = OutputMode::Both;
            } else {
                error = "ultimate-scrapper.active-output-mode must be \"Runes\", \"Custom\", or \"Both\"";
                return false;
            }
        }
        if (const auto* buttonModeNode = section->get("transmute-button-mode")) {
            const auto buttonMode = buttonModeNode->value<std::string>();
            if (!buttonMode) {
                error = "ultimate-scrapper.transmute-button-mode must be \"Native\" or \"Separate\"";
                return false;
            }
            if (*buttonMode == "Native") {
                destination.transmuteButtonMode = TransmuteButtonMode::Native;
            } else if (*buttonMode == "Separate") {
                destination.transmuteButtonMode = TransmuteButtonMode::Separate;
            } else {
                error = "ultimate-scrapper.transmute-button-mode must be \"Native\" or \"Separate\"";
                return false;
            }
        }
        if (const auto* batchCheckboxNode = section->get(
                "show-batch-checkboxes")) {
            const auto showBatchCheckboxes = batchCheckboxNode->value<bool>();
            if (!showBatchCheckboxes) {
                error = "ultimate-scrapper.show-batch-checkboxes must be true or false";
                return false;
            }
            destination.showBatchCheckboxes = *showBatchCheckboxes;
        }
        if (const auto* batchAlwaysOnNode = section->get(
                "batch-scrap-always-on")) {
            const auto batchAlwaysOn = batchAlwaysOnNode->value<bool>();
            if (!batchAlwaysOn) {
                error = "ultimate-scrapper.batch-scrap-always-on must be true or false";
                return false;
            }
            destination.batchScrapAlwaysOn = *batchAlwaysOn;
        }

        if (destination.activeOutputMode != OutputMode::Custom) {
            if (!ReadInteger(*section, "failure-chance-percent", 0, 100,
                    destination.runes.failureChancePercent, error)) {
                return false;
            }

            const auto* countNode = section->get("output-count-weights");
            if (countNode != nullptr) {
                const auto* counts = countNode->as_table();
                if (counts == nullptr) {
                    error = "ultimate-scrapper.output-count-weights must be a table";
                    return false;
                }
                if (!ReadInteger(*counts, "one", 0, 100,
                        destination.runes.outputCountWeights[0], error)
                        || !ReadInteger(*counts, "two", 0, 100,
                        destination.runes.outputCountWeights[1], error)
                        || !ReadInteger(*counts, "three", 0, 100,
                        destination.runes.outputCountWeights[2], error)) {
                    return false;
                }
            }
            const std::uint32_t countWeightTotal = destination.runes.outputCountWeights[0]
                + destination.runes.outputCountWeights[1] + destination.runes.outputCountWeights[2];
            if (countWeightTotal != 100) {
                error = "output-count-weights one, two, and three must sum to 100";
                return false;
            }

            const auto* runeNode = section->get("rune-weights");
            if (runeNode != nullptr) {
                const auto* runeWeights = runeNode->as_table();
                if (runeWeights == nullptr) {
                    error = "ultimate-scrapper.rune-weights must be a table";
                    return false;
                }
                for (std::size_t index = 0; index < RuneNames.size(); ++index) {
                    if (!ReadWeight(*runeWeights, RuneNames[index],
                            destination.runes.runeWeights[index], error)) {
                        return false;
                    }
                }
            }
            for (const double weight : destination.runes.runeWeights) {
                destination.runes.totalRuneWeight += weight;
            }
            if (!std::isfinite(destination.runes.totalRuneWeight)
                    || destination.runes.totalRuneWeight <= 0.0) {
                error = "at least one rune weight must be greater than zero";
                return false;
            }
        }
        if (destination.activeOutputMode != OutputMode::Runes) {
            const auto* customNode = section->get("custom");
            const auto* custom = customNode == nullptr ? nullptr : customNode->as_table();
            if (custom == nullptr) {
                error = "ultimate-scrapper.custom must be a table when Custom or Both mode is active";
                return false;
            }
            if (!ReadInteger(*custom, "failure-chance-percent", 0, 100,
                    destination.custom.failureChancePercent, error)
                    || !ReadCustomItemLevel(*custom, destination.custom, error)) {
                return false;
            }
            const auto* countNode = custom->get("output-count-weights");
            if (countNode != nullptr) {
                const auto* counts = countNode->as_table();
                if (counts == nullptr) {
                    error = "ultimate-scrapper.custom.output-count-weights must be a table";
                    return false;
                }
                constexpr std::array<std::string_view, MaximumCustomRandomOutputs> CountNames{
                    "one", "two", "three", "four", "five", "six",
                    "seven", "eight", "nine", "ten", "eleven", "twelve",
                };
                for (std::size_t index = 0; index < CountNames.size(); ++index) {
                    if (!ReadInteger(*counts, CountNames[index], 0, 100,
                            destination.custom.outputCountWeights[index], error)) {
                        return false;
                    }
                }
            } else if (custom->get("output-count") != nullptr) {
                std::uint32_t legacyOutputCount{};
                if (!ReadInteger(*custom, "output-count", 1,
                        MaximumCustomRandomOutputs, legacyOutputCount, error)) {
                    error = "legacy custom output-count must be from 1 through 12; replace it with output-count-weights";
                    return false;
                }
                destination.custom.outputCountWeights.fill(0);
                destination.custom.outputCountWeights[legacyOutputCount - 1] = 100;
            }
            std::uint32_t customCountWeightTotal{};
            for (const std::uint32_t weight : destination.custom.outputCountWeights) {
                customCountWeightTotal += weight;
            }
            if (customCountWeightTotal != 100) {
                error = "custom output-count-weights one through twelve must sum to 100";
                return false;
            }
            const auto* itemsNode = custom->get("items");
            const auto* items = itemsNode == nullptr ? nullptr : itemsNode->as_array();
            if (items == nullptr || items->empty()) {
                error = "ultimate-scrapper.custom.items must contain at least one item entry";
                return false;
            }
            if (items->size() > destination.custom.items.size()) {
                error = "ultimate-scrapper.custom.items may contain at most 64 entries";
                return false;
            }
            for (const auto& itemNode : *items) {
                const auto* item = itemNode.as_table();
                if (item == nullptr) {
                    error = "each ultimate-scrapper.custom.items entry must be a table";
                    return false;
                }
                const auto* codeNode = item->get("code");
                const auto codeText = codeNode == nullptr
                    ? std::optional<std::string>{}
                    : codeNode->value<std::string>();
                if (!codeText || codeText->empty() || codeText->size() > 4) {
                    error = "each custom item code must be a one- to four-character D2 item code";
                    return false;
                }
                std::array<char, 4> codeChars{' ', ' ', ' ', ' '};
                for (std::size_t index = 0; index < codeText->size(); ++index) {
                    const unsigned char character = static_cast<unsigned char>((*codeText)[index]);
                    if (character < 0x21 || character > 0x7E) {
                        error = "custom item codes may contain only visible ASCII characters";
                        return false;
                    }
                    codeChars[index] = static_cast<char>(character);
                }

                const auto* qualityNode = item->get("quality");
                const auto qualityText = qualityNode == nullptr
                    ? std::optional<std::string>{}
                    : qualityNode->value<std::string>();
                if (qualityNode != nullptr && !qualityText) {
                    error = "custom item quality must be a string (or blank)";
                    return false;
                }
                const bool randomQuality = qualityNode == nullptr || qualityText->empty();
                std::array<D2RL::Items::Quality, MaximumItemQualities> qualities{};
                std::size_t qualityCount{};
                if (!randomQuality) {
                    std::string_view qualityList(*qualityText);
                    std::size_t tokenStart{};
                    for (;;) {
                        const std::size_t comma = qualityList.find(',', tokenStart);
                        const std::string_view token = TrimInputText(
                            qualityList.substr(tokenStart,
                                comma == std::string_view::npos
                                    ? qualityList.size() - tokenStart
                                    : comma - tokenStart));
                        D2RL::Items::Quality parsedQuality{};
                        if (token.empty() || qualityCount >= qualities.size()
                                || !ParseItemQuality(token, parsedQuality)) {
                            error = "custom quality must be blank or a comma-separated list of inferior, normal, superior, magic, set, rare, unique, or crafted";
                            return false;
                        }
                        if (std::find(qualities.begin(),
                                qualities.begin() + static_cast<std::ptrdiff_t>(qualityCount),
                                parsedQuality) != qualities.begin()
                                    + static_cast<std::ptrdiff_t>(qualityCount)) {
                            error = "custom quality lists may not repeat a quality";
                            return false;
                        }
                        qualities[qualityCount++] = parsedQuality;
                        if (comma == std::string_view::npos) break;
                        tokenStart = comma + 1;
                    }
                }

                CustomOutputEntry& entry = destination.custom.items[destination.custom.itemCount];
                entry.code = D2RL::Items::MakeItemCode(codeChars[0], codeChars[1],
                    codeChars[2], codeChars[3]);
                entry.qualities = qualities;
                entry.qualityCount = qualityCount;
                entry.randomQuality = randomQuality;
                if (const auto* guaranteedNode = item->get("guaranteed")) {
                    const auto guaranteed = guaranteedNode->value<bool>();
                    if (!guaranteed) {
                        error = "custom item guaranteed must be true or false";
                        return false;
                    }
                    entry.guaranteed = *guaranteed;
                }
                if (!ReadInteger(*item, "quantity", 1, 5000, entry.quantity, error)
                        || !ReadWeight(*item, "weight", entry.weight, error)) {
                    return false;
                }
                if (entry.guaranteed) {
                    ++destination.custom.guaranteedCount;
                } else {
                    destination.custom.totalWeight += entry.weight;
                }
                ++destination.custom.itemCount;
            }
            if (!std::isfinite(destination.custom.totalWeight)
                    || destination.custom.totalWeight <= 0.0) {
                error = "at least one non-guaranteed custom item weight must be greater than zero";
                return false;
            }
            std::size_t maximumRandomOutputCount{};
            for (std::size_t index = 0;
                    index < destination.custom.outputCountWeights.size(); ++index) {
                if (destination.custom.outputCountWeights[index] > 0) {
                    maximumRandomOutputCount = index + 1;
                }
            }
            if (destination.custom.guaranteedCount + maximumRandomOutputCount
                    > MaximumCustomOutputs) {
                error = "guaranteed custom outputs plus the maximum random output count may not exceed 64 items";
                return false;
            }
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

auto LoadConfig() noexcept -> bool {
    TypeRuleRequested = false;
    std::string configStorage;
    std::string_view text = DefaultConfig;
    try {
        const auto path = ConfigPath();
        if (!path.empty()) {
            std::error_code error;
            if (!std::filesystem::exists(path, error) && !error) {
                std::filesystem::create_directories(path.parent_path(), error);
                if (!error) {
                    std::ofstream output(path, std::ios::binary);
                    if (output.is_open()) {
                        output.write(DefaultConfig,
                            static_cast<std::streamsize>(sizeof(DefaultConfig) - 1));
                    }
                }
            }
            error.clear();
            if (std::filesystem::is_regular_file(path, error) && !error) {
                std::ifstream input(path, std::ios::binary);
                if (input.is_open()) {
                    configStorage.assign(std::istreambuf_iterator<char>(input),
                        std::istreambuf_iterator<char>());
                    if (!input.bad()) text = configStorage;
                } else {
                    Context->LogWarn("UltimateScrapper: config could not be opened; embedded defaults will be used.");
                }
            }
        }
    } catch (...) {
        Context->LogWarn("UltimateScrapper: config access failed; embedded defaults will be used.");
    }

    Settings parsed;
    std::string error;
    if (!ParseConfig(text, parsed, error)) {
        const std::string message = "UltimateScrapper: invalid TOML config (" + error + ").";
        Context->LogError(message.c_str());
        return false;
    }
    Config = parsed;
    if (!Config.input.valid) {
        Context->LogWarn("UltimateScrapper: custom input rules are invalid; no cube inputs will be accepted. Check that every recipe contains 1 through 12 valid entries.");
        if (TypeRuleRequested && !ItemTypeData.available) {
            Context->LogWarn("UltimateScrapper: itemtype rules require itemtypes.txt, armor.txt, weapons.txt, and misc.txt under the active mod's data/global/excel directory.");
        }
    } else if (Config.input.usingDefaultRules) {
        Context->LogInfo("UltimateScrapper: no custom input list was found; defaulting to exactly one Unique or Set item in the cube.");
    }
    return true;
}

void ResetPending() noexcept {
    Pending = {};
    ExchangePending.store(false, std::memory_order_release);
}

void __cdecl ExecuteExchange(const D2RL::PluginContext* context, void*) noexcept {
    const std::lock_guard<std::mutex> lock(PendingMutex);
    if (context == nullptr || context != Context || !Active.load(std::memory_order_acquire)) {
        ResetPending();
        return;
    }

    CubeCollector cube;
    const auto inventoryResult = ReadCubeItems(Pending.player, cube);
    if (inventoryResult != D2RL::Inventory::Result::Success) {
        char message[144]{};
        (void)std::snprintf(message, sizeof(message),
            "UltimateScrapper: could not re-read cube contents on the game thread (Inventory result=%u).",
            static_cast<unsigned>(inventoryResult));
        Context->LogWarn(message);
        ResetPending();
        return;
    }
    if (!MatchesPending(cube, Pending)) {
        char message[160]{};
        (void)std::snprintf(message, sizeof(message),
            "UltimateScrapper: cube contents or an input item changed before the transaction (%u item(s), %u expected); no items were changed.",
            static_cast<unsigned>(cube.count),
            static_cast<unsigned>(Pending.itemCount));
        Context->LogWarn(message);
        ResetPending();
        return;
    }
    for (std::size_t index = 0; index < cube.count; ++index) {
        if (cube.items[index].handle == D2RL::InvalidItemHandle) {
            Context->LogWarn("UltimateScrapper: an input item changed before the transaction; no items were changed.");
            ResetPending();
            return;
        }
    }

    std::array<D2RL::Items::TransactionInput, MaximumCubeItems> inputs{};
    for (std::size_t index = 0; index < cube.count; ++index) {
        inputs[index] = {
            .item = cube.items[index].handle,
            .quantity = static_cast<std::uint32_t>(
                std::max(cube.items[index].quantity, 1)),
            .socketedItemPolicy = D2RL::Items::SocketedItemPolicy::DestroyContents,
        };
    }
    std::array<D2RL::Items::ItemCreateSpec, MaximumCustomOutputs> outputs{};
    std::array<D2RL::ItemHandle, MaximumCustomOutputs> outputItems{};
    D2RL::Items::Transaction transaction{
        .structSize = D2RL::Items::TransactionSize,
        .flags = 0,
        .player = Pending.player,
        .inputCount = static_cast<std::uint32_t>(cube.count),
        .outputCount = 0,
        .inputs = inputs.data(),
        .outputs = outputs.data(),
        .outputItems = outputItems.data(),
        .outputCapacity = MaximumCustomOutputs,
        .reserved = 0,
    };
    D2RL::Items::TransactionResult result{
        .structSize = D2RL::Items::TransactionResultSize,
        .flags = 0,
        .outputCount = 0,
        .reserved = 0,
    };

    const bool processIndividually = Pending.processIndividually;
    const OutputMode recipeOutputMode = processIndividually
        ? OutputMode::Runes : ChooseOutputMode();
    std::size_t outputCount{};
    std::uint32_t failedInputCount{};
    std::uint32_t successfulInputCount{};
    const std::uint32_t attemptCount = processIndividually
        ? static_cast<std::uint32_t>(cube.count) : 1U;
    bool outputCapacityExceeded{};
    for (std::uint32_t attempt = 0; attempt < attemptCount; ++attempt) {
        const OutputMode outputMode = processIndividually
            ? ChooseOutputMode() : recipeOutputMode;
        const std::uint32_t failureChance = outputMode == OutputMode::Runes
            ? Config.runes.failureChancePercent
            : Config.custom.failureChancePercent;
        const std::uint32_t failureRoll =
            std::uniform_int_distribution<std::uint32_t>(1, 100)(RandomEngine);
        if (failureRoll <= failureChance) {
            ++failedInputCount;
            continue;
        }
        ++successfulInputCount;
        if (!AppendOutputsForMode(outputMode, outputs, outputCount)) {
            outputCapacityExceeded = true;
            break;
        }
    }

    if (outputCapacityExceeded) {
        Context->LogWarn("UltimateScrapper: this batch would exceed ItemService's output limit; no input items were changed.");
        ResetPending();
        return;
    }

    transaction.outputCount = static_cast<std::uint32_t>(outputCount);

    const auto status = ItemService->executeTransaction(context, &transaction, &result);
    if (status == D2RL::Items::Result::Success) {
        char message[192]{};
        if (processIndividually) {
            (void)std::snprintf(message, sizeof(message),
                "UltimateScrapper: batch processed %u input item(s) independently: %u failed, %u succeeded, %u output item(s) created.",
                static_cast<unsigned>(cube.count), failedInputCount,
                successfulInputCount, static_cast<unsigned>(outputCount));
        } else if (failedInputCount != 0) {
            const std::uint32_t failureChance = recipeOutputMode == OutputMode::Runes
                ? Config.runes.failureChancePercent
                : Config.custom.failureChancePercent;
            (void)std::snprintf(message, sizeof(message),
                "UltimateScrapper: recipe failed (%u%% chance); %u input item(s) and socket contents were consumed with no output.",
                failureChance, static_cast<unsigned>(cube.count));
        } else if (recipeOutputMode == OutputMode::Runes) {
            (void)std::snprintf(message, sizeof(message),
                "UltimateScrapper: transmuted %u configured input item(s) into %u random rune(s).",
                static_cast<unsigned>(cube.count), static_cast<unsigned>(outputCount));
        } else {
            (void)std::snprintf(message, sizeof(message),
                "UltimateScrapper: transmuted %u configured input item(s) into %u custom item(s).",
                static_cast<unsigned>(cube.count), static_cast<unsigned>(outputCount));
        }
        Context->LogInfo(message);
        if (result.outputCount != transaction.outputCount) {
            Context->LogWarn("UltimateScrapper: ItemService committed the transaction but reported an unexpected output count.");
        }
    } else {
        char message[176]{};
        (void)std::snprintf(message, sizeof(message),
            "UltimateScrapper: transaction rolled back (ItemService result=%u); the input item(s) were left untouched.",
            static_cast<unsigned>(status));
        Context->LogWarn(message);
    }
    ResetPending();
}

auto __cdecl OnUiMessage(const D2RL::PluginContext* context,
        const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept
        -> D2RL::SharedEvents::UiMessageAction {
    if (context != Context || event == nullptr
            || event->structSize < D2RL::SharedEvents::UiMessageEventRequiredSize
            || event->target == nullptr || event->command == nullptr
            || !Active.load(std::memory_order_acquire)) {
        return D2RL::SharedEvents::UiMessageAction::Continue;
    }

    const std::string_view target(event->target);
    const std::string_view command(event->command);
    const std::string_view text = event->text != nullptr
        ? std::string_view(event->text) : std::string_view{};
    const bool isCubeOrStashTarget = target == ConvertTarget
        || target == StashConvertTarget;
    const bool batchToggleAction = isCubeOrStashTarget
        && command == ConvertCommand && text == BatchToggleMarker;
    if (batchToggleAction) {
        if (Config.batchScrapAlwaysOn) {
            BatchModeEnabled.store(true, std::memory_order_release);
            return D2RL::SharedEvents::UiMessageAction::Consume;
        }
        bool current = BatchModeEnabled.load(std::memory_order_acquire);
        while (!BatchModeEnabled.compare_exchange_weak(current, !current,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        Context->LogInfo(current
            ? "UltimateScrapper: individual batch processing is disabled."
            : "UltimateScrapper: individual batch processing is enabled.");
        return D2RL::SharedEvents::UiMessageAction::Consume;
    }

    const bool pluginButtonAction = isCubeOrStashTarget
        && command == ConvertCommand && text == ScrapperButtonMarker;
    const bool nativeConvertAction = isCubeOrStashTarget
        && command == ConvertCommand && !pluginButtonAction;
    if (!pluginButtonAction && !nativeConvertAction) {
        return D2RL::SharedEvents::UiMessageAction::Continue;
    }

    const bool nativeButtonMode = Config.transmuteButtonMode
        == TransmuteButtonMode::Native;
    if (nativeConvertAction && !nativeButtonMode) {
        if (!ConvertMessageLogged.exchange(true, std::memory_order_acq_rel)) {
            Context->LogInfo("UltimateScrapper: observed a native cube or stash Convert message; passing it through.");
        }
        return D2RL::SharedEvents::UiMessageAction::Continue;
    }

    const bool nativeFallback = nativeButtonMode;
    if (pluginButtonAction
            && !ScrapperButtonMessageLogged.exchange(true, std::memory_order_acq_rel)) {
        Context->LogInfo("UltimateScrapper: observed the plugin transmute button action.");
    }
    if (nativeConvertAction
            && !ConvertMessageLogged.exchange(true, std::memory_order_acq_rel)) {
        Context->LogInfo("UltimateScrapper: Native button mode is checking configured inputs before passing unmatched recipes to D2R.");
    }

    D2RL::PlayerHandle player{D2RL::InvalidPlayerHandle};
    if (InventoryService->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success
            || player == D2RL::InvalidPlayerHandle) {
        return nativeFallback ? D2RL::SharedEvents::UiMessageAction::Continue
            : D2RL::SharedEvents::UiMessageAction::Consume;
    }

    CubeCollector cube;
    if (ReadCubeItems(player, cube) != D2RL::Inventory::Result::Success) {
        if (!MismatchMessageLogged.exchange(true, std::memory_order_acq_rel)) {
            Context->LogWarn(nativeFallback
                ? "UltimateScrapper: could not read cube contents in Native button mode; passing the action through to D2R."
                : "UltimateScrapper: could not read cube contents; transmute action was blocked.");
        }
        return nativeFallback ? D2RL::SharedEvents::UiMessageAction::Continue
            : D2RL::SharedEvents::UiMessageAction::Consume;
    }
    const bool batchModeRequested = BatchModeEnabled.load(
        std::memory_order_acquire);
    bool processIndividually{};
    if (!MatchesConfiguredInputs(cube, batchModeRequested,
            processIndividually)) {
        if (!MismatchMessageLogged.exchange(true, std::memory_order_acq_rel)) {
            Context->LogInfo(nativeFallback
                ? "UltimateScrapper: no configured input recipe matched; passing the action through to D2R."
                : "UltimateScrapper: no configured input recipe matched the cube contents.");
        }
        return nativeFallback ? D2RL::SharedEvents::UiMessageAction::Continue
            : D2RL::SharedEvents::UiMessageAction::Consume;
    }
    for (std::size_t index = 0; index < cube.count; ++index) {
        if (cube.items[index].handle == D2RL::InvalidItemHandle) {
            return D2RL::SharedEvents::UiMessageAction::Consume;
        }
    }
    if (ExchangePending.exchange(true, std::memory_order_acq_rel)) {
        return D2RL::SharedEvents::UiMessageAction::Consume;
    }
    {
        const std::lock_guard<std::mutex> lock(PendingMutex);
        Pending = {};
        Pending.player = player;
        Pending.itemCount = cube.count;
        Pending.batchModeRequested = batchModeRequested;
        Pending.processIndividually = processIndividually;
        for (std::size_t index = 0; index < cube.count; ++index) {
            Pending.items[index] = cube.items[index];
        }
    }
    if (ThreadService->runOnGameThread(context, ExecuteExchange, nullptr)
            != D2RL::Threads::Result::Success) {
        const std::lock_guard<std::mutex> lock(PendingMutex);
        ResetPending();
        Context->LogWarn("UltimateScrapper: could not schedule the transaction on the game thread.");
        return D2RL::SharedEvents::UiMessageAction::Consume;
    }
    return D2RL::SharedEvents::UiMessageAction::Consume;
}

auto Initialize(const D2RL::PluginContext* context) -> bool {
    BatchModeEnabled.store(false, std::memory_order_release);
    if (context->QueryService(D2RL::ServiceId::Inventory,
            D2RL::InventoryServiceV1Version, &InventoryService)
            != D2RL::ServiceQueryResult::Success
            || InventoryService == nullptr
            || InventoryService->serviceSize < D2RL::InventoryServiceV1RequiredSize
            || InventoryService->getLocalPlayer == nullptr
            || InventoryService->forEachInventoryItem == nullptr) {
        context->LogError("UltimateScrapper: D2RLoader InventoryService v1 is required.");
        return false;
    }
    if (context->QueryService(D2RL::ServiceId::Item,
            D2RL::ItemServiceV1Version, &ItemService)
            != D2RL::ServiceQueryResult::Success
            || ItemService == nullptr
            || ItemService->serviceSize < D2RL::ItemServiceV1RequiredSize
            || ItemService->destroyItem == nullptr
            || ItemService->executeTransaction == nullptr) {
        context->LogError("UltimateScrapper: D2RLoader ItemService v1 transaction support is required.");
        return false;
    }
    if (context->QueryService(D2RL::ServiceId::Thread,
            D2RL::ThreadServiceV1Version, &ThreadService)
            != D2RL::ServiceQueryResult::Success
            || ThreadService == nullptr
            || ThreadService->serviceSize < D2RL::ThreadServiceV1RequiredSize
            || ThreadService->runOnGameThread == nullptr) {
        context->LogError("UltimateScrapper: D2RLoader game-thread service is required.");
        return false;
    }
    if (context->QueryService(D2RL::ServiceId::SharedEvent,
            D2RL::SharedEventServiceV1Version, &EventService)
            != D2RL::ServiceQueryResult::Success
            || EventService == nullptr
            || EventService->serviceSize < D2RL::SharedEventServiceV1RequiredSize
            || EventService->registerUiMessageListener == nullptr
            || EventService->unregisterUiMessageListener == nullptr) {
        context->LogError("UltimateScrapper: D2RLoader shared UI-message service is required.");
        return false;
    }
    (void)LoadItemTypeCatalog();
    if (!LoadConfig()) return false;
    BatchModeEnabled.store(Config.batchScrapAlwaysOn,
        std::memory_order_release);
    ApplyButtonLayout();

    std::uint64_t seed = GetTickCount64()
        ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U);
    try {
        std::random_device random;
        seed ^= (static_cast<std::uint64_t>(random()) << 32U) | random();
    } catch (...) {
    }
    RandomEngine.seed(static_cast<std::mt19937::result_type>(seed ^ (seed >> 32U)));

    const D2RL::SharedEvents::UiMessageListener listener{
        .structSize = D2RL::SharedEvents::UiMessageListenerSize,
        .flags = 0,
        .priority = 10000,
        .reserved = 0,
        .callback = OnUiMessage,
        .userData = nullptr,
    };
    if (EventService->registerUiMessageListener(context, &listener, &MessageHandle)
            != D2RL::SharedEvents::Result::Success
            || MessageHandle == D2RL::SharedEvents::InvalidHandle) {
        context->LogError("UltimateScrapper: could not register for the Horadric Cube Convert action.");
        return false;
    }
    Active.store(true, std::memory_order_release);
    if (Config.activeOutputMode == OutputMode::Runes) {
        context->LogInfo("UltimateScrapper 0.1.0 is ready in Runes mode; configured input items are accepted.");
    } else if (Config.activeOutputMode == OutputMode::Custom) {
        context->LogInfo("UltimateScrapper 0.1.0 is ready in Custom mode; configured input items are accepted.");
    } else {
        context->LogInfo("UltimateScrapper 0.1.0 is ready in Both mode; each transmute chooses Runes or Custom with equal probability.");
    }
    if (Config.transmuteButtonMode == TransmuteButtonMode::Native) {
        context->LogInfo("UltimateScrapper: Native button mode is active; configured plugin recipes take priority on Convert, and unmatched contents pass to D2R.");
    } else {
        context->LogInfo("UltimateScrapper: Separate button mode is active; native Convert messages pass through and the marked button runs plugin recipes.");
    }
    if (Config.batchScrapAlwaysOn) {
        context->LogInfo("UltimateScrapper: batch scrap is always enabled; its checkbox and label are omitted from the active layouts.");
    }
    return true;
}

void Shutdown() noexcept {
    Active.store(false, std::memory_order_release);
    if (Context != nullptr && EventService != nullptr
            && MessageHandle != D2RL::SharedEvents::InvalidHandle
            && EventService->unregisterUiMessageListener != nullptr) {
        (void)EventService->unregisterUiMessageListener(Context, MessageHandle);
    }
    MessageHandle = D2RL::SharedEvents::InvalidHandle;
    const std::lock_guard<std::mutex> lock(PendingMutex);
    ResetPending();
    RandomEngine.seed(0);
    ConvertMessageLogged.store(false, std::memory_order_release);
    ScrapperButtonMessageLogged.store(false, std::memory_order_release);
    MismatchMessageLogged.store(false, std::memory_order_release);
    ItemTypeData = {};
    TypeRuleRequested = false;
    EventService = nullptr;
    ThreadService = nullptr;
    ItemService = nullptr;
    InventoryService = nullptr;
}

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    Shutdown();
    if (!D2RL::HasContext(context) || context->apiVersion != D2RL_PLUGIN_API_VERSION) return false;
    Context = context;
    bool initialized{};
    try {
        initialized = Initialize(context);
    } catch (...) {
        Context->LogError("UltimateScrapper: plugin initialization failed.");
    }
    if (!initialized) {
        Shutdown();
        Context = nullptr;
        return false;
    }
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Shutdown();
    Context = nullptr;
}

