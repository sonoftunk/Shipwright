#include "randomizer_item_tracker.h"
#include "../../util.h"
#include "../../OTRGlobals.h"
#include <libultraship/ImGuiImpl.h>
#include "../../UIWidgets.hpp"

#include <map>
#include <string>
#include <vector>
#include <set>
#include <libultraship/Cvar.h>
#include <libultraship/Hooks.h>
#include "3drando/item_location.hpp"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
}
extern "C" uint32_t ResourceMgr_IsSceneMasterQuest(s16 sceneNum);

std::set<RandomizerCheck> checkedLocations;
std::set<RandomizerCheck> skippedLocations;
std::set<RandomizerCheck> prevCheckedLocations;
RandomizerCheck lastLocationChecked;

bool HasItemBeenSkipped(RandomizerCheckObject obj) {
    return skippedLocations.find(obj.rc) != skippedLocations.end();
}

bool HasItemBeenCollected(RandomizerCheckObject obj) {
    // TODO doesn't consider vanilla/MQ?

    // TODO move all the code to a static function in item_location
    // return Location(obj.rc)->GetCollectionCheck().IsChecked(gSaveContext);

    ItemLocation* x = Location(obj.rc);
    SpoilerCollectionCheck check = x->GetCollectionCheck();
    auto flag = check.flag;
    auto scene = check.scene;
    auto type = check.type;

    int shift;
    int mask;

    switch (type) {
        case SpoilerCollectionCheckType::SPOILER_CHK_ALWAYS_COLLECTED:
            return true;
        case SpoilerCollectionCheckType::SPOILER_CHK_BIGGORON:
            return gSaveContext.bgsFlag & flag;
        case SpoilerCollectionCheckType::SPOILER_CHK_CHEST:
            return gSaveContext.sceneFlags[scene].chest & (1 << flag);
        case SpoilerCollectionCheckType::SPOILER_CHK_COLLECTABLE:
            return gSaveContext.sceneFlags[scene].collect & (1 << flag);
        case SpoilerCollectionCheckType::SPOILER_CHK_MERCHANT:
        case SpoilerCollectionCheckType::SPOILER_CHK_SHOP_ITEM:
        case SpoilerCollectionCheckType::SPOILER_CHK_COW:
        case SpoilerCollectionCheckType::SPOILER_CHK_SCRUB:
        case SpoilerCollectionCheckType::SPOILER_CHK_RANDOMIZER_INF:
            return Flags_GetRandomizerInf(randomizerFlagLookup[obj.rc]); // TODO randomizer.cpp has rcToRandomizerInf
        case SpoilerCollectionCheckType::SPOILER_CHK_EVENT_CHK_INF:
            return gSaveContext.eventChkInf[flag / 16] & (0x01 << flag % 16);
        case SpoilerCollectionCheckType::SPOILER_CHK_GERUDO_MEMBERSHIP_CARD:
            return CHECK_FLAG_ALL(gSaveContext.eventChkInf[0x09], 0x0F);
        case SpoilerCollectionCheckType::SPOILER_CHK_GOLD_SKULLTULA:
            return GET_GS_FLAGS(scene) & flag;
        case SpoilerCollectionCheckType::SPOILER_CHK_INF_TABLE:
            // Magic to flip an index `flag` to a lookup for 16bit big endian integers. Probably an easier way.....
            shift = 7 - (flag % 8) + ((flag % 16) / 8) * 8;
            mask = 0x8000 >> shift;
            return gSaveContext.infTable[scene] & mask;
        case SpoilerCollectionCheckType::SPOILER_CHK_ITEM_GET_INF:
            // Magic to flip an index `flag` to a lookup for 16bit big endian integers. Probably an easier way.....
            shift = 7 - (flag % 8) + ((flag % 16) / 8) * 8;
            mask = 0x8000 >> shift;
            return gSaveContext.itemGetInf[flag / 16] & mask;
        case SpoilerCollectionCheckType::SPOILER_CHK_MAGIC_BEANS:
            return BEANS_BOUGHT >= 10;
        case SpoilerCollectionCheckType::SPOILER_CHK_MINIGAME:
            if (obj.rc == RC_LH_CHILD_FISHING)
                return HIGH_SCORE(HS_FISHING) & 0x400;
            if (obj.rc == RC_LH_ADULT_FISHING)
                return HIGH_SCORE(HS_FISHING) & 0x800;
        case SpoilerCollectionCheckType::SPOILER_CHK_NONE:
            return false;
        case SpoilerCollectionCheckType::SPOILER_CHK_POE_POINTS:
            return gSaveContext.highScores[HS_POE_POINTS] >= 1000;
        case SpoilerCollectionCheckType::SPOILER_CHK_GRAVEDIGGER:
            // Gravedigger has a fix in place that means one of two save locations. Check both.
            return (gSaveContext.itemGetInf[1] & 0x1000) ||
                   CVar_GetS32("gGravediggingTourFix", 0) && gSaveContext.sceneFlags[scene].collect & (1 << flag);
        default:
            return false;
    }
    return false;
}

RandomizerCheckArea lastArea = RCAREA_INVALID;
void DrawLocations() {

    if (ImGui::BeginTable("tableRandoChecks", 2, ImGuiTableFlags_BordersH | ImGuiTableFlags_BordersV)) {
        ImGui::TableSetupColumn("To Check", ImGuiTableColumnFlags_WidthStretch, 200.0f);
        ImGui::TableSetupColumn("Checked", ImGuiTableColumnFlags_WidthStretch, 200.0f);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();

        // COLUMN 1 - TO CHECK LOCATIONS
        ImGui::TableNextColumn();

        static ImGuiTextFilter locationSearch;
        locationSearch.Draw();

        bool lastItemFound = false;
        bool doAreaScroll = false;
        bool inGame = gPlayState != nullptr && gSaveContext.fileNum >= 0 && gSaveContext.fileNum <= 2;
        RandomizerCheckArea currentArea = RCAREA_INVALID;
        SceneID sceneId = SCENE_ID_MAX;
        if (gPlayState != nullptr) {
            sceneId = (SceneID)gPlayState->sceneNum;
            currentArea = RandomizerCheckObjects::GetRCAreaBySceneID(sceneId);
        }

        ImGui::BeginChild("ChildToCheckLocations", ImVec2(0, -8));
        for (auto& [rcArea, rcObjects] : RandomizerCheckObjects::GetAllRCObjectsByArea()) {
            bool hasItems = false;
            for (auto& locationIt : rcObjects) {
                if (!locationIt.second.visibleInImgui)
                    continue;

                if (!checkedLocations.count(locationIt.second.rc) && !skippedLocations.count(locationIt.second.rc) &&
                    locationSearch.PassFilter(locationIt.second.rcSpoilerName.c_str())) {

                    hasItems = true;
                    doAreaScroll =
                        (currentArea != RCAREA_INVALID && sceneId != SCENE_KAKUSIANA && // Don't move for grottos
                         sceneId != SCENE_YOUSEI_IZUMI_TATE &&
                         sceneId != SCENE_YOUSEI_IZUMI_YOKO && // Don't move for fairy fountains
                         sceneId != SCENE_SHOP1 &&
                         sceneId !=
                             SCENE_SYATEKIJYOU && // Don't move for Bazaar/Gallery, as it moves between Kak and Market
                         currentArea != lastArea &&
                         currentArea == rcArea);
                    break;
                }
            }

            if (hasItems) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNode(RandomizerCheckObjects::GetRCAreaName(rcArea).c_str())) {
                    if (doAreaScroll) {
                        ImGui::SetScrollHereY(0.0f);
                        doAreaScroll = false;
                    }
                    for (auto& locationIt : rcObjects) {
                        if (!locationIt.second.visibleInImgui)
                            continue;

                        bool checked = HasItemBeenCollected(locationIt.second);
                        bool skipped = HasItemBeenSkipped(locationIt.second);

                        // If the location has its scene flag set
                        if (inGame && checked) {
                            // show it as checked
                            checkedLocations.insert(locationIt.second.rc);
                            if (skipped)
                                skippedLocations.erase(locationIt.second.rc);

                            if (!lastItemFound &&
                                prevCheckedLocations.find(locationIt.second.rc) == prevCheckedLocations.end()) {
                                lastItemFound = true;
                                prevCheckedLocations.insert(locationIt.second.rc);
                                lastLocationChecked = locationIt.second.rc;
                            }
                        }

                        if (locationIt.second.visibleInImgui && !checkedLocations.count(locationIt.second.rc) &&
                            !skippedLocations.count(locationIt.second.rc) &&
                            locationSearch.PassFilter(locationIt.second.rcSpoilerName.c_str())) {

                            if (ImGui::ArrowButton(std::to_string(locationIt.second.rc).c_str(), ImGuiDir_Right)) {
                                skippedLocations.insert(locationIt.second.rc);
                            } else {
                                ImGui::SameLine();
                                ImGui::Text(locationIt.second.rcShortName.c_str());
                            }
                        }
                    }
                    ImGui::TreePop();
                }
            }
        }
        ImGui::EndChild();

        // COLUMN 2 - CHECKED LOCATIONS
        doAreaScroll = false;
        ImGui::TableNextColumn();
        ImGui::BeginChild("ChildCheckedLocations", ImVec2(0, -8));
        for (auto& [rcArea, rcObjects] : RandomizerCheckObjects::GetAllRCObjectsByArea()) {
            bool hasItems = false;
            for (auto& locationIt : rcObjects) {
                if (!locationIt.second.visibleInImgui)
                    continue;

                if (checkedLocations.count(locationIt.second.rc) || skippedLocations.count(locationIt.second.rc)) {
                    hasItems = true;
                    doAreaScroll =
                        (currentArea != RCAREA_INVALID &&
                         sceneId != SCENE_KAKUSIANA && // Don't move for kakusiana/grottos
                         sceneId != SCENE_YOUSEI_IZUMI_TATE &&
                         sceneId != SCENE_YOUSEI_IZUMI_YOKO && // Don't move for fairy fountains
                         sceneId != SCENE_SHOP1 &&
                         sceneId !=
                             SCENE_SYATEKIJYOU && // Don't move for Bazaar/Gallery, as it moves between Kak and Market
                         currentArea != lastArea &&
                         currentArea == rcArea);
                    break;
                }
            }

            if (hasItems) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNode(RandomizerCheckObjects::GetRCAreaName(rcArea).c_str())) {
                    if (doAreaScroll) {
                        ImGui::SetScrollHereY(0.0f);
                        doAreaScroll = false;
                    }
                    for (auto& locationIt : rcObjects) {
                        if (!locationIt.second.visibleInImgui)
                            continue;

                        bool checked = HasItemBeenCollected(locationIt.second);
                        bool skipped = HasItemBeenSkipped(locationIt.second);

                        auto elfound = checkedLocations.find(locationIt.second.rc);
                        auto skfound = skippedLocations.find(locationIt.second.rc);
                        if (locationIt.second.visibleInImgui &&
                            (elfound != checkedLocations.end() || skfound != skippedLocations.end())) {
                            // If the location has its scene flag set
                            if (!inGame || (!checked && !skipped)) {
                                // show it as unchecked
                                if (!checked && elfound != checkedLocations.end())
                                    checkedLocations.erase(elfound);
                                if (!skipped && skfound != skippedLocations.end())
                                    skippedLocations.erase(skfound);
                            } else if (skipped && ImGui::ArrowButton(std::to_string(locationIt.second.rc).c_str(),
                                                                     ImGuiDir_Left)) {
                                if (skipped)
                                    skippedLocations.erase(skfound);
                            } else if (!skipped) {
                                float sz = ImGui::GetFrameHeight();
                                ImGui::InvisibleButton("", ImVec2(sz, sz));
                            }
                            ImGui::SameLine();
                            std::string txt =
                                (lastLocationChecked == locationIt.second.rc
                                     ? "* "
                                     : "") + // Indicate the last location checked (before app reset at least)
                                locationIt.second.rcShortName;

                            if (skipped)
                                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
                            ImGui::Text(txt.c_str());
                            if (!skipped)
                                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 185, 0, 255));

                            if (skipped)
                                txt = "Skipped";
                            else
                                txt = OTRGlobals::Instance->gRandomizer
                                          ->EnumToSpoilerfileGetName[gSaveContext.itemLocations[locationIt.second.rc]
                                                                         .get.rgID][gSaveContext.language];
                            ImGui::SameLine();
                            ImGui::Text("(%s)", txt.c_str());
                            ImGui::PopStyleColor();
                        }
                    }
                    ImGui::TreePop();
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndTable();

        if (sceneId != SCENE_KAKUSIANA && sceneId != SCENE_YOUSEI_IZUMI_TATE && sceneId != SCENE_YOUSEI_IZUMI_YOKO &&
            sceneId != SCENE_SYATEKIJYOU && sceneId != SCENE_SHOP1)
            lastArea = currentArea;
    }
}

// Windowing stuff
ImVec4 CheckTrackerChromaKeyBackground = { 0, 0, 0, 0 }; // Float value, 1 = 255 in rgb value.
void BeginFloatingWindows(std::string UniqueName, ImGuiWindowFlags flags = 0) {
    ImGuiWindowFlags windowFlags = flags;

    if (windowFlags == 0) {
        windowFlags |=
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoResize;
    }

    if (!CVar_GetS32("gCheckTrackerWindowType", 0)) {
        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        windowFlags |= ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar |
                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;

        if (!CVar_GetS32("gCheckTrackerHudEditMode", 0)) {
            windowFlags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
        }
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, CheckTrackerChromaKeyBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::Begin(UniqueName.c_str(), nullptr, windowFlags);
}
void EndFloatingWindows() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImGui::End();
}

/* TODO: These need to be moved to a common imgui file */
void LabeledComboBoxRightAligned(const char* label, const char* cvar, std::vector<std::string> options,
                                 s32 defaultValue) {
    s32 currentValue = CVar_GetS32(cvar, defaultValue);
    std::string hiddenLabel = "##" + std::string(cvar);
    ImGui::Text(label);
#ifdef __WIIU__
    ImGui::SameLine(ImGui::GetContentRegionAvail().x -
                    (ImGui::CalcTextSize(options[currentValue].c_str()).x * 1.0f + 40.0f));
    ImGui::PushItemWidth((ImGui::CalcTextSize(options[currentValue].c_str()).x * 1.0f) + 60.0f);
#else
    ImGui::SameLine(ImGui::GetContentRegionAvail().x -
                    (ImGui::CalcTextSize(options[currentValue].c_str()).x * 1.0f + 20.0f));
    ImGui::PushItemWidth((ImGui::CalcTextSize(options[currentValue].c_str()).x * 1.0f) + 30.0f);
#endif
    if (ImGui::BeginCombo(hiddenLabel.c_str(), options[currentValue].c_str())) {
        for (int i = 0; i < options.size(); i++) {
            if (ImGui::Selectable(options[i].c_str())) {
                CVar_SetS32(cvar, i);
                SohImGui::RequestCvarSaveOnNextTick();
                //shouldUpdateVectors = true;
            }
        }

        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
}

void PaddedEnhancementCheckbox(const char* text, const char* cvarName, s32 defaultValue = 0, bool padTop = true,
                               bool padBottom = true) {
    if (padTop) {
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
    bool val = (bool)CVar_GetS32(cvarName, defaultValue);
    if (ImGui::Checkbox(text, &val)) {
        CVar_SetS32(cvarName, val);
        SohImGui::RequestCvarSaveOnNextTick();
        //shouldUpdateVectors = true;
    }
    if (padBottom) {
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
}
/* ****************************************************** */

void DrawCheckTracker(bool& open) {
    if (!open) {
        CVar_SetS32("gCheckTrackerEnabled", 0);
        return;
    }

    if (CVar_GetS32("gCheckTrackerWindowType", 0) == 1) {
        if (CVar_GetS32("gCheckTrackerDisplayType", 0) == 0) {
            ImGui::SetNextWindowSize(ImVec2(600, 1000), ImGuiCond_FirstUseEver);
            BeginFloatingWindows("Check Tracker", ImGuiWindowFlags_NoFocusOnAppearing);
            DrawLocations();
            EndFloatingWindows();
        }
    }
}

void DrawCheckTrackerOptions(bool& open) {
    if (!open) {
        CVar_SetS32("gCheckTrackerSettingsEnabled", 0);
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(600, 375), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Check Tracker Settings", &open, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, { 8.0f, 8.0f });
    ImGui::BeginTable("checkTrackerSettingsTable", 2, ImGuiTableFlags_BordersH | ImGuiTableFlags_BordersV);
    ImGui::TableSetupColumn("General settings", ImGuiTableColumnFlags_WidthStretch, 200.0f);
    ImGui::TableSetupColumn("Section settings", ImGuiTableColumnFlags_WidthStretch, 200.0f);
    ImGui::TableHeadersRow();
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("BG Color");
    ImGui::SameLine();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::ColorEdit4("BG Color##gCheckTrackerBgColor", (float*)&CheckTrackerChromaKeyBackground,
                          ImGuiColorEditFlags_AlphaPreview | ImGuiColorEditFlags_AlphaBar |
                              ImGuiColorEditFlags_NoLabel)) {
        CVar_SetFloat("gCheckTrackerBgColorR", CheckTrackerChromaKeyBackground.x);
        CVar_SetFloat("gCheckTrackerBgColorG", CheckTrackerChromaKeyBackground.y);
        CVar_SetFloat("gCheckTrackerBgColorB", CheckTrackerChromaKeyBackground.z);
        CVar_SetFloat("gCheckTrackerBgColorA", CheckTrackerChromaKeyBackground.w);
        SohImGui::RequestCvarSaveOnNextTick();
    }
    ImGui::PopItemWidth();

    LabeledComboBoxRightAligned("Window Type", "gCheckTrackerWindowType", { "Floating", "Window" }, 0);

    if (CVar_GetS32("gCheckTrackerWindowType", 0) == 0) {
        PaddedEnhancementCheckbox("Enable Dragging", "gCheckTrackerHudEditMode", 0);
    }
    UIWidgets::PaddedSeparator();
    ImGui::TableNextColumn();

    ImGui::PopStyleVar(1);
    ImGui::EndTable();

    ImGui::End();
}

void InitCheckTracker() {
    SohImGui::AddWindow("Randomizer", "Check Tracker", DrawCheckTracker, CVar_GetS32("gCheckTrackerEnabled", 0) == 1);
    SohImGui::AddWindow("Randomizer", "Check Tracker Settings", DrawCheckTrackerOptions);
    float trackerBgR = CVar_GetFloat("gCheckTrackerBgColorR", 0);
    float trackerBgG = CVar_GetFloat("gCheckTrackerBgColorG", 0);
    float trackerBgB = CVar_GetFloat("gCheckTrackerBgColorB", 0);
    float trackerBgA = CVar_GetFloat("gCheckTrackerBgColorA", 1);
    CheckTrackerChromaKeyBackground = { trackerBgR, trackerBgG, trackerBgB, trackerBgA }; // Float value, 1 = 255 in rgb value.
    // Crashes when the itemTrackerNotes is empty, so add an empty character to it
    Ship::RegisterHook<Ship::LoadFile>([](uint32_t fileNum) {
        RandomizerCheckObjects::UpdateTrackerImGuiVisibility();
    });
    //Ship::RegisterHook<Ship::SaveFile>([](uint32_t fileNum) {
    //    RandomizerCheckObjects::UpdateTrackerImGuiVisibility();
    //    UpdateChecks();
    //});
    //Ship::RegisterHook<Ship::DeleteFile>([](uint32_t fileNum) {
    //     RandomizerCheckObjects::UpdateTrackerImGuiVisibility();
    //     UpdateChecks();
    //});
    RandomizerCheckObjects::UpdateTrackerImGuiVisibility();
    LocationTable_Init();
}
