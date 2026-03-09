#include "stdafx.h"

#include "Agr.h"
#include "ClientEntitySystem.h"
#include "DeathMsg.h"
#include "WrpConsole.h"
#include "Globals.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#include "../shared/AfxConsole.h"
//#include "../shared/binutils.h"
#include "../shared/FFITools.h"
#include "../shared/StringTools.h"

#include "AfxHookSource2Rs.h"
#include "SchemaSystem.h"

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include <map>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

template <typename T>
struct Source2UtlVectorView {
	int m_Size = 0;
	int m_Padding = 0;
	T* m_pMemory = nullptr;
	int m_nAllocationCount = 0;
	int m_nGrowSize = 0;
};

struct Source2QuaternionStorage {
	float x;
	float y;
	float z;
	float w;
};

struct Source2TransformStorage {
	SOURCESDK::Vector position;
	Source2QuaternionStorage orientation;
};

constexpr ptrdiff_t kPermModelDataModelSkeleton = 0x188;
constexpr ptrdiff_t kModelSkeletonParents = 0x18;
constexpr ptrdiff_t kModelSkeletonBonePosParent = 0x60;
constexpr ptrdiff_t kModelSkeletonBoneRotParent = 0x78;
constexpr int kMaxBindPoseBones = 512;

std::unordered_map<const void*, std::vector<SOURCESDK::matrix3x4_t>> g_BindPoseBoneCache;
std::unordered_map<const void*, const void*> g_ModelHandlePermModelDataCache;

bool SafeReadMemory(const void* src, void* dst, size_t size) {
	if (!src || !dst || !size) return false;

#ifdef _MSC_VER
	__try {
		memcpy(dst, src, size);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
#else
	memcpy(dst, src, size);
	return true;
#endif
}

template <typename T>
bool SafeReadObject(const void* src, T& outValue) {
	return SafeReadMemory(src, &outValue, sizeof(T));
}

const char* SafeReadCString(const char* value) {
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

template <typename T>
bool SafeReadVectorView(const void* src, Source2UtlVectorView<T>& outValue) {
	return SafeReadMemory(src, &outValue, sizeof(outValue));
}

template <typename T>
bool IsPlausibleVector(const Source2UtlVectorView<T>& vectorView, int expectedCount = -1) {
	if (vectorView.m_Size <= 0 || vectorView.m_Size > kMaxBindPoseBones) return false;
	if (!vectorView.m_pMemory) return false;
	if (vectorView.m_nAllocationCount < vectorView.m_Size) return false;
	if (-1 != expectedCount && vectorView.m_Size != expectedCount) return false;
	return true;
}

bool InvertRigidMatrix(const SOURCESDK::matrix3x4_t& matrix, SOURCESDK::matrix3x4_t& outMatrix) {
	outMatrix[0][0] = matrix[0][0];
	outMatrix[0][1] = matrix[1][0];
	outMatrix[0][2] = matrix[2][0];
	outMatrix[1][0] = matrix[0][1];
	outMatrix[1][1] = matrix[1][1];
	outMatrix[1][2] = matrix[2][1];
	outMatrix[2][0] = matrix[0][2];
	outMatrix[2][1] = matrix[1][2];
	outMatrix[2][2] = matrix[2][2];

	outMatrix[0][3] = -(outMatrix[0][0] * matrix[0][3] + outMatrix[0][1] * matrix[1][3] + outMatrix[0][2] * matrix[2][3]);
	outMatrix[1][3] = -(outMatrix[1][0] * matrix[0][3] + outMatrix[1][1] * matrix[1][3] + outMatrix[1][2] * matrix[2][3]);
	outMatrix[2][3] = -(outMatrix[2][0] * matrix[0][3] + outMatrix[2][1] * matrix[1][3] + outMatrix[2][2] * matrix[2][3]);

	return true;
}

bool IsFiniteMatrix(const SOURCESDK::matrix3x4_t& matrix) {
	for (int y = 0; y < 3; ++y) {
		for (int x = 0; x < 4; ++x) {
			if (!std::isfinite(matrix[y][x])) return false;
		}
	}

	const float maxTranslation = 1000000.0f;
	return
		std::abs(matrix[0][3]) < maxTranslation
		&& std::abs(matrix[1][3]) < maxTranslation
		&& std::abs(matrix[2][3]) < maxTranslation;
}

bool BuildParentSpaceBonesFromWorld(
	const CEntityInstance* entity,
	const std::vector<int16_t>& skeletonParents,
	const std::vector<SOURCESDK::matrix3x4_t>& worldBoneTransforms,
	std::vector<SOURCESDK::matrix3x4_t>& outBones) {
	if (!entity || skeletonParents.size() != worldBoneTransforms.size() || worldBoneTransforms.empty()) return false;

	SOURCESDK::matrix3x4_t rootTransform;
	const_cast<CEntityInstance*>(entity)->GetAbsTransform(rootTransform);
	SOURCESDK::matrix3x4_t rootInverse;
	if (!InvertRigidMatrix(rootTransform, rootInverse)) return false;

	outBones.resize(worldBoneTransforms.size());
	for (size_t i = 0; i < outBones.size(); ++i) {
		const int16_t parent = skeletonParents[i];
		SOURCESDK::matrix3x4_t parentInverse;
		if (0 <= parent && static_cast<size_t>(parent) < worldBoneTransforms.size()) {
			if (!InvertRigidMatrix(worldBoneTransforms[parent], parentInverse)) return false;
		}
		else {
			parentInverse = rootInverse;
		}
		SOURCESDK::R_ConcatTransforms(parentInverse, worldBoneTransforms[i], outBones[i]);
	}

	return !outBones.empty();
}

void QuaternionMatrix(const Source2QuaternionStorage& q, const SOURCESDK::Vector& position, SOURCESDK::matrix3x4_t& outMatrix) {
	const float xx = q.x * q.x;
	const float yy = q.y * q.y;
	const float zz = q.z * q.z;
	const float xy = q.x * q.y;
	const float xz = q.x * q.z;
	const float yz = q.y * q.z;
	const float wx = q.w * q.x;
	const float wy = q.w * q.y;
	const float wz = q.w * q.z;

	outMatrix[0][0] = 1.0f - 2.0f * (yy + zz);
	outMatrix[0][1] = 2.0f * (xy - wz);
	outMatrix[0][2] = 2.0f * (xz + wy);
	outMatrix[0][3] = position.x;

	outMatrix[1][0] = 2.0f * (xy + wz);
	outMatrix[1][1] = 1.0f - 2.0f * (xx + zz);
	outMatrix[1][2] = 2.0f * (yz - wx);
	outMatrix[1][3] = position.y;

	outMatrix[2][0] = 2.0f * (xz - wy);
	outMatrix[2][1] = 2.0f * (yz + wx);
	outMatrix[2][2] = 1.0f - 2.0f * (xx + yy);
	outMatrix[2][3] = position.z;
}

bool TryReadModelSkeletonParents(const void* permModelData, std::vector<int16_t>& outParents) {
	outParents.clear();
	if (!permModelData) return false;

	const auto* base = reinterpret_cast<const unsigned char*>(permModelData) + kPermModelDataModelSkeleton;

	Source2UtlVectorView<int16_t> parents;
	if (!SafeReadVectorView(base + kModelSkeletonParents, parents)) return false;
	if (!IsPlausibleVector(parents)) return false;
	outParents.resize(static_cast<size_t>(parents.m_Size));
	if (!SafeReadMemory(parents.m_pMemory, outParents.data(), sizeof(int16_t) * outParents.size())) return false;
	return !outParents.empty();
}

bool TryBuildBindPoseBones(const void* permModelData, std::vector<SOURCESDK::matrix3x4_t>& outBones) {
	outBones.clear();
	if (!permModelData) return false;

	const auto* base = reinterpret_cast<const unsigned char*>(permModelData) + kPermModelDataModelSkeleton;

	Source2UtlVectorView<int16_t> parents;
	Source2UtlVectorView<SOURCESDK::Vector> positions;
	Source2UtlVectorView<Source2QuaternionStorage> rotations;

	if (!SafeReadVectorView(base + kModelSkeletonParents, parents)) return false;
	if (!IsPlausibleVector(parents)) return false;
	if (!SafeReadVectorView(base + kModelSkeletonBonePosParent, positions)) return false;
	if (!IsPlausibleVector(positions, parents.m_Size)) return false;
	if (!SafeReadVectorView(base + kModelSkeletonBoneRotParent, rotations)) return false;
	if (!IsPlausibleVector(rotations, parents.m_Size)) return false;

	std::vector<SOURCESDK::Vector> positionData(static_cast<size_t>(parents.m_Size));
	std::vector<Source2QuaternionStorage> rotationData(static_cast<size_t>(parents.m_Size));
	if (!SafeReadMemory(positions.m_pMemory, positionData.data(), sizeof(SOURCESDK::Vector) * positionData.size())) return false;
	if (!SafeReadMemory(rotations.m_pMemory, rotationData.data(), sizeof(Source2QuaternionStorage) * rotationData.size())) return false;

	outBones.resize(positionData.size());
	for (size_t i = 0; i < outBones.size(); ++i) {
		QuaternionMatrix(rotationData[i], positionData[i], outBones[i]);
	}

	return !outBones.empty();
}

bool LooksLikePermModelData(const void* candidate, const char* expectedModelName) {
	if (!candidate) return false;

	const char* resourceName = nullptr;
	if (!SafeReadObject(candidate, resourceName)) return false;
	resourceName = SafeReadCString(resourceName);

	if (expectedModelName && expectedModelName[0] && resourceName && 0 != _stricmp(resourceName, expectedModelName)) {
		return false;
	}

	std::vector<SOURCESDK::matrix3x4_t> bones;
	if (!TryBuildBindPoseBones(candidate, bones)) return false;

	return true;
}

void CollectPermModelDataCandidates(void* rawHandle, std::vector<const void*>& outCandidates) {
	auto pushCandidate = [&outCandidates](const void* candidate) {
		if (!candidate) return;
		if (std::find(outCandidates.begin(), outCandidates.end(), candidate) != outCandidates.end()) return;
		outCandidates.push_back(candidate);
	};

	std::vector<const void*> frontier;
	std::unordered_set<const void*> visited;

	auto pushFrontier = [&pushCandidate, &frontier, &visited](const void* candidate) {
		if (!candidate) return;
		if (!visited.emplace(candidate).second) return;
		pushCandidate(candidate);
		frontier.push_back(candidate);
	};

	pushFrontier(rawHandle);

	void* firstPointer = nullptr;
	if (SafeReadObject(rawHandle, firstPointer)) {
		pushFrontier(firstPointer);
	}

	for (int depth = 0; depth < 3 && !frontier.empty(); ++depth) {
		std::vector<const void*> nextFrontier;

		for (const void* scanBase : frontier) {
			const auto* bytes = reinterpret_cast<const unsigned char*>(scanBase);
			const size_t scanLimit = 0x100;

			for (size_t offset = 0; offset <= scanLimit; offset += sizeof(void*)) {
				void* nestedPointer = nullptr;
				if (!SafeReadObject(bytes + offset, nestedPointer) || !nestedPointer) continue;
				if (!visited.emplace(nestedPointer).second) continue;
				pushCandidate(nestedPointer);
				nextFrontier.push_back(nestedPointer);
			}
		}

		frontier = std::move(nextFrontier);
	}
}

const void* ResolvePermModelData(void* rawHandle, const char* expectedModelName) {
	if (!rawHandle) return nullptr;

	auto cacheIt = g_ModelHandlePermModelDataCache.find(rawHandle);
	if (cacheIt != g_ModelHandlePermModelDataCache.end()) {
		if (!expectedModelName || !expectedModelName[0] || LooksLikePermModelData(cacheIt->second, expectedModelName)) {
			return cacheIt->second;
		}
	}

	std::vector<const void*> candidates;
	CollectPermModelDataCandidates(rawHandle, candidates);

	for (const void* candidate : candidates) {
		if (LooksLikePermModelData(candidate, expectedModelName)) {
			g_ModelHandlePermModelDataCache[rawHandle] = candidate;
			return candidate;
		}
	}

	if (expectedModelName && expectedModelName[0]) {
		for (const void* candidate : candidates) {
			if (LooksLikePermModelData(candidate, nullptr)) {
				g_ModelHandlePermModelDataCache[rawHandle] = candidate;
				return candidate;
			}
		}
	}

	return nullptr;
}

const void* ResolveEntityPermModelData(const CEntityInstance* entity) {
	if (!entity || 0 == g_clientDllOffsets.CModelState.m_hModel) return nullptr;

	auto pBodyComponent = *(u_char**)((u_char*)entity + g_clientDllOffsets.C_BaseEntity.m_CBodyComponent);
	if (!pBodyComponent) return nullptr;

	auto pSkeletonInstance = pBodyComponent + g_clientDllOffsets.CBodyComponentSkeletonInstance.m_skeletonInstance;
	auto pModelState = pSkeletonInstance + g_clientDllOffsets.CSkeletonInstance.m_modelState;

	void* rawModelHandle = nullptr;
	if (!SafeReadObject(pModelState + g_clientDllOffsets.CModelState.m_hModel, rawModelHandle) || !rawModelHandle) {
		return nullptr;
	}

	return ResolvePermModelData(rawModelHandle, entity->GetModelName());
}

unsigned char* GetEntitySkeletonInstance(const CEntityInstance* entity) {
	if (!entity) return nullptr;

	auto pBodyComponent = *(u_char**)((u_char*)entity + g_clientDllOffsets.C_BaseEntity.m_CBodyComponent);
	if (!pBodyComponent) return nullptr;

	return pBodyComponent + g_clientDllOffsets.CBodyComponentSkeletonInstance.m_skeletonInstance;
}

bool ReadRagdollFlag(const CEntityInstance* entity, ptrdiff_t offset) {
	if (!entity || 0 == offset) return false;

	unsigned char value = 0;
	if (!SafeReadObject((const unsigned char*)entity + offset, value)) return false;
	return 0 != value;
}

} // namespace

void ** g_pEntityList = nullptr;
GetHighestEntityIndex_t  g_GetHighestEntityIndex = nullptr;
GetEntityFromIndex_t g_GetEntityFromIndex = nullptr;

/*
cl_track_render_eye_angles 1
cl_ent_absbox 192
cl_ent_viewoffset 192
*/

// CEntityInstance: Root class for all entities
// Retrieved from script function.
const char * CEntityInstance::GetName() {
	const char * pszName = (const char*)*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x18);
	if(pszName) return pszName;
	return "";
}

// Retrieved from script function.
// can return nullptr!
const char * CEntityInstance::GetDebugName() {
	const char * pszName = (const char*)*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x18);
	if(pszName) return pszName;
	return **(const char***)(*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x8)+0x78);
}

// Retrieved from script function.
const char * CEntityInstance::GetClassName() {
	const char * pszName = (const char*)*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x20);
	if(pszName) return pszName;
	return "";
}

extern HMODULE g_H_ClientDll;

// Retrieved from script function.
const char * CEntityInstance::GetClientClassName() {
    // GetClientClass function.
    // find it by searching for 4th full-ptr ref to "C_PlantedC4" subtract sizeof(void*) (0x8) and search function that references this struct.
    // you need to search for raw bytes, GiHidra doesn't seem to find the reference.
    void * pClientClass = ((void * (__fastcall *)(void *)) (*(void***)this)[42]) (this);

    if(pClientClass) {
        return *(const char**)((unsigned char*)pClientClass + 0x10);
    }
    return nullptr;
}

// Retrieved from script function.
// GetEntityHandle ...

bool CEntityInstance::IsPlayerPawn() {
	// See cl_ent_text drawing function.
	return ((bool (__fastcall *)(void *)) (*(void***)this)[151]) (this);
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetPlayerPawnHandle() {
	// See cl_ent_text drawing function.
	if(!IsPlayerController())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int *)((unsigned char *)this + g_clientDllOffsets.CBasePlayerController.m_hPawn));
}

bool CEntityInstance::IsPlayerController() {
	// See cl_ent_text drawing function. Near "Pawn: (%d) Name: %s".
	return ((bool (__fastcall *)(void *)) (*(void***)this)[152]) (this);    
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetPlayerControllerHandle() {
	// See cl_ent_text drawing function.
	if(!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int *)((unsigned char *)this + g_clientDllOffsets.C_BasePlayerPawn.m_hController));
}

unsigned int CEntityInstance::GetHealth() {
	// See cl_ent_text drawing function. Near "Health: %d\n".
	return *(unsigned int *)((unsigned char *)this + g_clientDllOffsets.C_BaseEntity.m_iHealth);
}

int CEntityInstance::GetTeam() {
    return *(int*)((u_char*)(this) + g_clientDllOffsets.C_BaseEntity.m_iTeamNum);
}

unsigned int CEntityInstance::GetEffects() const {
	if (0 == g_clientDllOffsets.C_BaseEntity.m_fEffects) return 0;
	return *(unsigned int*)((unsigned char*)this + g_clientDllOffsets.C_BaseEntity.m_fEffects);
}

bool CEntityInstance::IsDormant() const {
	auto pSceneNode = *(u_char**)((u_char*)this + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	if (!pSceneNode || 0 == g_clientDllOffsets.CGameSceneNode.m_bDormant) return false;
	return 0 != *(unsigned char*)(pSceneNode + g_clientDllOffsets.CGameSceneNode.m_bDormant);
}

uint8_t CEntityInstance::GetRenderAlpha() const {
	if (0 == g_clientDllOffsets.C_BaseModelEntity.m_clrRender) return 255;
	auto color = reinterpret_cast<SOURCESDK::CS2::Color*>((unsigned char*)this + g_clientDllOffsets.C_BaseModelEntity.m_clrRender);
	return (uint8_t)color->a();
}

uint8_t CEntityInstance::GetRenderMode() const {
	if (0 == g_clientDllOffsets.C_BaseModelEntity.m_nRenderMode) return 0;
	return *(uint8_t*)((unsigned char*)this + g_clientDllOffsets.C_BaseModelEntity.m_nRenderMode);
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetOwnerHandle() {
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)this + g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity));
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetPrevOwnerHandle() {
	if (0 == g_clientDllOffsets.C_BasePlayerWeapon.m_hPrevOwner) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerWeapon.m_hPrevOwner));
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetParentHandle() {
	auto pSceneNode = *(u_char**)((u_char*)this + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	if (!pSceneNode) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();

	auto pParentNode = *(u_char**)(pSceneNode + g_clientDllOffsets.CGameSceneNode.m_pParent);
	if (!pParentNode) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();

	auto pParentOwner = *(u_char**)(pParentNode + g_clientDllOffsets.CGameSceneNode.m_pOwner);
	if (!pParentOwner) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();

	return reinterpret_cast<CEntityInstance*>(pParentOwner)->GetHandle();
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetViewmodelAttachmentHandle() {
	if (0 == g_clientDllOffsets.C_EconEntity.m_hViewmodelAttachment) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)this + g_clientDllOffsets.C_EconEntity.m_hViewmodelAttachment));
}


/**
 * @remarks FLOAT_MAX if invalid
 */
void CEntityInstance::GetOrigin(float & x, float & y, float & z) {
    auto ptr = *(u_char**)((u_char*)this + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	if (!ptr) {
		x = FLT_MAX;
		y = FLT_MAX;
		z = FLT_MAX;
		return;
	}
	// See cl_ent_text drawing function. Near "Position: %0.3f, %0.3f, %0.3f\n" or cl_ent_viewoffset related function.
	auto vector = (float*)(ptr + g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin);
	x =  vector[0];
	y =  vector[1];
	z =  vector[2];
}

void CEntityInstance::GetAbsAngles(float& pitch, float& yaw, float& roll) {
    auto ptr = *(u_char**)((u_char*)this + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	if (!ptr) {
		pitch = 0.0f;
		yaw = 0.0f;
		roll = 0.0f;
		return;
	}
	auto angles = (float*)(ptr + g_clientDllOffsets.CGameSceneNode.m_angAbsRotation);
	pitch = angles[0];
	yaw = angles[1];
	roll = angles[2];
}

void CEntityInstance::GetAbsTransform(SOURCESDK::matrix3x4_t& outMatrix) {
    float x, y, z;
    float pitch, yaw, roll;
    GetOrigin(x, y, z);
    GetAbsAngles(pitch, yaw, roll);

    SOURCESDK::QAngle angles = { pitch, yaw, roll };
    SOURCESDK::Vector origin = { x, y, z };
    SOURCESDK::AngleMatrix(angles, origin, outMatrix);
}

void CEntityInstance::GetRenderEyeOrigin(float outOrigin[3]) {
	// GetRenderEyeAngles vtable offset minus 2
	((void (__fastcall *)(void *,float outOrigin[3])) (*(void***)this)[166]) (this,outOrigin);
}

void CEntityInstance::GetRenderEyeAngles(float outAngles[3]) {
	// See cl_track_render_eye_angles. Near "Render eye angles: %.7f, %.7f, %.7f\n".
	((void (__fastcall *)(void *,float outAngles[3])) (*(void***)this)[167]) (this,outAngles);
}

unsigned int CEntityInstance::GetPlayerFov() {
	if (!IsPlayerPawn() || 0 == g_clientDllOffsets.CCSPlayerBase_CameraServices.m_iFOV) return 0;
	void* pCameraServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pCameraServices);
	if (!pCameraServices) return 0;
	return *(unsigned int*)((unsigned char*)pCameraServices + g_clientDllOffsets.CCSPlayerBase_CameraServices.m_iFOV);
}

float CEntityInstance::GetViewmodelFov() {
	if (!IsPlayerPawn() || 0 == g_clientDllOffsets.C_CSPlayerPawn.m_flViewmodelFOV) return 0.0f;
	return *(float*)((unsigned char*)this + g_clientDllOffsets.C_CSPlayerPawn.m_flViewmodelFOV);
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetViewEntityHandle() {
	if (!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
    void * pCameraServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pCameraServices);
    if(nullptr == pCameraServices) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)pCameraServices + g_clientDllOffsets.CPlayer_CameraServices.m_hViewEntity));
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetActiveWeaponHandle() {
	if (!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
    void * pWeaponServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pWeaponServices);
    if(nullptr == pWeaponServices) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)pWeaponServices + g_clientDllOffsets.CPlayer_WeaponServices.m_hActiveWeapon));
}

const char * CEntityInstance::GetPlayerName(){
    if (!IsPlayerController()) return nullptr;
    return *(const char **)((u_char*)(this) + g_clientDllOffsets.CBasePlayerController.m_iszPlayerName);
}

uint64_t CEntityInstance::GetSteamId(){
    if (!IsPlayerController())  return 0;
    return *(uint64_t*)((u_char*)(this) + g_clientDllOffsets.CBasePlayerController.m_steamID);
}

const char * CEntityInstance::GetSanitizedPlayerName() {
   if (!IsPlayerController()) return nullptr;
    return *(const char **)((u_char*)(this) + g_clientDllOffsets.CCSPlayerController.m_sSanitizedPlayerName);

}

uint8_t CEntityInstance::GetObserverMode() {
	if (!IsPlayerPawn()) return 0;
    void * pObserverServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
    if(nullptr == pObserverServices) return 0;
	return *(uint8_t*)((unsigned char*)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_iObserverMode);    
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetObserverTarget() {
	if (!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
    void * pObserverServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
    if(nullptr == pObserverServices) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_hObserverTarget));    
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetHandle() {
	if (auto pEntityIdentity = *(u_char**)((u_char*)this + g_clientDllOffsets.CEntityInstance.m_pEntity)) {
		return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(uint32_t*)(pEntityIdentity + 0x10));
	}

	return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
}

const char* CEntityInstance::GetModelName() const {
    auto pSkeletonInstance = GetEntitySkeletonInstance(this);
    if (!pSkeletonInstance) return nullptr;
    auto pModelState = pSkeletonInstance + g_clientDllOffsets.CSkeletonInstance.m_modelState;
    return *(const char**)(pModelState + g_clientDllOffsets.CModelState.m_ModelName);
}

bool CEntityInstance::GetCurrentBones(std::vector<SOURCESDK::matrix3x4_t>& outBones) const {
	outBones.clear();
	return false;
}

bool CEntityInstance::GetBindPoseBones(std::vector<SOURCESDK::matrix3x4_t>& outBones) const {
	outBones.clear();

	const void* permModelData = ResolveEntityPermModelData(this);
	if (!permModelData) return false;

	auto it = g_BindPoseBoneCache.find(permModelData);
	if (it != g_BindPoseBoneCache.end()) {
		outBones = it->second;
		return !outBones.empty();
	}

	std::vector<SOURCESDK::matrix3x4_t> bones;
	if (!TryBuildBindPoseBones(permModelData, bones)) return false;

	g_BindPoseBoneCache.emplace(permModelData, bones);
	outBones = std::move(bones);
	return !outBones.empty();
}

bool CEntityInstance::GetRagdollBones(std::vector<SOURCESDK::matrix3x4_t>& outBones) const {
	outBones.clear();

	const void* permModelData = ResolveEntityPermModelData(this);
	if (!permModelData) return false;

	std::vector<int16_t> skeletonParents;
	if (!TryReadModelSkeletonParents(permModelData, skeletonParents)) return false;

	CEntityInstance* entity = const_cast<CEntityInstance*>(this);
	const bool looksLikeDeadPawn = entity->IsPlayerPawn() && 0 == entity->GetHealth() && !entity->IsDormant() && 0 < entity->GetRenderAlpha();

	if ((HasAnyRagdollState() || looksLikeDeadPawn)
		&& 0 != g_clientDllOffsets.CBaseAnimGraph.m_RagdollPose
		&& 0 != g_clientDllOffsets.PhysicsRagdollPose_t.m_Transforms) {
		Source2UtlVectorView<Source2TransformStorage> ragdollTransforms;
		const auto* ragdollPoseTransforms =
			(const unsigned char*)this
			+ g_clientDllOffsets.CBaseAnimGraph.m_RagdollPose
			+ g_clientDllOffsets.PhysicsRagdollPose_t.m_Transforms;

		if (SafeReadVectorView(ragdollPoseTransforms, ragdollTransforms)
			&& IsPlausibleVector(ragdollTransforms, static_cast<int>(skeletonParents.size()))) {
			std::vector<Source2TransformStorage> transformData(skeletonParents.size());
			if (SafeReadMemory(ragdollTransforms.m_pMemory, transformData.data(), sizeof(Source2TransformStorage) * transformData.size())) {
				std::vector<SOURCESDK::matrix3x4_t> poseBoneTransforms(transformData.size());
				for (size_t i = 0; i < transformData.size(); ++i) {
					QuaternionMatrix(transformData[i].orientation, transformData[i].position, poseBoneTransforms[i]);
					if (!IsFiniteMatrix(poseBoneTransforms[i])) return false;
				}

				float originX, originY, originZ;
				entity->GetOrigin(originX, originY, originZ);
				const bool looksWorldSpace =
					1.0f >= std::abs(poseBoneTransforms[0][0][3] - originX)
					&& 1.0f >= std::abs(poseBoneTransforms[0][1][3] - originY)
					&& 1.0f >= std::abs(poseBoneTransforms[0][2][3] - originZ);

				if (looksWorldSpace) {
					if (BuildParentSpaceBonesFromWorld(this, skeletonParents, poseBoneTransforms, outBones)) {
						return true;
					}
				}
				else {
					outBones = std::move(poseBoneTransforms);
					if (!outBones.empty()) return true;
				}
			}
		}
	}

	if (0 != g_clientDllOffsets.C_RagdollProp.m_ragPos && 0 != g_clientDllOffsets.C_RagdollProp.m_ragAngles) {
		Source2UtlVectorView<SOURCESDK::Vector> ragPositions;
		Source2UtlVectorView<SOURCESDK::QAngle> ragAngles;
		if (SafeReadVectorView((unsigned char*)this + g_clientDllOffsets.C_RagdollProp.m_ragPos, ragPositions)
			&& IsPlausibleVector(ragPositions, static_cast<int>(skeletonParents.size()))
			&& SafeReadVectorView((unsigned char*)this + g_clientDllOffsets.C_RagdollProp.m_ragAngles, ragAngles)
			&& IsPlausibleVector(ragAngles, static_cast<int>(skeletonParents.size()))) {
			std::vector<SOURCESDK::Vector> worldPositions(skeletonParents.size());
			std::vector<SOURCESDK::QAngle> worldAngles(skeletonParents.size());
			if (SafeReadMemory(ragPositions.m_pMemory, worldPositions.data(), sizeof(SOURCESDK::Vector) * worldPositions.size())
				&& SafeReadMemory(ragAngles.m_pMemory, worldAngles.data(), sizeof(SOURCESDK::QAngle) * worldAngles.size())) {
				std::vector<SOURCESDK::matrix3x4_t> worldBoneTransforms(skeletonParents.size());
				for (size_t i = 0; i < worldBoneTransforms.size(); ++i) {
					SOURCESDK::AngleMatrix(worldAngles[i], worldPositions[i], worldBoneTransforms[i]);
					if (!IsFiniteMatrix(worldBoneTransforms[i])) return false;
				}

				if (BuildParentSpaceBonesFromWorld(this, skeletonParents, worldBoneTransforms, outBones)) {
					return true;
				}
			}
		}
	}

	return false;
}

bool CEntityInstance::HasBuiltRagdoll() const {
	return ReadRagdollFlag(this, g_clientDllOffsets.CBaseAnimGraph.m_bBuiltRagdoll);
}

bool CEntityInstance::HasClientSideRagdoll() const {
	if (ReadRagdollFlag(this, g_clientDllOffsets.CBaseAnimGraph.m_bRagdollClientSide)) return true;

	if (0 == g_clientDllOffsets.CBaseAnimGraph.m_pClientsideRagdoll) return false;

	CEntityInstance* clientSideRagdoll = nullptr;
	if (!SafeReadObject((const unsigned char*)this + g_clientDllOffsets.CBaseAnimGraph.m_pClientsideRagdoll, clientSideRagdoll) || !clientSideRagdoll) {
		return false;
	}

	return clientSideRagdoll != this;
}

bool CEntityInstance::IsRagdollEnabled() const {
	return ReadRagdollFlag(this, g_clientDllOffsets.CBaseAnimGraph.m_bRagdollEnabled);
}

bool CEntityInstance::HasAnyRagdollState() const {
	return HasBuiltRagdoll() || HasClientSideRagdoll() || IsRagdollEnabled();
}

typedef	void (__fastcall * org_LookupAttachment_t)(void* This, uint8_t& outIdx, const char* attachmentName);
org_LookupAttachment_t org_LookupAttachment = nullptr;

typedef	bool (__fastcall * org_GetAttachment_t)(void* This, uint8_t idx, void* out);
org_GetAttachment_t org_GetAttachment = nullptr;

uint8_t CEntityInstance::LookupAttachment(const char* attachmentName) {
	uint8_t idx = 0;
	org_LookupAttachment(this, idx, attachmentName);
	return idx;
}

bool CEntityInstance::GetAttachment(uint8_t idx, SOURCESDK::Vector &origin, SOURCESDK::Quaternion &angles) {
	alignas(16) float resData[8] = {0};

	if(org_GetAttachment(this, idx, resData)) {
		origin.x = resData[0];
		origin.y = resData[1];
		origin.z = resData[2];

		angles.x = resData[4];
		angles.y = resData[5];
		angles.z = resData[6];
		angles.w = resData[7];

		return true;
	}

	return false;
}

class CAfxEntityInstanceRef {
public:
    static CAfxEntityInstanceRef * Aquire(CEntityInstance * pInstance) {
        CAfxEntityInstanceRef * pRef;
        auto it = m_Map.find(pInstance);
        if(it != m_Map.end()) {    
            pRef = it->second;
        } else {
            pRef = new CAfxEntityInstanceRef(pInstance);
            m_Map[pInstance] = pRef;
        }
        pRef->AddRef();
        return pRef;
    }

    static void Invalidate(CEntityInstance * pInstance) {
        if(m_Map.empty()) return;
        auto it = m_Map.find(pInstance);
        if(it != m_Map.end()) {
            auto & pInstance = it->second;
            pInstance->m_pInstance = nullptr;
            m_Map.erase(it);
        }        
    }

    CEntityInstance * GetInstance() {
        return m_pInstance;
    }

    bool IsValid() {
        return nullptr != m_pInstance;
    }

    void AddRef() {
        m_RefCount++;
    }

    void Release() {
        m_RefCount--;
        if(0 == m_RefCount) {
            delete this;
        }
    }

protected:
    CAfxEntityInstanceRef(class CEntityInstance * pInstance)
    : m_pInstance(pInstance)
    {
    }

    ~CAfxEntityInstanceRef() {
        m_Map.erase(m_pInstance);
    }

private:
    int m_RefCount = 0;
    class CEntityInstance * m_pInstance;
    static std::map<CEntityInstance *,CAfxEntityInstanceRef *> m_Map;
};

std::map<CEntityInstance *,CAfxEntityInstanceRef *> CAfxEntityInstanceRef::m_Map;


typedef void* (__fastcall * OnAddEntity_t)(void* This, CEntityInstance* pInstance, SOURCESDK::uint32 handle);
OnAddEntity_t g_Org_OnAddEntity = nullptr;


void* __fastcall New_OnAddEntity(void* This, CEntityInstance* pInstance, SOURCESDK::uint32 handle) {

    void * result =  g_Org_OnAddEntity(This,pInstance,handle);

    if(g_b_on_add_entity && pInstance) {
        auto pRef = CAfxEntityInstanceRef::Aquire(pInstance);
        AfxHookSource2Rs_Engine_OnAddEntity(pRef,handle);
        pRef->Release();
    }

    return result;
}

typedef void* (__fastcall * OnRemoveEntity_t)(void* This, CEntityInstance* inst, SOURCESDK::uint32 handle);
OnRemoveEntity_t g_Org_OnRemoveEntity = nullptr;

void* __fastcall New_OnRemoveEntity(void* This, CEntityInstance* pInstance, SOURCESDK::uint32 handle) {

    if(g_b_on_remove_entity && pInstance) {
        auto pRef = CAfxEntityInstanceRef::Aquire(pInstance);
        AfxHookSource2Rs_Engine_OnRemoveEntity(pRef,handle);
        pRef->Release();
    }

    CAgrRecorder::Get().OnEntityDeleted((int)handle);

    CAfxEntityInstanceRef::Invalidate(pInstance);

    void * result =  g_Org_OnRemoveEntity(This,pInstance,handle);
    return result;
}

#define STRINGIZE(x) STRINGIZE2(x)
#define STRINGIZE2(x) #x
#define MkErrStr(file,line) "Problem in " file ":" STRINGIZE(line)
extern void ErrorBox(char const * messageText);

bool Hook_ClientEntitySystem( void* pEntityList, void * pFnGetHighestEntityIterator, void * pFnGetEntityFromIndex ) {
    static bool firstResult = false;
    static bool firstRun = true;

    if(firstRun) {
        firstRun = false;
        g_pEntityList = (void**)pEntityList;
        g_GetHighestEntityIndex = (GetHighestEntityIndex_t)pFnGetHighestEntityIterator;
        g_GetEntityFromIndex = (GetEntityFromIndex_t)pFnGetEntityFromIndex;
        firstResult = true;
    }

    return firstResult;
}

bool Hook_ClientEntitySystem2() {
    static bool firstResult = false;
    static bool firstRun = true;

    if(g_pEntityList && *g_pEntityList) {
        // https://github.com/bruhmoment21/cs2-sdk
        void ** vtable = **(void****)g_pEntityList;
        g_Org_OnAddEntity = (OnAddEntity_t)vtable[15];
        g_Org_OnRemoveEntity = (OnRemoveEntity_t)vtable[16];
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Org_OnAddEntity, New_OnAddEntity);
        DetourAttach(&(PVOID&)g_Org_OnRemoveEntity, New_OnRemoveEntity);
        firstResult = NO_ERROR == DetourTransactionCommit();
    }

    return firstResult;    
}

void Hook_ClientEntitySystem3(HMODULE clientDll) {
	// these two called one after each other
	//
	// 1808ce654 e8  d7  50       CALL       FUN_180623730
	//           d5  ff
	// 1808ce659 80  bd  e0       CMP        byte ptr [RBP + local_res8], 0x0
	//           04  00  00  00
	// 1808ce660 0f  84  c5       JZ         LAB_1808cf52b
	//           0e  00  00
	// 1808ce666 0f  b6  95       MOVZX      EDX, byte ptr [RBP + local_res10]
	//           e8  04  00  00
	// 1808ce66d 84  d2           TEST       DL,DL
	// 1808ce66f 0f  84  b6       JZ         LAB_1808cf52b
	//           0e  00  00
	// 1808ce675 4c  8d  45  40   LEA        R8=>local_498, [RBP + 0x40]
	// 1808ce679 48  8b  cf       MOV        RCX, RDI
	// 1808ce67c e8  5f  67       CALL       FUN_180614de0
	//           d4  ff
	//
	// Function where they called has "weapon_hand_R" string
	// also it's 2th in vtable for ".?AV?$_Func_impl_no_alloc@V<lambda_2>@?8??FrameUpdateBegin@CPlayerPawnFrameUpdateSystem@@QEAAXXZ@X$$V@std@@"
	// vtable could be find near "AsyncFrameUpdate" where it queues it

	if (auto startAddr = getAddress(clientDll, "E8 ?? ?? ?? ?? 80 BD ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 0F B6 95 ?? ?? ?? ?? 84 D2 0F 84 ?? ?? ?? ?? 4C 8D 45 ?? 48 8B CF E8 ?? ?? ?? ??")) {
		org_LookupAttachment = (org_LookupAttachment_t)(startAddr + 5 + *(int32_t*)(startAddr + 1));
		org_GetAttachment = (org_GetAttachment_t)(startAddr + 40 + 5 + *(int32_t*)(startAddr + 40 + 1));
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));
}

int GetHighestEntityIndex() {
    return 2048; // Hardcoded for now, because the function we have is the count, not the index and we need to change mirv-script API to support that better.
    //return g_pEntityList && g_GetHighestEntityIndex ? g_GetHighestEntityIndex(*g_pEntityList, false) : -1;
}

struct MirvEntityEntry {
	int entryIndex;
	int handle;
	std::string debugName;
	std::string className;
	std::string clientClassName;
	SOURCESDK::Vector origin;
	SOURCESDK::QAngle angles;
};

CON_COMMAND(mirv_listentities, "List entities.")
{
	auto argC = args->ArgC();
	auto arg0 = args->ArgV(0);

	bool filterPlayers = false;
	bool sortByDistance = false;
	int printCount = -1;

	if (2 <= argC && 0 == _stricmp(args->ArgV(1), "help")) {
		advancedfx::Message(
			"%s help - Print this help.\n"
			"%s <option1> <option2> ... - Customize printed output with options.\n"
			"Where <option> is (you don't have to use all):\n"
			"\t\"isPlayer=1\" - Show only player related entities. Unless you need handles, the \"mirv_deathmsg help players\" might be more useful.\n"
			"\t\"sort=distance\" - Sort entities by distance relative to current position, from closest to most distant.\n"
			"\t\"limit=<i>\" - Limit number of printed entries.\n"
			"Example:\n"
			"%s sort=distance limit=10\n" 
			, arg0, arg0, arg0
		);
		return;
	} else {
		for (int i = 1; i < argC; i++) {
			const char * argI = args->ArgV(i);
			if (StringIBeginsWith(argI, "limit=")) {
				printCount = atoi(argI + strlen("limit="));
			} 
			else if (StringIBeginsWith(argI, "sort=")) {
				if (0 == _stricmp(argI + strlen("sort="), "distance")) sortByDistance = true;
			}
			else if (0 == _stricmp(argI, "isPlayer=1")) {
				filterPlayers = true;
			}
		}
	}

	std::vector<MirvEntityEntry> entries;

    int highestIndex = GetHighestEntityIndex();
    for(int i = 0; i < highestIndex + 1; i++) {
        if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i)) {
			if (filterPlayers && !ent->IsPlayerController() && !ent->IsPlayerPawn()) continue;
			
            float render_origin[3];
            float render_angles[3];
            ent->GetRenderEyeOrigin(render_origin);
            ent->GetRenderEyeAngles(render_angles);

			auto debugName = ent->GetDebugName();
			auto className = ent->GetClassName();
			auto clientClassName = ent->GetClientClassName();

			entries.emplace_back(
				MirvEntityEntry {
					i, ent->GetHandle().ToInt(), 
					debugName ? debugName : "", className ? className : "", clientClassName ? clientClassName : "",
					SOURCESDK::Vector {render_origin[0], render_origin[1], render_origin[2]},
					SOURCESDK::QAngle {render_angles[0], render_angles[1], render_angles[2]} 
				}
			);

        }
    }

	if (sortByDistance) {
		SOURCESDK::Vector curPos = {(float)g_CurrentGameCamera.origin[0], (float)g_CurrentGameCamera.origin[1], (float)g_CurrentGameCamera.origin[2]};

		std::sort(entries.begin(), entries.end(), [&](MirvEntityEntry & a, MirvEntityEntry & b) {
			auto distA = (curPos - a.origin).LengthSqr();
			auto distB = (curPos - b.origin).LengthSqr();
			return distA < distB;
		});
	}

	advancedfx::Message("entryIndex / handle / debugName / className / clientClassName / [ x , y , z , rX , rY , rZ ]\n");
	if (printCount == -1) printCount = entries.size();
	for (int i = 0; i < printCount; i++) {
		auto e = entries[i];
		advancedfx::Message("%i / %i / %s / %s / %s / [ %f , %f , %f , %f , %f , %f ]\n"
			, e.entryIndex, e.handle
			, e.debugName.c_str(), e.className.c_str(), e.clientClassName.c_str()
			, e.origin.x, e.origin.y, e.origin.z 
			, e.angles.x, e.angles.y, e.angles.z
		);
	}
}

extern "C" int afx_hook_source2_get_highest_entity_index() {
    int highestIndex = GetHighestEntityIndex();
    return highestIndex;
}

extern "C" void * afx_hook_source2_get_entity_ref_from_index(int index) {
    if(CEntityInstance * result = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,index)) {
        return CAfxEntityInstanceRef::Aquire(result);
    }
    return nullptr;
}

extern "C" void afx_hook_source2_add_ref_entity_ref(void * pRef) {
    ((CAfxEntityInstanceRef *)pRef)->AddRef();
}

extern "C" void afx_hook_source2_release_entity_ref(void * pRef) {
    ((CAfxEntityInstanceRef *)pRef)->Release();
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_is_valid(void * pRef) {
    return BOOL_TO_FFIBOOL(((CAfxEntityInstanceRef *)pRef)->IsValid());
}

extern "C" const char * afx_hook_source2_get_entity_ref_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetName();
    }
    return "";
}

extern "C" const char * afx_hook_source2_get_entity_ref_debug_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetDebugName();
    }
    return nullptr;
}

extern "C" const char * afx_hook_source2_get_entity_ref_class_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetClassName();
    }
    return "";
}

extern "C" const char * afx_hook_source2_get_entity_ref_client_class_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetClientClassName();
    }
    return "";
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_is_player_pawn(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return BOOL_TO_FFIBOOL(pInstance->IsPlayerPawn());
    }
    return FFIBOOL_FALSE;
}

extern "C" int afx_hook_source2_get_entity_ref_player_pawn_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetPlayerPawnHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;    
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_is_player_controller(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return BOOL_TO_FFIBOOL(pInstance->IsPlayerController());
    }
    return FFIBOOL_FALSE;    
}

extern "C" int afx_hook_source2_get_entity_ref_player_controller_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetPlayerControllerHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;  
}

extern "C" int afx_hook_source2_get_entity_ref_health(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetHealth();
    }
    return 0;    
}

extern "C" int afx_hook_source2_get_entity_ref_team(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetTeam();
    }
    return 0;    
}


extern "C" void afx_hook_source2_get_entity_ref_origin(void * pRef, float & x, float & y, float & z) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       pInstance->GetOrigin(x,y,z);
    }    
}

extern "C" void afx_hook_source2_get_entity_ref_render_eye_origin(void * pRef, float & x, float & y, float & z) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        float tmp[3];
       pInstance->GetRenderEyeOrigin(tmp);
       x = tmp[0];
       y = tmp[1];
       z = tmp[2];
    }    
}

extern "C" void afx_hook_source2_get_entity_ref_render_eye_angles(void * pRef, float & x, float & y, float & z) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        float tmp[3];
       pInstance->GetRenderEyeAngles(tmp);
       x = tmp[0];
       y = tmp[1];
       z = tmp[2];
    }    
}

extern "C" int afx_hook_source2_get_entity_ref_view_entity_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetViewEntityHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
}

extern "C" int afx_hook_source2_get_entity_ref_active_weapon_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetActiveWeaponHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
}

extern "C" const char* afx_hook_source2_get_entity_ref_player_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetPlayerName();
    }
    return nullptr;
}

extern "C" uint64_t afx_hook_source2_get_entity_ref_steam_id(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetSteamId();
    }
    return 0;
}

extern "C" const char* afx_hook_source2_get_entity_ref_sanitized_player_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetSanitizedPlayerName();
    }
    return nullptr;
}

typedef CEntityInstance *  (__fastcall * ClientDll_GetSplitScreenPlayer_t)(int slot);
ClientDll_GetSplitScreenPlayer_t g_ClientDll_GetSplitScreenPlayer = nullptr;

bool Hook_GetSplitScreenPlayer( void* pAddr) {
    g_ClientDll_GetSplitScreenPlayer = (ClientDll_GetSplitScreenPlayer_t)pAddr;
    return true;
}

CEntityInstance* GetSplitScreenPlayer(int index) {
	if (!g_ClientDll_GetSplitScreenPlayer) return nullptr;
	return g_ClientDll_GetSplitScreenPlayer(index);
}

extern "C" void * afx_hook_source2_get_entity_ref_from_split_screen_player(int index) {
    if(0 == index && g_ClientDll_GetSplitScreenPlayer) {
        if(CEntityInstance * result = g_ClientDll_GetSplitScreenPlayer(index)) {
            return CAfxEntityInstanceRef::Aquire(result);
        }
    }
    return nullptr;
}

extern "C" uint8_t afx_hook_source2_get_entity_ref_observer_mode(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetObserverMode();
    }
    return 0;
}

extern "C" int afx_hook_source2_get_entity_ref_observer_target_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetObserverTarget().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_attachment(void * pRef, const char* attachmentName, double outPosition[3], double outAngles[4]) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
		auto idx = pInstance->LookupAttachment(attachmentName);
		if (0 == idx) return FFIBOOL_FALSE;
		
		SOURCESDK::Vector origin;
		SOURCESDK::Quaternion angles;

		if (pInstance->GetAttachment(idx, origin, angles)) {
			outPosition[0] = origin.x;
			outPosition[1] = origin.y;
			outPosition[2] = origin.z;

			outAngles[0] = angles.w;
			outAngles[1] = angles.x;
			outAngles[2] = angles.y;
			outAngles[3] = angles.z;

			return FFIBOOL_TRUE;
		}
    }

    return FFIBOOL_FALSE;
}
