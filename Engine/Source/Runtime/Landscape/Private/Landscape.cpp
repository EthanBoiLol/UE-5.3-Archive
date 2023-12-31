// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
Landscape.cpp: Terrain rendering
=============================================================================*/

#include "Landscape.h"

#include "Serialization/MemoryWriter.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/MemoryReader.h"
#include "UObject/RenderingObjectVersion.h"
#include "UObject/UObjectIterator.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/DevObjectVersion.h"
#include "UObject/LinkerLoad.h"
#include "Framework/Application/SlateApplication.h"
#include "LandscapePrivate.h"
#include "LandscapeStreamingProxy.h"
#include "LandscapeInfo.h"
#include "LightMap.h"
#include "Engine/MapBuildDataRegistry.h"
#include "ShadowMap.h"
#include "LandscapeComponent.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeInfoMap.h"
#include "EditorSupportDelegates.h"
#include "LandscapeMeshProxyComponent.h"
#include "LandscapeNaniteComponent.h"
#include "LandscapeRender.h"
#include "LandscapePrivate.h"
#include "Logging/TokenizedMessage.h"
#include "Logging/MessageLog.h"
#include "Misc/UObjectToken.h"
#include "Misc/MapErrors.h"
#include "Misc/PackageSegment.h"
#include "DerivedDataCacheInterface.h"
#include "Interfaces/ITargetPlatform.h"
#include "LandscapeMeshCollisionComponent.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "LandscapeMaterialInstanceConstant.h"
#include "Engine/CollisionProfile.h"
#include "LandscapeMeshProxyActor.h"
#include "Materials/MaterialExpressionLandscapeLayerWeight.h"
#include "Materials/MaterialExpressionLandscapeLayerSwitch.h"
#include "Materials/MaterialExpressionLandscapeLayerSample.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Materials/MaterialExpressionLandscapeVisibilityMask.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProfilingDebugging/CookStats.h"
#include "ILandscapeSplineInterface.h"
#include "LandscapeGrassType.h"
#include "LandscapeSplineActor.h"
#include "LandscapeSplinesComponent.h"
#include "EngineGlobals.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "ComponentRecreateRenderStateContext.h"
#include "LandscapeWeightmapUsage.h"
#include "LandscapeSubsystem.h"
#include "LandscapeCulling.h"
#include "ContentStreaming.h"
#include "UObject/ObjectSaveContext.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "PixelShaderUtils.h"
#include "LandscapeEditResources.h"
#include "Rendering/Texture2DResource.h"
#include "RenderCaptureInterface.h"
#include "VisualLogger/VisualLogger.h"
#include "NaniteSceneProxy.h"
#include "Misc/ArchiveMD5.h"
#include "LandscapeTextureStorageProvider.h"
#include "LandscapeVersion.h"
#include "UObject/FortniteMainBranchObjectVersion.h"
#include "UObject/FortniteReleaseBranchCustomObjectVersion.h"
#include "UObject/EditorObjectVersion.h"
#include "UObject/UObjectThreadContext.h"
#include "LandscapeDataAccess.h"
#include "LandscapeNotification.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Algo/BinarySearch.h"
#include "Algo/Count.h"
#include "Algo/Transform.h"
#include "Algo/AllOf.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "WorldPartition/Landscape/LandscapeActorDesc.h"

#if WITH_EDITOR
#include "Rendering/StaticLightingSystemInterface.h"
#include "Misc/ScopedSlowTask.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "LandscapeEdit.h"
#include "LandscapeEditTypes.h"
#include "MaterialUtilities.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Texture2D.h"
#include "AssetCompilingManager.h"
#endif

/** Landscape stats */

DEFINE_STAT(STAT_LandscapeDynamicDrawTime);
DEFINE_STAT(STAT_LandscapeVFDrawTimeVS);
DEFINE_STAT(STAT_LandscapeVFDrawTimePS);
DEFINE_STAT(STAT_LandscapeComponentRenderPasses);
DEFINE_STAT(STAT_LandscapeDrawCalls);
DEFINE_STAT(STAT_LandscapeTriangles);
DEFINE_STAT(STAT_LandscapeLayersRegenerateDrawCalls);

#if ENABLE_COOK_STATS
namespace LandscapeCookStats
{
	static FCookStats::FDDCResourceUsageStats UsageStats;
	static FCookStatsManager::FAutoRegisterCallback RegisterCookStats([](FCookStatsManager::AddStatFuncRef AddStat)
	{
		UsageStats.LogStats(AddStat, TEXT("Landscape.Usage"), TEXT(""));
	});
}
#endif

#define LOCTEXT_NAMESPACE "Landscape"

static void PrintNumLandscapeShadows()
{
	int32 NumComponents = 0;
	int32 NumShadowCasters = 0;
	for (TObjectIterator<ULandscapeComponent> It; It; ++It)
	{
		ULandscapeComponent* LC = *It;
		NumComponents++;
		if (LC->CastShadow && LC->bCastDynamicShadow)
		{
			NumShadowCasters++;
		}
	}
	UE_LOG(LogConsoleResponse, Display, TEXT("%d/%d landscape components cast shadows"), NumShadowCasters, NumComponents);
}

FAutoConsoleCommand CmdPrintNumLandscapeShadows(
	TEXT("ls.PrintNumLandscapeShadows"),
	TEXT("Prints the number of landscape components that cast shadows."),
	FConsoleCommandDelegate::CreateStatic(PrintNumLandscapeShadows)
	);

int32 RenderCaptureNextHeightmapRenders = 0;
static FAutoConsoleVariableRef CVarRenderCaptureNextHeightmapRenders(
	TEXT("landscape.RenderCaptureNextHeightmapRenders"),
	RenderCaptureNextHeightmapRenders,
	TEXT("Trigger a render capture during the next N RenderHeightmap draws"));

#if WITH_EDITOR

float LandscapeNaniteAsyncDebugWait = 0.0f;
static FAutoConsoleVariableRef CVarNaniteAsyncDebugWait(
	TEXT("landscape.Nanite.AsyncDebugWait"),
	LandscapeNaniteAsyncDebugWait,
	TEXT("Time in seconds to pause the async Nanite build. Used for debugging"));

float LandscapeNaniteBuildLag = 0.25f;
static FAutoConsoleVariableRef CVarNaniteUpdateLag(
	TEXT("landscape.Nanite.UpdateLag"),
	LandscapeNaniteBuildLag,
	TEXT("Time to wait in seconds after the last landscape update before triggering a nanite rebuild"));

static FAutoConsoleVariable CVarForceInvalidateNaniteOnLoad(
	TEXT("landscape.ForceInvalidateNaniteOnLoad"),
	false,
	TEXT("Trigger a rebuild of Nanite representation on load (for debugging purposes)"));

static FAutoConsoleVariable CVarSilenceSharedPropertyDeprecationFixup(
	TEXT("landscape.SilenceSharedPropertyDeprecationFixup"),
	true,
	TEXT("Silently performs the fixup of discrepancies in shared properties when handling data modified before the enforcement introduction."));
#endif // WITH_EDITOR

int32 GRenderNaniteLandscape = 1;
FAutoConsoleVariableRef CVarRenderNaniteLandscape(
	TEXT("landscape.RenderNanite"),
	GRenderNaniteLandscape,
	TEXT("Render Landscape using Nanite."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

struct FCompareULandscapeComponentClosest
{
	FCompareULandscapeComponentClosest(const FIntPoint& InCenter) : Center(InCenter) {}

	FORCEINLINE bool operator()(const ULandscapeComponent* A, const ULandscapeComponent* B) const
	{
		const FIntPoint ABase = A->GetSectionBase();
		const FIntPoint BBase = B->GetSectionBase();

		int32 DistA = (ABase - Center).SizeSquared();
		int32 DistB = (BBase - Center).SizeSquared();

		return DistA < DistB;
	}

	FIntPoint Center;

};

ULandscapeComponent::ULandscapeComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, bNaniteActive(false)
#if WITH_EDITORONLY_DATA
	, LayerUpdateFlagPerMode(0)
	, bPendingCollisionDataUpdate(false)
	, bPendingLayerCollisionDataUpdate(false)
	, WeightmapsHash(0)
	, SplineHash(0)
	, PhysicalMaterialHash(0)
#endif
	, GrassData(MakeShareable(new FLandscapeComponentGrassData()))
	, ChangeTag(0)
{
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);

	bUseAsOccluder = true;
	bAllowCullDistanceVolume = false;
	CollisionMipLevel = 0;
	StaticLightingResolution = 0.f; // Default value 0 means no overriding

	MaterialInstances.AddDefaulted(); // make sure we always have a MaterialInstances[0]	
	LODIndexToMaterialIndex.AddDefaulted(); // make sure we always have a MaterialInstances[0]	

	HeightmapScaleBias = FVector4(0.0f, 0.0f, 0.0f, 1.0f);
	WeightmapScaleBias = FVector4(0.0f, 0.0f, 0.0f, 1.0f);

	bBoundsChangeTriggersStreamingDataRebuild = true;
	ForcedLOD = -1;
	LODBias = 0;
#if WITH_EDITORONLY_DATA
	LightingLODBias = -1; // -1 Means automatic LOD calculation based on ForcedLOD + LODBias
#endif

	Mobility = EComponentMobility::Static;

#if WITH_EDITORONLY_DATA
	EditToolRenderData = FLandscapeEditToolRenderData();
#endif

	// We don't want to load this on the server, this component is for graphical purposes only
	AlwaysLoadOnServer = false;

	// Default sort priority of landscape to -1 so that it will default to the first thing rendered in any runtime virtual texture
	TranslucencySortPriority = -1;
}

int32 ULandscapeComponent::GetMaterialInstanceCount(bool InDynamic) const
{
	ALandscapeProxy* Actor = GetLandscapeProxy();

	if (Actor != nullptr && Actor->bUseDynamicMaterialInstance && InDynamic)
	{
		return MaterialInstancesDynamic.Num();
	}

	return MaterialInstances.Num();
}

UMaterialInstance* ULandscapeComponent::GetMaterialInstance(int32 InIndex, bool InDynamic) const
{
	ALandscapeProxy* Actor = GetLandscapeProxy();

	if (Actor != nullptr && Actor->bUseDynamicMaterialInstance && InDynamic)
	{
		check(MaterialInstancesDynamic.IsValidIndex(InIndex));
		return MaterialInstancesDynamic[InIndex];
	}

	check(MaterialInstances.IsValidIndex(InIndex));
	return MaterialInstances[InIndex];
}

UMaterialInstanceDynamic* ULandscapeComponent::GetMaterialInstanceDynamic(int32 InIndex) const
{
	ALandscapeProxy* Actor = GetLandscapeProxy();

	if (Actor != nullptr && Actor->bUseDynamicMaterialInstance)
	{
		if (MaterialInstancesDynamic.IsValidIndex(InIndex))
		{
			return MaterialInstancesDynamic[InIndex];
		}
	}

	return nullptr;
}

#if WITH_EDITOR

void ULandscapeComponent::BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform)
{
	Super::BeginCacheForCookedPlatformData(TargetPlatform);

	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		if (TargetPlatform->SupportsFeature(ETargetPlatformFeatures::MobileRendering))
		{
			CheckGenerateMobilePlatformData(/*bIsCooking = */ true, TargetPlatform);
		}
	}
}

// Deprecated, use CheckGenerateMobilePlatformData
void ALandscapeProxy::CheckGenerateLandscapePlatformData(bool bIsCooking, const ITargetPlatform* TargetPlatform)
{
	return CheckGenerateMobilePlatformData(bIsCooking, TargetPlatform);
}

void ALandscapeProxy::CheckGenerateMobilePlatformData(bool bIsCooking, const ITargetPlatform* TargetPlatform)
{
	for (ULandscapeComponent* Component : LandscapeComponents)
	{
		Component->CheckGenerateMobilePlatformData(bIsCooking, TargetPlatform);
	}
}

bool ALandscapeProxy::IsNaniteMeshUpToDate() const
{
	if (IsNaniteEnabled() && !HasAnyFlags(RF_ClassDefaultObject) && LandscapeComponents.Num() > 0)
	{
		const FGuid NaniteContentId = GetNaniteContentId();
		return AreNaniteComponentsValid(NaniteContentId);
	}

	return true;
}

FGraphEventRef ALandscapeProxy::UpdateNaniteRepresentationAsync(const ITargetPlatform* InTargetPlatform)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ALandscapeProxy::UpdateNaniteRepresentationAsync);
	FGraphEventRef BatchBuildEvent;

	if (IsNaniteEnabled() && !HasAnyFlags(RF_ClassDefaultObject) && LandscapeComponents.Num() > 0)
	{
		const FGuid NaniteContentId = GetNaniteContentId();

		int32 NumNewNaniteComponents = NumNaniteRequiredComponents();
		if (NumNewNaniteComponents != NaniteComponents.Num())
		{
			RemoveNaniteComponents();
			CreateNaniteComponents(NumNewNaniteComponents);
		}

		const FGuid ComponentNaniteContentId = GetNaniteComponentContentId();
		const bool bNaniteContentDirty = ComponentNaniteContentId != NaniteContentId;

		UE_LOG(LogLandscape, Log, TEXT("UpdateNaniteRepresentationAsync actor: '%s' package:'%s' dirty:%i component guid:'%s' proxy guid:'%s'"), *GetActorNameOrLabel(), *GetPackage()->GetName(), bNaniteContentDirty, *ComponentNaniteContentId.ToString(), *NaniteContentId.ToString());

		TArray<ULandscapeComponent*> StableOrderComponents(LandscapeComponents);
		ULandscapeSubsystem* Subsystem = GetWorld()->GetSubsystem<ULandscapeSubsystem>();
		FGraphEventArray UpdateDependencies;
		for (int32 i = 0; i < NumNewNaniteComponents; ++i)
		{
			FGraphEventArray SingleProxyDependencies;

			if (bNaniteContentDirty)
			{
				const int32 StartComponentIndex = i * NaniteMaxComponents;
				const int32 EndComponentIndex = FMath::Min(LandscapeComponents.Num(), (i + 1) * NaniteMaxComponents);
				const int32 NumComponents = EndComponentIndex - StartComponentIndex;

				TArrayView<ULandscapeComponent*> StableOrderComponentsView = TArrayView<ULandscapeComponent*>(&StableOrderComponents[StartComponentIndex], LandscapeComponents.Num() - StartComponentIndex);

				ULandscapeComponent** MinComponent = Algo::MinElementBy(StableOrderComponentsView,
					[](const ULandscapeComponent* Component) { return Component->GetSectionBase(); },
					[](const FIntPoint& A, const FIntPoint& B) { return (A.Y == B.Y) ? (A.X < B.X) : (A.Y < B.Y); }
				);
				check(MinComponent);
				Algo::Sort(StableOrderComponentsView, FCompareULandscapeComponentClosest((*MinComponent)->GetSectionBase()));

				TArrayView<ULandscapeComponent*> ComponentsToExport(StableOrderComponentsView.GetData(), NumComponents);
				SingleProxyDependencies.Add(NaniteComponents[i]->InitializeForLandscapeAsync(this, NaniteContentId, Subsystem->IsMultithreadedNaniteBuildEnabled(), ComponentsToExport, i) );
			}

			// TODO: Add a flag that only initializes the platform if we called InitializeForLandscape during the PreSave for this or a previous platform
			TWeakObjectPtr<ULandscapeNaniteComponent> WeakComponent = NaniteComponents[i];
			TWeakObjectPtr<ALandscapeProxy> WeakProxy = this;
			FGraphEventRef FinalizeEvent = FFunctionGraphTask::CreateAndDispatchWhenReady([WeakComponent, WeakProxy, Name = GetActorNameOrLabel(), InTargetPlatform]() {
				if (!WeakComponent.IsValid() || !WeakProxy.IsValid())
				{
					UE_LOG(LogLandscape, Log, TEXT("UpdateNaniteRepresentationAsync Component on: '%s' Is Invalid"), *Name);
					return;
				}
				WeakComponent->InitializePlatformForLandscape(WeakProxy.Get(), InTargetPlatform);
				WeakComponent->UpdatedSharedPropertiesFromActor();
				},
				TStatId(),
				&SingleProxyDependencies,
				ENamedThreads::GameThread);

			UpdateDependencies.Add(FinalizeEvent);
		}
			
		BatchBuildEvent = FFunctionGraphTask::CreateAndDispatchWhenReady([] {}, TStatId(), &UpdateDependencies, ENamedThreads::GameThread);


	}
	else
	{
		InvalidateNaniteRepresentation(/* bInCheckContentId = */false);
	}

	return BatchBuildEvent;
}

void ALandscapeProxy::UpdateNaniteRepresentation(const ITargetPlatform* InTargetPlatform)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ALandscapeProxy::UpdateNaniteRepresentation);

	FGraphEventRef GraphEvent = UpdateNaniteRepresentationAsync(InTargetPlatform);
	ULandscapeSubsystem* Subsystem = GetWorld()->GetSubsystem<ULandscapeSubsystem>();

	if (!GraphEvent.IsValid())
	{
		return;
	}

	UE_LOG(LogLandscape, Display, TEXT("UpdateNaniteRepresentation proxy:%p target platform:%p subsystem:%p"), this, InTargetPlatform, Subsystem);
	if (!Subsystem->IsMultithreadedNaniteBuildEnabled() || IsRunningCookCommandlet())
	{
		UE_LOG(LogLandscape, Display, TEXT("Waiting for nanite build: '%s'"), *GetActorNameOrLabel());
		while (!GraphEvent->IsComplete())
		{
			ENamedThreads::Type CurrentThread = FTaskGraphInterface::Get().GetCurrentThreadIfKnown();
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(CurrentThread);
			FAssetCompilingManager::Get().ProcessAsyncTasks();	
		}
		UE_LOG(LogLandscape, Display, TEXT("Complete nanite build '%s'"), *GetActorNameOrLabel());
	}
}

void ALandscapeProxy::InvalidateNaniteRepresentation(bool bInCheckContentId)
{
	if (HasNaniteComponents())
	{
		if (!bInCheckContentId || GetNaniteComponentContentId() != GetNaniteContentId())
		{
			RemoveNaniteComponents();
		}
	}
}

void ALandscapeProxy::InvalidateOrUpdateNaniteRepresentation(bool bInCheckContentId, const ITargetPlatform* InTargetPlatform)
{
	TRACE_BOOKMARK(TEXT("ALandscapeProxy::InvalidateOrUpdateNaniteRepresentation"));	

	ULandscapeSubsystem* Subsystem = GetWorld()->GetSubsystem<ULandscapeSubsystem>();
	if (Subsystem->IsLiveNaniteRebuildEnabled())
	{
		UpdateNaniteRepresentation(InTargetPlatform);
	}
	else
	{
		InvalidateNaniteRepresentation(bInCheckContentId);
	}
}

FGuid ALandscapeProxy::GetNaniteContentId() const
{
	if (!IsNaniteEnabled())
	{
		return FGuid();
	}

	FBufferArchive ContentStateAr;

	int32 LocalNaniteLODIndex = GetNaniteLODIndex();
	ContentStateAr << LocalNaniteLODIndex;

	struct FCompareULandscapeComponentBySectionBase
	{
		FORCEINLINE bool operator()(const ULandscapeComponent* A, const ULandscapeComponent* B) const
		{
			if (!A)
			{
				return true;
			}
			if (!B)
			{
				return false;
			}
			// Sort components based on their SectionBase (i.e. 2D index relative to the entire landscape) to ensure stable ID generation
			return (A->GetSectionBase().X == B->GetSectionBase().X) ? (A->GetSectionBase().Y < B->GetSectionBase().Y) : (A->GetSectionBase().X < B->GetSectionBase().X);
		}
	};
	TArray<ULandscapeComponent*> StableOrderComponents(LandscapeComponents);
	Algo::Sort(StableOrderComponents, FCompareULandscapeComponentBySectionBase());

	for (ULandscapeComponent* Component : StableOrderComponents)
	{
		if (Component == nullptr)
		{
			continue;
		}

		// Bump if changes to ULandscapeNaniteComponent::InitializeForLandscape() need to be enforced.
		static FGuid ExportRawMeshGuid("36208D9A475B4D93B33BF84FFEDA1536");
		ContentStateAr << ExportRawMeshGuid;

		FGuid HeightmapGuid = Component->GetHeightmap()->Source.GetId();
		ContentStateAr << HeightmapGuid;

		// Take into account the Heightmap offset per component
		ContentStateAr << Component->HeightmapScaleBias.Z;
		ContentStateAr << Component->HeightmapScaleBias.W;

		// Visibility affects the generated Nanite mesh so it has to be taken into account :
		//  Note : visibility might be different at runtime if using a masked material (per-pixel visibility) but we obviously cannot take that into account
		//  when baking the visibility into the mesh like we do with Nanite landscape
		if (Component->ComponentHasVisibilityPainted())
		{
			const TArray<UTexture2D*>& WeightmapTextures = Component->GetWeightmapTextures();
			const TArray<FWeightmapLayerAllocationInfo>& AllocInfos = Component->GetWeightmapLayerAllocations();
			for (const FWeightmapLayerAllocationInfo& AllocInfo : AllocInfos)
			{
				if (AllocInfo.IsAllocated() && AllocInfo.LayerInfo == ALandscapeProxy::VisibilityLayer)
				{
					UTexture2D* VisibilityWeightmap = WeightmapTextures[AllocInfo.WeightmapTextureIndex];
					check(VisibilityWeightmap != nullptr);

					// TODO [jonathan.bard] : technically, this is not good, we would need to only check the hash of AllocInfo.WeightmapTextureChannel. We'll leave it as is, though, for 
					//  as long as we don't store the source weightmaps individually, so that this function stays fast : 
					FGuid VisibilityWeightmapGuid = VisibilityWeightmap->Source.GetId();
					ContentStateAr << VisibilityWeightmapGuid;
				}
			}
		}
	}

	// nanite content depends on if the skirt geometry is enabled & the depth.
	int32 NaniteSkirtEnabled = bNaniteSkirtEnabled;
	float NaniteSkirtDepthTest = bNaniteSkirtEnabled ? NaniteSkirtDepth : 0.0f; // The hash should only change if Skirts are enabled.
	ContentStateAr << NaniteSkirtEnabled;
	ContentStateAr << NaniteSkirtDepthTest;

	uint32 Hash[5];
	FSHA1::HashBuffer(ContentStateAr.GetData(), ContentStateAr.Num(), (uint8*)Hash);
	return FGuid(Hash[0] ^ Hash[4], Hash[1], Hash[2], Hash[3]);
}

void ULandscapeComponent::CheckGenerateMobilePlatformData(bool bIsCooking, const ITargetPlatform* TargetPlatform)
{
	// Regenerate platform data only when it's missing or there is a valid hash-mismatch.
	FBufferArchive ComponentStateAr;
	SerializeStateHashes(ComponentStateAr);

	// Serialize the version guid as part of the hash so we can invalidate DDC data if needed
	FString MobileVersion = FDevSystemGuids::GetSystemGuid(FDevSystemGuids::Get().LANDSCAPE_MOBILE_COOK_VERSION).ToString();
	ComponentStateAr << MobileVersion;

	uint32 Hash[5];
	FSHA1::HashBuffer(ComponentStateAr.GetData(), ComponentStateAr.Num(), (uint8*)Hash);
	FGuid NewSourceHash = FGuid(Hash[0] ^ Hash[4], Hash[1], Hash[2], Hash[3]);

	bool bHashMismatch = MobileDataSourceHash != NewSourceHash;
	bool bMissingPixelData = MobileMaterialInterfaces.Num() == 0 || MaterialPerLOD.Num() == 0;
	bool bRegeneratePixelData = bMissingPixelData || bHashMismatch;

	if (bRegeneratePixelData)
	{
		GenerateMobilePlatformPixelData(bIsCooking, TargetPlatform);
	}

	MobileDataSourceHash = NewSourceHash;
}

#endif // WITH_EDITOR

void ULandscapeComponent::SetForcedLOD(int32 InForcedLOD)
{
	SetLOD(/*bForced = */true, InForcedLOD);
}

void ULandscapeComponent::SetLODBias(int32 InLODBias)
{
	SetLOD(/*bForced = */false, InLODBias);
}

void ULandscapeComponent::SetLOD(bool bForcedLODChanged, int32 InLODValue)
{
	if (bForcedLODChanged)
	{
		ForcedLOD = InLODValue;
		if (ForcedLOD >= 0)
		{
			ForcedLOD = FMath::Clamp<int32>(ForcedLOD, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
		}
		else
		{
			ForcedLOD = -1;
		}
	}
	else
	{
		int32 MaxLOD = FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1;
		LODBias = FMath::Clamp<int32>(InLODValue, -MaxLOD, MaxLOD);
	}

	InvalidateLightingCache();
	MarkRenderStateDirty();

#if WITH_EDITOR
	// Update neighbor components for lighting cache (only relevant in the editor ATM) : 
	ULandscapeInfo* Info = GetLandscapeInfo();
	if (Info)
	{
		FIntPoint ComponentBase = GetSectionBase() / ComponentSizeQuads;
		FIntPoint LandscapeKey[8] =
		{
			ComponentBase + FIntPoint(-1, -1),
			ComponentBase + FIntPoint(+0, -1),
			ComponentBase + FIntPoint(+1, -1),
			ComponentBase + FIntPoint(-1, +0),
			ComponentBase + FIntPoint(+1, +0),
			ComponentBase + FIntPoint(-1, +1),
			ComponentBase + FIntPoint(+0, +1),
			ComponentBase + FIntPoint(+1, +1)
		};

		for (int32 Idx = 0; Idx < 8; ++Idx)
		{
			ULandscapeComponent* Comp = Info->XYtoComponentMap.FindRef(LandscapeKey[Idx]);
			if (Comp)
			{
				Comp->Modify();
				Comp->InvalidateLightingCache();
				Comp->MarkRenderStateDirty();
			}
		}
	}
#endif // WITH_EDITOR
}

void ULandscapeComponent::SetNaniteActive(bool bValue)
{
	if (bNaniteActive != bValue)
	{
		bNaniteActive = bValue;
		MarkRenderStateDirty();
	}
}

void ULandscapeComponent::Serialize(FArchive& Ar)
{
	LLM_SCOPE(ELLMTag::Landscape);
	Ar.UsingCustomVersion(FRenderingObjectVersion::GUID);
	Ar.UsingCustomVersion(FFortniteMainBranchObjectVersion::GUID);
	Ar.UsingCustomVersion(FEditorObjectVersion::GUID);

#if WITH_EDITOR
	if (Ar.IsCooking() && !HasAnyFlags(RF_ClassDefaultObject))
	{
		// for -oldcook:
		// the old cooker calls BeginCacheForCookedPlatformData after the package export set is tagged, so the mobile material doesn't get saved, so we have to do CheckGenerateMobilePlatformData in serialize
		// the new cooker clears the texture source data before calling serialize, causing GeneratePlatformVertexData to crash, so we have to do CheckGenerateMobilePlatformData in BeginCacheForCookedPlatformData
		if (Ar.CookingTarget()->SupportsFeature(ETargetPlatformFeatures::MobileRendering))
		{
			CheckGenerateMobilePlatformData(/*bIsCooking = */ true, Ar.CookingTarget());
		}
	}

	// Avoid the archiver in the PIE duplicate writer case because we want to share landscape textures & materials
	if (Ar.GetPortFlags() & PPF_DuplicateForPIE)
	{
		if (Ar.IsLoading())
		{
			Super::Serialize(Ar);
		}

		TArray<UObject**> TexturesAndMaterials;
		TexturesAndMaterials.Add((UObject**)&HeightmapTexture);
		TexturesAndMaterials.Add((UObject**)&XYOffsetmapTexture);
		for (TObjectPtr<UTexture2D>& WeightmapTexture : WeightmapTextures)
		{
			TexturesAndMaterials.Add((UObject**)&static_cast<UTexture2D*&>(WeightmapTexture));
		}
		for (TObjectPtr<UTexture2D>& MobileWeightmapTexture : MobileWeightmapTextures)
		{
			TexturesAndMaterials.Add((UObject**)&static_cast<UTexture2D*&>(MobileWeightmapTexture));
		}
		for (auto& ItPair : LayersData)
		{
			FLandscapeLayerComponentData& LayerComponentData = ItPair.Value;
			TexturesAndMaterials.Add((UObject**)&LayerComponentData.HeightmapData.Texture);
			for (TObjectPtr<UTexture2D>& WeightmapTexture : LayerComponentData.WeightmapData.Textures)
			{
				TexturesAndMaterials.Add((UObject**)&static_cast<UTexture2D*&>(WeightmapTexture));
			}
		}
		for (TObjectPtr<UMaterialInstanceConstant>& MaterialInstance : MaterialInstances)
		{
			TexturesAndMaterials.Add((UObject**)&static_cast<UMaterialInstanceConstant*&>(MaterialInstance));
		}
		for (TObjectPtr<UMaterialInterface>& MobileMaterialInterface : MobileMaterialInterfaces)
		{
			TexturesAndMaterials.Add((UObject**)(&static_cast<UMaterialInterface*&>(MobileMaterialInterface)));
		}
		for (TObjectPtr<UMaterialInstanceConstant>& MobileCombinationMaterialInstance : MobileCombinationMaterialInstances)
		{
			TexturesAndMaterials.Add((UObject**)&static_cast<UMaterialInstanceConstant*&>(MobileCombinationMaterialInstance));
		}

		if (Ar.IsSaving())
		{
			TArray<UObject*> BackupTexturesAndMaterials;
			BackupTexturesAndMaterials.AddZeroed(TexturesAndMaterials.Num());
			for (int i = 0; i < TexturesAndMaterials.Num(); ++i)
			{
				Exchange(*TexturesAndMaterials[i], BackupTexturesAndMaterials[i]);
			}

			Super::Serialize(Ar);

			for (int i = 0; i < TexturesAndMaterials.Num(); ++i)
			{
				Exchange(*TexturesAndMaterials[i], BackupTexturesAndMaterials[i]);
			}
		}
		// Manually serialize pointers
		for (UObject** Object : TexturesAndMaterials)
		{
			Ar.Serialize(Object, sizeof(UObject*));
		}
	}
	else if (Ar.IsCooking() && !HasAnyFlags(RF_ClassDefaultObject))
	{
		if (!Ar.CookingTarget()->SupportsFeature(ETargetPlatformFeatures::DeferredRendering))
		{
			// These are used for SM5 rendering
			UTexture2D* BackupXYOffsetmapTexture = nullptr;
			TArray<TObjectPtr<UMaterialInstanceConstant>> BackupMaterialInstances;
			TArray<TObjectPtr<UTexture2D>> BackupWeightmapTextures;

			Exchange(BackupXYOffsetmapTexture, XYOffsetmapTexture);
			Exchange(BackupMaterialInstances, MaterialInstances);
			Exchange(BackupWeightmapTextures, WeightmapTextures);

			Super::Serialize(Ar);

			Exchange(BackupXYOffsetmapTexture, XYOffsetmapTexture);
			Exchange(BackupMaterialInstances, MaterialInstances);
			Exchange(BackupWeightmapTextures, WeightmapTextures);
		}
		else if (!Ar.CookingTarget()->SupportsFeature(ETargetPlatformFeatures::MobileRendering))
		{
			// These properties are only for Mobile
			TArray<TObjectPtr<UMaterialInterface>> BackupMobileMaterialInterfaces;
			TArray<TObjectPtr<UTexture2D>> BackupMobileWeightmapTextures;

			Exchange(MobileMaterialInterfaces, BackupMobileMaterialInterfaces);
			Exchange(MobileWeightmapTextures, BackupMobileWeightmapTextures);

			Super::Serialize(Ar);

			Exchange(MobileMaterialInterfaces, BackupMobileMaterialInterfaces);
			Exchange(MobileWeightmapTextures, BackupMobileWeightmapTextures);
		}
		else
		{
			// Serialize both mobile and SM5 properties
			Super::Serialize(Ar);
		}
	}
	else
#endif // WITH_EDITOR
	{
		Super::Serialize(Ar);
	}

	if (Ar.IsLoading() && Ar.CustomVer(FRenderingObjectVersion::GUID) < FRenderingObjectVersion::MapBuildDataSeparatePackage)
	{
		FMeshMapBuildData* LegacyMapBuildData = new FMeshMapBuildData();
		Ar << LegacyMapBuildData->LightMap;
		Ar << LegacyMapBuildData->ShadowMap;

#if WITH_EDITORONLY_DATA
		LegacyMapBuildData->IrrelevantLights = IrrelevantLights_DEPRECATED;
#endif // WITH_EDITORONLY_DATA

		FMeshMapBuildLegacyData LegacyComponentData;
		LegacyComponentData.Data.Emplace(MapBuildDataId, LegacyMapBuildData);
		GComponentsWithLegacyLightmaps.AddAnnotation(this, MoveTemp(LegacyComponentData));
	}

#if WITH_EDITORONLY_DATA
	if (Ar.IsLoading() && Ar.CustomVer(FFortniteMainBranchObjectVersion::GUID) < FFortniteMainBranchObjectVersion::NewLandscapeMaterialPerLOD)
	{
		if (MobileMaterialInterface_DEPRECATED != nullptr)
		{
			MobileMaterialInterfaces.AddUnique(MobileMaterialInterface_DEPRECATED);
		}

		if (MobileCombinationMaterialInstance_DEPRECATED != nullptr)
		{
			MobileCombinationMaterialInstances.AddUnique(MobileCombinationMaterialInstance_DEPRECATED);
		}
	}
#endif // WITH_EDITORONLY_DATA

	if (Ar.UEVer() >= VER_UE4_SERIALIZE_LANDSCAPE_GRASS_DATA)
	{
		// Share the shared ref so PIE can share this data
		if (Ar.GetPortFlags() & PPF_DuplicateForPIE)
		{
			if (Ar.IsSaving())
			{
				PTRINT GrassDataPointer = (PTRINT)&GrassData;
				Ar << GrassDataPointer;
			}
			else
			{
				PTRINT GrassDataPointer;
				Ar << GrassDataPointer;
				// Duplicate shared reference
				GrassData = *(TSharedRef<FLandscapeComponentGrassData, ESPMode::ThreadSafe>*)GrassDataPointer;
			}
		}
		else
		{
			Ar << GrassData.Get();
		}

		// When loading or saving a component, validate that grass data is valid : 
		checkf(IsTemplate() || !Ar.IsLoading() || !Ar.IsSaving() || GrassData->HasValidData(), TEXT("If this asserts, then serialization occurred on grass data that wasn't properly loaded/computed. It's a problem"));
	}

#if WITH_EDITOR
	if (Ar.IsTransacting())
	{
		Ar << EditToolRenderData.SelectedType;
	}
#endif

	bool bCooked = false;

	if (Ar.UEVer() >= VER_UE4_LANDSCAPE_PLATFORMDATA_COOKING && !HasAnyFlags(RF_ClassDefaultObject))
	{
		bCooked = Ar.IsCooking() || (FPlatformProperties::RequiresCookedData() && Ar.IsSaving());
		// This is needed when loading cooked data, to know to serialize differently
		Ar << bCooked;
	}

	if (FPlatformProperties::RequiresCookedData() && !bCooked && Ar.IsLoading())
	{
		UE_LOG(LogLandscape, Fatal, TEXT("This platform requires cooked packages, and this landscape does not contain cooked data %s."), *GetName());
	}

#if WITH_EDITOR
	if (Ar.IsSaving() && Ar.IsPersistent())
	{
		//Update the last saved Hash for physical material
		LastSavedPhysicalMaterialHash = PhysicalMaterialHash;
	}
#endif
}

void ULandscapeComponent::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);

	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(GrassData->GetAllocatedSize());
}

UMaterialInterface* ULandscapeComponent::GetLandscapeMaterial(int8 InLODIndex) const
{
	if (InLODIndex != INDEX_NONE)
	{
		UWorld* World = GetWorld();

		if (World != nullptr)
		{
			if (const FLandscapePerLODMaterialOverride* LocalMaterialOverride = PerLODOverrideMaterials.FindByPredicate(
				[InLODIndex](const FLandscapePerLODMaterialOverride& InOverride) { return (InOverride.LODIndex == InLODIndex) && (InOverride.Material != nullptr); }))
			{
				return LocalMaterialOverride->Material;
			}
		}
	}

	if (OverrideMaterial != nullptr)
	{
		return OverrideMaterial;
	}

	ALandscapeProxy* Proxy = GetLandscapeProxy();
	if (Proxy)
	{
		return Proxy->GetLandscapeMaterial(InLODIndex);
	}

	return UMaterial::GetDefaultMaterial(MD_Surface);
}

UMaterialInterface* ULandscapeComponent::GetLandscapeHoleMaterial() const
{
	if (OverrideHoleMaterial)
	{
		return OverrideHoleMaterial;
	}
	ALandscapeProxy* Proxy = GetLandscapeProxy();
	if (Proxy)
	{
		return Proxy->GetLandscapeHoleMaterial();
	}
	return nullptr;
}

#if WITH_EDITOR
bool ULandscapeComponent::IsLandscapeHoleMaterialValid() const
{
	UMaterialInterface* HoleMaterial = GetLandscapeHoleMaterial();
	if (!HoleMaterial)
	{
		HoleMaterial = GetLandscapeMaterial();
	}

	return HoleMaterial ? HoleMaterial->GetMaterial()->HasAnyExpressionsInMaterialAndFunctionsOfType<UMaterialExpressionLandscapeVisibilityMask>() : false;
}

bool ULandscapeComponent::ComponentHasVisibilityPainted() const
{
	for (const FWeightmapLayerAllocationInfo& Allocation : WeightmapLayerAllocations)
	{
		if (Allocation.LayerInfo == ALandscapeProxy::VisibilityLayer)
		{
			return true;
		}
	}

	return false;
}

ULandscapeLayerInfoObject* ULandscapeComponent::GetVisibilityLayer() const
{
	for (const FWeightmapLayerAllocationInfo& Allocation : WeightmapLayerAllocations)
	{
		if (Allocation.LayerInfo == ALandscapeProxy::VisibilityLayer)
		{
			return Allocation.LayerInfo;
		}
	}

	return nullptr;
}

void ULandscapeComponent::GetLayerDebugColorKey(int32& R, int32& G, int32& B) const
{
	ULandscapeInfo* Info = GetLandscapeInfo();
	if (Info != nullptr)
	{
		R = INDEX_NONE, G = INDEX_NONE, B = INDEX_NONE;

		for (auto It = Info->Layers.CreateConstIterator(); It; It++)
		{
			const FLandscapeInfoLayerSettings& LayerStruct = *It;
			if (LayerStruct.DebugColorChannel > 0
				&& LayerStruct.LayerInfoObj)
			{
				const TArray<FWeightmapLayerAllocationInfo>& ComponentWeightmapLayerAllocations = GetWeightmapLayerAllocations();

				for (int32 LayerIdx = 0; LayerIdx < ComponentWeightmapLayerAllocations.Num(); LayerIdx++)
				{
					if (ComponentWeightmapLayerAllocations[LayerIdx].LayerInfo == LayerStruct.LayerInfoObj)
					{
						if (LayerStruct.DebugColorChannel & 1) // R
						{
							R = (ComponentWeightmapLayerAllocations[LayerIdx].WeightmapTextureIndex * 4 + ComponentWeightmapLayerAllocations[LayerIdx].WeightmapTextureChannel);
						}
						if (LayerStruct.DebugColorChannel & 2) // G
						{
							G = (ComponentWeightmapLayerAllocations[LayerIdx].WeightmapTextureIndex * 4 + ComponentWeightmapLayerAllocations[LayerIdx].WeightmapTextureChannel);
						}
						if (LayerStruct.DebugColorChannel & 4) // B
						{
							B = (ComponentWeightmapLayerAllocations[LayerIdx].WeightmapTextureIndex * 4 + ComponentWeightmapLayerAllocations[LayerIdx].WeightmapTextureChannel);
						}
						break;
					}
				}
			}
		}
	}
}
#endif	//WITH_EDITOR

ULandscapeInfo::ULandscapeInfo(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
#if WITH_EDITORONLY_DATA
	, bDirtyOnlyInMode(false)
#endif
	, XYComponentBounds(MAX_int32, MAX_int32, MIN_int32, MIN_int32)
{
}

#if WITH_EDITOR
void ULandscapeInfo::UpdateDebugColorMaterial()
{
	FlushRenderingCommands();
	//GWarn->BeginSlowTask( *FString::Printf(TEXT("Compiling layer color combinations for %s"), *GetName()), true);

	for (auto It = XYtoComponentMap.CreateIterator(); It; ++It)
	{
		ULandscapeComponent* Comp = It.Value();
		if (Comp)
		{
			Comp->EditToolRenderData.UpdateDebugColorMaterial(Comp);
			Comp->UpdateEditToolRenderData();
		}
	}
	FlushRenderingCommands();
	//GWarn->EndSlowTask();
}
#endif // WITH_EDITOR

void ULandscapeComponent::UpdatedSharedPropertiesFromActor()
{
	ALandscapeProxy* LandscapeProxy = GetLandscapeProxy();

	CastShadow = LandscapeProxy->CastShadow;
	bCastDynamicShadow = LandscapeProxy->bCastDynamicShadow;
	bCastStaticShadow = LandscapeProxy->bCastStaticShadow;
	bCastContactShadow = LandscapeProxy->bCastContactShadow;
	bCastFarShadow = LandscapeProxy->bCastFarShadow;
	bCastHiddenShadow = LandscapeProxy->bCastHiddenShadow;
	bCastShadowAsTwoSided = LandscapeProxy->bCastShadowAsTwoSided;
	bAffectDistanceFieldLighting = LandscapeProxy->bAffectDistanceFieldLighting;
	bRenderCustomDepth = LandscapeProxy->bRenderCustomDepth;
	CustomDepthStencilWriteMask = LandscapeProxy->CustomDepthStencilWriteMask;
	CustomDepthStencilValue = LandscapeProxy->CustomDepthStencilValue;
	SetCullDistance(LandscapeProxy->LDMaxDrawDistance);
	LightingChannels = LandscapeProxy->LightingChannels;
	ShadowCacheInvalidationBehavior = LandscapeProxy->ShadowCacheInvalidationBehavior;

	UpdateNavigationRelevance();
	UpdateRejectNavmeshUnderneath();
}

void ULandscapeComponent::PostLoad()
{
	Super::PostLoad();

	if (IsComponentPSOPrecachingEnabled())
	{
		TArray<UMaterialInterface*> Materials;
		bool bGetDebugMaterials = false;
		GetUsedMaterials(Materials, bGetDebugMaterials);

		FPSOPrecacheParams PrecachePSOParams;
		SetupPrecachePSOParams(PrecachePSOParams);

		FPSOPrecacheVertexFactoryDataList VertexFactoryDataList;
		if (!XYOffsetmapTexture)
		{
			VertexFactoryDataList.Add(FPSOPrecacheVertexFactoryData(&FLandscapeVertexFactory::StaticType));
		}
		else
		{
			VertexFactoryDataList.Add(FPSOPrecacheVertexFactoryData(&FLandscapeXYOffsetVertexFactory::StaticType));
		}

		// we need the fixed grid vertex factory for both virtual texturing and grass
		ERHIFeatureLevel::Type FeatureLevel = GMaxRHIFeatureLevel;
		bool bNeedsFixedGridVertexFactory = UseVirtualTexturing(FeatureLevel);
		if (bNeedsFixedGridVertexFactory)
		{
			VertexFactoryDataList.Add(FPSOPrecacheVertexFactoryData(&FLandscapeFixedGridVertexFactory::StaticType));
		}

		using namespace UE::Landscape;
		
		if (Culling::UseCulling(GMaxRHIShaderPlatform))
		{
			VertexFactoryDataList.Add(FPSOPrecacheVertexFactoryData(Culling::GetTileVertexFactoryType()));
		}

		TArray<FMaterialPSOPrecacheRequestID> MaterialPrecacheRequestIDs;
		for (UMaterialInterface* MaterialInterface : Materials)
		{
			if (MaterialInterface)
			{
				MaterialInterface->PrecachePSOs(VertexFactoryDataList, PrecachePSOParams, EPSOPrecachePriority::High, MaterialPrecacheRequestIDs);
			}
		}
	}

#if WITH_EDITOR
	ALandscapeProxy* LandscapeProxy = GetLandscapeProxy();
	if (ensure(LandscapeProxy))
	{
		// Ensure that the component's lighting settings matches the actor's.
		UpdatedSharedPropertiesFromActor();

		// check SectionBaseX/Y are correct
		const FVector LocalRelativeLocation = GetRelativeLocation();
		int32 CheckSectionBaseX = FMath::RoundToInt32(LocalRelativeLocation.X) + LandscapeProxy->LandscapeSectionOffset.X;
		int32 CheckSectionBaseY = FMath::RoundToInt32(LocalRelativeLocation.Y) + LandscapeProxy->LandscapeSectionOffset.Y;
		if (CheckSectionBaseX != SectionBaseX ||
			CheckSectionBaseY != SectionBaseY)
		{
			UE_LOG(LogLandscape, Warning, TEXT("LandscapeComponent SectionBaseX disagrees with its location, attempted automated fix: '%s', %d,%d vs %d,%d."),
				*GetFullName(), SectionBaseX, SectionBaseY, CheckSectionBaseX, CheckSectionBaseY);
			SectionBaseX = CheckSectionBaseX;
			SectionBaseY = CheckSectionBaseY;
		}
	}

	if (GIsEditor && !HasAnyFlags(RF_ClassDefaultObject))
	{
		// This is to ensure that component relative location is exact section base offset value
		FVector LocalRelativeLocation = GetRelativeLocation();
		float CheckRelativeLocationX = float(SectionBaseX - LandscapeProxy->LandscapeSectionOffset.X);
		float CheckRelativeLocationY = float(SectionBaseY - LandscapeProxy->LandscapeSectionOffset.Y);
		if (!FMath::IsNearlyEqual(CheckRelativeLocationX, LocalRelativeLocation.X, UE_DOUBLE_KINDA_SMALL_NUMBER) ||
			!FMath::IsNearlyEqual(CheckRelativeLocationY, LocalRelativeLocation.Y, UE_DOUBLE_KINDA_SMALL_NUMBER))
		{
			UE_LOG(LogLandscape, Warning, TEXT("LandscapeComponent RelativeLocation disagrees with its section base, attempted automated fix: '%s', %f,%f vs %f,%f."),
				*GetFullName(), LocalRelativeLocation.X, LocalRelativeLocation.Y, CheckRelativeLocationX, CheckRelativeLocationY);
			LocalRelativeLocation.X = CheckRelativeLocationX;
			LocalRelativeLocation.Y = CheckRelativeLocationY;

			SetRelativeLocation_Direct(LocalRelativeLocation);
		}

		// Remove standalone flags from data textures to ensure data is unloaded in the editor when reverting an unsaved level.
		// Previous version of landscape set these flags on creation.
		if (HeightmapTexture && HeightmapTexture->HasAnyFlags(RF_Standalone))
		{
			HeightmapTexture->ClearFlags(RF_Standalone);
		}
		for (int32 Idx = 0; Idx < WeightmapTextures.Num(); Idx++)
		{
			if (WeightmapTextures[Idx] && WeightmapTextures[Idx]->HasAnyFlags(RF_Standalone))
			{
				WeightmapTextures[Idx]->ClearFlags(RF_Standalone);
			}
		}

		LastSavedPhysicalMaterialHash = PhysicalMaterialHash;

		PRAGMA_DISABLE_DEPRECATION_WARNINGS;
		if (!OverrideMaterials_DEPRECATED.IsEmpty())
		{
			PerLODOverrideMaterials.Reserve(OverrideMaterials_DEPRECATED.Num());
			for (const FLandscapeComponentMaterialOverride& LocalMaterialOverride : OverrideMaterials_DEPRECATED)
			{
				PerLODOverrideMaterials.Add({ LocalMaterialOverride.LODIndex.Default, LocalMaterialOverride.Material });
			}
			OverrideMaterials_DEPRECATED.Reset();
		}
		PRAGMA_ENABLE_DEPRECATION_WARNINGS;
	}

#if WITH_EDITORONLY_DATA
	// Handle old MaterialInstance
	if (MaterialInstance_DEPRECATED)
	{
		MaterialInstances.Empty(1);
		MaterialInstances.Add(MaterialInstance_DEPRECATED);
		MaterialInstance_DEPRECATED = nullptr;

		if (GIsEditor && MaterialInstances.Num() > 0 && MaterialInstances[0] != nullptr)
		{
			MaterialInstances[0]->ConditionalPostLoad();
			UpdateMaterialInstances();
		}
	}

	auto DropMipChain = [](UTexture2D* InTexture) 
	{
		if (InTexture->Source.GetNumMips() <= 1)
		{
			return;
		}

		TArray64<uint8> TopMipData;
		InTexture->Source.GetMipData(TopMipData, 0);

		InTexture->PreEditChange(nullptr);
		InTexture->Source.Init(InTexture->Source.GetSizeX(), InTexture->Source.GetSizeY(), 1, 1, InTexture->Source.GetFormat(), TopMipData.GetData());
		InTexture->UpdateResource();

		InTexture->PostEditChange();
	};

	// Remove Non zero mip levels found in layer textures
	for (auto& LayerIt : LayersData)
	{
		DropMipChain(LayerIt.Value.HeightmapData.Texture);
		for (int32 i = 0; i < LayerIt.Value.WeightmapData.Textures.Num(); ++i)
		{
			DropMipChain(LayerIt.Value.WeightmapData.Textures[i]);
		}
	}
	
#endif

	auto ReparentObject = [this](UObject* Object)
	{
		if (Object && !Object->HasAllFlags(RF_Public | RF_Standalone) && (Object->GetOuter() != GetOuter()) && (Object->GetOutermost() == GetOutermost()))
		{
			Object->Rename(nullptr, GetOuter(), REN_ForceNoResetLoaders);
			return true;
		}
		return false;
	};

	ReparentObject(HeightmapTexture);
	ReparentObject(XYOffsetmapTexture);

	for (UTexture2D* WeightmapTexture : WeightmapTextures)
	{
		ReparentObject(WeightmapTexture);
	}

	for (UTexture2D* MobileWeightmapTexture : MobileWeightmapTextures)
	{
		ReparentObject(MobileWeightmapTexture);
	}

	for (auto& ItPair : LayersData)
	{
		FLandscapeLayerComponentData& LayerComponentData = ItPair.Value;
		ReparentObject(LayerComponentData.HeightmapData.Texture);
		for (UTexture2D* WeightmapTexture : LayerComponentData.WeightmapData.Textures)
		{
			ReparentObject(WeightmapTexture);
		}

		// Fixup missing/mismatching edit layer names :
		if (const FLandscapeLayer* EditLayer = GetLandscapeActor() ? GetLandscapeActor()->GetLayer(ItPair.Key) : nullptr)
		{
			if (LayerComponentData.DebugName != EditLayer->Name)
			{
				LayerComponentData.DebugName = EditLayer->Name;
			}
		}
	}

	for (UMaterialInstance* MaterialInstance : MaterialInstances)
	{
		ULandscapeMaterialInstanceConstant* CurrentMIC = Cast<ULandscapeMaterialInstanceConstant>(MaterialInstance);
		while (ReparentObject(CurrentMIC))
		{
			CurrentMIC = Cast<ULandscapeMaterialInstanceConstant>(MaterialInstance->Parent);
		}
	}

	for (UMaterialInterface* MobileMaterialInterface : MobileMaterialInterfaces)
	{
		while (ReparentObject(MobileMaterialInterface))
		{
			MobileMaterialInterface = Cast<UMaterialInstance>(MobileMaterialInterface) ? Cast<UMaterialInstance>(((UMaterialInstance*)MobileMaterialInterface)->Parent) : nullptr;
		}
	}

 	for (UMaterialInstance* MobileCombinationMaterialInstance : MobileCombinationMaterialInstances)
	{
		while (ReparentObject(MobileCombinationMaterialInstance))
		{
			MobileCombinationMaterialInstance = Cast<UMaterialInstance>(MobileCombinationMaterialInstance->Parent);
		}
	}

#if !UE_BUILD_SHIPPING
	// This will fix the data in case there is mismatch between save of asset/maps
	const int8 MaxLOD = static_cast<int8>(FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);

	TArray<ULandscapeMaterialInstanceConstant*> ResolvedMaterials;

	if (LODIndexToMaterialIndex.Num() != MaxLOD+1)
	{
		if (GIsEditor)
		{
			UpdateMaterialInstances();
		}
		else
		{
			// Correct in-place differences by applying the highest LOD value we have to the newly added items as most case will be missing items added at the end
			LODIndexToMaterialIndex.SetNumZeroed(MaxLOD + 1);

			int8 LastLODIndex = 0;

			for (int32 i = 0; i < LODIndexToMaterialIndex.Num(); ++i)
			{
				if (LODIndexToMaterialIndex[i] > LastLODIndex)
				{
					LastLODIndex = LODIndexToMaterialIndex[i];
				}

				if (LODIndexToMaterialIndex[i] == 0 && LastLODIndex != 0)
				{
					LODIndexToMaterialIndex[i] = LastLODIndex;
				}
			}
		}
	}
#endif // UE_BUILD_SHIPPING

	if (GIsEditor && !HasAnyFlags(RF_ClassDefaultObject))
	{
		// Move the MICs and Textures back to the Package if they're currently in the level
		// Moving them into the level caused them to be duplicated when running PIE, which is *very very slow*, so we've reverted that change
		// Also clear the public flag to avoid various issues, e.g. generating and saving thumbnails that can never be seen
		if (ULevel* Level = GetLevel())
		{
			TArray<UObject*> ObjectsToMoveFromLevelToPackage;
			GetGeneratedTexturesAndMaterialInstances(ObjectsToMoveFromLevelToPackage);

			UPackage* MyPackage = GetOutermost();
			for (auto* Obj : ObjectsToMoveFromLevelToPackage)
			{
				Obj->ClearFlags(RF_Public);
				if (Obj->GetOuter() == Level)
				{
					Obj->Rename(nullptr, MyPackage, REN_DoNotDirty | REN_DontCreateRedirectors | REN_ForceNoResetLoaders | REN_NonTransactional);
				}
			}
		}
	}

#if !UE_BUILD_SHIPPING
	if (MobileCombinationMaterialInstances.Num() == 0)
	{
		if (GIsEditor)
		{
			UpdateMaterialInstances();
		}
		else
		{
			UE_LOG(LogLandscape, Error, TEXT("Landscape component (%d, %d) Does not have a valid mobile combination material. To correct this issue, open the map in the editor and resave the map."), SectionBaseX, SectionBaseY);
		}
	}
#endif // UE_BUILD_SHIPPING


	// May have been saved without mobile layer allocations, but those are serialized now
	if (MobileWeightmapLayerAllocations.Num() == 0)
	{
		GenerateMobileWeightmapLayerAllocations();
	}

	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		FSceneInterface* SceneInterface = GetScene();
		ERHIFeatureLevel::Type FeatureLevel = ((GEngine->GetDefaultWorldFeatureLevel() == ERHIFeatureLevel::ES3_1) || (SceneInterface && (SceneInterface->GetFeatureLevel() <= ERHIFeatureLevel::ES3_1)))
			? ERHIFeatureLevel::ES3_1 : GMaxRHIFeatureLevel;

		// If we're loading on a platform that doesn't require cooked data, but defaults to a mobile feature level, generate or preload data from the DDC
		if (!FPlatformProperties::RequiresCookedData() && FeatureLevel == ERHIFeatureLevel::ES3_1)
		{
			CheckGenerateMobilePlatformData(/*bIsCooking = */ false, /*TargetPlatform = */ nullptr);
		}
	}

	GrassData->ConditionalDiscardDataOnLoad();

#if WITH_EDITORONLY_DATA
	// If the Collision Component is not set yet and we're transferring the property from the lazy object pointer it was previously stored as to the soft object ptr it is now stored as :
	if (!CollisionComponentRef && CollisionComponent_DEPRECATED.IsValid())
	{
		CollisionComponentRef = CollisionComponent_DEPRECATED.Get();
		CollisionComponent_DEPRECATED = nullptr;
	}
#endif // !WITH_EDITORONLY_DATA

#endif // WITH_EDITOR
}

#if WITH_EDITORONLY_DATA
void ULandscapeComponent::DeclareConstructClasses(TArray<FTopLevelAssetPath>& OutConstructClasses, const UClass* SpecificSubclass)
{
	Super::DeclareConstructClasses(OutConstructClasses, SpecificSubclass);
	OutConstructClasses.Add(FTopLevelAssetPath(ULandscapeMaterialInstanceConstant::StaticClass()));
}
#endif

void ULandscapeComponent::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	Super::AddReferencedObjects(InThis, Collector);

	ThisClass* const TypedThis = Cast<ThisClass>(InThis);
	Collector.AddReferencedObjects(TypedThis->GrassData->WeightOffsets, TypedThis);
}

#if WITH_EDITORONLY_DATA
TArray<ALandscapeProxy*> ALandscapeProxy::LandscapeProxies;
#endif

ALandscapeProxy::ALandscapeProxy(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
#if WITH_EDITORONLY_DATA
	, TargetDisplayOrder(ELandscapeLayerDisplayMode::Default)
#endif // WITH_EDITORONLY_DATA
#if !WITH_EDITORONLY_DATA
	, LandscapeMaterialCached(nullptr)
	, LandscapeGrassTypes()
	, GrassMaxDiscardDistance(0.0f)
#endif
	, bHasLandscapeGrass(true)
{
	bReplicates = false;
	NetUpdateFrequency = 10.0f;
	SetHidden(false);
	SetReplicatingMovement(false);
	SetCanBeDamaged(false);

	CastShadow = true;
	bCastDynamicShadow = true;
	bCastStaticShadow = true;
	bCastContactShadow = true;
	bCastFarShadow = true;
	bCastHiddenShadow = false;
	bCastShadowAsTwoSided = false;
	bAffectDistanceFieldLighting = true;

	RootComponent->SetRelativeScale3D(FVector(128.0f, 128.0f, 256.0f)); // Old default scale, preserved for compatibility. See ULandscapeEditorObject::NewLandscape_Scale
	RootComponent->Mobility = EComponentMobility::Static;
	LandscapeSectionOffset = FIntPoint::ZeroValue;

	StaticLightingResolution = 1.0f;
	StreamingDistanceMultiplier = 1.0f;
	MaxLODLevel = -1;
	bUseDynamicMaterialInstance = false;
#if WITH_EDITORONLY_DATA
	bLockLocation = true;
#endif // WITH_EDITORONLY_DATA
	ComponentScreenSizeToUseSubSections = 0.65f;
	LOD0ScreenSize = 0.5f;
	LOD0DistributionSetting = 1.25f;
	LODDistributionSetting = 3.0f;
	bCastStaticShadow = true;
	ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Auto;
	bUsedForNavigation = true;
	bFillCollisionUnderLandscapeForNavmesh = false;
	BodyInstance.SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	bGenerateOverlapEvents = false;
#if WITH_EDITORONLY_DATA
	MaxPaintedLayersPerComponent = 0;
	bHasLayersContent = false;
#endif

#if WITH_EDITOR
	NumComponentsNeedingGrassMapRender = 0;
	NumTexturesToStreamForVisibleGrassMapRender = 0;

	if (VisibilityLayer == nullptr)
	{
		// Structure to hold one-time initialization
		struct FConstructorStatics
		{
			ConstructorHelpers::FObjectFinderOptional<ULandscapeLayerInfoObject> DataLayer;
			FConstructorStatics()
				: DataLayer(TEXT("LandscapeLayerInfoObject'/Engine/EditorLandscapeResources/DataLayer.DataLayer'"))
			{
			}
		};
		static FConstructorStatics ConstructorStatics;

		VisibilityLayer = ConstructorStatics.DataLayer.Get();
		check(VisibilityLayer);
#if WITH_EDITORONLY_DATA
		// This layer should be no weight blending
		VisibilityLayer->bNoWeightBlend = true;
#endif
		VisibilityLayer->LayerName = UMaterialExpressionLandscapeVisibilityMask::ParameterName;
		VisibilityLayer->LayerUsageDebugColor = FLinearColor(0, 0, 0, 0);
		VisibilityLayer->AddToRoot();
	}

	if (!HasAnyFlags(RF_ArchetypeObject | RF_ClassDefaultObject) && GetWorld() != nullptr)
	{
		FOnFeatureLevelChanged::FDelegate FeatureLevelChangedDelegate = FOnFeatureLevelChanged::FDelegate::CreateUObject(this, &ALandscapeProxy::OnFeatureLevelChanged);
		FeatureLevelChangedDelegateHandle = GetWorld()->AddOnFeatureLevelChangedHandler(FeatureLevelChangedDelegate);
	}
#endif

	static uint32 FrameOffsetForTickIntervalInc = 0;
	FrameOffsetForTickInterval = FrameOffsetForTickIntervalInc++;

#if WITH_EDITORONLY_DATA
	LandscapeProxies.Add(this);
#endif
}

#if WITH_EDITORONLY_DATA
ALandscape::FLandscapeEdModeInfo::FLandscapeEdModeInfo()
	: ViewMode(ELandscapeViewMode::Invalid)
	, ToolTarget(ELandscapeToolTargetType::Invalid)
{
}
#endif

ALandscape::ALandscape(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
#if WITH_EDITORONLY_DATA
	bLockLocation = false;
	WasCompilingShaders = false;
	LayerContentUpdateModes = 0;
	bSplineLayerUpdateRequested = false;
	CombinedLayersWeightmapAllMaterialLayersResource = nullptr;
	CurrentLayersWeightmapAllMaterialLayersResource = nullptr;
	WeightmapScratchExtractLayerTextureResource = nullptr;
	WeightmapScratchPackLayerTextureResource = nullptr;
	bLandscapeLayersAreInitialized = false;
	bLandscapeLayersAreUsingLocalMerge = false;
	LandscapeEdMode = nullptr;
	bGrassUpdateEnabled = true;
	bIsSpatiallyLoaded = false;
	bDefaultOutlinerExpansionState = false;
#endif // WITH_EDITORONLY_DATA
}

ALandscapeStreamingProxy::ALandscapeStreamingProxy(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
#if WITH_EDITORONLY_DATA
	bLockLocation = true;
#endif // WITH_EDITORONLY_DATA
}

const ALandscape* ALandscape::GetLandscapeActor() const
{
	return this;
}

ALandscape* ALandscape::GetLandscapeActor()
{
	return this;
}

const ALandscape* ALandscapeStreamingProxy::GetLandscapeActor() const
{
	return LandscapeActorRef.Get();
}

ALandscape* ALandscapeStreamingProxy::GetLandscapeActor()
{
	return LandscapeActorRef.Get();
}

void ALandscapeStreamingProxy::SetLandscapeActor(ALandscape* InLandscape)
{
	LandscapeActorRef = InLandscape;
}

void ALandscape::SetLODGroupKey(uint32 InLODGroupKey)
{
	SetLODGroupKeyInternal(InLODGroupKey);

	// change LODGroupKey on any proxies that are currently registered
	// (any proxies that get registered later will copy the value on registration)
	if (ULandscapeInfo* Info = GetLandscapeInfo())
	{
		Info->ForEachLandscapeProxy([InLODGroupKey](ALandscapeProxy* Proxy)
			{
				Proxy->SetLODGroupKeyInternal(InLODGroupKey);
				return true;
			});
	}
}

void ALandscapeProxy::SetLODGroupKeyInternal(uint32 InLODGroupKey)
{
	if (LODGroupKey != InLODGroupKey)
	{
		LODGroupKey = InLODGroupKey;
		MarkComponentsRenderStateDirty();
	}
}

uint32 ALandscape::GetLODGroupKey()
{
	return LODGroupKey;
}

void ALandscape::MarkAllLandscapeRenderStateDirty()
{
	if (ULandscapeInfo* Info = GetLandscapeInfo())
	{
		Info->ForEachLandscapeProxy([](ALandscapeProxy* Proxy)
			{
				Proxy->MarkComponentsRenderStateDirty();
				return true;
			});
	}
}

ULandscapeInfo* ALandscapeProxy::CreateLandscapeInfo(bool bMapCheck, bool bUpdateAllAddCollisions)
{
	ULandscapeInfo* LandscapeInfo = ULandscapeInfo::FindOrCreate(GetWorld(), LandscapeGuid);
	LandscapeInfo->RegisterActor(this, bMapCheck, bUpdateAllAddCollisions);
	return LandscapeInfo;
}

ULandscapeInfo* ALandscapeProxy::GetLandscapeInfo() const
{
	return ULandscapeInfo::Find(GetWorld(), LandscapeGuid);
}

FTransform ALandscapeProxy::LandscapeActorToWorld() const
{
	FTransform TM = ActorToWorld();
	// Add this proxy landscape section offset to obtain landscape actor transform
	TM.AddToTranslation(TM.TransformVector(-FVector(LandscapeSectionOffset)));
	return TM;
}

void ALandscapeProxy::UpdateSharedProperties(ULandscapeInfo* InLandscapeInfo)
{
	check(LandscapeGuid == InLandscapeInfo->LandscapeGuid);
}

static TArray<float> GetLODScreenSizeArray(const ALandscapeProxy* InLandscapeProxy, const int32 InNumLODLevels)
{
	static TConsoleVariableData<float>* CVarSMLODDistanceScale = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.StaticMeshLODDistanceScale"));
	static IConsoleVariable* CVarLSLOD0DistributionScale = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LandscapeLOD0DistributionScale"));
	float CurrentScreenSize = InLandscapeProxy->LOD0ScreenSize / CVarSMLODDistanceScale->GetValueOnGameThread();
	const float ScreenSizeMult = 1.f / FMath::Max(InLandscapeProxy->LOD0DistributionSetting * CVarLSLOD0DistributionScale->GetFloat(), 1.01f);

	TArray<float> Result;
	Result.Empty(InNumLODLevels);
	for (int32 Idx = 0; Idx < InNumLODLevels; ++Idx)
	{
		Result.Add(CurrentScreenSize);
		CurrentScreenSize *= ScreenSizeMult;
	}
	return Result;
}

TArray<float> ALandscapeProxy::GetLODScreenSizeArray() const
{
	const int32 MaxPossibleLOD = FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1;
	const int32 MaxLOD = MaxLODLevel != INDEX_NONE ? FMath::Min<int32>(MaxLODLevel, MaxPossibleLOD) : MaxPossibleLOD;

	const int32 NumLODLevels = MaxLOD + 1;
	return ::GetLODScreenSizeArray(this, NumLODLevels);
}


ALandscape* ULandscapeComponent::GetLandscapeActor() const
{
	ALandscapeProxy* Landscape = GetLandscapeProxy();
	if (Landscape)
	{
		return Landscape->GetLandscapeActor();
	}
	return nullptr;
}

ULevel* ULandscapeComponent::GetLevel() const
{
	AActor* MyOwner = GetOwner();
	return MyOwner ? MyOwner->GetLevel() : nullptr;
}

#if WITH_EDITOR
TArray<UTexture*> ULandscapeComponent::GetGeneratedTextures() const
{
	TArray<UTexture*> OutTextures;
	if (HeightmapTexture)
	{
		OutTextures.Add(HeightmapTexture);
	}

	for (const auto& ItPair : LayersData)
	{
		const FLandscapeLayerComponentData& LayerComponentData = ItPair.Value;

		OutTextures.Add(LayerComponentData.HeightmapData.Texture);
		OutTextures.Append(LayerComponentData.WeightmapData.Textures);
	}

	OutTextures.Append(WeightmapTextures);

	if (XYOffsetmapTexture)
	{
		OutTextures.Add(XYOffsetmapTexture);
	}

	TArray<UMaterialInstance*> OutMaterials;
	for (UMaterialInstance* MaterialInstance : MaterialInstances)
	{
		for (ULandscapeMaterialInstanceConstant* CurrentMIC = Cast<ULandscapeMaterialInstanceConstant>(MaterialInstance); CurrentMIC; CurrentMIC = Cast<ULandscapeMaterialInstanceConstant>(CurrentMIC->Parent))
		{
			// Sometimes weight map is not registered in the WeightmapTextures, so
			// we need to get it from here.
			FTextureParameterValue* WeightmapPtr = CurrentMIC->TextureParameterValues.FindByPredicate(
				[](const FTextureParameterValue& ParamValue)
			{
				static const FName WeightmapParamName("Weightmap0");
				return ParamValue.ParameterInfo.Name == WeightmapParamName;
			});

			if (WeightmapPtr != nullptr)
			{
				OutTextures.AddUnique(WeightmapPtr->ParameterValue);
			}
		}
	}

	OutTextures.Remove(nullptr);

	return OutTextures;
}

TArray<UMaterialInstance*> ULandscapeComponent::GetGeneratedMaterialInstances() const
{
	TArray<UMaterialInstance*> OutMaterials;
	for (UMaterialInstance* MaterialInstance : MaterialInstances)
	{
		for (ULandscapeMaterialInstanceConstant* CurrentMIC = Cast<ULandscapeMaterialInstanceConstant>(MaterialInstance); CurrentMIC; CurrentMIC = Cast<ULandscapeMaterialInstanceConstant>(CurrentMIC->Parent))
		{
			OutMaterials.Add(CurrentMIC);
		}
	}

	for (UMaterialInstanceConstant* MaterialInstance : MobileCombinationMaterialInstances)
	{
		for (ULandscapeMaterialInstanceConstant* CurrentMIC = Cast<ULandscapeMaterialInstanceConstant>(MaterialInstance); CurrentMIC; CurrentMIC = Cast<ULandscapeMaterialInstanceConstant>(CurrentMIC->Parent))
		{
			OutMaterials.Add(CurrentMIC);
		}
	}

	return OutMaterials;
}

void ULandscapeComponent::GetGeneratedTexturesAndMaterialInstances(TArray<UObject*>& OutTexturesAndMaterials) const
{
	TArray<UTexture*> LocalTextures = GetGeneratedTextures();
	TArray<UMaterialInstance*> LocalMaterialInstances = GetGeneratedMaterialInstances();
	OutTexturesAndMaterials.Reserve(LocalTextures.Num() + LocalMaterialInstances.Num());
	OutTexturesAndMaterials.Append(LocalTextures);
	OutTexturesAndMaterials.Append(LocalMaterialInstances);
}
#endif

ALandscapeProxy* ULandscapeComponent::GetLandscapeProxy() const
{
	return CastChecked<ALandscapeProxy>(GetOuter());
}

const FMeshMapBuildData* ULandscapeComponent::GetMeshMapBuildData() const
{
	AActor* Owner = GetOwner();

	if (Owner)
	{
		ULevel* OwnerLevel = Owner->GetLevel();

#if WITH_EDITOR
		if (FStaticLightingSystemInterface::GetPrimitiveMeshMapBuildData(this))
		{
			return FStaticLightingSystemInterface::GetPrimitiveMeshMapBuildData(this);
		}
#endif

		if (OwnerLevel && OwnerLevel->OwningWorld)
		{
			ULevel* ActiveLightingScenario = OwnerLevel->OwningWorld->GetActiveLightingScenario();
			UMapBuildDataRegistry* MapBuildData = NULL;

			if (ActiveLightingScenario && ActiveLightingScenario->MapBuildData)
			{
				MapBuildData = ActiveLightingScenario->MapBuildData;
			}
			else if (OwnerLevel->MapBuildData)
			{
				MapBuildData = OwnerLevel->MapBuildData;
			}

			if (MapBuildData)
			{
				return MapBuildData->GetMeshBuildData(MapBuildDataId);
			}
		}
	}

	return NULL;
}

bool ULandscapeComponent::IsPrecomputedLightingValid() const
{
	return GetMeshMapBuildData() != NULL;
}

void ULandscapeComponent::PropagateLightingScenarioChange()
{
	FComponentRecreateRenderStateContext Context(this);
}

bool ULandscapeComponent::IsHLODRelevant() const
{
	if (HasAnyFlags(RF_Transient))
	{
		return false;
	}

#if WITH_EDITOR
	return bEnableAutoLODGeneration;
#else
	return false;
#endif
}

TArray<URuntimeVirtualTexture*> const& ULandscapeComponent::GetRuntimeVirtualTextures() const
{
	return GetLandscapeProxy()->RuntimeVirtualTextures;
}

ERuntimeVirtualTextureMainPassType ULandscapeComponent::GetVirtualTextureRenderPassType() const
{
	return GetLandscapeProxy()->VirtualTextureRenderPassType;
}

ULandscapeInfo* ULandscapeComponent::GetLandscapeInfo() const
{
	return GetLandscapeProxy()->GetLandscapeInfo();
}

void ULandscapeComponent::BeginDestroy()
{
	Super::BeginDestroy();

#if WITH_EDITOR
	// Ask render thread to destroy EditToolRenderData
	EditToolRenderData = FLandscapeEditToolRenderData();
	UpdateEditToolRenderData();

	if (GIsEditor && !HasAnyFlags(RF_ClassDefaultObject))
	{
		ALandscapeProxy* Proxy = GetLandscapeProxy();

		// Remove any weightmap allocations from the Landscape Actor's map
		for (int32 LayerIdx = 0; LayerIdx < WeightmapLayerAllocations.Num(); LayerIdx++)
		{
			int32 WeightmapIndex = WeightmapLayerAllocations[LayerIdx].WeightmapTextureIndex;
			if (WeightmapTextures.IsValidIndex(WeightmapIndex))
			{
				UTexture2D* WeightmapTexture = WeightmapTextures[WeightmapIndex];
				TObjectPtr<ULandscapeWeightmapUsage>* Usage = Proxy->WeightmapUsageMap.Find(WeightmapTexture);
				if (Usage != nullptr && (*Usage) != nullptr)
				{
					(*Usage)->ChannelUsage[WeightmapLayerAllocations[LayerIdx].WeightmapTextureChannel] = nullptr;

					if ((*Usage)->IsEmpty())
					{
						Proxy->WeightmapUsageMap.Remove(WeightmapTexture);
					}
				}
			}
		}

		WeightmapTexturesUsage.Reset();
	}
#endif
}

FPrimitiveSceneProxy* ULandscapeComponent::CreateSceneProxy()
{
	return new FLandscapeComponentSceneProxy(this);
}

bool ULandscapeComponent::IsShown(const FEngineShowFlags& ShowFlags) const
{
	return ShowFlags.Landscape;
}

void ULandscapeComponent::DestroyComponent(bool bPromoteChildren/*= false*/)
{
	ALandscapeProxy* Proxy = GetLandscapeProxy();
	if (Proxy)
	{
		Proxy->LandscapeComponents.Remove(this);
	}

	Super::DestroyComponent(bPromoteChildren);
}

FBoxSphereBounds ULandscapeComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FBox MyBounds = CachedLocalBox.TransformBy(LocalToWorld);
	MyBounds = MyBounds.ExpandBy({ 0, 0, NegativeZBoundsExtension }, { 0, 0, PositiveZBoundsExtension });

	ALandscapeProxy* Proxy = GetLandscapeProxy();
	if (Proxy)
	{
		MyBounds = MyBounds.ExpandBy({ 0, 0, Proxy->NegativeZBoundsExtension }, { 0, 0, Proxy->PositiveZBoundsExtension });
	}

	return FBoxSphereBounds(MyBounds);
}

static void OnStaticMeshLODDistanceScaleChanged()
{
	extern RENDERER_API TAutoConsoleVariable<float> CVarStaticMeshLODDistanceScale;

	static float LastValue = 1.0f;

	if (LastValue != CVarStaticMeshLODDistanceScale.GetValueOnAnyThread())
	{
		LastValue = CVarStaticMeshLODDistanceScale.GetValueOnAnyThread();

		for (auto* LandscapeComponent : TObjectRange<ULandscapeComponent>(RF_ClassDefaultObject | RF_ArchetypeObject, true, EInternalObjectFlags::Garbage))
		{
			LandscapeComponent->MarkRenderStateDirty();
		}
	}
}

FAutoConsoleVariableSink OnStaticMeshLODDistanceScaleChangedSink(FConsoleCommandDelegate::CreateStatic(&OnStaticMeshLODDistanceScaleChanged));

void ULandscapeComponent::OnRegister()
{
	Super::OnRegister();

	if (GetLandscapeProxy())
	{
		// Generate MID representing the MIC
		if (GetLandscapeProxy()->bUseDynamicMaterialInstance)
		{
			MaterialInstancesDynamic.Reserve(MaterialInstances.Num());

			for (int32 i = 0; i < MaterialInstances.Num(); ++i)
			{
				MaterialInstancesDynamic.Add(UMaterialInstanceDynamic::Create(MaterialInstances[i], this));
			}
		}

		// AActor::GetWorld checks for Unreachable and BeginDestroyed
		UWorld* World = GetLandscapeProxy()->GetWorld();
		if (World)
		{
			ULandscapeInfo* Info = GetLandscapeInfo();
			if (Info)
			{
				Info->RegisterActorComponent(this);
			}
		}
	}
}

void ULandscapeComponent::OnUnregister()
{
	Super::OnUnregister();

#if WITH_EDITOR
	PhysicalMaterialTask.Release();
#endif

	if (GetLandscapeProxy())
	{
		// Generate MID representing the MIC
		if (GetLandscapeProxy()->bUseDynamicMaterialInstance)
		{
			MaterialInstancesDynamic.Empty();
		}

		// AActor::GetWorld checks for Unreachable and BeginDestroyed
		UWorld* World = GetLandscapeProxy()->GetWorld();

		if (World)
		{
			ULandscapeInfo* Info = GetLandscapeInfo();
			if (Info)
			{
				Info->UnregisterActorComponent(this);
			}
		}
	}
}

UTexture2D* ULandscapeComponent::GetHeightmap(bool InReturnEditingHeightmap) const
{
#if WITH_EDITORONLY_DATA
	if (InReturnEditingHeightmap)
	{
		if (const FLandscapeLayerComponentData* EditingLayer = GetEditingLayer())
		{
			return EditingLayer->HeightmapData.Texture;
		}
	}
#endif

	return HeightmapTexture;
}

UTexture2D* ULandscapeComponent::GetHeightmap(const FGuid& InLayerGuid) const
{
#if WITH_EDITORONLY_DATA
	if (InLayerGuid.IsValid())
	{
		if (const FLandscapeLayerComponentData* LayerData = GetLayerData(InLayerGuid))
		{
			return LayerData->HeightmapData.Texture;
		}
	}
#endif

	return HeightmapTexture;
}

const TArray<UTexture2D*>& ULandscapeComponent::GetWeightmapTextures(bool InReturnEditingWeightmap) const
{
#if WITH_EDITORONLY_DATA
	if (InReturnEditingWeightmap)
	{
		if (const FLandscapeLayerComponentData* EditingLayer = GetEditingLayer())
		{
			return EditingLayer->WeightmapData.Textures;
		}
	}
#endif

	return WeightmapTextures;
}

TArray<TObjectPtr<UTexture2D>>& ULandscapeComponent::GetWeightmapTextures(bool InReturnEditingWeightmap)
{
#if WITH_EDITORONLY_DATA
	if (InReturnEditingWeightmap)
	{
		if (FLandscapeLayerComponentData* EditingLayer = GetEditingLayer())
		{
			return EditingLayer->WeightmapData.Textures;
		}
	}
#endif

	return WeightmapTextures;
}

const TArray<UTexture2D*>& ULandscapeComponent::GetWeightmapTextures(const FGuid& InLayerGuid) const
{
#if WITH_EDITORONLY_DATA
	if (InLayerGuid.IsValid())
	{
		if (const FLandscapeLayerComponentData* LayerData = GetLayerData(InLayerGuid))
		{
			return LayerData->WeightmapData.Textures;
		}
	}
#endif

	return WeightmapTextures;
}

TArray<TObjectPtr<UTexture2D>>& ULandscapeComponent::GetWeightmapTextures(const FGuid& InLayerGuid)
{
#if WITH_EDITORONLY_DATA
	if (InLayerGuid.IsValid())
	{
		if (FLandscapeLayerComponentData* LayerData = GetLayerData(InLayerGuid))
		{
			return LayerData->WeightmapData.Textures;
		}
	}
#endif

	return WeightmapTextures;
}

const TArray<FWeightmapLayerAllocationInfo>& ULandscapeComponent::GetWeightmapLayerAllocations(bool InReturnEditingWeightmap) const
{
#if WITH_EDITORONLY_DATA
	if (InReturnEditingWeightmap)
	{
		if (const FLandscapeLayerComponentData* EditingLayer = GetEditingLayer())
		{
			return EditingLayer->WeightmapData.LayerAllocations;
		}
	}
#endif

	return WeightmapLayerAllocations;
}

TArray<FWeightmapLayerAllocationInfo>& ULandscapeComponent::GetWeightmapLayerAllocations(const FGuid& InLayerGuid)
{
#if WITH_EDITORONLY_DATA
	if (InLayerGuid.IsValid())
	{
		if (FLandscapeLayerComponentData* LayerData = GetLayerData(InLayerGuid))
		{
			return LayerData->WeightmapData.LayerAllocations;
		}
	}
#endif

	return WeightmapLayerAllocations;
}

const TArray<FWeightmapLayerAllocationInfo>& ULandscapeComponent::GetWeightmapLayerAllocations(const FGuid& InLayerGuid) const
{
#if WITH_EDITORONLY_DATA
	if (InLayerGuid.IsValid())
	{
		if (const FLandscapeLayerComponentData* LayerData = GetLayerData(InLayerGuid))
		{
			return LayerData->WeightmapData.LayerAllocations;
		}
	}
#endif

	return WeightmapLayerAllocations;
}

TArray<FWeightmapLayerAllocationInfo>& ULandscapeComponent::GetWeightmapLayerAllocations(bool InReturnEditingWeightmap)
{
#if WITH_EDITORONLY_DATA
	if (InReturnEditingWeightmap)
	{
		if (FLandscapeLayerComponentData* EditingLayer = GetEditingLayer())
		{
			return EditingLayer->WeightmapData.LayerAllocations;
		}
	}
#endif

	return WeightmapLayerAllocations;
}

const TArray<FWeightmapLayerAllocationInfo>& ULandscapeComponent::GetCurrentRuntimeWeightmapLayerAllocations() const
{

	bool bIsMobile = GetWorld()->GetFeatureLevel() == ERHIFeatureLevel::ES3_1;
	return bIsMobile ? MobileWeightmapLayerAllocations : WeightmapLayerAllocations;
}

TArray<FWeightmapLayerAllocationInfo>& ULandscapeComponent::GetCurrentRuntimeWeightmapLayerAllocations()
{
	bool bIsMobile = GetWorld()->GetFeatureLevel() == ERHIFeatureLevel::ES3_1;
	return bIsMobile ? MobileWeightmapLayerAllocations : WeightmapLayerAllocations;
}

#if WITH_EDITOR

FLandscapeLayerComponentData* ULandscapeComponent::GetEditingLayer()
{
	const FGuid& EditingLayerGuid = GetLandscapeActor()->GetEditingLayer();
	return EditingLayerGuid.IsValid() ? LayersData.Find(EditingLayerGuid) : nullptr;
}

const FLandscapeLayerComponentData* ULandscapeComponent::GetEditingLayer() const
{
	const FGuid& EditingLayerGuid = GetLandscapeActor()->GetEditingLayer();
	return EditingLayerGuid.IsValid() ? LayersData.Find(EditingLayerGuid) : nullptr;
}

void ULandscapeComponent::CopyFinalLayerIntoEditingLayer(FLandscapeEditDataInterface& DataInterface, TSet<UTexture2D*>& ProcessedHeightmaps)
{
	Modify();
	GetLandscapeProxy()->Modify();

	// Heightmap	
	UTexture2D* EditingTexture = GetHeightmap(true);
	if (!ProcessedHeightmaps.Contains(EditingTexture))
	{
		DataInterface.CopyTextureFromHeightmap(EditingTexture, this, 0);
		ProcessedHeightmaps.Add(EditingTexture);
	}

	// Weightmap
	const TArray<FWeightmapLayerAllocationInfo>& FinalWeightmapLayerAllocations = GetWeightmapLayerAllocations();
	TArray<FWeightmapLayerAllocationInfo>& EditingLayerWeightmapLayerAllocations = GetWeightmapLayerAllocations(GetEditingLayerGUID());

	// Add missing Alloc Infos
	for (const FWeightmapLayerAllocationInfo& FinalAllocInfo : FinalWeightmapLayerAllocations)
	{
		int32 Index = EditingLayerWeightmapLayerAllocations.IndexOfByPredicate([&FinalAllocInfo](const FWeightmapLayerAllocationInfo& EditingAllocInfo) { return EditingAllocInfo.LayerInfo == FinalAllocInfo.LayerInfo; });
		if (Index == INDEX_NONE)
		{
			new (EditingLayerWeightmapLayerAllocations) FWeightmapLayerAllocationInfo(FinalAllocInfo.LayerInfo);
		}
	}

	const bool bEditingWeighmaps = true;
	const bool bSaveToTransactionBuffer = true;
	ReallocateWeightmaps(&DataInterface, bEditingWeighmaps, bSaveToTransactionBuffer);

	const TArray<TObjectPtr<UTexture2D>>& EditingWeightmapTextures = GetWeightmapTextures(true);
	for (const FWeightmapLayerAllocationInfo& AllocInfo : EditingLayerWeightmapLayerAllocations)
	{
		DataInterface.CopyTextureFromWeightmap(EditingWeightmapTextures[AllocInfo.WeightmapTextureIndex], AllocInfo.WeightmapTextureChannel, this, AllocInfo.LayerInfo, 0);
	}
}

FGuid ULandscapeComponent::GetEditingLayerGUID() const
{
	ALandscape* Landscape = GetLandscapeActor();
	return Landscape != nullptr ? Landscape->GetEditingLayer() : FGuid();
}

bool ULandscapeComponent::HasLayersData() const
{
	return LayersData.Num() > 0;
}

const FLandscapeLayerComponentData* ULandscapeComponent::GetLayerData(const FGuid& InLayerGuid) const
{
	return LayersData.Find(InLayerGuid);
}

FLandscapeLayerComponentData* ULandscapeComponent::GetLayerData(const FGuid& InLayerGuid)
{
	return LayersData.Find(InLayerGuid);
}

void ULandscapeComponent::ForEachLayer(TFunctionRef<void(const FGuid&, struct FLandscapeLayerComponentData&)> Fn)
{
	for (auto& Pair : LayersData)
	{
		Fn(Pair.Key, Pair.Value);
	}
}

void ULandscapeComponent::AddLayerData(const FGuid& InLayerGuid, const FLandscapeLayerComponentData& InData)
{
	Modify();
	FLandscapeLayerComponentData& Data = LayersData.FindOrAdd(InLayerGuid);
	Data = InData;
}

void ULandscapeComponent::AddDefaultLayerData(const FGuid& InLayerGuid, const TArray<ULandscapeComponent*>& InComponentsUsingHeightmap, TMap<UTexture2D*, UTexture2D*>& InOutCreatedHeightmapTextures)
{
	Modify();

	UTexture2D* ComponentHeightmap = GetHeightmap();

	// Compute per layer data
	FLandscapeLayerComponentData* LayerData = GetLayerData(InLayerGuid);

	if (LayerData == nullptr || !LayerData->IsInitialized())
	{
		const FLandscapeLayer* EditLayer = GetLandscapeActor() ? GetLandscapeActor()->GetLayer(InLayerGuid) : nullptr;
		FLandscapeLayerComponentData NewData(EditLayer ? EditLayer->Name : FName());

		// Setup Heightmap data
		UTexture2D** LayerHeightmap = InOutCreatedHeightmapTextures.Find(ComponentHeightmap);

		if (LayerHeightmap == nullptr)
		{
			// No mipchain required as these layer weight maps are used in layer compositing to generate a final set of weight maps to be used for rendering
			UTexture2D* NewLayerHeightmap = GetLandscapeProxy()->CreateLandscapeTexture(ComponentHeightmap->Source.GetSizeX(), ComponentHeightmap->Source.GetSizeY(), TEXTUREGROUP_Terrain_Heightmap, ComponentHeightmap->Source.GetFormat(), /* OptionalOverrideOuter = */ nullptr, /* bCompress = */ false, /* bMipChain = */ false);
			LayerHeightmap = &InOutCreatedHeightmapTextures.Add(ComponentHeightmap, NewLayerHeightmap);

			// Init Mip0 to be at 32768 which is equal to "0"
			TArrayView<FColor> Mip0Data((FColor*)NewLayerHeightmap->Source.LockMip(0), NewLayerHeightmap->Source.GetSizeX() * NewLayerHeightmap->Source.GetSizeY());

			for (ULandscapeComponent* ComponentUsingHeightmap : InComponentsUsingHeightmap)
			{
				int32 HeightmapComponentOffsetX = FMath::RoundToInt32(NewLayerHeightmap->Source.GetSizeX() * ComponentUsingHeightmap->HeightmapScaleBias.Z);
				int32 HeightmapComponentOffsetY = FMath::RoundToInt32(NewLayerHeightmap->Source.GetSizeY() * ComponentUsingHeightmap->HeightmapScaleBias.W);

				for (int32 SubsectionY = 0; SubsectionY < NumSubsections; SubsectionY++)
				{
					for (int32 SubsectionX = 0; SubsectionX < NumSubsections; SubsectionX++)
					{
						for (int32 SubY = 0; SubY <= SubsectionSizeQuads; SubY++)
						{
							for (int32 SubX = 0; SubX <= SubsectionSizeQuads; SubX++)
							{
								// X/Y of the vertex we're looking at in component's coordinates.
								const int32 CompX = SubsectionSizeQuads * SubsectionX + SubX;
								const int32 CompY = SubsectionSizeQuads * SubsectionY + SubY;

								// X/Y of the vertex we're looking indexed into the texture data
								const int32 TexX = (SubsectionSizeQuads + 1) * SubsectionX + SubX;
								const int32 TexY = (SubsectionSizeQuads + 1) * SubsectionY + SubY;

								const int32 HeightTexDataIdx = (HeightmapComponentOffsetX + TexX) + (HeightmapComponentOffsetY + TexY) * NewLayerHeightmap->Source.GetSizeX();

								// copy height and normal data
								const uint16 HeightValue = LandscapeDataAccess::GetTexHeight(0.f);

								Mip0Data[HeightTexDataIdx].R = HeightValue >> 8;
								Mip0Data[HeightTexDataIdx].G = HeightValue & 255;

								// Normal with get calculated later
								Mip0Data[HeightTexDataIdx].B = 0;
								Mip0Data[HeightTexDataIdx].A = 0;
							}
						}
					}
				}
			}

			NewLayerHeightmap->Source.UnlockMip(0);

			NewLayerHeightmap->UpdateResource();
		}

		NewData.HeightmapData.Texture = *LayerHeightmap;

		// Nothing to do for Weightmap by default

		AddLayerData(InLayerGuid, MoveTemp(NewData));
	}
}

void ULandscapeComponent::RemoveLayerData(const FGuid& InLayerGuid)
{
	Modify();
	LayersData.Remove(InLayerGuid);
}

#endif // WITH_EDITOR

void ULandscapeComponent::SetHeightmap(UTexture2D* NewHeightmap)
{
	check(NewHeightmap != nullptr);
	HeightmapTexture = NewHeightmap;
}

void ULandscapeComponent::SetWeightmapTextures(const TArray<UTexture2D*>& InNewWeightmapTextures, bool InApplyToEditingWeightmap)
{
#if WITH_EDITOR
	FLandscapeLayerComponentData* EditingLayer = GetEditingLayer();

	if (InApplyToEditingWeightmap && EditingLayer != nullptr)
	{
		EditingLayer->WeightmapData.Textures.Reset(InNewWeightmapTextures.Num());
		EditingLayer->WeightmapData.Textures.Append(InNewWeightmapTextures);
	}
	else
#endif // WITH_EDITOR
	{
		WeightmapTextures = InNewWeightmapTextures;
	}
}

// Note that there is a slight difference in behavior with the Internal function:
// unlike SetWeightmapTextures, this function will never set the runtime WeightmapTextures when you intended to set an edit layer's WeightmapData.Textures
void ULandscapeComponent::SetWeightmapTexturesInternal(const TArray<UTexture2D*>& InNewWeightmapTextures, const FGuid& InEditLayerGuid)
{
	if (InEditLayerGuid.IsValid())
	{
#if WITH_EDITOR
		FLandscapeLayerComponentData* EditingLayer = GetLayerData(InEditLayerGuid);
		if (ensure(EditingLayer))
		{
			EditingLayer->WeightmapData.Textures.Reset(InNewWeightmapTextures.Num());
			EditingLayer->WeightmapData.Textures.Append(InNewWeightmapTextures);
		}
#endif // WITH_EDITOR
	}
	else
	{
		WeightmapTextures = InNewWeightmapTextures;
	}
}


#if WITH_EDITOR
void ULandscapeComponent::SetWeightmapLayerAllocations(const TArray<FWeightmapLayerAllocationInfo>& InNewWeightmapLayerAllocations)
{
	WeightmapLayerAllocations = InNewWeightmapLayerAllocations;
}

TArray<TObjectPtr<ULandscapeWeightmapUsage>>& ULandscapeComponent::GetWeightmapTexturesUsage(bool InReturnEditingWeightmap)
{
	if (InReturnEditingWeightmap)
	{
		if (FLandscapeLayerComponentData* EditingLayer = GetEditingLayer())
		{
			return EditingLayer->WeightmapData.TextureUsages;
		}
	}

	return WeightmapTexturesUsage;
}

const TArray<ULandscapeWeightmapUsage*>& ULandscapeComponent::GetWeightmapTexturesUsage(bool InReturnEditingWeightmap) const
{
	if (InReturnEditingWeightmap)
	{
		if (const FLandscapeLayerComponentData* EditingLayer = GetEditingLayer())
		{
			return EditingLayer->WeightmapData.TextureUsages;
		}
	}

	return WeightmapTexturesUsage;
}

TArray<TObjectPtr<ULandscapeWeightmapUsage>>& ULandscapeComponent::GetWeightmapTexturesUsage(const FGuid& InLayerGuid)
{
	if (InLayerGuid.IsValid())
	{
		if (FLandscapeLayerComponentData* LayerData = GetLayerData(InLayerGuid))
		{
			return LayerData->WeightmapData.TextureUsages;
		}
	}

	return WeightmapTexturesUsage;
}

const TArray<ULandscapeWeightmapUsage*>& ULandscapeComponent::GetWeightmapTexturesUsage(const FGuid& InLayerGuid) const
{
	if (InLayerGuid.IsValid())
	{
		if (const FLandscapeLayerComponentData* LayerData = GetLayerData(InLayerGuid))
		{
			return LayerData->WeightmapData.TextureUsages;
		}
	}

	return WeightmapTexturesUsage;
}

void ULandscapeComponent::SetWeightmapTexturesUsage(const TArray<ULandscapeWeightmapUsage*>& InNewWeightmapTexturesUsage, bool InApplyToEditingWeightmap)
{
	FLandscapeLayerComponentData* EditingLayer = GetEditingLayer();

	if (InApplyToEditingWeightmap && EditingLayer != nullptr)
	{
		EditingLayer->WeightmapData.TextureUsages.Reset(InNewWeightmapTexturesUsage.Num());
		EditingLayer->WeightmapData.TextureUsages.Append(InNewWeightmapTexturesUsage);
	}
	else
	{
		WeightmapTexturesUsage = InNewWeightmapTexturesUsage;
	}
}

void ULandscapeComponent::SetWeightmapTexturesUsageInternal(const TArray<ULandscapeWeightmapUsage*>& InNewWeightmapTexturesUsage, const FGuid& InEditLayerGuid)
{
	if (InEditLayerGuid.IsValid())
	{
#if WITH_EDITOR
		FLandscapeLayerComponentData* EditingLayer = GetLayerData(InEditLayerGuid);
		if (ensure(EditingLayer))
		{
			EditingLayer->WeightmapData.TextureUsages.Reset(InNewWeightmapTexturesUsage.Num());
			EditingLayer->WeightmapData.TextureUsages.Append(InNewWeightmapTexturesUsage);
		}
#endif // WITH_EDITOR
	}
	else
	{
		WeightmapTexturesUsage = InNewWeightmapTexturesUsage;
	}
}

void ULandscapeComponent::DeleteLayerAllocation(const FGuid& InEditLayerGuid, int32 InLayerAllocationIdx, bool bInShouldDirtyPackage)
{
	TArray<FWeightmapLayerAllocationInfo>& ComponentWeightmapLayerAllocations = GetWeightmapLayerAllocations(InEditLayerGuid);
	TArray<TObjectPtr<UTexture2D>>& ComponentWeightmapTextures = GetWeightmapTextures(InEditLayerGuid);
	TArray<TObjectPtr<ULandscapeWeightmapUsage>>& ComponentWeightmapTexturesUsage = GetWeightmapTexturesUsage(InEditLayerGuid);

	const FWeightmapLayerAllocationInfo& LayerAllocation = ComponentWeightmapLayerAllocations[InLayerAllocationIdx];
	const int32 DeleteLayerWeightmapTextureIndex = LayerAllocation.WeightmapTextureIndex;

	ALandscapeProxy* Proxy = GetLandscapeProxy();
	Modify(bInShouldDirtyPackage);
	Proxy->Modify(bInShouldDirtyPackage);

	// Mark the weightmap channel as unallocated, so we can reuse it later
	ULandscapeWeightmapUsage* Usage = ComponentWeightmapTexturesUsage.IsValidIndex(DeleteLayerWeightmapTextureIndex) ? ComponentWeightmapTexturesUsage[DeleteLayerWeightmapTextureIndex] : nullptr;
	if (Usage) // can be null if WeightmapUsageMap hasn't been built yet
	{
		Usage->ChannelUsage[LayerAllocation.WeightmapTextureChannel] = nullptr;
	}

	// Remove the layer:
	ComponentWeightmapLayerAllocations.RemoveAt(InLayerAllocationIdx);

	// Check if the weightmap texture used by the material layer we just removed is used by any other material layer -- if not then we can remove the texture from the local list (as it's not used locally)
	bool bCanRemoveLayerTexture = !ComponentWeightmapLayerAllocations.ContainsByPredicate([DeleteLayerWeightmapTextureIndex](const FWeightmapLayerAllocationInfo& Allocation) { return Allocation.WeightmapTextureIndex == DeleteLayerWeightmapTextureIndex; });
	if (bCanRemoveLayerTexture)
	{
	    // Make sure the texture can be garbage collected, if necessary
		ComponentWeightmapTextures[DeleteLayerWeightmapTextureIndex]->ClearFlags(RF_Standalone);

		// Remove from our local list of textures and usages
		ComponentWeightmapTextures.RemoveAt(DeleteLayerWeightmapTextureIndex);
		if (Usage)
		{
			ComponentWeightmapTexturesUsage.RemoveAt(DeleteLayerWeightmapTextureIndex);
		}

		// Adjust WeightmapTextureIndex for other allocations (as we just reordered the Weightmap list with the deletions above)
		for (FWeightmapLayerAllocationInfo& Allocation : ComponentWeightmapLayerAllocations)
		{
			if (Allocation.WeightmapTextureIndex > DeleteLayerWeightmapTextureIndex)
			{
				Allocation.WeightmapTextureIndex--;
			}
			check(Allocation.WeightmapTextureIndex < ComponentWeightmapTextures.Num());
		}
	}

	Proxy->ValidateProxyLayersWeightmapUsage();
}

#endif // WITH_EDITOR

void ALandscapeProxy::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	ULandscapeInfo* LandscapeInfo = nullptr;
	if (!IsPendingKillPending())
	{
		// Duplicated or newly spawned Landscapes don't have a valid guid until PostEditImport is called, we'll register then
		if (LandscapeGuid.IsValid())
		{
			LandscapeInfo = GetLandscapeInfo();

			// Depending what action triggered this callback, we may have already registered.  If not register now with LandscapeInfo.
			if ((LandscapeInfo == nullptr) || !LandscapeInfo->IsRegistered(this))
			{
				LandscapeInfo = CreateLandscapeInfo(true);
			}

#if WITH_EDITOR
			if (GIsEditor)
			{
				// Note: This can happen when loading certain cooked assets in an editor
				// Todo: Determine the root cause of this and fix it at a higher level!
				if (LandscapeComponents.Num() > 0 && LandscapeComponents[0] == nullptr)
				{
					LandscapeComponents.Empty();
				}

				if (WeightmapFixupVersion != CurrentVersion)
				{
					FixupWeightmaps();
				}

				UpdateCachedHasLayersContent(true);

				// Cache the value at this point as CreateLandscapeInfo (-> RegisterActor) might create/destroy layers content if there was a mismatch between landscape & proxy
				// Check the actual flag here not HasLayersContent() which could return true if the LandscapeActorRef is valid.
				bool bHasLayersContentBefore = bHasLayersContent;

				check(WeightmapFixupVersion == CurrentVersion);

				const bool bNeedOldDataMigration = !bHasLayersContentBefore && CanHaveLayersContent();
				if (bNeedOldDataMigration && LandscapeInfo->LandscapeActor.IsValid() && LandscapeInfo->LandscapeActor->HasLayersContent())
				{
					LandscapeInfo->LandscapeActor->CopyOldDataToDefaultLayer(this);
				}
			}
#endif // WITH_EDITOR
		}

		if (UWorld* OwningWorld = GetWorld())
		{
			if (ULandscapeSubsystem* LandscapeSubsystem = OwningWorld->GetSubsystem<ULandscapeSubsystem>())
			{
				LandscapeSubsystem->RegisterActor(this);
			}
		}
	}
#if WITH_EDITOR
	if ((LandscapeInfo != nullptr) && !IsPendingKillPending() && LandscapeGuid.IsValid())
	{
		LandscapeInfo->FixupProxiesTransform();
	}
#endif // WITH_EDITOR
}

void ALandscapeProxy::UnregisterAllComponents(const bool bForReregister)
{
	// On shutdown the world will be unreachable
	if (GetWorld() && IsValidChecked(GetWorld()) && !GetWorld()->IsUnreachable() &&
		// When redoing the creation of a landscape we may get UnregisterAllComponents called when
		// we are in a "pre-initialized" state (empty guid, etc)
		LandscapeGuid.IsValid())
	{
		ULandscapeInfo* LandscapeInfo = GetLandscapeInfo();
		if (LandscapeInfo)
		{
			LandscapeInfo->UnregisterActor(this);
		}

		if (ULandscapeSubsystem* LandscapeSubsystem = GetWorld()->GetSubsystem<ULandscapeSubsystem>())
		{
			LandscapeSubsystem->UnregisterActor(this);
		}
	}

	Super::UnregisterAllComponents(bForReregister);
}


FArchive& operator<<(FArchive& Ar, FWeightmapLayerAllocationInfo& U)
{
	return Ar << U.LayerInfo << U.WeightmapTextureChannel << U.WeightmapTextureIndex;
}

#if WITH_EDITORONLY_DATA
FArchive& operator<<(FArchive& Ar, FLandscapeAddCollision& U)
{
	return Ar << U.Corners[0] << U.Corners[1] << U.Corners[2] << U.Corners[3];
}
#endif // WITH_EDITORONLY_DATA

FArchive& operator<<(FArchive& Ar, FLandscapeLayerStruct*& L)
{
	if (L)
	{
		Ar << L->LayerInfoObj;
#if WITH_EDITORONLY_DATA
		return Ar << L->ThumbnailMIC;
#else
		return Ar;
#endif // WITH_EDITORONLY_DATA
	}
	return Ar;
}

void ULandscapeInfo::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	if (Ar.IsTransacting())
	{
		Ar << XYtoComponentMap;
#if WITH_EDITORONLY_DATA
		Ar << XYtoAddCollisionMap;
#endif
		Ar << SelectedComponents;
		Ar << SelectedRegion;
		Ar << SelectedRegionComponents;
	}
}

void ALandscape::PostInitProperties()
{
	Super::PostInitProperties();
	
#if WITH_EDITOR
	if (!IsTemplate())
	{
		if (const UWorld* World = GetWorld())
		{
			if (ULandscapeSubsystem* LandscapeSubSystem = World->GetSubsystem<ULandscapeSubsystem>())
			{
				if (FLandscapeNotificationManager* LandscapeNotificationManager = LandscapeSubSystem->GetNotificationManager())
				{
					// Create conditional GrassRenderingNotification
					GrassRenderingNotification = MakeShared<FLandscapeNotification>(this, FLandscapeNotification::EType::GrassRendering,
						[this]() { return NumComponentsNeedingGrassMapRender > 0 && !(FSlateApplication::Get().HasAnyMouseCaptor() || GUnrealEd->IsUserInteracting()); },
						[this](FText& InText)
						{
							// Accumulating all outstanding grass maps that need to be rendered for ALL landscapes because only one such notification can be displayed at a time
							int ComponentsNeedingGrassMapRenderAllLandscapes = 0;
							for (const ALandscape* Landscape : TObjectRange<ALandscape>(RF_ClassDefaultObject | RF_ArchetypeObject, true, EInternalObjectFlags::Garbage))
							{
								ComponentsNeedingGrassMapRenderAllLandscapes += Landscape->NumComponentsNeedingGrassMapRender;
							}
							
							FFormatNamedArguments Args;
							Args.Add(TEXT("OutstandingGrassMaps"), FText::AsNumber(ComponentsNeedingGrassMapRenderAllLandscapes));
							InText = FText::Format(NSLOCTEXT("GrassMapRender", "GrassMapRenderFormat", "Building Grass Maps ({OutstandingGrassMaps})"), Args);
						});
			
					LandscapeNotificationManager->RegisterNotification(GrassRenderingNotification);
				}
			}
		}
	}
#endif // WITH_EDITOR
}


void ALandscape::PostLoad()
{
	if (!LandscapeGuid.IsValid())
	{
		LandscapeGuid = FGuid::NewGuid();
	}
	else
	{
#if WITH_EDITOR
		UWorld* CurrentWorld = GetWorld();
		for (ALandscape* Landscape : TObjectRange<ALandscape>(RF_ClassDefaultObject | RF_BeginDestroyed))
		{
			if (Landscape && Landscape != this && Landscape->LandscapeGuid == LandscapeGuid && Landscape->GetWorld() == CurrentWorld)
			{
				// Duplicated landscape level, need to generate new GUID. This can happen during PIE or gameplay when streaming the same landscape actor.
				Modify();
				LandscapeGuid = FGuid::NewGuid();
				break;
			}
		}
#endif // WITH_EDITOR
	}

#if WITH_EDITOR
	for (FLandscapeLayer& Layer : LandscapeLayers)
	{
		// For now, only Layer reserved for Landscape Spline uses AlphaBlend
		Layer.BlendMode = (Layer.Guid == LandscapeSplinesTargetLayerGuid) ? LSBM_AlphaBlend : LSBM_AdditiveBlend;
		for (FLandscapeLayerBrush& Brush : Layer.Brushes)
		{
			Brush.SetOwner(this);
		}
	}
#endif // WITH_EDITOR

	Super::PostLoad();
}

FBox ALandscape::GetLoadedBounds() const
{
	return GetLandscapeInfo()->GetLoadedBounds();
}


// ----------------------------------------------------------------------------------

// This shader allows to render parts of the heightmaps (all pixels except the redundant ones on the right/bottom edges) in an atlas render target (uncompressed height)
class FLandscapeMergeHeightmapsPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLandscapeMergeHeightmapsPS);
	SHADER_USE_PARAMETER_STRUCT(FLandscapeMergeHeightmapsPS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector4, InHeightmapAtlasSubregion)
		SHADER_PARAMETER(FUintVector4, InHeightmapSubregion)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InHeightmap)
		RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& InParameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& InParameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("MERGE_HEIGHTMAP"), 1);
	}

	static void MergeHeightmap(FRDGBuilder& GraphBuilder, FParameters* InParameters, const FIntRect& InRenderTargetArea)
	{
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FLandscapeMergeHeightmapsPS> PixelShader(ShaderMap);

		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			ShaderMap,
			RDG_EVENT_NAME("LandscapeLayers_MergeHeightmap"),
			PixelShader,
			InParameters,
			InRenderTargetArea);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLandscapeMergeHeightmapsPS, "/Engine/Private/Landscape/LandscapeMergeHeightmapsPS.usf", "MergeHeightmap", SF_Pixel);


// ----------------------------------------------------------------------------------

// This shader allows to resample the heightmap (bilinear interpolation) from a given atlas usually heightmap produced by FLandscapeMergeHeightmapsPS :
//  The output heightmap's heights can be either compressed or uncompressed depending on the render target format (8 bits/channel for the former, 16/32 bits/channel for the latter)
class FLandscapeResampleHeightmapsPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLandscapeResampleHeightmapsPS);
	SHADER_USE_PARAMETER_STRUCT(FLandscapeResampleHeightmapsPS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FMatrix44f, InOutputUVToMergedHeightmapUV)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InMergedHeightmap)
		SHADER_PARAMETER_SAMPLER(SamplerState, InMergedHeightmapSampler)
		SHADER_PARAMETER(FUintVector2, InRenderAreaSize)
		RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		class FCompressHeight : SHADER_PERMUTATION_BOOL("COMPRESS_HEIGHT");
	using FPermutationDomain = TShaderPermutationDomain<FCompressHeight>;

	static FPermutationDomain GetPermutationVector(bool bCompressHeight)
	{
		FPermutationDomain PermutationVector;
		PermutationVector.Set<FCompressHeight>(bCompressHeight);
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& InParameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& InParameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("RESAMPLE_HEIGHTMAP"), 1);
	}

	static void ResampleHeightmap(FRDGBuilder& GraphBuilder, FParameters* InParameters, bool bCompressHeight)
	{
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

		const FLandscapeResampleHeightmapsPS::FPermutationDomain PixelPermutationVector = FLandscapeResampleHeightmapsPS::GetPermutationVector(bCompressHeight);

		TShaderMapRef<FLandscapeResampleHeightmapsPS> PixelShader(ShaderMap, PixelPermutationVector);

		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			ShaderMap,
			RDG_EVENT_NAME("LandscapeLayers_ResampleHeightmap"),
			PixelShader,
			InParameters,
			FIntRect(0, 0, InParameters->InRenderAreaSize.X, InParameters->InRenderAreaSize.Y));
	}
};

IMPLEMENT_GLOBAL_SHADER(FLandscapeResampleHeightmapsPS, "/Engine/Private/Landscape/LandscapeMergeHeightmapsPS.usf", "ResampleHeightmap", SF_Pixel);


// Render-thread version of the data / functions we need for the local merge of edit layers : 
namespace RenderMergedLandscape_RenderThread
{
	struct FHeightmapRenderInfo
	{
		// Transform to go from the output render area space ((0,0) in the lower left corner, (1,1) in the upper-right) to the temporary render target space
		FMatrix OutputUVToMergedHeightmapUV;
		FBox RenderAreaExtents;
		// TODO [jonathan.bard] : remove ? This is only used to compute the temporary render target size
		FIntPoint SubsectionSizeQuads;
		int32 NumSubsections = 1;
		bool bCompressHeight = false;

		TMap<FIntPoint, FTexture2DResourceSubregion> ComponentHeightmapsToRender;
	};

	void RenderMergedHeightmap(const FHeightmapRenderInfo& InRenderInfo, FRDGBuilder& GraphBuilder, FRDGTextureRef OutputTexture)
	{
		// Find the total area that those components need to be rendered to :
		FIntRect ComponentKeyRect;
		for (auto Iter = InRenderInfo.ComponentHeightmapsToRender.CreateConstIterator(); Iter; ++Iter)
		{
			ComponentKeyRect.Include(Iter.Key());
		}

		ComponentKeyRect.Max += FIntPoint(1, 1);
		FIntPoint NumComponentsToRender(ComponentKeyRect.Width(), ComponentKeyRect.Height());
		FIntPoint NumSubsectionsToRender = NumComponentsToRender * InRenderInfo.NumSubsections;
		FIntPoint RenderTargetSize = NumSubsectionsToRender * InRenderInfo.SubsectionSizeQuads + 1; // add one for the end vertex
		FIntPoint ComponentSizeQuads = InRenderInfo.SubsectionSizeQuads * InRenderInfo.NumSubsections;

		// We need a temporary render target that can contain all heightmaps. Use PF_G16 (decoded height) as this will be resampled using bilinear sampling :
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(RenderTargetSize, PF_G16, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
		FRDGTextureRef AtlasTexture = GraphBuilder.CreateTexture(Desc, TEXT("LandscapeHeightmapAtlas"));
		FRDGTextureSRVRef AtlasTextureSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(AtlasTexture));
		FRenderTargetBinding AtlasTextureRT(AtlasTexture, ERenderTargetLoadAction::ELoad);

		TMap<FTexture2DResource*, FRDGTextureSRVRef> HeightmapTextureSRVs;

		// Fill that render target subsection by subsection, in order to bypass the redundant columns/lines on the subsection edges:
		for (int32 ComponentY = ComponentKeyRect.Min.Y; ComponentY < ComponentKeyRect.Max.Y; ++ComponentY)
		{
			for (int32 ComponentX = ComponentKeyRect.Min.X; ComponentX < ComponentKeyRect.Max.X; ++ComponentX)
			{
				FIntPoint LandscapeComponentKey(ComponentX, ComponentY);
				if (const FTexture2DResourceSubregion* HeightmapResourceSubregion = InRenderInfo.ComponentHeightmapsToRender.Find(LandscapeComponentKey))
				{
					FIntPoint SubsectionSubregionSize = HeightmapResourceSubregion->Subregion.Size() / InRenderInfo.NumSubsections;
					FRDGTextureSRVRef* Heightmap = HeightmapTextureSRVs.Find(HeightmapResourceSubregion->Texture);
					if (Heightmap == nullptr)
					{
						FString* DebugString = GraphBuilder.AllocObject<FString>(HeightmapResourceSubregion->Texture->GetTextureName().ToString());
						FRDGTextureRef TextureRef = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(HeightmapResourceSubregion->Texture->TextureRHI, **DebugString));
						Heightmap = &HeightmapTextureSRVs.Add(HeightmapResourceSubregion->Texture, GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TextureRef)));
					}

					for (int32 SubsectionY = 0; SubsectionY < InRenderInfo.NumSubsections; ++SubsectionY)
					{
						for (int32 SubsectionX = 0; SubsectionX < InRenderInfo.NumSubsections; ++SubsectionX)
						{
							FIntPoint SubsectionLocalKey(SubsectionX, SubsectionY);
							FIntPoint SubsectionKey = LandscapeComponentKey * InRenderInfo.NumSubsections + SubsectionLocalKey;

							FIntRect HeightmapAtlasSubregion;
							HeightmapAtlasSubregion.Min = SubsectionKey * InRenderInfo.SubsectionSizeQuads;
							// We only really need the +1 on the very last subsection to get the last row/column, since we end up overwriting the other end
							// rows/columns when we proceed to the next tile. However it's much easier to add the +1 here and do a small amount of duplicate
							// writes, because otherwise we would have to adjust HeightmapSubregion to align with the region we're writing, which would get
							// messy in cases of different mip levels.
							HeightmapAtlasSubregion.Max = HeightmapAtlasSubregion.Min + InRenderInfo.SubsectionSizeQuads + 1;

							FIntRect HeightmapSubregion;
							HeightmapSubregion.Min = HeightmapResourceSubregion->Subregion.Min + SubsectionLocalKey * SubsectionSubregionSize;
							HeightmapSubregion.Max = HeightmapSubregion.Min + SubsectionSubregionSize;

							FLandscapeMergeHeightmapsPS::FParameters* MergeHeightmapsPSParams = GraphBuilder.AllocParameters<FLandscapeMergeHeightmapsPS::FParameters>();
							MergeHeightmapsPSParams->InHeightmapAtlasSubregion = FUintVector4(HeightmapAtlasSubregion.Min.X, HeightmapAtlasSubregion.Min.Y, HeightmapAtlasSubregion.Max.X, HeightmapAtlasSubregion.Max.Y);
							MergeHeightmapsPSParams->InHeightmapSubregion = FUintVector4(HeightmapSubregion.Min.X, HeightmapSubregion.Min.Y, HeightmapSubregion.Max.X, HeightmapSubregion.Max.Y);
							MergeHeightmapsPSParams->InHeightmap = *Heightmap;
							MergeHeightmapsPSParams->RenderTargets[0] = AtlasTextureRT;

							FLandscapeMergeHeightmapsPS::MergeHeightmap(GraphBuilder, MergeHeightmapsPSParams, HeightmapAtlasSubregion);
						}
					}
				}
				else
				{
					// We can clear all subsections of a given component at once : 
					FIntRect HeightmapAtlasSubregion;
					HeightmapAtlasSubregion.Min = LandscapeComponentKey * ComponentSizeQuads;
					HeightmapAtlasSubregion.Max = HeightmapAtlasSubregion.Min + ComponentSizeQuads;
					FRDGTextureClearInfo ClearInfo;
					ClearInfo.Viewport = HeightmapAtlasSubregion;
					AddClearRenderTargetPass(GraphBuilder, AtlasTexture, ClearInfo);
				}
			}
		}

		{
			FIntVector RenderAreaSize = OutputTexture->Desc.GetSize();
			FLandscapeResampleHeightmapsPS::FParameters* ResampleHeightmapsPSParams = GraphBuilder.AllocParameters<FLandscapeResampleHeightmapsPS::FParameters>();
			ResampleHeightmapsPSParams->InOutputUVToMergedHeightmapUV = FMatrix44f(InRenderInfo.OutputUVToMergedHeightmapUV);
			ResampleHeightmapsPSParams->InMergedHeightmap = AtlasTextureSRV;
			ResampleHeightmapsPSParams->InMergedHeightmapSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			ResampleHeightmapsPSParams->InRenderAreaSize = FUintVector2((uint32)RenderAreaSize.X, (uint32)RenderAreaSize.Y);
			ResampleHeightmapsPSParams->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ENoAction);;

			// We now need to resample the atlas texture where the render area is : 
			FLandscapeResampleHeightmapsPS::ResampleHeightmap(GraphBuilder, ResampleHeightmapsPSParams, InRenderInfo.bCompressHeight);
		}
	}
}

bool ALandscape::IsValidRenderTargetFormatHeightmap(EPixelFormat InRenderTargetFormat, bool& bOutCompressHeight)
{
	bOutCompressHeight = false;
	switch (InRenderTargetFormat)
	{
		// 8 bits formats : need compression
	case PF_A8R8G8B8:
	case PF_R8G8B8A8:
	case PF_R8G8:
	case PF_B8G8R8A8:
	{
		bOutCompressHeight = true;
		return true;
	}
	// 16 bits formats :
	case PF_G16:
	// We don't use 16 bit float formats because they will have precision issues
	// (we need 16 bits of mantissa)

	// TODO: We can support 32 bit floating point formats, but for these, we probably
	// want to output the height as an unpacked, signed values. We'll add support for
	// that in a later CL.
	//case PF_R32_FLOAT:
	//case PF_G32R32F:
	//case PF_R32G32B32F:
	//case PF_A32B32G32R32F:
	{
		return true;
	}
	default:
		break;
	}

	return false;
}

void ALandscape::RenderHeightmap(const FTransform& InRenderAreaWorldTransform, const FBox2D& InRenderAreaExtents, UTextureRenderTarget2D* OutRenderTarget)
{
	// TODO: We may want a version of this function that returns a lambda that can be passed to the render thread and run
	// there to add the pass to an existing FRDGBuilder, in case the user wants this to be a part of a render graph with
	// other passes. In that case RenderHeightmap would just use that function.

	TRACE_CPUPROFILER_EVENT_SCOPE(Landscape_RenderMergedHeightmap);

	RenderCaptureInterface::FScopedCapture RenderCapture((RenderCaptureNextHeightmapRenders != 0), TEXT("RenderHeightmapCapture"));
	RenderCaptureNextHeightmapRenders = FMath::Max(RenderCaptureNextHeightmapRenders - 1, 0);

	ULandscapeInfo* Info = GetLandscapeInfo();
	if (Info == nullptr)
	{
		UE_LOG(LogLandscape, Error, TEXT("RenderHeightmap : Cannot render anything if there's no associated landscape info with this landscape (%s)"), *GetFullName());
		return;
	}

	// Check render target validity :
	if (OutRenderTarget == nullptr)
	{
		UE_LOG(LogLandscape, Error, TEXT("RenderHeightmap : Missing render target"));
		return;
	}

	bool bCompressHeight = false;
	if (!IsValidRenderTargetFormatHeightmap(OutRenderTarget->GetFormat(), bCompressHeight))
	{
		UE_LOG(LogLandscape, Warning, TEXT("RenderHeightmap : invalid render target format for rendering heightmap (%s)"), GetPixelFormatString(OutRenderTarget->GetFormat()));
		return;
	}

	// It can be helpful to visualize where the render happened so leave a visual log for that: 
	UE_VLOG_OBOX(this, LogLandscape, Log, FBox(FVector(InRenderAreaExtents.Min, 0.0), FVector(InRenderAreaExtents.Max, 0.0)), InRenderAreaWorldTransform.ToMatrixWithScale(), FColor::Blue, TEXT(""));

	// Don't do anything if this render area overlaps with no landscape component :
	TMap<FIntPoint, ULandscapeComponent*> OverlappedComponents;
	FIntRect ComponentIndicesBoundingRect;
	if (!Info->GetOverlappedComponents(InRenderAreaWorldTransform, InRenderAreaExtents, OverlappedComponents, ComponentIndicesBoundingRect))
	{
		UE_LOG(LogLandscape, Log, TEXT("RenderHeightmap : no heightmap to render"));
		return;
	}

	const FTransform& LandscapeTransform = GetTransform();

	RenderMergedLandscape_RenderThread::FHeightmapRenderInfo HeightmapMergeRenderInfo;
	// For now, merge the heightmap at max resolution :
	HeightmapMergeRenderInfo.SubsectionSizeQuads = SubsectionSizeQuads;
	HeightmapMergeRenderInfo.NumSubsections = NumSubsections;
	HeightmapMergeRenderInfo.bCompressHeight = bCompressHeight;

	for (auto It : OverlappedComponents)
	{
		ULandscapeComponent* Component = It.Value;
		FIntPoint ComponentKey = It.Key;
		UTexture2D* ComponentHeightmap = Component->GetHeightmap();
		
		// Get the subregion of the heightmap that this component uses (differs due to texture sharing).
		// HeightmapScaleBias ZW values give us the offset of the component in a shared texture. You might think that XY would
		// give the portion of the texture it occupies, but no, XY are 1/size, for some reason. Just calculate the subregion
		// size ourselves.
		int32 ComponentSize = Component->NumSubsections * (Component->SubsectionSizeQuads + 1);

		FIntPoint HeightmapOffset(0, 0);
		FTextureResource* HeightmapResource = ComponentHeightmap->GetResource();
		if (ensure(HeightmapResource))
		{
			// We get the overall heightmap size via the resource instead of direct GetSizeX/Y calls because apparently
			// the latter are unreliable while the texture is being built.
			HeightmapOffset = FIntPoint(
				FMath::RoundToInt32(Component->HeightmapScaleBias.Z * HeightmapResource->GetSizeX()),
				FMath::RoundToInt32(Component->HeightmapScaleBias.W * HeightmapResource->GetSizeY()));
		}
			
		// When mips are partially loaded, we need to take that into consideration when merging the heightmap :
		uint32 MipBias = ComponentHeightmap->GetNumMips() - ComponentHeightmap->GetNumResidentMips();

		// Theoretically speaking, all of our component heightmaps should be powers of two when we include the duplicated
		// rows/columns across subsections, so we shouldn't get weird truncation results here...
		HeightmapOffset.X >>= MipBias;
		HeightmapOffset.Y >>= MipBias;
		ComponentSize >>= MipBias;

		// Effective area of the texture affecting this component (because of texture sharing) :
		FIntRect HeightmapSubregion(HeightmapOffset, HeightmapOffset + ComponentSize);
		HeightmapMergeRenderInfo.ComponentHeightmapsToRender.Add(ComponentKey, FTexture2DResourceSubregion(ComponentHeightmap->GetResource()->GetTexture2DResource(), HeightmapSubregion));
	}

	// Create the transform that will go from output target UVs to world space: 
	FVector OutputUVOrigin = InRenderAreaWorldTransform.TransformPosition(FVector(InRenderAreaExtents.Min.X, InRenderAreaExtents.Min.Y, 0.0));
	FVector OutputUVScale = InRenderAreaWorldTransform.GetScale3D() * FVector(InRenderAreaExtents.GetSize(), 1.0);
	FTransform OutputUVToWorld(InRenderAreaWorldTransform.GetRotation(), OutputUVOrigin, OutputUVScale);

	// Create the transform that will go from merged heightmap UVs to world space. Note that this is slightly trickier because
	// vertices in the landscape correspond to pixel centers. So UV (0,0) is not at the minimal landscape vertex, but is instead 
	// half a quad further (one pixel is one quad in size, so the center of the first pixel ends up at the minimal vertex).
	// For related reasons, the size of the merged heightmap in world coordinates is actually one quad bigger in each direction.
	check((ComponentIndicesBoundingRect.Min.X < ComponentIndicesBoundingRect.Max.X) && (ComponentIndicesBoundingRect.Min.Y < ComponentIndicesBoundingRect.Max.Y));
	FVector MergedHeightmapScale = (FVector(ComponentIndicesBoundingRect.Max - ComponentIndicesBoundingRect.Min) * static_cast<double>(ComponentSizeQuads) + 1) 
		* LandscapeTransform.GetScale3D();
	MergedHeightmapScale.Z = 1.0f;
	FVector MergedHeightmapUVOrigin = LandscapeTransform.TransformPosition(FVector(ComponentIndicesBoundingRect.Min) * (double)ComponentSizeQuads - FVector(0.5,0.5,0));
	FTransform MergedHeightmapUVToWorld(LandscapeTransform.GetRotation(), MergedHeightmapUVOrigin, MergedHeightmapScale);

	HeightmapMergeRenderInfo.OutputUVToMergedHeightmapUV = OutputUVToWorld.ToMatrixWithScale() * MergedHeightmapUVToWorld.ToInverseMatrixWithScale();

	// Extract the render thread version of the render target :
	FTextureRenderTarget2DResource* OutputRenderTargetResource = OutRenderTarget->GameThread_GetRenderTargetResource()->GetTextureRenderTarget2DResource();
	check(OutputRenderTargetResource != nullptr);

	ENQUEUE_RENDER_COMMAND(RenderHeightmap)([HeightmapMergeRenderInfo, OutputRenderTargetResource](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("RenderHeightmap"));

		FRDGTextureRef OutputHeightmap = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(OutputRenderTargetResource->GetTextureRHI(), TEXT("MergedHeightmap")));
		RenderMergedLandscape_RenderThread::RenderMergedHeightmap(HeightmapMergeRenderInfo, GraphBuilder, OutputHeightmap);

		GraphBuilder.Execute();
	});
}

#if WITH_EDITOR

FBox ALandscape::GetCompleteBounds() const
{
	if (GetLandscapeInfo())
	{
		return GetLandscapeInfo()->GetCompleteBounds();
	}
	else
	{
		return FBox(EForceInit::ForceInit);
	}
}

void ALandscape::SetUseGeneratedLandscapeSplineMeshesActors(bool bInEnabled)
{
	if (bUseGeneratedLandscapeSplineMeshesActors != bInEnabled)
	{
		Modify();
		bUseGeneratedLandscapeSplineMeshesActors = bInEnabled;
	}
}

bool ALandscape::GetUseGeneratedLandscapeSplineMeshesActors() const
{
	return bUseGeneratedLandscapeSplineMeshesActors;
}

void ALandscapeProxy::OnFeatureLevelChanged(ERHIFeatureLevel::Type NewFeatureLevel)
{
	FlushGrassComponents();

	UpdateAllComponentMaterialInstances();

	if (NewFeatureLevel == ERHIFeatureLevel::ES3_1)
	{
		for (ULandscapeComponent* Component : LandscapeComponents)
		{
			if (Component != nullptr)
			{
				Component->CheckGenerateMobilePlatformData(/*bIsCooking = */ false, /*TargetPlatform = */ nullptr);
			}
		}
	}

	UpdateRenderingMethod();
}
#endif

void ALandscapeProxy::PreSave(const class ITargetPlatform* TargetPlatform)
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS;
	Super::PreSave(TargetPlatform);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS;
}

void ALandscapeProxy::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	Super::PreSave(ObjectSaveContext);

#if WITH_EDITOR
	// Work out whether we have grass or not for the next game run
	BuildGrassMaps();

	for (ULandscapeComponent* Component : LandscapeComponents)
	{
		// Reset flag
		Component->GrassData->bIsDirty = false;
	}

	
	if (ULandscapeInfo* LandscapeInfo = GetLandscapeInfo())
	{
		LandscapeInfo->UpdateNanite(ObjectSaveContext.GetTargetPlatform());
	}
	
	if (ALandscape* Landscape = GetLandscapeActor())
	{
		for (ULandscapeComponent* LandscapeComponent : LandscapeComponents)
		{
			Landscape->ClearDirtyData(LandscapeComponent);

			// Make sure edit layer debug names are synchronized upon save :
			LandscapeComponent->ForEachLayer([&](const FGuid& LayerGuid, FLandscapeLayerComponentData& LayerData)
			{
				if (const FLandscapeLayer* EditLayer = Landscape->GetLayer(LayerGuid))
				{
					LayerData.DebugName = EditLayer->Name;
				}
			});
		}
		UpdateNaniteRepresentation(ObjectSaveContext.GetTargetPlatform());
		UpdateRenderingMethod();
	}

	// Warn if there are any active Physical Material Renders on this Proxy
	// don.boogert todo: We should block or wait for this to complete within some timeout period.
	for (ULandscapeComponent* LandscapeComponent : LandscapeComponents)
	{
		if (LandscapeComponent->PhysicalMaterialTask.IsInProgress())
		{
			UE_LOG(LogLandscape, Warning, TEXT("Physical material render on component: '%s' in progress."), *LandscapeComponent->GetFullName());
		}
	}

	if (LandscapeGuid.IsValid())
	{
		if (ULandscapeInfo* LandscapeInfo = GetLandscapeInfo())
		{
			LandscapeInfo->OnModifiedPackageSaved(GetPackage());
		}
	}

	if (bUseCompressedHeightmapStorage && ObjectSaveContext.IsCooking())
	{
		FString PlatformName = ObjectSaveContext.GetTargetPlatform()->PlatformName();

		// TODO [chris.tchou] : remove the limitation to only work on windows, once we validate other platforms work as well...
		if (PlatformName.StartsWith(TEXT("Windows")))
		{
			for (ULandscapeComponent* LandscapeComponent : LandscapeComponents)
			{
				UTexture2D* Tex = LandscapeComponent->GetHeightmap();
				FVector LandscapeGridScale = GetRootComponent()->GetRelativeScale3D();
				ULandscapeTextureStorageProviderFactory::ApplyTo(Tex, LandscapeGridScale);
			}
		}
	}
#endif // WITH_EDITOR
}

void ALandscapeProxy::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FLandscapeCustomVersion::GUID);
	Ar.UsingCustomVersion(FEditorObjectVersion::GUID);
	Ar.UsingCustomVersion(FFortniteReleaseBranchCustomObjectVersion::GUID);
	Ar.UsingCustomVersion(FFortniteMainBranchObjectVersion::GUID);

#if WITH_EDITORONLY_DATA
	if (Ar.IsLoading() && Ar.CustomVer(FLandscapeCustomVersion::GUID) < FLandscapeCustomVersion::MigrateOldPropertiesToNewRenderingProperties)
	{
		if (LODDistanceFactor_DEPRECATED > 0)
		{
			const float LOD0LinearDistributionSettingMigrationTable[11] = { 1.75f, 1.75f, 1.75f, 1.75f, 1.75f, 1.68f, 1.55f, 1.4f, 1.25f, 1.25f, 1.25f };
			const float LODDLinearDistributionSettingMigrationTable[11] = { 2.0f, 2.0f, 2.0f, 1.65f, 1.35f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f };
			const float LOD0SquareRootDistributionSettingMigrationTable[11] = { 1.75f, 1.6f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f };
			const float LODDSquareRootDistributionSettingMigrationTable[11] = { 2.0f, 1.8f, 1.55f, 1.3f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f, 1.25f };

			if (LODFalloff_DEPRECATED == ELandscapeLODFalloff::Type::Linear)
			{
				LOD0DistributionSetting = LOD0LinearDistributionSettingMigrationTable[FMath::RoundToInt(LODDistanceFactor_DEPRECATED)];
				LODDistributionSetting = LODDLinearDistributionSettingMigrationTable[FMath::RoundToInt(LODDistanceFactor_DEPRECATED)];
			}
			else if (LODFalloff_DEPRECATED == ELandscapeLODFalloff::Type::SquareRoot)
			{
				LOD0DistributionSetting = LOD0SquareRootDistributionSettingMigrationTable[FMath::RoundToInt(LODDistanceFactor_DEPRECATED)];
				LODDistributionSetting = LODDSquareRootDistributionSettingMigrationTable[FMath::RoundToInt(LODDistanceFactor_DEPRECATED)];
			}
		}
	}
#endif
}

void ALandscapeProxy::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	ALandscapeProxy* This = CastChecked<ALandscapeProxy>(InThis);

	Super::AddReferencedObjects(InThis, Collector);

#if WITH_EDITORONLY_DATA
	Collector.AddReferencedObjects(This->MaterialInstanceConstantMap, This);
#endif
}

#if WITH_EDITOR

FName FLandscapeInfoLayerSettings::GetLayerName() const
{
	checkSlow(LayerInfoObj == nullptr || LayerInfoObj->LayerName == LayerName);

	return LayerName;
}

FLandscapeEditorLayerSettings& FLandscapeInfoLayerSettings::GetEditorSettings() const
{
	check(LayerInfoObj);

	ULandscapeInfo* LandscapeInfo = Owner->GetLandscapeInfo();
	return LandscapeInfo->GetLayerEditorSettings(LayerInfoObj);
}

FLandscapeEditorLayerSettings& ULandscapeInfo::GetLayerEditorSettings(ULandscapeLayerInfoObject* LayerInfo) const
{
	ALandscapeProxy* Proxy = GetLandscapeProxy();
	FLandscapeEditorLayerSettings* EditorLayerSettings = Proxy->EditorLayerSettings.FindByKey(LayerInfo);
	if (EditorLayerSettings)
	{
		return *EditorLayerSettings;
	}
	else
	{
		int32 Index = Proxy->EditorLayerSettings.Add(FLandscapeEditorLayerSettings(LayerInfo));
		return Proxy->EditorLayerSettings[Index];
	}
}

void ULandscapeInfo::CreateLayerEditorSettingsFor(ULandscapeLayerInfoObject* LayerInfo)
{
	ForEachLandscapeProxy([LayerInfo](ALandscapeProxy* Proxy)
	{
		FLandscapeEditorLayerSettings* EditorLayerSettings = Proxy->EditorLayerSettings.FindByKey(LayerInfo);
		if (!EditorLayerSettings)
		{
			Proxy->Modify();
			Proxy->EditorLayerSettings.Add(FLandscapeEditorLayerSettings(LayerInfo));
		}
		return true;
	});
}

ULandscapeLayerInfoObject* ULandscapeInfo::GetLayerInfoByName(FName LayerName, ALandscapeProxy* Owner /*= nullptr*/) const
{
	ULandscapeLayerInfoObject* LayerInfo = nullptr;
	for (int32 j = 0; j < Layers.Num(); j++)
	{
		if (Layers[j].LayerInfoObj && Layers[j].LayerInfoObj->LayerName == LayerName
			&& (Owner == nullptr || Layers[j].Owner == Owner))
		{
			LayerInfo = Layers[j].LayerInfoObj;
		}
	}
	return LayerInfo;
}

int32 ULandscapeInfo::GetLayerInfoIndex(ULandscapeLayerInfoObject* LayerInfo, ALandscapeProxy* Owner /*= nullptr*/) const
{
	for (int32 j = 0; j < Layers.Num(); j++)
	{
		if (Layers[j].LayerInfoObj && Layers[j].LayerInfoObj == LayerInfo
			&& (Owner == nullptr || Layers[j].Owner == Owner))
		{
			return j;
		}
	}

	return INDEX_NONE;
}

int32 ULandscapeInfo::GetLayerInfoIndex(FName LayerName, ALandscapeProxy* Owner /*= nullptr*/) const
{
	for (int32 j = 0; j < Layers.Num(); j++)
	{
		if (Layers[j].GetLayerName() == LayerName
			&& (Owner == nullptr || Layers[j].Owner == Owner))
		{
			return j;
		}
	}

	return INDEX_NONE;
}


bool ULandscapeInfo::UpdateLayerInfoMapInternal(ALandscapeProxy* Proxy, bool bInvalidate)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ULandscapeInfo::UpdateLayerInfoMapInternal);

	bool bHasCollision = false;
	if (GIsEditor)
	{
		if (Proxy)
		{
			if (bInvalidate)
			{
				// this is a horribly dangerous combination of parameters...

				for (int32 i = 0; i < Layers.Num(); i++)
				{
					if (Layers[i].Owner == Proxy)
					{
						Layers.RemoveAt(i--);
					}
				}
			}
			else // Proxy && !bInvalidate
			{
				TArray<FName> LayerNames = Proxy->GetLayersFromMaterial();

				// Validate any existing layer infos owned by this proxy
				for (int32 i = 0; i < Layers.Num(); i++)
				{
					if (Layers[i].Owner == Proxy)
					{
						Layers[i].bValid = LayerNames.Contains(Layers[i].GetLayerName());
					}
				}

				// Add placeholders for any unused material layers
				for (int32 i = 0; i < LayerNames.Num(); i++)
				{
					int32 LayerInfoIndex = GetLayerInfoIndex(LayerNames[i]);
					if (LayerInfoIndex == INDEX_NONE)
					{
						FLandscapeInfoLayerSettings LayerSettings(LayerNames[i], Proxy);
						LayerSettings.bValid = true;
						Layers.Add(LayerSettings);
					}
				}

				// Populate from layers used in components
				for (int32 ComponentIndex = 0; ComponentIndex < Proxy->LandscapeComponents.Num(); ComponentIndex++)
				{
					ULandscapeComponent* Component = Proxy->LandscapeComponents[ComponentIndex];

					// Add layers from per-component override materials
					if ((Component != nullptr) && (Component->OverrideMaterial != nullptr))
					{
						TArray<FName> ComponentLayerNames = Proxy->GetLayersFromMaterial(Component->OverrideMaterial);
						for (int32 i = 0; i < ComponentLayerNames.Num(); i++)
						{
							int32 LayerInfoIndex = GetLayerInfoIndex(ComponentLayerNames[i]);
							if (LayerInfoIndex == INDEX_NONE)
							{
								FLandscapeInfoLayerSettings LayerSettings(ComponentLayerNames[i], Proxy);
								LayerSettings.bValid = true;
								Layers.Add(LayerSettings);
							}
						}
					}

					const TArray<FWeightmapLayerAllocationInfo>& ComponentWeightmapLayerAllocations = Component->GetWeightmapLayerAllocations();

					for (int32 AllocationIndex = 0; AllocationIndex < ComponentWeightmapLayerAllocations.Num(); AllocationIndex++)
					{
						ULandscapeLayerInfoObject* LayerInfo = ComponentWeightmapLayerAllocations[AllocationIndex].LayerInfo;
						if (LayerInfo)
						{
							int32 LayerInfoIndex = GetLayerInfoIndex(LayerInfo);
							bool bValid = LayerNames.Contains(LayerInfo->LayerName);

							if (bValid)
							{
								//LayerInfo->IsReferencedFromLoadedData = true;
							}

							if (LayerInfoIndex != INDEX_NONE)
							{
								FLandscapeInfoLayerSettings& LayerSettings = Layers[LayerInfoIndex];

								// Valid layer infos take precedence over invalid ones
								// Landscape Actors take precedence over Proxies
								if ((bValid && !LayerSettings.bValid)
									|| (bValid == LayerSettings.bValid && Proxy->IsA<ALandscape>()))
								{
									LayerSettings.Owner = Proxy;
									LayerSettings.bValid = bValid;
									LayerSettings.ThumbnailMIC = nullptr;
								}
							}
							else
							{
								// handle existing placeholder layers
								LayerInfoIndex = GetLayerInfoIndex(LayerInfo->LayerName);
								if (LayerInfoIndex != INDEX_NONE)
								{
									FLandscapeInfoLayerSettings& LayerSettings = Layers[LayerInfoIndex];

									//if (LayerSettings.Owner == Proxy)
									{
										LayerSettings.Owner = Proxy;
										LayerSettings.LayerInfoObj = LayerInfo;
										LayerSettings.bValid = bValid;
										LayerSettings.ThumbnailMIC = nullptr;
									}
								}
								else
								{
									FLandscapeInfoLayerSettings LayerSettings(LayerInfo, Proxy);
									LayerSettings.bValid = bValid;
									Layers.Add(LayerSettings);
								}
							}
						}
					}
				}

				// Add any layer infos cached in the actor
				Proxy->EditorLayerSettings.RemoveAll([](const FLandscapeEditorLayerSettings& Settings) { return Settings.LayerInfoObj == nullptr; });
				for (int32 i = 0; i < Proxy->EditorLayerSettings.Num(); i++)
				{
					FLandscapeEditorLayerSettings& EditorLayerSettings = Proxy->EditorLayerSettings[i];
					if (LayerNames.Contains(EditorLayerSettings.LayerInfoObj->LayerName))
					{
						// intentionally using the layer name here so we don't add layer infos from
						// the cache that have the same name as an actual assignment from a component above
						int32 LayerInfoIndex = GetLayerInfoIndex(EditorLayerSettings.LayerInfoObj->LayerName);
						if (LayerInfoIndex != INDEX_NONE)
						{
							FLandscapeInfoLayerSettings& LayerSettings = Layers[LayerInfoIndex];
							if (LayerSettings.LayerInfoObj == nullptr)
							{
								LayerSettings.Owner = Proxy;
								LayerSettings.LayerInfoObj = EditorLayerSettings.LayerInfoObj;
								LayerSettings.bValid = true;
							}
						}
					}
					else
					{
						Proxy->Modify();
						Proxy->EditorLayerSettings.RemoveAt(i--);
					}
				}

				// Add Visibility Layer info if not initialized
				if (ALandscapeProxy::VisibilityLayer != nullptr)
				{
					int32 LayerInfoIndex = GetLayerInfoIndex(ALandscapeProxy::VisibilityLayer->LayerName);

					if ((LayerInfoIndex != INDEX_NONE) && (Layers[LayerInfoIndex].LayerInfoObj == nullptr))
					{
						Layers[LayerInfoIndex].LayerInfoObj = ALandscapeProxy::VisibilityLayer;
					}
				}
			}
		}
		else // !Proxy
		{
			Layers.Empty();

			if (!bInvalidate)
			{
				ForEachLandscapeProxy([this](ALandscapeProxy* EachProxy)
				{
					if (!EachProxy->IsPendingKillPending())
					{
						checkSlow(EachProxy->GetLandscapeInfo() == this);
						UpdateLayerInfoMapInternal(EachProxy, false);
					}
					return true;
				});
			}
		}
	}
	return bHasCollision;
}

bool ULandscapeInfo::UpdateLayerInfoMap(ALandscapeProxy* Proxy /*= nullptr*/, bool bInvalidate /*= false*/)
{
	bool bResult = UpdateLayerInfoMapInternal(Proxy, bInvalidate);
	if (GIsEditor)
	{
		ALandscape* Landscape = LandscapeActor.Get();
		if (Landscape && Landscape->HasLayersContent())
		{
			Landscape->RequestLayersInitialization(/*bInRequestContentUpdate*/false);
		}
	}
	return bResult;
}

#endif // WITH_EDITOR

/* if the outer world is instanced, we need to change our landscape guid(in a deterministic way)
 * this avoids guid collisions when you instance a world (and its landscapes) multiple times,
 * while maintaining the same GUID between landscape proxy objects within an instance
 */ 
void ALandscapeProxy::PostLoadFixupLandscapeGuidsIfInstanced()
{
	// record the original value before modification
	check(!OriginalLandscapeGuid.IsValid() || (OriginalLandscapeGuid == LandscapeGuid));
	OriginalLandscapeGuid = this->LandscapeGuid;

	// we shouldn't be dealing with any instanced landscapes in these cases, early out
	if (this->IsTemplate() || IsRunningCookCommandlet())
	{
		return;
	}

	UWorldPartition* WorldPartition = FWorldPartitionHelpers::GetWorldPartition(this);
	UWorld* OuterWorld = WorldPartition ? WorldPartition->GetTypedOuter<UWorld>() : this->GetTypedOuter<UWorld>();

	// TODO [chris.tchou] : Note this is not 100% correct, IsInstanced() returns TRUE when using PIE on non-instanced landscapes.
	// That is generally ok however, as the GUID remaps are still deterministic and landscape still works.
	if (OuterWorld && OuterWorld->IsInstanced())
	{
		FArchiveMD5 Ar;
		FGuid OldLandscapeGuid = this->LandscapeGuid;
		Ar << OldLandscapeGuid;

		UPackage* OuterWorldPackage = OuterWorld->GetPackage();
		if (ensure(OuterWorldPackage))
		{
			FName PackageName = OuterWorldPackage->GetFName();
			Ar << PackageName;
		}

		this->LandscapeGuid = Ar.GetGuidFromHash();
	}
}

void ALandscapeProxy::PostLoad()
{
	Super::PostLoad();

	PostLoadFixupLandscapeGuidsIfInstanced();

#if WITH_EDITOR
	FixupOverriddenSharedProperties();

	ALandscape* LandscapeActor = GetLandscapeActor();

	// Try to fixup shared properties if everything is ready for it as some postload process may depend on it.
	if ((GetLandscapeInfo() != nullptr) && (LandscapeActor != nullptr) && (LandscapeActor != this))
	{
		const bool bMapCheck = true;
		FixupSharedData(LandscapeActor, bMapCheck);
	}
#endif // WITH_EDITOR

	// Temporary
	if (ComponentSizeQuads == 0 && LandscapeComponents.Num() > 0)
	{
		ULandscapeComponent* Comp = LandscapeComponents[0];
		if (Comp)
		{
			ComponentSizeQuads = Comp->ComponentSizeQuads;
			SubsectionSizeQuads = Comp->SubsectionSizeQuads;
			NumSubsections = Comp->NumSubsections;
		}
	}

	if (IsTemplate() == false)
	{
		BodyInstance.FixupData(this);
	}

#if WITH_EDITOR
	PRAGMA_DISABLE_DEPRECATION_WARNINGS;
	if (!LandscapeMaterialsOverride_DEPRECATED.IsEmpty())
	{
		PerLODOverrideMaterials.Reserve(LandscapeMaterialsOverride_DEPRECATED.Num());
		for (const FLandscapeProxyMaterialOverride& LocalMaterialOverride : LandscapeMaterialsOverride_DEPRECATED)
		{
			PerLODOverrideMaterials.Add({ LocalMaterialOverride.LODIndex.Default, LocalMaterialOverride.Material });
		}
		LandscapeMaterialsOverride_DEPRECATED.Reset();
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS;

	if (GIsEditor)
	{
		// We may not have run PostLoad on LandscapeComponents yet
		for (TObjectPtr<ULandscapeComponent>& LandscapeComponent : LandscapeComponents)
		{
			if (LandscapeComponent)
			{
				LandscapeComponent->ConditionalPostLoad();
			}
		}
		
		// We may not have run PostLoad on CollisionComponent yet
		for (TObjectPtr<ULandscapeHeightfieldCollisionComponent>& CollisionComponent : CollisionComponents)
		{
			if (CollisionComponent)
			{
				CollisionComponent->ConditionalPostLoad();
			}
		}

		if ((GetLinker() && (GetLinker()->UEVer() < VER_UE4_LANDSCAPE_COMPONENT_LAZY_REFERENCES)) ||
			LandscapeComponents.Num() != CollisionComponents.Num() ||
			LandscapeComponents.ContainsByPredicate([](ULandscapeComponent* Comp) { return ((Comp != nullptr) && (Comp->GetCollisionComponent() == nullptr)); }) ||
			CollisionComponents.ContainsByPredicate([](ULandscapeHeightfieldCollisionComponent* Comp) { return ((Comp != nullptr) && (Comp->GetRenderComponent() == nullptr)); }))
		{
			// Need to clean up invalid collision and render components
			RecreateCollisionComponents();
		}
	}

	EditorLayerSettings.RemoveAll([](const FLandscapeEditorLayerSettings& Settings) { return Settings.LayerInfoObj == nullptr; });

	if (EditorCachedLayerInfos_DEPRECATED.Num() > 0)
	{
		for (int32 i = 0; i < EditorCachedLayerInfos_DEPRECATED.Num(); i++)
		{
			EditorLayerSettings.Add(FLandscapeEditorLayerSettings(EditorCachedLayerInfos_DEPRECATED[i]));
		}
		EditorCachedLayerInfos_DEPRECATED.Empty();
	}

	bool bFixedUpInvalidMaterialInstances = false;

	UMaterial* BaseLandscapeMaterial = GetLandscapeMaterial()->GetMaterial();

	for (ULandscapeComponent* Comp : LandscapeComponents)
	{
		if (Comp == nullptr)
		{
			continue;
		}

		// Validate the layer combination and store it in the MaterialInstanceConstantMap
		UMaterialInstance* MaterialInstance = Comp->GetMaterialInstance(0, false);

		if (MaterialInstance == nullptr)
		{
			continue;
		}

		UMaterialInstanceConstant* CombinationMaterialInstance = Cast<UMaterialInstanceConstant>(MaterialInstance->Parent);
		// Only validate if uncooked and in the editor/commandlet mode (we cannot re-build material instance constants if this is not the case : see UMaterialInstance::CacheResourceShadersForRendering, which is only called if FApp::CanEverRender() returns true) 
		if (!Comp->GetOutermost()->HasAnyPackageFlags(PKG_FilterEditorOnly) && (GIsEditor && FApp::CanEverRender()))
		{
			// MaterialInstance is different from the used LandscapeMaterial, we need to update the material as we cannot properly validate used combinations.
			if (MaterialInstance->GetMaterial() != BaseLandscapeMaterial)
			{
				Comp->UpdateMaterialInstances();
				bFixedUpInvalidMaterialInstances = true;
				continue;
			}

			if (Comp->ValidateCombinationMaterial(CombinationMaterialInstance))
			{
				MaterialInstanceConstantMap.Add(*ULandscapeComponent::GetLayerAllocationKey(Comp->GetWeightmapLayerAllocations(), CombinationMaterialInstance->Parent), CombinationMaterialInstance);
			}
			else
			{
				// There was a problem with the loaded material : it doesn't match the expected material combination, we need to regenerate the material instances : 
				Comp->UpdateMaterialInstances();
				bFixedUpInvalidMaterialInstances = true;
			}
		}
		else if (CombinationMaterialInstance)
		{
			// Skip ValidateCombinationMaterial
			MaterialInstanceConstantMap.Add(*ULandscapeComponent::GetLayerAllocationKey(Comp->GetWeightmapLayerAllocations(), CombinationMaterialInstance->Parent), CombinationMaterialInstance);
		}
	}

	if (bFixedUpInvalidMaterialInstances)
	{
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("LandscapeName"), FText::FromString(GetPathName()));
		Arguments.Add(TEXT("ProxyPackage"), FText::FromString(GetOutermost()->GetName()));
		FMessageLog("MapCheck").Info()
			->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_Message_FixedUpInvalidLandscapeMaterialInstances", "{LandscapeName} : Fixed up invalid landscape material instances. Please re-save {ProxyPackage}."), Arguments)))
			->AddToken(FMapErrorToken::Create(FMapErrors::FixedUpInvalidLandscapeMaterialInstances));
	}
	UWorld* World = GetWorld();

	// track feature level change to flush grass cache
	if (World)
	{
		FOnFeatureLevelChanged::FDelegate FeatureLevelChangedDelegate = FOnFeatureLevelChanged::FDelegate::CreateUObject(this, &ALandscapeProxy::OnFeatureLevelChanged);
		FeatureLevelChangedDelegateHandle = World->AddOnFeatureLevelChangedHandler(FeatureLevelChangedDelegate);
	}
	RepairInvalidTextures();
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (NaniteComponent_DEPRECATED)
	{
		NaniteComponents.Add(NaniteComponent_DEPRECATED);
		NaniteComponent_DEPRECATED = nullptr;
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	// Handle Nanite representation invalidation on load: 
	if (!HasAnyFlags(RF_ClassDefaultObject) && !FPlatformProperties::RequiresCookedData())
	{
		// FFortniteReleaseBranchCustomObjectVersion::FixupNaniteLandscapeMeshes : Fixup Nanite meshes which were using the wrong material and didn't have proper UVs
		// FFortniteReleaseBranchCustomObjectVersion::RemoveUselessLandscapeMeshesCookedCollisionData : Remove cooked collision data from Nanite landscape meshes, since collisions are handled by ULandscapeHeightfieldCollisionComponent
		// FFortniteReleaseBranchCustomObjectVersion::FixNaniteLandscapeMeshNames : Fix the names of the generated Nanite landscape UStaticMesh so that it's unique in a given package
		// FFortniteMainBranchObjectVersion::FixNaniteLandscapeMeshDDCKey : Fix the non-deterministic hash being used by the generated Nanite landscape UStaticMesh so that it can benefit from DDC sharing if it's identical to a previously uploaded mesh derived data
		if ((GetLinkerCustomVersion(FFortniteReleaseBranchCustomObjectVersion::GUID) < FFortniteReleaseBranchCustomObjectVersion::FixNaniteLandscapeMeshNames)
			|| (GetLinkerCustomVersion(FFortniteMainBranchObjectVersion::GUID) < FFortniteMainBranchObjectVersion::FixNaniteLandscapeMeshDDCKey)
			|| CVarForceInvalidateNaniteOnLoad->GetBool())
		{
			// This will force the Nanite meshes to be properly regenerated during the next save :
			InvalidateNaniteRepresentation(/* bCheckContentId = */ false);
		}
		else
		{
			// On load, get rid of the Nanite representation if it's not up-to-date so that it's marked as needing an update and will get fixed by the user when building Nanite data
			InvalidateNaniteRepresentation(/* bCheckContentId = */ true);
		}

		// Remove RF_Transactional from Nanite components : they're re-created upon transacting now : 
		ClearNaniteTransactional();
	}
#endif // WITH_EDITOR

	UpdateRenderingMethod();
}

FIntPoint ALandscapeProxy::GetSectionBaseOffset() const
{
	return LandscapeSectionOffset;
}

#if WITH_EDITOR

void ALandscapeProxy::Destroyed()
{
	Super::Destroyed();

	UWorld* World = GetWorld();

	if (GIsEditor)
	{
		ULandscapeInfo::RecreateLandscapeInfo(World, false);

		if (SplineComponent)
		{
			SplineComponent->ModifySplines();
		}

		NumComponentsNeedingGrassMapRender = 0;
		TotalTexturesToStreamForVisibleGrassMapRender -= NumTexturesToStreamForVisibleGrassMapRender;
		NumTexturesToStreamForVisibleGrassMapRender = 0;
	}

	// Destroy the Nanite component when we get destroyed so that we don't restore a garbage Nanite component (it's non-transactional and will get regenerated anyway)
	InvalidateNaniteRepresentation(/* bInCheckContentId = */false);

	// unregister feature level changed handler for grass
	if (FeatureLevelChangedDelegateHandle.IsValid())
	{
		World->RemoveOnFeatureLevelChangedHandler(FeatureLevelChangedDelegateHandle);
		FeatureLevelChangedDelegateHandle.Reset();
	}
}
#endif // WITH_EDITOR

void ALandscapeProxy::GetSharedProperties(ALandscapeProxy* Landscape)
{
	if (Landscape)
	{
		Modify();

		LandscapeGuid = Landscape->LandscapeGuid;
		OriginalLandscapeGuid = Landscape->OriginalLandscapeGuid;

		//@todo UE merge, landscape, this needs work
		RootComponent->SetRelativeScale3D(Landscape->GetRootComponent()->GetComponentToWorld().GetScale3D());

		StaticLightingResolution = Landscape->StaticLightingResolution;
		CastShadow = Landscape->CastShadow;
		bCastDynamicShadow = Landscape->bCastDynamicShadow;
		bCastStaticShadow = Landscape->bCastStaticShadow;
		bCastContactShadow = Landscape->bCastContactShadow;
		bCastFarShadow = Landscape->bCastFarShadow;
		bCastHiddenShadow = Landscape->bCastHiddenShadow;
		bCastShadowAsTwoSided = Landscape->bCastShadowAsTwoSided;
		bAffectDistanceFieldLighting = Landscape->bAffectDistanceFieldLighting;
		LightingChannels = Landscape->LightingChannels;
		bRenderCustomDepth = Landscape->bRenderCustomDepth;
		CustomDepthStencilWriteMask = Landscape->CustomDepthStencilWriteMask;
		CustomDepthStencilValue = Landscape->CustomDepthStencilValue;
		LDMaxDrawDistance = Landscape->LDMaxDrawDistance;
		ComponentSizeQuads = Landscape->ComponentSizeQuads;
		NumSubsections = Landscape->NumSubsections;
		SubsectionSizeQuads = Landscape->SubsectionSizeQuads;
		MaxLODLevel = Landscape->MaxLODLevel;
		ComponentScreenSizeToUseSubSections = Landscape->ComponentScreenSizeToUseSubSections;
		LODDistributionSetting = Landscape->LODDistributionSetting;
		LOD0DistributionSetting = Landscape->LOD0DistributionSetting;
		LOD0ScreenSize = Landscape->LOD0ScreenSize;
		LODGroupKey = Landscape->LODGroupKey;
		NegativeZBoundsExtension = Landscape->NegativeZBoundsExtension;
		PositiveZBoundsExtension = Landscape->PositiveZBoundsExtension;
		CollisionMipLevel = Landscape->CollisionMipLevel;
		bBakeMaterialPositionOffsetIntoCollision = Landscape->bBakeMaterialPositionOffsetIntoCollision;
		RuntimeVirtualTextures = Landscape->RuntimeVirtualTextures;
		VirtualTextureLodBias = Landscape->VirtualTextureLodBias;
		bVirtualTextureRenderWithQuad = Landscape->bVirtualTextureRenderWithQuad;
		bVirtualTextureRenderWithQuadHQ = Landscape->bVirtualTextureRenderWithQuadHQ;
		VirtualTextureNumLods = Landscape->VirtualTextureNumLods;
		VirtualTextureRenderPassType = Landscape->VirtualTextureRenderPassType;
		bEnableNanite = Landscape->bEnableNanite;
		ShadowCacheInvalidationBehavior = Landscape->ShadowCacheInvalidationBehavior;

		bUseCompressedHeightmapStorage = Landscape->bUseCompressedHeightmapStorage;
#if WITH_EDITORONLY_DATA
		bNaniteSkirtEnabled = Landscape->bNaniteSkirtEnabled;
		NaniteSkirtDepth = Landscape->NaniteSkirtDepth;
		NaniteLODIndex = Landscape->NaniteLODIndex;
#endif // WITH_EDITORONLY_DATA

		if (!LandscapeMaterial)
		{
			LandscapeMaterial = Landscape->LandscapeMaterial;
			PerLODOverrideMaterials = Landscape->PerLODOverrideMaterials;
		}
		if (!LandscapeHoleMaterial)
		{
			LandscapeHoleMaterial = Landscape->LandscapeHoleMaterial;
		}
		if (!DefaultPhysMaterial)
		{
			DefaultPhysMaterial = Landscape->DefaultPhysMaterial;
		}
		LightmassSettings = Landscape->LightmassSettings;
	}

#if WITH_EDITOR
	if (GIsEditor && Landscape)
	{
		LODDistanceFactor_DEPRECATED = Landscape->LODDistanceFactor_DEPRECATED;
		LODFalloff_DEPRECATED = Landscape->LODFalloff_DEPRECATED;
		if (LandscapeMaterial == Landscape->LandscapeMaterial)
		{
			EditorLayerSettings = Landscape->EditorLayerSettings;
		}
	}
#endif // WITH_EDITOR
}

#if WITH_EDITOR

namespace UE::Landscape::Private
{
	static bool CopyProperty(FProperty* InProperty, UObject* InSourceObject, UObject* InDestinationObject)
	{
		void* SrcValuePtr = InProperty->ContainerPtrToValuePtr<void>(InSourceObject);
		void* DestValuePtr = InProperty->ContainerPtrToValuePtr<void>(InDestinationObject);

		if ((DestValuePtr == nullptr) || (SrcValuePtr == nullptr))
		{
			return false;
		}

		InProperty->CopyCompleteValue(DestValuePtr, SrcValuePtr);

		return true;
	}

	static bool CopyPostEditPropertyByName(const TWeakObjectPtr<ALandscapeProxy>& InLandscapeProxy, const TWeakObjectPtr<ALandscape>& InParentLandscape, const FString& InPropertyName)
	{
		if (!InLandscapeProxy.IsValid() || !InParentLandscape.IsValid())
		{
			return false;
		}

		UClass* LandscapeProxyClass = InLandscapeProxy->GetClass();

		if (LandscapeProxyClass == nullptr)
		{
			return false;
		}

		FProperty* PropertyToCopy = LandscapeProxyClass->FindPropertyByName(FName(InPropertyName));

		if (PropertyToCopy == nullptr)
		{
			return false;
		}

		CopyProperty(PropertyToCopy, InParentLandscape.Get(), InLandscapeProxy.Get());

		// Some properties may need additional processing (ex: LandscapeMaterial), notify the proxy of the change.
		FPropertyChangedEvent PropertyChangedEvent(PropertyToCopy);
		InLandscapeProxy->PostEditChangeProperty(PropertyChangedEvent);

		InLandscapeProxy->Modify();

		return true;
	}

	static void DisplaySynchronizedPropertiesMapcheckWarning(const TArray<FName>& InSynchronizedProperties, const ALandscapeProxy& InSynchronizedProxy, const ALandscapeProxy& InParentLandscape, const bool bAddSilencingMessage = false)
	{
		TStringBuilder<1024> SynchronizedPropertiesStringBuilder;
		ULandscapeSubsystem* LandscapeSubsystem = InSynchronizedProxy.GetWorld() ? InSynchronizedProxy.GetWorld()->GetSubsystem<ULandscapeSubsystem>() : nullptr;
		checkf(LandscapeSubsystem != nullptr, TEXT("DisplaySynchronizedPropertiesMapcheckWarning can only be called when a subsystem is available"));

		for (const FName& SynchronizedProperty : InSynchronizedProperties)
		{
			if (SynchronizedPropertiesStringBuilder.Len() > 0)
			{
				SynchronizedPropertiesStringBuilder.Append(TEXT(", "));
			}

			SynchronizedPropertiesStringBuilder.Append(SynchronizedProperty.ToString());
		}

		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("Proxy"), FText::FromString(InSynchronizedProxy.GetActorNameOrLabel()));
		Arguments.Add(TEXT("Landscape"), FText::FromString(InParentLandscape.GetActorNameOrLabel()));
		TSharedRef<FTokenizedMessage> Message = FMessageLog("MapCheck").Warning()
			->AddToken(FUObjectToken::Create(&InSynchronizedProxy, FText::FromString(InSynchronizedProxy.GetActorNameOrLabel())))
			->AddToken(FTextToken::Create(LOCTEXT("MapCheck_Message_LandscapeProxy_FixupSharedData", "had some shared properties not in sync with its parent landscape actor. This has been fixed but the proxy needs to be saved in order to ensure cooking behaves as expected. ")))
			->AddToken(FActionToken::Create(LOCTEXT("MapCheck_SaveFixedUpData", "Save Modified Landscapes"), LOCTEXT("MapCheck_SaveFixedUpData_Desc", "Saves the modified landscape proxy actors"),
				FOnActionTokenExecuted::CreateUObject(LandscapeSubsystem, &ULandscapeSubsystem::SaveModifiedLandscapes),
				FCanExecuteActionToken::CreateUObject(LandscapeSubsystem, &ULandscapeSubsystem::HasModifiedLandscapes),
				/*bInSingleUse = */false))
			->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_Message_LandscapeProxy_FixupSharedData_SharedProperties", "The following properties were synchronized: {0}."), FText::FromString(SynchronizedPropertiesStringBuilder.ToString()))));
			
		if (bAddSilencingMessage)
		{
			Message->AddToken(FTextToken::Create(LOCTEXT("MapCheck_Message_LandscapeProxy_SilenceWarning", "You can silence this warning and perform the deprecation silently using the landscape.SilenceSharedPropertyDeprecationFixup CVar. ")));
		}

		Message->AddToken(FMapErrorToken::Create(FMapErrors::LandscapeComponentPostLoad_Warning));

		// Show MapCheck window
		FMessageLog("MapCheck").Open(EMessageSeverity::Warning);
	}
} // namespace UE::Landscape::Private

void ALandscapeProxy::CopySharedProperties(ALandscapeProxy* InLandscape)
{
	SynchronizeUnmarkedSharedProperties(InLandscape);

	for (TFieldIterator<FProperty> PropertyIterator(GetClass()); PropertyIterator; ++PropertyIterator)
	{
		FProperty* Property = *PropertyIterator;

		if (Property == nullptr)
		{
			continue;
		}

		if (IsSharedProperty(Property))
		{
			UE::Landscape::Private::CopyProperty(Property, InLandscape, this);
		}
	}
}

TArray<FName> ALandscapeProxy::SynchronizeSharedProperties(ALandscapeProxy* InLandscape)
{
	TArray<FName> SynchronizedProperties = SynchronizeUnmarkedSharedProperties(InLandscape);

	for (TFieldIterator<FProperty> PropertyIterator(GetClass()); PropertyIterator; ++PropertyIterator)
	{
		FProperty* Property = *PropertyIterator;

		if (Property == nullptr)
		{
			continue;
		}

		if ((IsPropertyInherited(Property) ||
			(IsPropertyOverridable(Property) && !IsSharedPropertyOverridden(Property->GetName()))) &&
			!Property->Identical_InContainer(this, InLandscape))
		{
			SynchronizedProperties.Emplace(Property->GetFName());
			UE::Landscape::Private::CopyProperty(Property, InLandscape, this);
		}
	}

	if (!SynchronizedProperties.IsEmpty())
	{
		Modify();
	}

	return SynchronizedProperties;
}

bool ALandscapeProxy::IsSharedProperty(const FName& InPropertyName) const
{
	FProperty* Property = StaticClass()->FindPropertyByName(InPropertyName);

	return IsSharedProperty(Property);
}

bool ALandscapeProxy::IsSharedProperty(const FProperty* InProperty) const
{
	return IsPropertyInherited(InProperty) || IsPropertyOverridable(InProperty);
}

bool ALandscapeProxy::IsPropertyInherited(const FProperty* InProperty) const
{
	if (InProperty == nullptr)
	{
		return false;
	}

	return InProperty->HasMetaData(LandscapeInheritedTag);
}

bool ALandscapeProxy::IsPropertyOverridable(const FProperty* InProperty) const
{
	if (InProperty == nullptr)
	{
		return false;
	}

	return InProperty->HasMetaData(LandscapeOverridableTag);
}
#endif // WITH_EDITOR

UMaterialInterface* ALandscapeProxy::GetLandscapeMaterial(int8 InLODIndex) const
{
	if (InLODIndex != INDEX_NONE)
	{
		UWorld* World = GetWorld();

		if (World != nullptr)
		{
			if (const FLandscapePerLODMaterialOverride* LocalMaterialOverride = PerLODOverrideMaterials.FindByPredicate(
				[InLODIndex](const FLandscapePerLODMaterialOverride& InOverride) { return (InOverride.LODIndex == InLODIndex) && (InOverride.Material != nullptr); }))
			{
				return LocalMaterialOverride->Material;
			}
		}
	}

	return LandscapeMaterial != nullptr ? LandscapeMaterial : UMaterial::GetDefaultMaterial(MD_Surface);
}

UMaterialInterface* ALandscapeProxy::GetLandscapeHoleMaterial() const
{
	return LandscapeHoleMaterial;
}

UMaterialInterface* ALandscapeStreamingProxy::GetLandscapeMaterial(int8 InLODIndex) const
{
	if (InLODIndex != INDEX_NONE)
	{
		UWorld* World = GetWorld();

		if (World != nullptr)
		{
			if (const FLandscapePerLODMaterialOverride* LocalMaterialOverride = PerLODOverrideMaterials.FindByPredicate(
				[InLODIndex](const FLandscapePerLODMaterialOverride& InOverride) { return (InOverride.LODIndex == InLODIndex) && (InOverride.Material != nullptr); }))
			{
				return LocalMaterialOverride->Material;
			}
		}
	}

	if (LandscapeMaterial != nullptr)
	{
		return LandscapeMaterial;
	}

	if (const ALandscape* Landscape = GetLandscapeActor())
	{
		return Landscape->GetLandscapeMaterial(InLODIndex);
	}

	return UMaterial::GetDefaultMaterial(MD_Surface);
}

UMaterialInterface* ALandscapeStreamingProxy::GetLandscapeHoleMaterial() const
{
	if (LandscapeHoleMaterial)
	{
		return LandscapeHoleMaterial;
	}
	else if (const ALandscape* Landscape = GetLandscapeActor())
	{
		return Landscape->GetLandscapeHoleMaterial();
	}
	return nullptr;
}

#if WITH_EDITOR

bool ALandscapeStreamingProxy::IsSharedPropertyOverridden(const FString& InPropertyName) const
{
	return OverriddenSharedProperties.Contains(InPropertyName);
}

void ALandscapeStreamingProxy::SetSharedPropertyOverride(const FString& InPropertyName, const bool bIsOverridden)
{
	check(IsSharedProperty(FName(InPropertyName)));

	Modify();

	if (bIsOverridden)
	{
		OverriddenSharedProperties.Add(InPropertyName);
	}
	else
	{
		TWeakObjectPtr<ALandscapeProxy> LandscapeProxy = this;
		TWeakObjectPtr<ALandscape> ParentLandscape = GetLandscapeActor();
		
		if (!ParentLandscape.IsValid())
		{
			UE_LOG(LogLandscape, Warning, TEXT("Unable to retrieve the parent landscape's shared property value (ALandscapeStreamingProxy: %s, Property: %s). The proper value will be fixedup when reloading this proxy."),
				   *GetFullName(), *InPropertyName);
		}
		else
		{
			UE::Landscape::Private::CopyPostEditPropertyByName(LandscapeProxy, ParentLandscape, InPropertyName);
		}

		OverriddenSharedProperties.Remove(InPropertyName);
	}
}

void ALandscapeStreamingProxy::FixupOverriddenSharedProperties()
{
	const UClass* StreamingProxyClass = StaticClass();

	for (const FString& PropertyName : OverriddenSharedProperties)
	{
		const FProperty* Property = StreamingProxyClass->FindPropertyByName(FName(PropertyName));
		checkf(Property != nullptr, TEXT("An overridden property is referenced but cannot be found. Please check this property hasn't been renamed or deprecated and/or provide the proper adapting mechanism."));
	}
}

void ALandscapeProxy::UpgradeSharedProperties(ALandscape* InParentLandscape)
{
	TArray<FName> SynchronizedProperties;
	bool bOpenMapCheckWindow = false;
	ULandscapeInfo* LandscapeInfo = GetLandscapeInfo();
	checkf(LandscapeInfo != nullptr, TEXT("UpgradeSharedProperties can only be called after the proxies are registered to ULandscapeInfo"));

	for (TFieldIterator<FProperty> PropertyIterator(GetClass()); PropertyIterator; ++PropertyIterator)
	{
		FProperty* Property = *PropertyIterator;

		if (Property == nullptr)
		{
			continue;
		}

		if (IsPropertyInherited(Property) && !Property->Identical_InContainer(this, InParentLandscape))
		{
			SynchronizedProperties.Emplace(Property->GetFName());
			UE::Landscape::Private::CopyProperty(Property, InParentLandscape, this);
		}
		else if (IsPropertyOverridable(Property) && !IsSharedPropertyOverridden(Property->GetName()) && !Property->Identical_InContainer(this, InParentLandscape))
		{
			if (CVarSilenceSharedPropertyDeprecationFixup->GetBool())
			{
				SetSharedPropertyOverride(Property->GetName(), true);
			}
			else
			{
				FFormatNamedArguments Arguments;
				TWeakObjectPtr<ALandscapeProxy> LandscapeProxy = this;
				TWeakObjectPtr<ALandscape> ParentLandscape = InParentLandscape;
				const FString PropertyName = Property->GetName();

				bOpenMapCheckWindow = true;

				Arguments.Add(TEXT("Proxy"), FText::FromString(GetActorNameOrLabel()));
				Arguments.Add(TEXT("Landscape"), FText::FromString(InParentLandscape->GetActorNameOrLabel()));
				FMessageLog("MapCheck").Warning()
					->AddToken(FUObjectToken::Create(this, FText::FromString(GetActorNameOrLabel())))
					->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_Message_LandscapeProxy_UpgradeSharedProperties", "Contains a property ({0}) different from parent's landscape actor. Please select between "), FText::FromString(PropertyName))))
					->AddToken(FActionToken::Create(LOCTEXT("MapCheck_OverrideProperty", "Override property"), LOCTEXT("MapCheck_OverrideProperty_Desc", "Keeping the current value and marking the property as overriding the parent landscape's value."),
						FOnActionTokenExecuted::CreateLambda([LandscapeProxy, PropertyName]()
							{
								if (LandscapeProxy.IsValid())
								{
									LandscapeProxy->SetSharedPropertyOverride(PropertyName, true);
								}
							}),
						/*bInSingleUse = */true))
					->AddToken(FTextToken::Create(LOCTEXT("MapCheck_Message_LandscapeProxy_UpgradeSharedProperties_Or", " or ")))
					->AddToken(FActionToken::Create(LOCTEXT("MapCheck_InheritProperty", "Inherit from parent landscape"), LOCTEXT("MapCheck_InheritProperty_Desc", "Copying the parent landscape's value for this property."),
							FOnActionTokenExecuted::CreateLambda([LandscapeProxy, ParentLandscape, PropertyName]()
								{
									UE::Landscape::Private::CopyPostEditPropertyByName(LandscapeProxy, ParentLandscape, PropertyName);
								}),
							/*bInSingleUse = */true))
					->AddToken(FMapErrorToken::Create(FMapErrors::LandscapeComponentPostLoad_Warning));
			}
		}
	}

	if (!SynchronizedProperties.IsEmpty())
	{
		LandscapeInfo->MarkObjectDirty(/*InObject = */this, /*bInForceResave = */true);

		if (!CVarSilenceSharedPropertyDeprecationFixup->GetBool())
		{
			UE::Landscape::Private::DisplaySynchronizedPropertiesMapcheckWarning(SynchronizedProperties, *this, *InParentLandscape, true);
		}
	}

	if (bOpenMapCheckWindow)
	{
		// Show MapCheck window
		FMessageLog("MapCheck").Open(EMessageSeverity::Warning);
	}
}

void ALandscapeProxy::FixupSharedData(ALandscape* Landscape, const bool bMapCheck)
{
	if ((Landscape == nullptr) || (Landscape == this))
	{
		return;
	}

	if (!bUpgradeSharedPropertiesPerformed && GetLinkerCustomVersion(FFortniteReleaseBranchCustomObjectVersion::GUID) < FFortniteReleaseBranchCustomObjectVersion::LandscapeSharedPropertiesEnforcement)
	{
		UpgradeSharedProperties(Landscape);
		bUpgradeSharedPropertiesPerformed = true;
	}
	else
	{
		ULandscapeInfo* LandscapeInfo = GetLandscapeInfo();
		checkf(LandscapeInfo != nullptr, TEXT("FixupSharedData can only be called after the proxies are registered to ULandscapeInfo"));

		TArray<FName> SynchronizedProperties = SynchronizeSharedProperties(Landscape);
		bool bUpdated = !SynchronizedProperties.IsEmpty();

		TSet<FGuid> LayerGuids;
		Algo::Transform(Landscape->LandscapeLayers, LayerGuids, [](const FLandscapeLayer& Layer) { return Layer.Guid; });
		bUpdated |= RemoveObsoleteLayers(LayerGuids);

		for (const FLandscapeLayer& Layer : Landscape->LandscapeLayers)
		{
			bUpdated |= AddLayer(Layer.Guid);
		}

		if (bUpdated)
		{
			// Force resave the proxy through the modified landscape system, so that the user can then use the Save Modified Landscapes menu and therefore manually trigger the re-save of all modified proxies. * /
			bool bNeedsManualResave = LandscapeInfo->MarkObjectDirty(/*InObject = */this, /*bInForceResave = */true);

			if (bMapCheck && bNeedsManualResave)
			{
				UE::Landscape::Private::DisplaySynchronizedPropertiesMapcheckWarning(SynchronizedProperties, *this, *Landscape);
			}
		}
	}
}

void ALandscapeProxy::SetAbsoluteSectionBase(FIntPoint InSectionBase)
{
	FIntPoint Difference = InSectionBase - LandscapeSectionOffset;
	LandscapeSectionOffset = InSectionBase;

	RecreateComponentsRenderState([Difference](ULandscapeComponent* Comp)
	{
		FIntPoint AbsoluteSectionBase = Comp->GetSectionBase() + Difference;
		Comp->SetSectionBase(AbsoluteSectionBase);
	});

	for (int32 CompIdx = 0; CompIdx < CollisionComponents.Num(); CompIdx++)
	{
		ULandscapeHeightfieldCollisionComponent* Comp = CollisionComponents[CompIdx];
		if (Comp)
		{
			FIntPoint AbsoluteSectionBase = Comp->GetSectionBase() + Difference;
			Comp->SetSectionBase(AbsoluteSectionBase);
		}
	}
}

void ALandscapeProxy::RecreateComponentsState()
{
	RecreateComponentsRenderState([](ULandscapeComponent* Comp)
	{
		Comp->UpdateComponentToWorld();
		Comp->UpdateCachedBounds();
		Comp->UpdateBounds();
	});

	for (int32 ComponentIndex = 0; ComponentIndex < CollisionComponents.Num(); ComponentIndex++)
	{
		ULandscapeHeightfieldCollisionComponent* Comp = CollisionComponents[ComponentIndex];
		if (Comp)
		{
			Comp->UpdateComponentToWorld();
			Comp->RecreatePhysicsState();
		}
	}
}

void ALandscapeProxy::RecreateComponentsRenderState(TFunctionRef<void(ULandscapeComponent*)> Fn)
{
	// Batch component render state recreation
	TArray<FComponentRecreateRenderStateContext> ComponentRecreateRenderStates;
	ComponentRecreateRenderStates.Reserve(LandscapeComponents.Num());

	for (int32 ComponentIndex = 0; ComponentIndex < LandscapeComponents.Num(); ComponentIndex++)
	{
		ULandscapeComponent* Comp = LandscapeComponents[ComponentIndex];
		if (Comp)
		{
			Fn(Comp);
			ComponentRecreateRenderStates.Emplace(Comp);
		}
	}
}

bool ULandscapeInfo::GetDirtyOnlyInMode() const
{
	if (const ALandscape* Landscape = LandscapeActor.Get())
	{
		if (const UWorld* World = Landscape->GetWorld())
		{
			if (const ULandscapeSubsystem* LandscapeSubsystem = World->GetSubsystem<ULandscapeSubsystem>())
			{
				return LandscapeSubsystem->GetDirtyOnlyInMode();
			}
		}
		return false;
	}

	return false;
}

FLandscapeDirtyOnlyInModeScope::FLandscapeDirtyOnlyInModeScope(ULandscapeInfo* InLandscapeInfo)
	: LandscapeInfo(InLandscapeInfo)
	, bDirtyOnlyInModePrevious(InLandscapeInfo->bDirtyOnlyInMode)
{
	LandscapeInfo->bDirtyOnlyInMode = LandscapeInfo->GetDirtyOnlyInMode();
}


FLandscapeDirtyOnlyInModeScope::FLandscapeDirtyOnlyInModeScope(ULandscapeInfo* InLandscapeInfo, bool bInOverrideDirtyMode)
	: LandscapeInfo(InLandscapeInfo)
	, bDirtyOnlyInModePrevious(InLandscapeInfo->bDirtyOnlyInMode)
{
	LandscapeInfo->bDirtyOnlyInMode = bInOverrideDirtyMode;
}

FLandscapeDirtyOnlyInModeScope::~FLandscapeDirtyOnlyInModeScope()
{
	LandscapeInfo->bDirtyOnlyInMode = bDirtyOnlyInModePrevious;
}

void ULandscapeInfo::OnModifiedPackageSaved(UPackage* InPackage)
{
	ModifiedPackages.Remove(InPackage);
}

TArray<UPackage*> ULandscapeInfo::GetModifiedPackages() const
{
	TArray<UPackage*> LocalModifiedPackages;
	LocalModifiedPackages.Reserve(ModifiedPackages.Num());
	Algo::TransformIf(ModifiedPackages, LocalModifiedPackages, 
		[](const TWeakObjectPtr<UPackage>& InWeakPackagePtr) { return InWeakPackagePtr.IsValid(); }, 
		[](const TWeakObjectPtr<UPackage>& InWeakPackagePtr) { return InWeakPackagePtr.Get(); });
	return LocalModifiedPackages;
}

void ULandscapeInfo::MarkModifiedPackagesAsDirty()
{
	for (TWeakObjectPtr<UPackage> WeakPackagePtr : ModifiedPackages)
	{
		if (UPackage* Package = WeakPackagePtr.Get())
		{
			Package->MarkPackageDirty();
		}
	}
	ModifiedPackages.Empty();
}

int32 ULandscapeInfo::GetModifiedPackageCount() const
{
	return IntCastChecked<int32>(Algo::CountIf(ModifiedPackages, [](const TWeakObjectPtr<UPackage>& InWeakPackagePtr) { return InWeakPackagePtr.IsValid(); }));
}

bool ULandscapeInfo::TryAddToModifiedPackages(UPackage* InPackage)
{
	ALandscape* LocalLandscapeActor = LandscapeActor.Get();
	check(LocalLandscapeActor);

	// We don't want to bother with packages being marked dirty for anything else than the Editor world 
	if (LocalLandscapeActor->GetWorld()->WorldType != EWorldType::Editor)
	{
		return false;
	}

	// Also don't track packages when rolling back a transaction because they are already dirty anyway 
	if (GIsTransacting)
	{
		return false;
	}

	// Don't consider unsaved packages as modified/not dirty because they will be saved later on anyway. What we're really after are existing packages made dirty on load
	if (FPackageName::IsTempPackage(InPackage->GetName()))
	{
		return false;
	}

	ModifiedPackages.Add(InPackage);
	return true;
}

bool ULandscapeInfo::MarkObjectDirty(UObject* InObject, bool bInForceResave)
{
	check(InObject && (InObject->IsA<ALandscapeProxy>() || InObject->GetTypedOuter<ALandscapeProxy>() != nullptr));

	bool bWasAddedToModifiedPackages = false;
	if (bInForceResave)
	{
		if (!InObject->MarkPackageDirty())
		{
			// When force-resaving (e.g. when syncing must-sync properties on load), unconditionally add the package to the list of packages to save if we couldn't mark it dirty already, so that 
			//  the user can manually resave all that needs to be saved with the Save Modified Landscapes button :
			bWasAddedToModifiedPackages = TryAddToModifiedPackages(InObject->GetPackage());
		}
	}
	else if (bDirtyOnlyInMode)
	{
		ALandscape* LocalLandscapeActor = LandscapeActor.Get();
		check(LocalLandscapeActor);
		if (LocalLandscapeActor->HasLandscapeEdMode())
		{
			InObject->MarkPackageDirty();
		}
		else
		{
			bWasAddedToModifiedPackages = TryAddToModifiedPackages(InObject->GetPackage());
		}
	}
	else
	{
		InObject->MarkPackageDirty();
	}

	return bWasAddedToModifiedPackages;
}

bool ULandscapeInfo::ModifyObject(UObject* InObject, bool bAlwaysMarkDirty)
{
	check(InObject && (InObject->IsA<ALandscapeProxy>() || InObject->GetTypedOuter<ALandscapeProxy>() != nullptr));
	bool bWasAddedToModifiedPackages = false;
	
	if (!bAlwaysMarkDirty)
	{
		InObject->Modify(false);
	}
	else if(!bDirtyOnlyInMode)
	{
		InObject->Modify(true);
	}
	else
	{
		ALandscape* LocalLandscapeActor = LandscapeActor.Get();
		check(LocalLandscapeActor);
		if (LocalLandscapeActor->HasLandscapeEdMode())
		{
			InObject->Modify(true);
		}
		else 
		{
			InObject->Modify(false);
			bWasAddedToModifiedPackages = TryAddToModifiedPackages(InObject->GetPackage());
		}
	}

	return bWasAddedToModifiedPackages;
}

ALandscapeProxy* ULandscapeInfo::GetLandscapeProxyForLevel(ULevel* Level) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ULandscapeInfo::GetLandscapeProxyForLevel);
	ALandscapeProxy* LandscapeProxy = nullptr;
	ForEachLandscapeProxy([&LandscapeProxy, Level](ALandscapeProxy* Proxy) -> bool 
	{
		if (Proxy->GetLevel() == Level)
		{
			LandscapeProxy = Proxy;
			return false;
		}
		return true;
	});
	return LandscapeProxy;
}
      
#endif // WITH_EDITOR

ALandscapeProxy* ULandscapeInfo::GetCurrentLevelLandscapeProxy(bool bRegistered) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ULandscapeInfo::GetCurrentLevelLandscapeProxy);

	ALandscapeProxy* LandscapeProxy = nullptr;
	ForEachLandscapeProxy([&LandscapeProxy, bRegistered](ALandscapeProxy* Proxy) -> bool
	{
		if (!bRegistered || Proxy->GetRootComponent()->IsRegistered())
		{
			UWorld* ProxyWorld = Proxy->GetWorld();
			if (ProxyWorld &&
				ProxyWorld->GetCurrentLevel() == Proxy->GetOuter())
			{
				LandscapeProxy = Proxy;
				return false;
			}
		}
		return true;
	});
	return LandscapeProxy;
}

ALandscapeProxy* ULandscapeInfo::GetLandscapeProxy() const
{
	// Mostly this Proxy used to calculate transformations
	// in Editor all proxies of same landscape actor have root components in same locations
	// so it doesn't really matter which proxy we return here

	// prefer LandscapeActor in case it is loaded
	if (LandscapeActor.IsValid())
	{
		ALandscape* Landscape = LandscapeActor.Get();
		USceneComponent* LandscapeRootComponent = (Landscape != nullptr) ? Landscape->GetRootComponent() : nullptr;

		if ((LandscapeRootComponent != nullptr) && (LandscapeRootComponent->IsRegistered()))
		{
			return Landscape;
		}
	}

	// prefer current level proxy 
	if (ALandscapeProxy* Proxy = GetCurrentLevelLandscapeProxy(true))
	{
		return Proxy;
	}

	// any proxy in the world
	for (TWeakObjectPtr<ALandscapeStreamingProxy> ProxyPtr : StreamingProxies)
	{
		ALandscapeStreamingProxy* Proxy = ProxyPtr.Get();
		USceneComponent* ProxyRootComponent = (Proxy != nullptr) ? Proxy->GetRootComponent() : nullptr;

		if ((ProxyRootComponent != nullptr) && (ProxyRootComponent->IsRegistered()))
		{
			return Proxy;
		}
	}

	return nullptr;
}

#if WITH_EDITOR

void ULandscapeInfo::Reset()
{
	LandscapeActor.Reset();

	StreamingProxies.Empty();
	XYtoComponentMap.Empty();
	XYtoAddCollisionMap.Empty();

	//SelectedComponents.Empty();
	//SelectedRegionComponents.Empty();
	//SelectedRegion.Empty();
}

void ULandscapeInfo::FixupProxiesTransform(bool bDirty)
{
	ALandscape* Landscape = LandscapeActor.Get();

	if (Landscape == nullptr ||
		Landscape->GetRootComponent()->IsRegistered() == false)
	{
		return;
	}

	// Make sure section offset of all proxies is multiple of ALandscapeProxy::ComponentSizeQuads
	for (TWeakObjectPtr<ALandscapeStreamingProxy> ProxyPtr : StreamingProxies)
	{
		ALandscapeProxy* Proxy = ProxyPtr.Get();
		if (!Proxy)
		{
			continue;
		}

		if (bDirty)
		{
			Proxy->Modify();
		}

		FIntPoint LandscapeSectionOffset = Proxy->LandscapeSectionOffset - Landscape->LandscapeSectionOffset;
		FIntPoint LandscapeSectionOffsetRem(
			LandscapeSectionOffset.X % Proxy->ComponentSizeQuads,
			LandscapeSectionOffset.Y % Proxy->ComponentSizeQuads);

		if (LandscapeSectionOffsetRem.X != 0 || LandscapeSectionOffsetRem.Y != 0)
		{
			FIntPoint NewLandscapeSectionOffset = Proxy->LandscapeSectionOffset - LandscapeSectionOffsetRem;

			UE_LOG(LogLandscape, Warning, TEXT("Landscape section base is not multiple of component size, attempted automated fix: '%s', %d,%d vs %d,%d."),
				*Proxy->GetFullName(), Proxy->LandscapeSectionOffset.X, Proxy->LandscapeSectionOffset.Y, NewLandscapeSectionOffset.X, NewLandscapeSectionOffset.Y);

			Proxy->SetAbsoluteSectionBase(NewLandscapeSectionOffset);
		}
	}

	FTransform LandscapeTM = Landscape->LandscapeActorToWorld();
	// Update transformations of all linked landscape proxies
	for (TWeakObjectPtr<ALandscapeStreamingProxy> ProxyPtr : StreamingProxies)
	{
		ALandscapeProxy* Proxy = ProxyPtr.Get();
		if (!Proxy)
		{
			continue;
		}

		FTransform ProxyRelativeTM(FVector(Proxy->LandscapeSectionOffset));
		FTransform ProxyTransform = ProxyRelativeTM * LandscapeTM;

		if (!Proxy->GetTransform().Equals(ProxyTransform))
		{
			Proxy->SetActorTransform(ProxyTransform);

			// Let other systems know that an actor was moved
			GEngine->BroadcastOnActorMoved(Proxy);
		}
	}
}

void ULandscapeInfo::UpdateComponentLayerAllowList()
{
	ForEachLandscapeProxy([](ALandscapeProxy* Proxy)
	{
		for (ULandscapeComponent* Comp : Proxy->LandscapeComponents)
		{
			Comp->UpdateLayerAllowListFromPaintedLayers();
		}
		return true;
	});
}

void ULandscapeInfo::RecreateLandscapeInfo(UWorld* InWorld, bool bMapCheck)
{
	check(InWorld);

	ULandscapeInfoMap& LandscapeInfoMap = ULandscapeInfoMap::GetLandscapeInfoMap(InWorld);
	LandscapeInfoMap.Modify();

	// reset all LandscapeInfo objects
	for (auto& LandscapeInfoPair : LandscapeInfoMap.Map)
	{
		ULandscapeInfo* LandscapeInfo = LandscapeInfoPair.Value;

		if (LandscapeInfo != nullptr)
		{
			LandscapeInfo->Modify();
			LandscapeInfo->Reset();
		}
	}

	TMap<FGuid, TArray<ALandscapeProxy*>> ValidLandscapesMap;
	// Gather all valid landscapes in the world
	for (ALandscapeProxy* Proxy : TActorRange<ALandscapeProxy>(InWorld))
	{
		if (Proxy->GetLevel() &&
			Proxy->GetLevel()->bIsVisible &&
			!Proxy->HasAnyFlags(RF_BeginDestroyed) &&
			IsValid(Proxy) &&
			!Proxy->IsPendingKillPending())
		{
			ValidLandscapesMap.FindOrAdd(Proxy->GetLandscapeGuid()).Add(Proxy);
		}
	}

	// Register landscapes in global landscape map
	for (auto& ValidLandscapesPair : ValidLandscapesMap)
	{
		auto& LandscapeList = ValidLandscapesPair.Value;
		for (ALandscapeProxy* Proxy : LandscapeList)
		{
			Proxy->CreateLandscapeInfo()->RegisterActor(Proxy, bMapCheck);
		}
	}

	// Remove empty entries from global LandscapeInfo map
	for (auto It = LandscapeInfoMap.Map.CreateIterator(); It; ++It)
	{
		ULandscapeInfo* Info = It.Value();

		if (Info != nullptr && Info->GetLandscapeProxy() == nullptr)
		{
			Info->MarkAsGarbage();
			It.RemoveCurrent();
		}
		else if (Info == nullptr) // remove invalid entry
		{
			It.RemoveCurrent();
		}
	}

	// We need to inform Landscape editor tools about LandscapeInfo updates
	FEditorSupportDelegates::WorldChange.Broadcast();
}


#endif // WITH_EDITOR

ULandscapeInfo* ULandscapeInfo::Find(UWorld* InWorld, const FGuid& LandscapeGuid)
{
	ULandscapeInfo* LandscapeInfo = nullptr;

	check(LandscapeGuid.IsValid());
	if (InWorld != nullptr)
	{
		auto& LandscapeInfoMap = ULandscapeInfoMap::GetLandscapeInfoMap(InWorld);
		LandscapeInfo = LandscapeInfoMap.Map.FindRef(LandscapeGuid);
	}
	return LandscapeInfo;
}

ULandscapeInfo* ULandscapeInfo::FindOrCreate(UWorld* InWorld, const FGuid& LandscapeGuid)
{
	ULandscapeInfo* LandscapeInfo = nullptr;

	check(LandscapeGuid.IsValid());
	check(InWorld);

	auto& LandscapeInfoMap = ULandscapeInfoMap::GetLandscapeInfoMap(InWorld);
	LandscapeInfo = LandscapeInfoMap.Map.FindRef(LandscapeGuid);

	if (!LandscapeInfo)
	{
		LandscapeInfo = NewObject<ULandscapeInfo>(GetTransientPackage(), NAME_None, RF_Transactional | RF_Transient);
		LandscapeInfoMap.Modify(false);
		LandscapeInfo->Initialize(InWorld, LandscapeGuid);
		LandscapeInfoMap.Map.Add(LandscapeGuid, LandscapeInfo);
	}
	check(LandscapeInfo);
	return LandscapeInfo;
}

void ULandscapeInfo::Initialize(UWorld* InWorld, const FGuid& InLandscapeGuid)
{
	LandscapeGuid = InLandscapeGuid;
}

void ULandscapeInfo::ForAllLandscapeProxies(TFunctionRef<void(ALandscapeProxy*)> Fn) const
{
	if (ALandscape* Landscape = LandscapeActor.Get())
	{
		Fn(Landscape);
	}

	for (TWeakObjectPtr<ALandscapeStreamingProxy> StreamingProxyPtr : StreamingProxies)
	{
		if (ALandscapeProxy* LandscapeProxy = StreamingProxyPtr.Get())
		{
			Fn(LandscapeProxy);
		}
	}
}

void ULandscapeInfo::ForEachLandscapeProxy(TFunctionRef<bool(ALandscapeProxy*)> Fn) const
{
	if (ALandscape* Landscape = LandscapeActor.Get())
	{
		if (!Fn(Landscape))
		{
			return;
		}
	}

	for (TWeakObjectPtr<ALandscapeStreamingProxy> StreamingProxyPtr : StreamingProxies)
	{
		if (ALandscapeProxy* LandscapeProxy = StreamingProxyPtr.Get())
		{
			if (!Fn(LandscapeProxy))
			{
				return;
			}
		}
	}
}

void ULandscapeInfo::UpdateNanite(const ITargetPlatform* InTargetPlatform)
{
	ALandscape* Landscape = LandscapeActor.Get();
	if (!Landscape)
	{
		return;
	}

#if WITH_EDITOR
	if (!Landscape->IsNaniteEnabled())
	{
		return;
	}

	UWorld* World = nullptr;
	FGraphEventArray AsyncEvents;
	ForEachLandscapeProxy([&AsyncEvents, InTargetPlatform, &World](ALandscapeProxy* LandscapeProxy)
		{
			AsyncEvents.Add(LandscapeProxy->UpdateNaniteRepresentationAsync(InTargetPlatform));
			if (!World)
			{
				World = LandscapeProxy->GetWorld();
			}
			
			return true;
		});


	if (!World)
	{
		return;
	}

	FGraphEventRef WaitForAllNaniteUpdates = FFunctionGraphTask::CreateAndDispatchWhenReady([]() {
		},
		TStatId(),
			&AsyncEvents,
			ENamedThreads::GameThread);

	// we have to drain the game thread tasks and static mesh builds
	ULandscapeSubsystem* LandscapeSubsystem = World->GetSubsystem<ULandscapeSubsystem>();
	while (LandscapeSubsystem->AreNaniteBuildsInProgress())
	{
		ENamedThreads::Type CurrentThread = FTaskGraphInterface::Get().GetCurrentThreadIfKnown();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(CurrentThread);
		FAssetCompilingManager::Get().ProcessAsyncTasks();
	}
#endif //WITH_EDITOR

}

bool ULandscapeInfo::IsRegistered(const ALandscapeProxy* Proxy) const
{
	if (Proxy == nullptr)
		return false;

	bool bResult = false;
	if (Proxy->IsA<ALandscape>())
	{
		bResult = (LandscapeActor.Get() == Proxy);
	}
	else if (const ALandscapeStreamingProxy *StreamingProxy = Cast<ALandscapeStreamingProxy>(Proxy))
	{
		TWeakObjectPtr<const ALandscapeStreamingProxy> StreamingProxyPtr = StreamingProxy;
		bResult = StreamingProxies.Contains(StreamingProxyPtr);
	}

#if WITH_EDITORONLY_DATA
	// NOTE: during an Undo operation, the LandscapeActor/StreamingProxies are transacted, and the registration status may be restored
	// however, in that case, the Proxy is NOT fully registered yet, because some other data in LandscapeInfo still needs to be updated (XY maps for instance are not transacted)
	// so we trust the bIsRegisteredWithLandscapeInfo flag over the actual pointers.

	// at minimum, if the proxy flag says it is registered, then the pointers should definitely be valid
	if (Proxy->bIsRegisteredWithLandscapeInfo)
	{
		check(bResult == Proxy->bIsRegisteredWithLandscapeInfo);
	}

	// trust the proxy flag over the landscape info pointers
	bResult = Proxy->bIsRegisteredWithLandscapeInfo;
#endif // WITH_EDITORONLY_DATA

	return bResult;
}

void ULandscapeInfo::RegisterActor(ALandscapeProxy* Proxy, bool bMapCheck, bool bUpdateAllAddCollisions)
{
	UWorld* OwningWorld = Proxy->GetWorld();
	// do not pass here invalid actors
	checkSlow(Proxy);
	check(Proxy->GetLandscapeGuid().IsValid());
	check(LandscapeGuid.IsValid());
	
	// in case this Info object is not initialized yet
	// initialized it with properties from passed actor
	if (GetLandscapeProxy() == nullptr)
	{
		ComponentSizeQuads = Proxy->ComponentSizeQuads;
		ComponentNumSubsections = Proxy->NumSubsections;
		SubsectionSizeQuads = Proxy->SubsectionSizeQuads;
		DrawScale = Proxy->GetRootComponent() != nullptr ? Proxy->GetRootComponent()->GetRelativeScale3D() : FVector(100.0f);
	}

	// check that passed actor matches all shared parameters
	check(LandscapeGuid == Proxy->GetLandscapeGuid());
	check(ComponentSizeQuads == Proxy->ComponentSizeQuads);
	check(ComponentNumSubsections == Proxy->NumSubsections);
	check(SubsectionSizeQuads == Proxy->SubsectionSizeQuads);

	if (Proxy->GetRootComponent() != nullptr && !DrawScale.Equals(Proxy->GetRootComponent()->GetRelativeScale3D()))
	{
		UE_LOG(LogLandscape, Warning, TEXT("Landscape proxy (%s) scale (%s) does not match to main actor scale (%s)."),
			*Proxy->GetName(), *Proxy->GetRootComponent()->GetRelativeScale3D().ToCompactString(), *DrawScale.ToCompactString());
	}

	// register
	if (ALandscape* Landscape = Cast<ALandscape>(Proxy))
	{
		if (!LandscapeActor.IsValid())
		{
			LandscapeActor = Landscape;

#if WITH_EDITOR
			// Update registered splines so they can pull the actor pointer
			for (TScriptInterface<ILandscapeSplineInterface> SplineActor : SplineActors)
			{
				SplineActor->UpdateSharedProperties(this);
			}

			// In world composition user is not allowed to move landscape in editor, only through WorldBrowser 
			bool bIsLockLocation = LandscapeActor->IsLockLocation();
			bIsLockLocation |= OwningWorld != nullptr ? OwningWorld->WorldComposition != nullptr : false;
			LandscapeActor->SetLockLocation(bIsLockLocation);
#endif // WITH_EDITOR

			// update proxies reference actor
			for (TWeakObjectPtr<ALandscapeStreamingProxy> StreamingProxyPtr : StreamingProxies)
			{
				if (ALandscapeStreamingProxy* StreamingProxy = StreamingProxyPtr.Get())
				{
					StreamingProxy->SetLandscapeActor(LandscapeActor.Get());
#if WITH_EDITOR
					StreamingProxy->FixupSharedData(Landscape, bMapCheck);
#endif // WITH_EDITOR
					StreamingProxy->SetLODGroupKeyInternal(Landscape->LODGroupKey);
				}
			}
		}
		else if (LandscapeActor != Landscape)
		{
			UE_LOG(LogLandscape, Warning, TEXT("Multiple landscape actors with the same GUID detected: %s vs %s"), * LandscapeActor->GetPathName(), * Landscape->GetPathName());
		}
	}
	else
	{
		auto LamdbdaLowerBound = [](TWeakObjectPtr<ALandscapeProxy> APtr, TWeakObjectPtr<ALandscapeProxy> BPtr)
		{
			ALandscapeProxy *A = APtr.Get();
			ALandscapeProxy *B = BPtr.Get();

			// sort nulls, assuming null < !null
			if (!A || !B)
			{
				return !!B;
			}
			
			FIntPoint SectionBaseA = A->GetSectionBaseOffset();
			FIntPoint SectionBaseB = B->GetSectionBaseOffset();

			if (SectionBaseA.X != SectionBaseB.X)
			{
				return SectionBaseA.X < SectionBaseB.X;
			}

			return SectionBaseA.Y < SectionBaseB.Y;
		};

		// Insert Proxies in a sorted fashion into the landscape info Proxies list, for generating deterministic results in the Layer system
		ALandscapeStreamingProxy* StreamingProxy = CastChecked<ALandscapeStreamingProxy>(Proxy);
		TWeakObjectPtr<ALandscapeStreamingProxy> StreamingProxyPtr = StreamingProxy;
		if (!StreamingProxies.Contains(StreamingProxyPtr))
		{
			// NOTE: if a streaming proxy somehow gets garbage collected without de-registering from the Proxies list, then
			// this search may return a non-deterministic index because the Proxies list will contain a null
			uint32 InsertIndex = Algo::LowerBound(StreamingProxies, StreamingProxyPtr, LamdbdaLowerBound);
			StreamingProxies.Insert(StreamingProxyPtr, InsertIndex);
		}

		if (LandscapeActor.IsValid())	// don't overwrite the proxy's landscape actor if we don't have one registered yet
		{
			StreamingProxy->SetLandscapeActor(LandscapeActor.Get());
			StreamingProxy->SetLODGroupKeyInternal(LandscapeActor.Get()->LODGroupKey);
#if WITH_EDITOR
			StreamingProxy->FixupSharedData(LandscapeActor.Get(), bMapCheck);
#endif // WITH_EDITOR
		}
	}

#if WITH_EDITOR
	UpdateLayerInfoMap(Proxy);
	if(bUpdateAllAddCollisions)
	{
		UpdateAllAddCollisions();
	}
	RegisterSplineActor(Proxy);
#endif // WITH_EDITOR

	//
	// add proxy components to the XY map
	//
	for (int32 CompIdx = 0; CompIdx < Proxy->LandscapeComponents.Num(); ++CompIdx)
	{
		RegisterActorComponent(Proxy->LandscapeComponents[CompIdx], bMapCheck);
	}

	for (ULandscapeHeightfieldCollisionComponent* CollComp : Proxy->CollisionComponents)
	{
		RegisterCollisionComponent(CollComp);
	}

#if WITH_EDITORONLY_DATA
	Proxy->bIsRegisteredWithLandscapeInfo = true;
#endif // WITH_EDITORONLY_DATA
}

void ULandscapeInfo::UnregisterActor(ALandscapeProxy* Proxy)
{
	UWorld* OwningWorld = Proxy->GetWorld();
	if (ALandscape* Landscape = Cast<ALandscape>(Proxy))
	{
		// Note: UnregisterActor sometimes gets triggered twice, e.g. it has been observed to happen during undo/ redo
		// Note: In some cases LandscapeActor could be updated to a new landscape actor before the old landscape is unregistered/destroyed
		// e.g. this has been observed when merging levels in the editor

		if (LandscapeActor.Get() == Landscape)
		{
			LandscapeActor = nullptr;
		}

		// update proxies reference to landscape actor
		for (TWeakObjectPtr<ALandscapeStreamingProxy> StreamingProxyPtr : StreamingProxies)
		{
			if (ALandscapeStreamingProxy* StreamingProxy = StreamingProxyPtr.Get())
			{
				StreamingProxy->SetLandscapeActor(LandscapeActor.Get());
			}
		}
	}
	else
	{
		ALandscapeStreamingProxy* StreamingProxy = CastChecked<ALandscapeStreamingProxy>(Proxy);
		TWeakObjectPtr<ALandscapeStreamingProxy> StreamingProxyPtr = StreamingProxy;
		StreamingProxies.Remove(StreamingProxyPtr);
	}

#if WITH_EDITOR
	UnregisterSplineActor(Proxy);
#endif // WITH_EDITOR

	// remove proxy components from the XY map
	for (int32 CompIdx = 0; CompIdx < Proxy->LandscapeComponents.Num(); ++CompIdx)
	{
		ULandscapeComponent* Component = Proxy->LandscapeComponents[CompIdx];
		if (Component) // When a landscape actor is being GC'd it's possible the components were already GC'd and are null
		{
			UnregisterActorComponent(Component);
		}
	}
	XYtoComponentMap.Compact();

	for (ULandscapeHeightfieldCollisionComponent* CollComp : Proxy->CollisionComponents)
	{
		if (CollComp)
		{
			UnregisterCollisionComponent(CollComp);
		}
	}
	XYtoCollisionComponentMap.Compact();

#if WITH_EDITOR
	UpdateLayerInfoMap();
	UpdateAllAddCollisions();
#endif

#if WITH_EDITORONLY_DATA
	Proxy->bIsRegisteredWithLandscapeInfo = false;
#endif // WITH_EDITORONLY_DATA
}

#if WITH_EDITOR
ALandscapeSplineActor* ULandscapeInfo::CreateSplineActor(const FVector& Location)
{
	check(LandscapeActor.Get());
	UWorld* World = LandscapeActor->GetWorld();
	check(World);
	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = World->PersistentLevel;
	SpawnParams.bNoFail = true;
	SpawnParams.ObjectFlags |= RF_Transactional;
	ALandscapeSplineActor* SplineActor = World->SpawnActor<ALandscapeSplineActor>(Location, FRotator::ZeroRotator, SpawnParams);
	SplineActor->GetSharedProperties(this);
	SplineActor->GetSplinesComponent()->ShowSplineEditorMesh(true);
	SplineActor->SetIsSpatiallyLoaded(AreNewLandscapeActorsSpatiallyLoaded());
	
	FActorLabelUtilities::SetActorLabelUnique(SplineActor, ALandscapeSplineActor::StaticClass()->GetName());

	RegisterSplineActor(SplineActor);
	return SplineActor;
}

void ULandscapeInfo::ForAllSplineActors(TFunctionRef<void(TScriptInterface<ILandscapeSplineInterface>)> Fn) const
{
	for (const TScriptInterface<ILandscapeSplineInterface>& SplineActor : SplineActors)
	{
		Fn(SplineActor);
	}
}

TArray<TScriptInterface<ILandscapeSplineInterface>> ULandscapeInfo::GetSplineActors() const
{
	TArray<TScriptInterface<ILandscapeSplineInterface>> CopySplineActors(SplineActors);
	return MoveTemp(CopySplineActors);
}

void ULandscapeInfo::RegisterSplineActor(TScriptInterface<ILandscapeSplineInterface> SplineActor)
{
	Modify();

	// Sort on insert to ensure spline actors are always processed in the same order, regardless of variation in the
	// sub level streaming/registration sequence.
	auto SortPredicate = [](const TScriptInterface<ILandscapeSplineInterface>& A, const TScriptInterface<ILandscapeSplineInterface>& B)
	{
		return Cast<UObject>(A.GetInterface())->GetPathName() < Cast<UObject>(B.GetInterface())->GetPathName();
	};

	// Add a unique entry, sorted
	const int32 LBoundIdx = Algo::LowerBound(SplineActors, SplineActor, SortPredicate);
	if (LBoundIdx == SplineActors.Num() || SplineActors[LBoundIdx] != SplineActor)
	{
		SplineActors.Insert(SplineActor, LBoundIdx);
	}

	SplineActor->UpdateSharedProperties(this);

	if (SplineActor->GetSplinesComponent())
	{
		RequestSplineLayerUpdate();
	}
}

void ULandscapeInfo::UnregisterSplineActor(TScriptInterface<ILandscapeSplineInterface> SplineActor)
{
	Modify();
	SplineActors.Remove(SplineActor);

	if (SplineActor->GetSplinesComponent())
	{
		RequestSplineLayerUpdate();
	}
}

void ULandscapeInfo::UpdateRegistrationForSplineActor(UWorld* InWorld, TScriptInterface<ILandscapeSplineInterface> InSplineActor)
{
	if (InWorld == nullptr)
		return;

	ULandscapeInfoMap& LandscapeInfoMap = ULandscapeInfoMap::GetLandscapeInfoMap(InWorld);
	FGuid SplineLandscapeGUID = InSplineActor->GetLandscapeGuid();

	// first let's unregister from any landscapes that have it (incorrectly) registered
	for (const auto& pair : LandscapeInfoMap.Map)
	{
		ULandscapeInfo* LandscapeInfo = pair.Value;

		// only unregister if the landscape guids don't match
		if ((LandscapeInfo->LandscapeGuid != SplineLandscapeGUID) &&
			LandscapeInfo->SplineActors.Contains(InSplineActor))
		{
			LandscapeInfo->UnregisterSplineActor(InSplineActor);
		}
	}

	// then let's make sure it is registered with the correct landscape info
	if (SplineLandscapeGUID.IsValid())
	{
		ULandscapeInfo* LandscapeInfo = InSplineActor->GetLandscapeInfo();
		check(LandscapeInfo);
		if (!LandscapeInfo->SplineActors.Contains(InSplineActor))
		{
			LandscapeInfo->RegisterSplineActor(InSplineActor);
		}
	}
}

void ULandscapeInfo::RequestSplineLayerUpdate()
{
	if (LandscapeActor.IsValid())
	{
		LandscapeActor->RequestSplineLayerUpdate();
	}
}

void ULandscapeInfo::ForceLayersFullUpdate()
{
	if (LandscapeActor.IsValid())
	{
		LandscapeActor->ForceLayersFullUpdate();
	}
}
#endif

void ULandscapeInfo::RegisterCollisionComponent(ULandscapeHeightfieldCollisionComponent* Component)
{
	if (Component == nullptr || !Component->IsRegistered())
	{
		return;
	}

	FIntPoint ComponentKey = Component->GetSectionBase() / Component->CollisionSizeQuads;
	auto RegisteredComponent = XYtoCollisionComponentMap.FindRef(ComponentKey);

	if (RegisteredComponent != Component)
	{
		if (RegisteredComponent == nullptr)
		{
			XYtoCollisionComponentMap.Add(ComponentKey, Component);
		}
	}
}

void ULandscapeInfo::UnregisterCollisionComponent(ULandscapeHeightfieldCollisionComponent* Component)
{
	if (ensure(Component))
	{
		FIntPoint ComponentKey = Component->GetSectionBase() / Component->CollisionSizeQuads;
		auto RegisteredComponent = XYtoCollisionComponentMap.FindRef(ComponentKey);

		if (RegisteredComponent == Component)
		{
			XYtoCollisionComponentMap.Remove(ComponentKey);
		}
	}
}

bool ULandscapeInfo::GetOverlappedComponents(const FTransform& InAreaWorldTransform, const FBox2D& InAreaExtents, 
	TMap<FIntPoint, ULandscapeComponent*>& OutOverlappedComponents, FIntRect& OutComponentIndicesBoundingRect)
{
	if (!LandscapeActor.IsValid())
	{ 
		return false;
	}

	// Compute the AABB for this area in landscape space to find which of the landscape components are overlapping :
	FVector Extremas[4];
	const FTransform& LandscapeTransform = LandscapeActor->GetTransform();
	Extremas[0] = LandscapeTransform.InverseTransformPosition(InAreaWorldTransform.TransformPosition(FVector(InAreaExtents.Min.X, InAreaExtents.Min.Y, 0.0)));
	Extremas[1] = LandscapeTransform.InverseTransformPosition(InAreaWorldTransform.TransformPosition(FVector(InAreaExtents.Min.X, InAreaExtents.Max.Y, 0.0)));
	Extremas[2] = LandscapeTransform.InverseTransformPosition(InAreaWorldTransform.TransformPosition(FVector(InAreaExtents.Max.X, InAreaExtents.Min.Y, 0.0)));
	Extremas[3] = LandscapeTransform.InverseTransformPosition(InAreaWorldTransform.TransformPosition(FVector(InAreaExtents.Max.X, InAreaExtents.Max.Y, 0.0)));
	FBox LocalExtents(Extremas, 4);

	// Indices of the landscape components needed for rendering this area : 
	FIntRect BoundingIndices;
	BoundingIndices.Min = FIntPoint(FMath::FloorToInt32(LocalExtents.Min.X / ComponentSizeQuads), FMath::FloorToInt32(LocalExtents.Min.Y / ComponentSizeQuads));
	// The max here is meant to be an exclusive bound, hence the +1
	BoundingIndices.Max = FIntPoint(FMath::FloorToInt32(LocalExtents.Max.X / ComponentSizeQuads), FMath::FloorToInt32(LocalExtents.Max.Y / ComponentSizeQuads)) + FIntPoint(1);

	// There could be missing components, so the effective area is actually a subset of this area :
	FIntRect EffectiveBoundingIndices;
	// Go through each loaded component and find out the actual bounds of the area we need to render :
	for (int32 KeyY = BoundingIndices.Min.Y; KeyY < BoundingIndices.Max.Y; ++KeyY)
	{
		for (int32 KeyX = BoundingIndices.Min.X; KeyX < BoundingIndices.Max.X; ++KeyX)
		{
			FIntPoint Key(KeyX, KeyY);
			if (ULandscapeComponent* Component = XYtoComponentMap.FindRef(Key))
			{
				EffectiveBoundingIndices.Union(FIntRect(Key, Key + FIntPoint(1)));
				OutOverlappedComponents.Add(Key, Component);
			}
		}
	}

	if (OutOverlappedComponents.IsEmpty())
	{
		return false;
	}

	OutComponentIndicesBoundingRect = EffectiveBoundingIndices;
	return true;
}

void ULandscapeInfo::RegisterActorComponent(ULandscapeComponent* Component, bool bMapCheck)
{
	// Do not register components which are not part of the world
	if (Component == nullptr ||
		Component->IsRegistered() == false)
	{
		return;
	}

	check(Component);

	FIntPoint ComponentKey = Component->GetSectionBase() / Component->ComponentSizeQuads;
	auto RegisteredComponent = XYtoComponentMap.FindRef(ComponentKey);

	if (RegisteredComponent != Component)
	{
		if (RegisteredComponent == nullptr)
		{
			XYtoComponentMap.Add(ComponentKey, Component);
		}
		else if (bMapCheck)
		{
#if WITH_EDITOR
			ALandscapeProxy* OurProxy = Component->GetLandscapeProxy();
			ALandscapeProxy* ExistingProxy = RegisteredComponent->GetLandscapeProxy();
			FFormatNamedArguments Arguments;
			Arguments.Add(TEXT("ProxyName1"), FText::FromString(OurProxy->GetName()));
			Arguments.Add(TEXT("LevelName1"), FText::FromString(OurProxy->GetLevel()->GetOutermost()->GetName()));
			Arguments.Add(TEXT("ProxyName2"), FText::FromString(ExistingProxy->GetName()));
			Arguments.Add(TEXT("LevelName2"), FText::FromString(ExistingProxy->GetLevel()->GetOutermost()->GetName()));
			Arguments.Add(TEXT("XLocation"), Component->GetSectionBase().X);
			Arguments.Add(TEXT("YLocation"), Component->GetSectionBase().Y);
			FMessageLog("MapCheck").Warning()
				->AddToken(FUObjectToken::Create(OurProxy))
				->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_Message_LandscapeComponentPostLoad_Warning", "Landscape {ProxyName1} of {LevelName1} has overlapping render components with {ProxyName2} of {LevelName2} at location ({XLocation}, {YLocation})."), Arguments)))
				->AddToken(FActionToken::Create(LOCTEXT("MapCheck_RemoveDuplicateLandscapeComponent", "Delete Duplicate"), LOCTEXT("MapCheck_RemoveDuplicateLandscapeComponentDesc", "Deletes the duplicate landscape component."), FOnActionTokenExecuted::CreateUObject(OurProxy, &ALandscapeProxy::RemoveOverlappingComponent, Component), true))
				->AddToken(FMapErrorToken::Create(FMapErrors::LandscapeComponentPostLoad_Warning));

			// Show MapCheck window
			FMessageLog("MapCheck").Open(EMessageSeverity::Warning);
#endif
		}
	}


#if WITH_EDITOR
	// Update Selected Components/Regions
	if (Component->EditToolRenderData.SelectedType)
	{
		if (Component->EditToolRenderData.SelectedType & FLandscapeEditToolRenderData::ST_COMPONENT)
		{
			SelectedComponents.Add(Component);
		}
		else if (Component->EditToolRenderData.SelectedType & FLandscapeEditToolRenderData::ST_REGION)
		{
			SelectedRegionComponents.Add(Component);
		}
	}
#endif

	XYComponentBounds.Include(ComponentKey);
}

void ULandscapeInfo::UnregisterActorComponent(ULandscapeComponent* Component)
{
	if (ensure(Component))
	{
		FIntPoint ComponentKey = Component->GetSectionBase() / Component->ComponentSizeQuads;
		auto RegisteredComponent = XYtoComponentMap.FindRef(ComponentKey);

		if (RegisteredComponent == Component)
		{
			XYtoComponentMap.Remove(ComponentKey);
		}

		SelectedComponents.Remove(Component);
		SelectedRegionComponents.Remove(Component);

		// When removing a key, we need to iterate to find the new bounds
		XYComponentBounds = FIntRect(MAX_int32, MAX_int32, MIN_int32, MIN_int32);

		for (const auto& XYComponentPair : XYtoComponentMap)
	{
			XYComponentBounds.Include(XYComponentPair.Key);
	}
	}
}

namespace LandscapeInfoBoundsHelper
{
	void AccumulateBounds(ALandscapeProxy* Proxy, FBox& Bounds)
	{
		const bool bOnlyCollidingComponents = false;
		const bool bIncludeChildActors = false;
		FVector Origin;
		FVector BoxExtents;

		Proxy->GetActorBounds(bOnlyCollidingComponents, Origin, BoxExtents, bIncludeChildActors);

		// Reject invalid bounds
		if (BoxExtents != FVector::Zero())
		{
			Bounds += FBox::BuildAABB(Origin, BoxExtents);
		}
	}
}

FBox ULandscapeInfo::GetLoadedBounds() const
{
	FBox Bounds(EForceInit::ForceInit);

	if (LandscapeActor.IsValid())
	{
		LandscapeInfoBoundsHelper::AccumulateBounds(LandscapeActor.Get(), Bounds);
	}

	// Since in PIE/in-game the Proxies aren't populated, we must iterate through the loaded components
	// but this is functionally equivalent to calling ForAllLandscapeProxies
	TSet<ALandscapeProxy*> LoadedProxies;
	for (auto It = XYtoComponentMap.CreateConstIterator(); It; ++It)
	{
		if (!It.Value())
		{
			continue;
		}

		if (ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(It.Value()->GetOwner()))
		{
			LoadedProxies.Add(Proxy);
		}
	}

	for (ALandscapeProxy* Proxy : LoadedProxies)
	{
		LandscapeInfoBoundsHelper::AccumulateBounds(Proxy, Bounds);
	}

	return Bounds;
}

#if WITH_EDITOR
FBox ULandscapeInfo::GetCompleteBounds() const
{
	ALandscape* Landscape = LandscapeActor.Get();

	// In a non-WP situation, the current actor's bounds will do.
	if(!Landscape || !Landscape->GetWorld() || !Landscape->GetWorld()->GetWorldPartition())
	{
		return GetLoadedBounds();
	}

	FBox Bounds(EForceInit::ForceInit);

	FWorldPartitionHelpers::ForEachActorDesc<ALandscapeProxy>(Landscape->GetWorld()->GetWorldPartition(), [this, &Bounds, Landscape](const FWorldPartitionActorDesc* ActorDesc)
	{
		FLandscapeActorDesc* LandscapeActorDesc = (FLandscapeActorDesc*)ActorDesc;
		ALandscapeProxy* LandscapeProxy = Cast<ALandscapeProxy>(ActorDesc->GetActor());

		// Prioritize loaded bounds, as the bounds in the actor desc might not be up-to-date
		if(LandscapeProxy && (LandscapeProxy->GetGridGuid() == LandscapeGuid))
		{
			LandscapeInfoBoundsHelper::AccumulateBounds(LandscapeProxy, Bounds);
		}
		else if (LandscapeActorDesc->GridGuid == LandscapeGuid)
		{
			Bounds += ActorDesc->GetEditorBounds();
		}

		return true;
	});

	return Bounds;
}
#endif

void ULandscapeComponent::PostInitProperties()
{
	Super::PostInitProperties();

	// Create a new guid in case this is a newly created component
	// If not, this guid will be overwritten when serialized
	FPlatformMisc::CreateGuid(StateId);

	// Initialize MapBuildDataId to something unique, in case this is a new ULandscapeComponent
	MapBuildDataId = FGuid::NewGuid();
}

void ULandscapeComponent::PostDuplicate(bool bDuplicateForPIE)
{
	if (!bDuplicateForPIE)
	{
		// Reset the StateId on duplication since it needs to be unique for each capture.
		// PostDuplicate covers direct calls to StaticDuplicateObject, but not actor duplication (see PostEditImport)
		FPlatformMisc::CreateGuid(StateId);
	}
}

ULandscapeWeightmapUsage::ULandscapeWeightmapUsage(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ClearUsage();
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
ALandscapeProxy::~ALandscapeProxy()
{
	for (int32 Index = 0; Index < AsyncFoliageTasks.Num(); Index++)
	{
		FAsyncTask<FAsyncGrassTask>* Task = AsyncFoliageTasks[Index];
		Task->EnsureCompletion(true);
		FAsyncGrassTask& Inner = Task->GetTask();
		delete Task;
	}
	AsyncFoliageTasks.Empty();

#if WITH_EDITOR
	NumComponentsNeedingGrassMapRender = 0;
	TotalTexturesToStreamForVisibleGrassMapRender -= NumTexturesToStreamForVisibleGrassMapRender;
	NumTexturesToStreamForVisibleGrassMapRender = 0;
#endif

#if WITH_EDITORONLY_DATA
	LandscapeProxies.Remove(this);
#endif
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

//
// ALandscapeMeshProxyActor
//
ALandscapeMeshProxyActor::ALandscapeMeshProxyActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetCanBeDamaged(false);

	LandscapeMeshProxyComponent = CreateDefaultSubobject<ULandscapeMeshProxyComponent>(TEXT("LandscapeMeshProxyComponent0"));
	LandscapeMeshProxyComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	LandscapeMeshProxyComponent->Mobility = EComponentMobility::Static;
	LandscapeMeshProxyComponent->SetGenerateOverlapEvents(false);

	RootComponent = LandscapeMeshProxyComponent;
}

//
// ULandscapeMeshProxyComponent
//
ULandscapeMeshProxyComponent::ULandscapeMeshProxyComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ULandscapeMeshProxyComponent::InitializeForLandscape(ALandscapeProxy* Landscape, int8 InProxyLOD)
{
	LandscapeGuid = Landscape->GetLandscapeGuid();
	LODGroupKey = Landscape->GetLandscapeActor()->GetLODGroupKey();

	for (ULandscapeComponent* Component : Landscape->LandscapeComponents)
	{
		if (Component)
		{
			ProxyComponentBases.Add(Component->GetSectionBase() / Component->ComponentSizeQuads);
		}
	}

	if (InProxyLOD != INDEX_NONE)
	{
		ProxyLOD = FMath::Clamp<int8>(InProxyLOD, 0, static_cast<int8>(FMath::CeilLogTwo(Landscape->SubsectionSizeQuads + 1) - 1));
	}
}

#if WITH_EDITOR

void  ALandscapeProxy::CreateNaniteComponents(int32 InNumComponents) 
{
	for (int32 i = 0; i < InNumComponents; ++i)
	{
		ULandscapeNaniteComponent* NaniteComponent = NewObject<ULandscapeNaniteComponent>(this, *FString::Format(TEXT("LandscapeNaniteComponent_{0}"), {{ i }}));
		NaniteComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		NaniteComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		NaniteComponent->SetMobility(EComponentMobility::Static);
		NaniteComponent->SetGenerateOverlapEvents(false);
		NaniteComponent->SetCanEverAffectNavigation(false);
		NaniteComponent->CanCharacterStepUpOn = ECanBeCharacterBase::ECB_No;
		NaniteComponent->bSelectable = false;
		NaniteComponent->DepthPriorityGroup = SDPG_World;
		NaniteComponent->bForceNaniteForMasked = true;
		NaniteComponent->RegisterComponent();
		NaniteComponent->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

		NaniteComponents.Add(NaniteComponent);
	}
}

void ALandscapeProxy::SerializeStateHashes(FArchive& Ar)
{
	for (FLandscapePerLODMaterialOverride& MaterialOverride : PerLODOverrideMaterials)
	{
		if (MaterialOverride.Material != nullptr)
		{
			FGuid LocalStateId = MaterialOverride.Material->GetMaterial_Concurrent()->StateId;
			Ar << LocalStateId;
			Ar << MaterialOverride.LODIndex;
		}
	}
}

void ULandscapeComponent::SerializeStateHashes(FArchive& Ar)
{
	FGuid HeightmapGuid = HeightmapTexture->Source.GetId();
	Ar << HeightmapGuid;
	for (auto WeightmapTexture : WeightmapTextures)
	{
		FGuid WeightmapGuid = WeightmapTexture->Source.GetId();
		Ar << WeightmapGuid;
	}

	bool bEnableNanite = GetLandscapeProxy()->IsNaniteEnabled();
	Ar << bEnableNanite;

	if (GetLandscapeHoleMaterial() && ComponentHasVisibilityPainted())
	{
		FGuid LocalStateId = GetLandscapeHoleMaterial()->GetMaterial_Concurrent()->StateId;
		Ar << LocalStateId;
	}

	// Take into account the Heightmap offset per component
	Ar << HeightmapScaleBias.Z;
	Ar << HeightmapScaleBias.W;

	if (OverrideMaterial != nullptr)
	{
		FGuid LocalStateId = OverrideMaterial->GetMaterial_Concurrent()->StateId;
		Ar << LocalStateId;
	}

	for (FLandscapePerLODMaterialOverride& MaterialOverride : PerLODOverrideMaterials)
	{
		if (MaterialOverride.Material != nullptr)
		{
			FGuid LocalStateId = MaterialOverride.Material->GetMaterial_Concurrent()->StateId;
			Ar << LocalStateId;
			Ar << MaterialOverride.LODIndex;
		}
	}

	ALandscapeProxy* Proxy = GetLandscapeProxy();

	if (Proxy->LandscapeMaterial != nullptr)
	{
		FGuid LocalStateId = Proxy->LandscapeMaterial->GetMaterial_Concurrent()->StateId;
		Ar << LocalStateId;
	}

	Proxy->SerializeStateHashes(Ar);
}

FLandscapePhysicalMaterialBuilder::FLandscapePhysicalMaterialBuilder(UWorld* InWorld)
	:World(InWorld)
	,OudatedPhysicalMaterialComponentsCount(0)
{
}

void FLandscapePhysicalMaterialBuilder::Build()
{
	if (World)
	{
		for (TActorIterator<ALandscapeProxy> ProxyIt(World); ProxyIt; ++ProxyIt)
		{
			ProxyIt->BuildPhysicalMaterial();
		}
	}
}

void FLandscapePhysicalMaterialBuilder::Rebuild()
{
	if (World)
	{
		for (TActorIterator<ALandscapeProxy> ProxyIt(World); ProxyIt; ++ProxyIt)
		{
			ProxyIt->InvalidatePhysicalMaterial();
			ProxyIt->BuildPhysicalMaterial();
		}
	}
}

int32 FLandscapePhysicalMaterialBuilder::GetOudatedPhysicalMaterialComponentsCount()
{
	if (World)
	{
		OudatedPhysicalMaterialComponentsCount = 0;
		for (TActorIterator<ALandscapeProxy> ProxyIt(World); ProxyIt; ++ProxyIt)
		{
			OudatedPhysicalMaterialComponentsCount += ProxyIt->GetOudatedPhysicalMaterialComponentsCount();
		}
	}
	return OudatedPhysicalMaterialComponentsCount;
}

int32 ALandscapeProxy::GetOudatedPhysicalMaterialComponentsCount() const
{
	int32 OudatedPhysicalMaterialComponentsCount = 0;
	UpdatePhysicalMaterialTasksStatus(nullptr, &OudatedPhysicalMaterialComponentsCount);
	return OudatedPhysicalMaterialComponentsCount;
}

UE::Landscape::EOutdatedDataFlags ALandscapeProxy::GetOutdatedDataFlags() const
{
	UE::Landscape::EOutdatedDataFlags OutdatedDataFlags = UE::Landscape::EOutdatedDataFlags::None;

	if (GetOutdatedGrassMapCount() > 0)
	{
		OutdatedDataFlags |= UE::Landscape::EOutdatedDataFlags::GrassMaps;
	}

	if (GetOudatedPhysicalMaterialComponentsCount() > 0)
	{
		OutdatedDataFlags |= UE::Landscape::EOutdatedDataFlags::PhysicalMaterials;
	}

	if (!IsNaniteMeshUpToDate())
	{
		OutdatedDataFlags |= UE::Landscape::EOutdatedDataFlags::NaniteMeshes;
	}

	return OutdatedDataFlags;
}

void ALandscapeProxy::ClearNaniteTransactional()
{
	for (ULandscapeNaniteComponent* NaniteComponent : NaniteComponents)
	{
		if (NaniteComponent)
		{
			NaniteComponent->ClearFlags(RF_Transactional);
		}
	}
}

void ALandscapeProxy::UpdateNaniteSharedPropertiesFromActor()
{	
	for (ULandscapeNaniteComponent* NaniteComponent : NaniteComponents)
	{
		if (NaniteComponent)
		{
			NaniteComponent->UpdatedSharedPropertiesFromActor();
		}	
	}
}

void ALandscapeProxy::InvalidatePhysicalMaterial()
{
	for (ULandscapeComponent* Component : LandscapeComponents)
	{
		Component->PhysicalMaterialHash = 0;
	}
}

void ALandscapeProxy::BuildPhysicalMaterial(struct FScopedSlowTask* InSlowTask)
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		const bool bShouldMarkDirty = true;
		UpdatePhysicalMaterialTasks(bShouldMarkDirty);
	}
}

void ALandscapeProxy::UpdatePhysicalMaterialTasksStatus(TSet<ULandscapeComponent*>* OutdatedComponents, int32* OutdatedComponentsCount) const
{
	int32 OutdatedCount = 0;
	for (ULandscapeComponent* Component : LandscapeComponents)
	{
		uint32 Hash = Component->CalculatePhysicalMaterialTaskHash();
		if (Component->PhysicalMaterialHash != Hash || Component->PhysicalMaterialTask.IsValid())
		{
			OutdatedCount++;
			if (OutdatedComponents)
			{
				OutdatedComponents->Add(Component);
			}
		}
	}

	if (OutdatedCount == 0)
	{
		for (ULandscapeComponent* Component : LandscapeComponents)
		{
			const bool bIsDirty = Component->GetPackage()->IsDirty();
			if (Component->LastSavedPhysicalMaterialHash != Component->PhysicalMaterialHash && !bIsDirty)
			{
				OutdatedCount++;
			}
		}
	}

	if (OutdatedComponentsCount)
{
		*OutdatedComponentsCount = OutdatedCount;
	}
}

void ALandscapeProxy::UpdatePhysicalMaterialTasks(bool bInShouldMarkDirty)
{
	TSet<ULandscapeComponent*> OutdatedComponents;
	int32 PendingComponentsToBeSaved = 0;
	UpdatePhysicalMaterialTasksStatus(&OutdatedComponents, &PendingComponentsToBeSaved);
	for (ULandscapeComponent* Component : OutdatedComponents)
	{
		Component->UpdatePhysicalMaterialTasks();
	}
	if (bInShouldMarkDirty && PendingComponentsToBeSaved >0)
	{
		MarkPackageDirty();
	}
}

void ALandscapeProxy::RemoveNaniteComponents()
{
	for (ULandscapeNaniteComponent* NaniteComponent : NaniteComponents)
	{
		if (NaniteComponent)
		{
			// Don't call modify when detaching the nanite component, this is non-transactional "derived data", regenerated any time the source landscape data changes. This prevents needlessly dirtying the package :
			NaniteComponent->DetachFromComponent(FDetachmentTransformRules(EDetachmentRule::KeepRelative, /*bInCallModify = */false));
			NaniteComponent->DestroyComponent();
		}
	}

	NaniteComponents.Empty();

}
#endif // WITH_EDITOR

void ALandscapeProxy::EnableNaniteComponents(bool bInNaniteActive)
{
	for (ULandscapeNaniteComponent* NaniteComponent : NaniteComponents)
	{
		if (NaniteComponent)
		{
			NaniteComponent->SetEnabled(bInNaniteActive);
		}
	}
}

bool ALandscapeProxy::AreNaniteComponentsValid(const FGuid& InProxyContentId) const
{
	if (NaniteComponents.IsEmpty())
	{
		return false;
	}

	for (const ULandscapeNaniteComponent* NaniteComponent : NaniteComponents)
	{
		if (!NaniteComponent)
		{
			return false;
		}

		if (NaniteComponent->GetProxyContentId() != InProxyContentId)
		{
			return false;
		}
	}

	return true;
}

TSet<FPrimitiveComponentId> ALandscapeProxy::GetNanitePrimitiveComponentIds() const
{
	TSet<FPrimitiveComponentId> PrimitiveComponentIds;
	for (const ULandscapeNaniteComponent* NaniteComponent : NaniteComponents)
	{
		if (NaniteComponent && NaniteComponent->SceneProxy)
		{
			PrimitiveComponentIds.Add(NaniteComponent->SceneProxy->GetPrimitiveComponentId());
		}

	}
	return PrimitiveComponentIds;
}

FGuid ALandscapeProxy::GetNaniteComponentContentId() const
{
	if (NaniteComponents.IsEmpty())
	{
		return FGuid();
	}

	FGuid ContentId = NaniteComponents[0] ? NaniteComponents[0]->GetProxyContentId() : FGuid();
	return ContentId;
}

bool ALandscapeProxy::AuditNaniteMaterials() const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ALandscapeProxy::AuditMaterials);
	for (const ULandscapeNaniteComponent* NaniteComponent : NaniteComponents)
	{
		if (!NaniteComponent)
		{
			return false;
		}

		Nanite::FMaterialAudit NaniteMaterials;
		Nanite::AuditMaterials(NaniteComponent, NaniteMaterials);

		const bool bIsMaskingAllowed = Nanite::IsMaskingAllowed(GetWorld(), NaniteComponent->bForceNaniteForMasked);
		if (!NaniteMaterials.IsValid(bIsMaskingAllowed))
		{
			return false;
		}
	}
	return true;
}



void ALandscapeProxy::InvalidateGeneratedComponentData(bool bInvalidateLightingCache)
{
	InvalidateGeneratedComponentData(LandscapeComponents, bInvalidateLightingCache);
}

void ALandscapeProxy::InvalidateGeneratedComponentData(const TArray<ULandscapeComponent*>& Components, bool bInvalidateLightingCache)
{
	TMap<ALandscapeProxy*, TSet<ULandscapeComponent*>> ByProxy;
	for (auto Iter = Components.CreateConstIterator(); Iter; ++Iter)
	{
		ULandscapeComponent* Component = *Iter;
		if (bInvalidateLightingCache)
		{
			Component->InvalidateLightingCache();
		}
		ByProxy.FindOrAdd(Component->GetLandscapeProxy()).Add(Component);
	}

	for (auto Iter = ByProxy.CreateConstIterator(); Iter; ++Iter)
	{
		ALandscapeProxy* Proxy = Iter.Key();
		Proxy->FlushGrassComponents(&Iter.Value());

	#if WITH_EDITOR
		ULandscapeSubsystem* Subsystem = Proxy->GetWorld()->GetSubsystem<ULandscapeSubsystem>();
		if (Subsystem->IsLiveNaniteRebuildEnabled())
		{
			Proxy->GetAsyncWorkMonitor().SetDelayedUpdateTimer(FAsyncWorkMonitor::EAsyncWorkType::BuildNaniteMeshes, LandscapeNaniteBuildLag);
		}
		else
		{
			Proxy->InvalidateOrUpdateNaniteRepresentation(/* bInCheckContentId = */true, /*InTargetPlatform = */nullptr);
		}
		
		FLandscapeProxyComponentDataChangedParams ChangeParams(Iter.Value());
		Proxy->OnComponentDataChanged.Broadcast(Iter.Key(), ChangeParams);
	#endif

		Proxy->UpdateRenderingMethod();
	}
}

void ALandscapeProxy::InvalidateGeneratedComponentData(const TSet<ULandscapeComponent*>& Components, bool bInvalidateLightingCache)
{
	InvalidateGeneratedComponentData(Components.Array(), bInvalidateLightingCache);
}

void ALandscapeProxy::UpdateRenderingMethod()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ALandscapeProxy::UpdateRenderingMethod);
	if (LandscapeComponents.Num() == 0)
	{
		return;
	}

	bool bNaniteActive = false;
	if ((GRenderNaniteLandscape != 0) && HasNaniteComponents())
	{
		bNaniteActive = UseNanite(GShaderPlatformForFeatureLevel[GMaxRHIFeatureLevel]);
#if WITH_EDITOR
		if (ALandscape* LandscapeActor = GetLandscapeActor())
		{
			if (UWorld* World = LandscapeActor->GetWorld())
			{
				if ((GEngine->GetDefaultWorldFeatureLevel() == ERHIFeatureLevel::ES3_1) || (World->GetFeatureLevel() <= ERHIFeatureLevel::ES3_1))
				{
					bNaniteActive = UseNanite(GShaderPlatformForFeatureLevel[ERHIFeatureLevel::ES3_1]);
				}
			}
		}
#endif //WITH_EDITOR
	}

#if WITH_EDITOR
	if (bNaniteActive)
	{
		bNaniteActive = GetNaniteComponentContentId() == GetNaniteContentId();
	}
#endif //WITH_EDITOR

	if (bNaniteActive)
	{
		bNaniteActive = AuditNaniteMaterials();
	}

	for (ULandscapeComponent* Component : LandscapeComponents)
	{
		if (Component)
		{
			Component->SetNaniteActive(bNaniteActive);
		}
	}

	EnableNaniteComponents(bNaniteActive);
}

ULandscapeLODStreamingProxy_DEPRECATED::ULandscapeLODStreamingProxy_DEPRECATED(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

#if WITH_EDITOR
FLandscapeProxyComponentDataChangedParams::FLandscapeProxyComponentDataChangedParams(const TSet<ULandscapeComponent*>& InComponents)
	: Components(InComponents.Array())
{
}

void FLandscapeProxyComponentDataChangedParams::ForEachComponent(TFunctionRef<void(const ULandscapeComponent*)> Func) const
{
	for (ULandscapeComponent* Component : Components)
	{
		Func(Component);
	}
}



bool FAsyncWorkMonitor::CheckIfUpdateTriggeredAndClear(EAsyncWorkType WorkType)
{
	bool& bUpdateTriggered = WorkTypeInfos[static_cast<uint32>(WorkType)].bUpdateTriggered;

	bool bReturn = bUpdateTriggered;
	bUpdateTriggered = false;
	return bReturn;
}

void FAsyncWorkMonitor::SetDelayedUpdateTimer(EAsyncWorkType WorkType, float InSecondsUntilDelayedUpdateTrigger)
{
	FAsyncWorkTypeInfo& Info = WorkTypeInfos[static_cast<uint32>(WorkType)];
	Info.SecondsUntilDelayedUpdateTrigger = InSecondsUntilDelayedUpdateTrigger;
}

void FAsyncWorkMonitor::Tick(float Detaltime)
{
	for (FAsyncWorkTypeInfo& Info : WorkTypeInfos)
	{
		if (Info.SecondsUntilDelayedUpdateTrigger > 0.0f)
		{
			Info.SecondsUntilDelayedUpdateTrigger -= Detaltime;

			if (Info.SecondsUntilDelayedUpdateTrigger <= 0.0f)
			{
				Info.SecondsUntilDelayedUpdateTrigger = 0.0f;
				Info.bUpdateTriggered = true;
			}
		}
	}
}
#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
