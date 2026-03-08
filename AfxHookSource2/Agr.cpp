#include "stdafx.h"

#include "Agr.h"

#include "ClientEntitySystem.h"
#include "MirvTime.h"
#include "RenderSystemDX11Hooks.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../shared/AfxGameRecord.h"
#include "../shared/FovScaling.h"
#include "../shared/StringTools.h"

#include <algorithm>
#include <cctype>
#include <vector>

extern float GetLastCameraFov();
extern void GetLastCameraData(float outOrigin[3], float outAngles[3], float& outFov);
extern void GetLastCameraSize(int& outWidth, int& outHeight);

namespace {

constexpr unsigned int kEntityEffectNoDraw = 0x20;
constexpr unsigned int kEntityEffectNoDrawButTransmit = 0x400;

void SetIdentityMatrix(SOURCESDK::matrix3x4_t& outMatrix) {
    outMatrix[0][0] = 1.0f; outMatrix[0][1] = 0.0f; outMatrix[0][2] = 0.0f; outMatrix[0][3] = 0.0f;
    outMatrix[1][0] = 0.0f; outMatrix[1][1] = 1.0f; outMatrix[1][2] = 0.0f; outMatrix[1][3] = 0.0f;
    outMatrix[2][0] = 0.0f; outMatrix[2][1] = 0.0f; outMatrix[2][2] = 1.0f; outMatrix[2][3] = 0.0f;
}

CEntityInstance* GetEntityByEntryIndex(int entryIndex) {
    if (entryIndex < 0 || !g_pEntityList || !g_GetEntityFromIndex) return nullptr;

    auto* entity = reinterpret_cast<CEntityInstance*>(g_GetEntityFromIndex(*g_pEntityList, entryIndex));
    if (!entity) return nullptr;

    auto handle = entity->GetHandle();
    if (!handle.IsValid() || handle.GetEntryIndex() != entryIndex) return nullptr;

    return entity;
}

CEntityInstance* GetEntityByHandle(SOURCESDK::CS2::CBaseHandle handle) {
    if (!handle.IsValid()) return nullptr;
    return GetEntityByEntryIndex(handle.GetEntryIndex());
}

bool StringContainsInsensitive(const char* value, const char* needle) {
    if (!value || !needle) return false;

    std::string haystack(value);
    std::string search(needle);

    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(search.begin(), search.end(), search.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return std::string::npos != haystack.find(search);
}

const char* SafeCString(const char* value) {
    if (!value) return nullptr;

#ifdef _MSC_VER
    __try {
        for (size_t i = 0; i < 2048; ++i) {
            unsigned char c = static_cast<unsigned char>(value[i]);
            if (0 == c) return value;
            if (!(std::isalnum(c) || c == '_' || c == '-' || c == '/' || c == '\\' || c == '.' || c == ':' || c == ' ')) {
                return nullptr;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    for (size_t i = 0; i < 2048; ++i) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (0 == c) return value;
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '/' || c == '\\' || c == '.' || c == ':' || c == ' ')) {
            return nullptr;
        }
    }
#endif

    return nullptr;
}

} // namespace

CAgrRecorder& CAgrRecorder::Get() {
    static advancedfx::CAfxGameRecord record;
    static CAgrRecorder instance;
    if (!instance.m_Record) instance.m_Record = &record;
    return instance;
}

bool CAgrRecorder::GetRecording() const {
    return m_Record && m_Record->GetRecording();
}

bool CAgrRecorder::StartRecording(const wchar_t* fileName) {
    if (!m_Record) return false;

    if (!m_Enabled) {
        m_Enabled = true;
    }

    m_FrameActive = false;
    m_TrackedHandles.clear();
    m_PendingDeletedHandles.clear();
    m_DebugStats.Reset();

    return m_Record->StartRecording(fileName, GetAgrVersion());
}

void CAgrRecorder::StopRecording() {
    if (!m_Record) return;

    if (m_Debug && 0 < m_DebugStats.frames) {
        advancedfx::Message(
            "AGR debug: frames=%d recorded=%d players=%d ragdolls=%d weapons=%d projectiles=%d viewmodels=%d playerCameras=%d hidden=%d deleted=%d\n",
            m_DebugStats.frames,
            m_DebugStats.recordedEntities,
            m_DebugStats.playerPawns,
            m_DebugStats.ragdolls,
            m_DebugStats.weapons,
            m_DebugStats.projectiles,
            m_DebugStats.viewModels,
            m_DebugStats.playerCameras,
            m_DebugStats.hidden,
            m_DebugStats.deleted
        );
    }

    m_FrameActive = false;
    m_TrackedHandles.clear();
    m_PendingDeletedHandles.clear();
    m_DebugStats.Reset();
    m_Record->EndRecording();
}

void CAgrRecorder::OnEntityDeleted(int handle) {
    if (SOURCESDK_CS2_INVALID_EHANDLE_INDEX == handle) return;
    m_PendingDeletedHandles.insert(handle);
}

void CAgrRecorder::OnBeginMainRenderPass() {
    if (!GetRecording() || m_FrameActive) return;

    m_Record->BeginFrame(g_MirvTime.absoluteframetime_get());
    m_FrameActive = true;
}

void CAgrRecorder::OnEndMainRenderPass() {
    if (!GetRecording() || !m_FrameActive) return;

    RecordEntities();
    WriteCamera();
    m_Record->EndFrame();
    m_FrameActive = false;
}

bool CAgrRecorder::IsWeaponEntity(const char* className, const char* clientClassName) const {
    return
        StringIBeginsWith(className ? className : "", "weapon_")
        || StringIBeginsWith(className ? className : "", "item_")
        || StringContainsInsensitive(className, "weapon")
        || StringContainsInsensitive(clientClassName, "weapon")
        || StringContainsInsensitive(clientClassName, "plantedc4")
        || StringContainsInsensitive(className, "plantedc4");
}

bool CAgrRecorder::IsProjectileEntity(const char* className, const char* clientClassName) const {
    return
        StringEndsWith(className ? className : "", "_projectile")
        || StringContainsInsensitive(clientClassName, "projectile");
}

bool CAgrRecorder::IsRagdollEntity(const char* className, const char* clientClassName) const {
    return StringContainsInsensitive(className, "ragdoll") || StringContainsInsensitive(clientClassName, "ragdoll");
}

bool CAgrRecorder::IsViewModelEntity(const char* className, const char* clientClassName) const {
    return
        StringContainsInsensitive(className, "viewmodel")
        || StringContainsInsensitive(clientClassName, "viewmodel")
        || StringContainsInsensitive(clientClassName, "previewmodel");
}

void CAgrRecorder::CollectFrameContext(FrameContext& outContext) const {
    outContext.viewModelOwnerByHandle.clear();
    outContext.viewModelWeaponOwnerByHandle.clear();
    outContext.activeWeaponHandles.clear();
    outContext.observedPlayerEntryIndices.clear();

    auto collectPlayerContext = [this, &outContext](CEntityInstance* player, int entryIndex, bool forceViewModelSelection) {
        if (!player || !player->IsPlayerPawn() || entryIndex < 0) return;

        const auto activeWeaponHandle = player->GetActiveWeaponHandle();
        if (activeWeaponHandle.IsValid()) {
            outContext.activeWeaponHandles.insert(activeWeaponHandle.ToInt());
        }

        if (!forceViewModelSelection && 0 == m_RecordViewModels) return;
        if (!forceViewModelSelection && -1 != m_RecordViewModels && entryIndex != m_RecordViewModels) return;
        if (!forceViewModelSelection && -1 == m_RecordViewModels && 0 >= player->GetHealth()) return;

        outContext.observedPlayerEntryIndices.insert(entryIndex);

        if (activeWeaponHandle.IsValid()) {
            outContext.viewModelWeaponOwnerByHandle.emplace(activeWeaponHandle.ToInt(), entryIndex);
        }

        auto* activeWeapon = GetEntityByHandle(activeWeaponHandle);
        if (activeWeaponHandle.IsValid() && activeWeapon) {
            const auto viewModelHandle = activeWeapon->GetViewmodelAttachmentHandle();
            if (viewModelHandle.IsValid()) {
                outContext.viewModelOwnerByHandle.emplace(viewModelHandle.ToInt(), entryIndex);
            }
        }

        const auto viewEntityHandle = player->GetViewEntityHandle();
        if (viewEntityHandle.IsValid() && viewEntityHandle.ToInt() != player->GetHandle().ToInt()) {
            outContext.viewModelOwnerByHandle.emplace(viewEntityHandle.ToInt(), entryIndex);
        }
    };

    const int highestIndex = GetHighestEntityIndex();
    for (int entryIndex = 0; entryIndex <= highestIndex; ++entryIndex) {
        auto* entity = GetEntityByEntryIndex(entryIndex);
        if (!entity || !entity->IsPlayerPawn()) continue;
        collectPlayerContext(entity, entryIndex, false);
    }

    if (auto* localPlayer = GetSplitScreenPlayer(0); localPlayer && localPlayer->IsPlayerPawn()) {
        CEntityInstance* observedPlayer = localPlayer;
        const auto observerTargetHandle = localPlayer->GetObserverTarget();
        if (observerTargetHandle.IsValid()) {
            if (auto* observerTarget = GetEntityByHandle(observerTargetHandle); observerTarget && observerTarget->IsPlayerPawn()) {
                observedPlayer = observerTarget;
            }
        }

        if (observedPlayer) {
            collectPlayerContext(observedPlayer, observedPlayer->GetHandle().GetEntryIndex(), true);
        }
    }
}

int CAgrRecorder::FindViewModelOwnerEntryIndex(const FrameContext& frame, CEntityInstance* entity) const {
    for (int depth = 0; entity && depth < 8; ++depth) {
        const int handle = entity->GetHandle().ToInt();
        auto it = frame.viewModelOwnerByHandle.find(handle);
        if (it != frame.viewModelOwnerByHandle.end()) return it->second;

        const auto parentHandle = entity->GetParentHandle();
        if (!parentHandle.IsValid() || parentHandle.ToInt() == handle) break;
        entity = GetEntityByHandle(parentHandle);
    }

    return -1;
}

CAgrRecorder::RecordCategory CAgrRecorder::ClassifyRecordedEntity(const FrameContext& frame, CEntityInstance* entity, bool isViewModel) const {
    if (!entity) return RecordCategory::None;

    const char* className = entity->GetClassName();
    const char* clientClassName = entity->GetClientClassName();

    if (entity->IsPlayerPawn()) return RecordCategory::PlayerPawn;
    if (IsRagdollEntity(className, clientClassName)) return RecordCategory::Ragdoll;
    if (isViewModel || -1 != FindViewModelOwnerEntryIndex(frame, entity)) return RecordCategory::ViewModel;
    if (IsProjectileEntity(className, clientClassName)) return RecordCategory::Projectile;
    if (IsWeaponEntity(className, clientClassName)) return RecordCategory::Weapon;

    return RecordCategory::None;
}

bool CAgrRecorder::ShouldRecordPlayerCamera(int entryIndex) const {
    return 0 != m_RecordPlayerCameras && (-1 == m_RecordPlayerCameras || entryIndex == m_RecordPlayerCameras);
}

bool CAgrRecorder::GetEntityVisible(CEntityInstance* entity, bool isViewModel) const {
    if (!entity) return false;
    if (isViewModel) return true;
    if (entity->IsDormant()) return false;

    const unsigned int effects = entity->GetEffects();
    if (0 != (effects & kEntityEffectNoDraw) && 0 == (effects & kEntityEffectNoDrawButTransmit)) {
        return false;
    }

    return 0 < entity->GetRenderAlpha();
}

int CAgrRecorder::GetAgrVersion() const {
    return advancedfx::kAfxGameRecordFormatVersionCurrent;
}

float CAgrRecorder::ScaleFov(float fov) const {
    int width = 0;
    int height = 0;
    GetLastCameraSize(width, height);
    return static_cast<float>(Apply_FovScaling(width, height, fov, FovScaling_AlienSwarm));
}

bool CAgrRecorder::ShouldRecordEntity(const FrameContext& frame, int entryIndex, CEntityInstance* entity, bool& outVisible, bool& outIsViewModel, bool& outHasPlayerCamera) const {
    outVisible = true;
    outIsViewModel = false;
    outHasPlayerCamera = false;

    if (!entity) return false;

    const char* className = entity->GetClassName();
    const char* clientClassName = entity->GetClientClassName();

    const bool isPlayerPawn = entity->IsPlayerPawn();
    const bool isRagdoll = IsRagdollEntity(className, clientClassName);
    const bool isWeapon = IsWeaponEntity(className, clientClassName);
    const bool isProjectile = IsProjectileEntity(className, clientClassName);
    const int viewModelOwnerEntryIndex = FindViewModelOwnerEntryIndex(frame, entity);
    const auto viewModelWeaponOwner = frame.viewModelWeaponOwnerByHandle.find(entity->GetHandle().ToInt());
    const int viewModelWeaponOwnerEntryIndex = viewModelWeaponOwner == frame.viewModelWeaponOwnerByHandle.end() ? -1 : viewModelWeaponOwner->second;
    const bool isViewModel =
        (-1 != viewModelOwnerEntryIndex)
        || (-1 != viewModelWeaponOwnerEntryIndex)
        || IsViewModelEntity(className, clientClassName);

    const bool actualVisible = GetEntityVisible(entity, isViewModel);
    outVisible = actualVisible;
    outIsViewModel = isViewModel;
    outHasPlayerCamera = isPlayerPawn;

    if ((isPlayerPawn || isRagdoll) && m_RecordPlayers) {
        if (!actualVisible && !m_RecordInvisible) return false;
        return true;
    }
    if (isProjectile && m_RecordProjectiles) {
        if (!actualVisible && !m_RecordInvisible) return false;
        return true;
    }

    if (isViewModel) {
        const int selectedViewModelOwnerEntryIndex =
            -1 != viewModelOwnerEntryIndex ? viewModelOwnerEntryIndex : viewModelWeaponOwnerEntryIndex;
        if (-1 != selectedViewModelOwnerEntryIndex) {
            outVisible = true;
            return -1 == m_RecordViewModels || selectedViewModelOwnerEntryIndex == m_RecordViewModels;
        }

        if (0 == m_RecordViewModels) return false;

        outVisible = false;
        return m_RecordInvisible;
    }

    if (isWeapon && m_RecordWeapons) {
        const int handle = entity->GetHandle().ToInt();
        const bool isActiveWeapon = frame.activeWeaponHandles.end() != frame.activeWeaponHandles.find(handle);
        const bool isObservedActiveWeapon = isActiveWeapon;
        const int selectedViewModelOwnerEntryIndex =
            -1 != viewModelOwnerEntryIndex ? viewModelOwnerEntryIndex : viewModelWeaponOwnerEntryIndex;

        bool ownerIsPlayer = false;
        if (auto ownerHandle = entity->GetOwnerHandle(); ownerHandle.IsValid()) {
            if (auto* owner = GetEntityByHandle(ownerHandle); owner && owner->IsPlayerPawn()) {
                ownerIsPlayer = true;
            }
        }

        bool prevOwnerIsPlayer = false;
        if (auto prevOwnerHandle = entity->GetPrevOwnerHandle(); prevOwnerHandle.IsValid()) {
            if (auto* prevOwner = GetEntityByHandle(prevOwnerHandle); prevOwner && prevOwner->IsPlayerPawn()) {
                prevOwnerIsPlayer = true;
            }
        }

        bool parentIsPlayer = false;
        bool parentIsViewModel = false;
        if (auto parentHandle = entity->GetParentHandle(); parentHandle.IsValid()) {
            if (auto* parent = GetEntityByHandle(parentHandle); parent) {
                parentIsPlayer = parent->IsPlayerPawn();
                parentIsViewModel =
                    -1 != FindViewModelOwnerEntryIndex(frame, parent)
                    || IsViewModelEntity(parent->GetClassName(), parent->GetClientClassName());
            }
        }

        bool hasViewModelAttachment = false;
        if (StringContainsInsensitive(clientClassName, "weapon") || StringContainsInsensitive(className, "weapon")) {
            hasViewModelAttachment = entity->GetViewmodelAttachmentHandle().IsValid();
        }

        const bool onSelectedViewModelPath = -1 != selectedViewModelOwnerEntryIndex;
        const bool ownedByPlayer = ownerIsPlayer || prevOwnerIsPlayer || parentIsPlayer;
        const bool isHeldWeapon = isObservedActiveWeapon || ownedByPlayer || parentIsViewModel || hasViewModelAttachment;

        // Hidden inventory items are not useful AGR output and quickly flood the file.
        if (!actualVisible && ownedByPlayer && !onSelectedViewModelPath && !parentIsViewModel && !isObservedActiveWeapon) {
            return false;
        }

        if (isHeldWeapon) {
            if (onSelectedViewModelPath || parentIsViewModel) {
                outVisible = true;
                return true;
            }

            if (actualVisible) {
                return true;
            }

            outVisible = false;
            return m_RecordInvisible && (isObservedActiveWeapon || hasViewModelAttachment);
        }

        if (!actualVisible && !m_RecordInvisible) return false;
        return true;
    }

    return false;
}

void CAgrRecorder::RecordEntities() {
    FrameContext frameContext;
    CollectFrameContext(frameContext);
    ++m_DebugStats.frames;

    int frameRecordedEntities = 0;
    int frameHidden = 0;
    int frameDeleted = 0;
    int framePlayers = 0;
    int frameRagdolls = 0;
    int frameWeapons = 0;
    int frameProjectiles = 0;
    int frameViewModels = 0;
    int framePlayerCameras = 0;

    for (int handle : m_PendingDeletedHandles) {
        auto it = m_TrackedHandles.find(handle);
        if (it == m_TrackedHandles.end()) continue;

        m_Record->WriteDictionary("deleted");
        m_Record->Write(handle);
        m_TrackedHandles.erase(it);
        ++frameDeleted;
        ++m_DebugStats.deleted;
        if (2 <= m_Debug) {
            advancedfx::Message("AGR debug: deleted handle=%d\n", handle);
        }
    }
    m_PendingDeletedHandles.clear();

    std::set<int> seenHandles;

    const int highestIndex = GetHighestEntityIndex();
    for (int entryIndex = 0; entryIndex <= highestIndex; ++entryIndex) {
        auto* entity = GetEntityByEntryIndex(entryIndex);
        if (!entity) continue;

        const int handle = entity->GetHandle().ToInt();
        if (SOURCESDK_CS2_INVALID_EHANDLE_INDEX == handle) continue;

        bool visible = true;
        bool isViewModel = false;
        bool hasPlayerCamera = false;
        if (!ShouldRecordEntity(frameContext, entryIndex, entity, visible, isViewModel, hasPlayerCamera)) {
            auto it = m_TrackedHandles.find(handle);
            if (it != m_TrackedHandles.end() && it->second) {
                m_Record->MarkHidden(handle);
                it->second = false;
                ++frameHidden;
                ++m_DebugStats.hidden;
                if (2 <= m_Debug) {
                    advancedfx::Message(
                        "AGR debug: hidden handle=%d class=%s clientClass=%s\n",
                        handle,
                        entity->GetClassName() ? entity->GetClassName() : "",
                        entity->GetClientClassName() ? entity->GetClientClassName() : ""
                    );
                }
            }
            continue;
        }

        seenHandles.insert(handle);
        ++frameRecordedEntities;
        ++m_DebugStats.recordedEntities;

        switch (ClassifyRecordedEntity(frameContext, entity, isViewModel)) {
        case RecordCategory::PlayerPawn:
            ++framePlayers;
            ++m_DebugStats.playerPawns;
            break;
        case RecordCategory::Ragdoll:
            ++frameRagdolls;
            ++m_DebugStats.ragdolls;
            break;
        case RecordCategory::Weapon:
            ++frameWeapons;
            ++m_DebugStats.weapons;
            break;
        case RecordCategory::Projectile:
            ++frameProjectiles;
            ++m_DebugStats.projectiles;
            break;
        case RecordCategory::ViewModel:
            ++frameViewModels;
            ++m_DebugStats.viewModels;
            break;
        default:
            break;
        }

        if (hasPlayerCamera && ShouldRecordPlayerCamera(entryIndex)) {
            ++framePlayerCameras;
            ++m_DebugStats.playerCameras;
        }

        if (2 <= m_Debug) {
            const char* modelName = SafeCString(entity->GetModelName());
            advancedfx::Message(
                "AGR debug: record handle=%d visible=%d viewmodel=%d playerCamera=%d class=%s clientClass=%s model=%s\n",
                handle,
                visible ? 1 : 0,
                isViewModel ? 1 : 0,
                hasPlayerCamera && ShouldRecordPlayerCamera(entryIndex) ? 1 : 0,
                entity->GetClassName() ? entity->GetClassName() : "",
                entity->GetClientClassName() ? entity->GetClientClassName() : "",
                modelName ? modelName : ""
            );
        }

        RecordEntity(entryIndex, entity, visible, isViewModel, hasPlayerCamera);
    }

    for (auto it = m_TrackedHandles.begin(); it != m_TrackedHandles.end();) {
        if (seenHandles.end() == seenHandles.find(it->first)) {
            m_Record->WriteDictionary("deleted");
            m_Record->Write(it->first);
            ++frameDeleted;
            ++m_DebugStats.deleted;
            if (2 <= m_Debug) {
                advancedfx::Message("AGR debug: deleted-missed-hook handle=%d\n", it->first);
            }
            it = m_TrackedHandles.erase(it);
        } else {
            ++it;
        }
    }

    if (1 <= m_Debug) {
        advancedfx::Message(
            "AGR debug frame %d: recorded=%d players=%d ragdolls=%d weapons=%d projectiles=%d viewmodels=%d playerCameras=%d hidden=%d deleted=%d\n",
            m_DebugStats.frames,
            frameRecordedEntities,
            framePlayers,
            frameRagdolls,
            frameWeapons,
            frameProjectiles,
            frameViewModels,
            framePlayerCameras,
            frameHidden,
            frameDeleted
        );
    }
}

void CAgrRecorder::RecordEntity(int entryIndex, CEntityInstance* entity, bool visible, bool isViewModel, bool hasPlayerCamera) {
    const int handle = entity->GetHandle().ToInt();
    const char* className = entity->GetClassName();
    const char* clientClassName = entity->GetClientClassName();

    m_Record->WriteDictionary("entity_state");
    m_Record->Write(handle);

    m_Record->WriteDictionary("baseentity");
    {
        const char* modelName = SafeCString(entity->GetModelName());
        if (!modelName || 0 == modelName[0]) modelName = className ? className : "[NULL]";

        m_Record->WriteDictionary(modelName);
        m_Record->Write(visible);

        SOURCESDK::matrix3x4_t parentTransform;
        entity->GetAbsTransform(parentTransform);
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 4; ++x) {
                m_Record->Write(parentTransform[y][x]);
            }
        }
    }

    m_TrackedHandles[handle] = visible;

    m_Record->WriteDictionary("baseanimating");
    {
        std::vector<SOURCESDK::matrix3x4_t> boneTransforms;
        bool hasBones = false;
        if (IsRagdollEntity(className, clientClassName)) {
            hasBones = entity->GetRagdollBones(boneTransforms);
        }
        if (!hasBones) {
            hasBones = entity->GetBindPoseBones(boneTransforms);
        }
        if (!hasBones && isViewModel) {
            auto parentHandle = entity->GetParentHandle();
            for (int depth = 0; depth < 6 && parentHandle.IsValid(); ++depth) {
                auto* parentEntity = GetEntityByHandle(parentHandle);
                if (!parentEntity || parentEntity == entity) break;
                if (parentEntity->GetBindPoseBones(boneTransforms)) {
                    hasBones = true;
                    break;
                }
                const auto nextParentHandle = parentEntity->GetParentHandle();
                if (!nextParentHandle.IsValid() || nextParentHandle.ToInt() == parentHandle.ToInt()) break;
                parentHandle = nextParentHandle;
            }
        }
        if (!hasBones) {
            const bool useFallbackBone =
                entity->IsPlayerPawn()
                || IsRagdollEntity(className, clientClassName)
                || isViewModel;

            if (useFallbackBone) {
                SOURCESDK::matrix3x4_t identityBone;
                SetIdentityMatrix(identityBone);
                boneTransforms.push_back(identityBone);
                hasBones = true;
            }
        }
        m_Record->Write(hasBones);
        if (hasBones) {
            m_Record->Write(static_cast<int>(boneTransforms.size()));
            for (const auto& boneTransform : boneTransforms) {
                for (int y = 0; y < 3; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        m_Record->Write(boneTransform[y][x]);
                    }
                }
            }
        }
    }

    if (hasPlayerCamera && ShouldRecordPlayerCamera(entryIndex)) {
        float origin[3];
        float angles[3];
        entity->GetRenderEyeOrigin(origin);
        entity->GetRenderEyeAngles(angles);
        float playerFov = static_cast<float>(entity->GetPlayerFov());
        if (!(0.0f < playerFov)) playerFov = GetLastCameraFov();

        m_Record->WriteDictionary("camera");
        m_Record->Write(false);
        m_Record->Write(origin[0]);
        m_Record->Write(origin[1]);
        m_Record->Write(origin[2]);
        m_Record->Write(angles[0]);
        m_Record->Write(angles[1]);
        m_Record->Write(angles[2]);
        m_Record->Write(ScaleFov(playerFov));
    }

    m_Record->WriteDictionary("/");
    m_Record->Write(isViewModel);
}

void CAgrRecorder::WriteCamera() {
    if (!m_RecordCamera) return;

    float origin[3];
    float angles[3];
    float fov = 90.0f;
    GetLastCameraData(origin, angles, fov);

    m_Record->WriteDictionary("afxCam");
    m_Record->Write(origin[0]);
    m_Record->Write(origin[1]);
    m_Record->Write(origin[2]);
    m_Record->Write(angles[0]);
    m_Record->Write(angles[1]);
    m_Record->Write(angles[2]);
    m_Record->Write(ScaleFov(fov));
}

void CAgrRecorder::PrintSettings(const char* prefix) const {
    advancedfx::Message(
        "%s enabled 0|1 - Enable or disable the AGR feature. On CS2, start will auto-enable recording if needed.\n"
        "Current value: %d\n",
        prefix,
        m_Enabled ? 1 : 0
    );
    advancedfx::Message(
        "%s recordCamera 0|1\n"
        "Current value: %d\n",
        prefix,
        m_RecordCamera ? 1 : 0
    );
    advancedfx::Message(
        "%s recordPlayers 0|1\n"
        "Current value: %d\n",
        prefix,
        m_RecordPlayers ? 1 : 0
    );
    advancedfx::Message(
        "%s recordPlayerCameras 0|<iPlayerEntIndex>|-1\n"
        "Current value: %d\n",
        prefix,
        m_RecordPlayerCameras
    );
    advancedfx::Message(
        "%s recordWeapons 0|1\n"
        "Current value: %d\n",
        prefix,
        m_RecordWeapons ? 1 : 0
    );
    advancedfx::Message(
        "%s recordProjectiles 0|1\n"
        "Current value: %d\n",
        prefix,
        m_RecordProjectiles ? 1 : 0
    );
    advancedfx::Message(
        "%s recordViewModel 0|1\n"
        "Current value: %d\n",
        prefix,
        m_RecordViewModels ? 1 : 0
    );
    advancedfx::Message(
        "%s recordViewModels 0|<iPlayerEntIndex>|-1\n"
        "Current value: %d\n",
        prefix,
        m_RecordViewModels
    );
    advancedfx::Message(
        "%s recordInvisible 0|1\n"
        "Current value: %d\n",
        prefix,
        m_RecordInvisible ? 1 : 0
    );
    advancedfx::Message(
        "%s debug 0|1|2\n"
        "Current value: %d\n",
        prefix,
        m_Debug
    );
    advancedfx::Message(
        "%s start <sFilePath> - Start recording to file <sFilePath>. Writes AGR version %d for Source 1 importer compatibility.\n"
        "%s stop - Stop recording.\n",
        prefix,
        GetAgrVersion(),
        prefix
    );
}

void CAgrRecorder::Console(advancedfx::ICommandArgs* args) {
    const int argc = args->ArgC();
    const char* prefix = args->ArgV(0);

    if (2 <= argc) {
        const char* cmd1 = args->ArgV(1);

        if (0 == _stricmp("enabled", cmd1)) {
            if (3 <= argc) {
                m_Enabled = 0 != atoi(args->ArgV(2));
                return;
            }

            advancedfx::Message("%s enabled 0|1 - On CS2, start will auto-enable recording if needed.\nCurrent value: %d\n", prefix, m_Enabled ? 1 : 0);
            return;
        }
        if (0 == _stricmp("start", cmd1)) {
            if (3 <= argc) {
                std::wstring wideFilePath;
                if (UTF8StringToWideString(args->ArgV(2), wideFilePath) && StartRecording(wideFilePath.c_str())) {
                    advancedfx::Message("Started AGR recording.\n");
                    return;
                }

                advancedfx::Warning("Error.\n");
                return;
            }
        }
        if (0 == _stricmp("stop", cmd1)) {
            StopRecording();
            advancedfx::Message("Stopped AGR recording.\n");
            return;
        }
        if (0 == _stricmp("recordCamera", cmd1) && 3 <= argc) {
            m_RecordCamera = 0 != atoi(args->ArgV(2));
            return;
        }
        if (0 == _stricmp("recordPlayers", cmd1) && 3 <= argc) {
            m_RecordPlayers = 0 != atoi(args->ArgV(2));
            return;
        }
        if (0 == _stricmp("recordPlayerCameras", cmd1) && 3 <= argc) {
            m_RecordPlayerCameras = atoi(args->ArgV(2));
            return;
        }
        if (0 == _stricmp("recordWeapons", cmd1) && 3 <= argc) {
            m_RecordWeapons = 0 != atoi(args->ArgV(2));
            return;
        }
        if (0 == _stricmp("recordProjectiles", cmd1) && 3 <= argc) {
            m_RecordProjectiles = 0 != atoi(args->ArgV(2));
            return;
        }
        if (0 == _stricmp("recordViewModel", cmd1) && 3 <= argc) {
            m_RecordViewModels = 0 != atoi(args->ArgV(2)) ? -1 : 0;
            return;
        }
        if (0 == _stricmp("recordViewModels", cmd1) && 3 <= argc) {
            m_RecordViewModels = atoi(args->ArgV(2));
            return;
        }
        if (0 == _stricmp("recordInvisible", cmd1) && 3 <= argc) {
            m_RecordInvisible = 0 != atoi(args->ArgV(2));
            return;
        }
        if (0 == _stricmp("debug", cmd1) && 3 <= argc) {
            m_Debug = atoi(args->ArgV(2));
            return;
        }
    }

    PrintSettings(prefix);
}

CON_COMMAND(mirv_agr, "AFX GameRecord")
{
    CAgrRecorder::Get().Console(args);
}
