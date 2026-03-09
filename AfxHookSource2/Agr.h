#pragma once

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace advancedfx {
class ICommandArgs;
class CAfxGameRecord;
}

class CAgrRecorder {
public:
    enum class RecordCategory {
        None,
        PlayerPawn,
        Ragdoll,
        Weapon,
        Projectile,
        ViewModel,
    };

    static CAgrRecorder& Get();

    void Console(advancedfx::ICommandArgs* args);

    bool GetRecording() const;
    bool StartRecording(const wchar_t* fileName);
    void StopRecording();
    void OnEntityDeleted(int handle);

    void OnBeginMainRenderPass();
    void OnEndMainRenderPass();

private:
    struct DebugStats {
        int frames = 0;
        int recordedEntities = 0;
        int playerPawns = 0;
        int ragdolls = 0;
        int weapons = 0;
        int projectiles = 0;
        int viewModels = 0;
        int playerCameras = 0;
        int hidden = 0;
        int deleted = 0;

        void Reset() {
            frames = 0;
            recordedEntities = 0;
            playerPawns = 0;
            ragdolls = 0;
            weapons = 0;
            projectiles = 0;
            viewModels = 0;
            playerCameras = 0;
            hidden = 0;
            deleted = 0;
        }
    };

    struct FrameContext {
        std::unordered_map<int, int> viewModelOwnerByHandle;
        std::unordered_map<int, int> viewModelWeaponOwnerByHandle;
        std::unordered_set<int> activeWeaponHandles;
        std::unordered_set<int> observedPlayerEntryIndices;
        std::unordered_set<int> ragdollHandles;
    };

    CAgrRecorder() = default;

    void CollectFrameContext(FrameContext& outContext) const;
    int FindViewModelOwnerEntryIndex(const FrameContext& frame, class CEntityInstance* entity) const;
    RecordCategory ClassifyRecordedEntity(const FrameContext& frame, class CEntityInstance* entity, bool isViewModel) const;
    bool GetEntityVisible(class CEntityInstance* entity, bool isViewModel) const;
    bool ShouldRecordEntity(const FrameContext& frame, int entryIndex, class CEntityInstance* entity, bool& outVisible, bool& outIsViewModel, bool& outHasPlayerCamera) const;
    bool IsWeaponEntity(const char* className, const char* clientClassName) const;
    bool IsProjectileEntity(const char* className, const char* clientClassName) const;
    bool IsObserverPawnEntity(const char* className, const char* clientClassName) const;
    bool IsPlayerEntity(class CEntityInstance* entity) const;
    bool IsDeadPlayerRagdollCandidate(class CEntityInstance* entity) const;
    bool IsRagdollEntity(const char* className, const char* clientClassName) const;
    bool HasRagdollState(const FrameContext& frame, class CEntityInstance* entity) const;
    bool IsViewModelEntity(const char* className, const char* clientClassName) const;
    bool ShouldRecordPlayerCamera(int entryIndex) const;
    int GetAgrVersion() const;
    float ScaleFov(float fov) const;

    void PrintSettings(const char* prefix) const;
    void RecordEntities();
    void RecordEntity(int entryIndex, class CEntityInstance* entity, bool visible, bool isViewModel, bool hasPlayerCamera);
    void WriteCamera();

    bool m_Enabled = false;
    bool m_FrameActive = false;

    int m_Debug = 0;
    bool m_RecordCamera = true;
    bool m_RecordPlayers = true;
    int m_RecordPlayerCameras = -1;
    bool m_RecordWeapons = true;
    bool m_RecordProjectiles = true;
    int m_RecordViewModels = 0;
    bool m_RecordInvisible = false;

    advancedfx::CAfxGameRecord* m_Record = nullptr;
    std::unordered_map<int, bool> m_TrackedHandles;
    std::unordered_set<int> m_PendingDeletedHandles;
    DebugStats m_DebugStats;
};
