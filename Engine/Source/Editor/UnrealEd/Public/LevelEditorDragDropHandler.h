// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "LevelEditorDragDropHandler.generated.h"

class FViewport;
struct FAssetData;

UCLASS(transient, MinimalAPI)
class ULevelEditorDragDropHandler : public UObject
{
public:
	GENERATED_BODY()
	
public:
	UNREALED_API ULevelEditorDragDropHandler();
	
	UNREALED_API virtual bool PreviewDropObjectsAtCoordinates(int32 MouseX, int32 MouseY, UWorld* World, FViewport* Viewport, const FAssetData& AssetData);
	UNREALED_API virtual bool PreDropObjectsAtCoordinates(int32 MouseX, int32 MouseY, UWorld* World, FViewport* Viewport, const TArray<UObject*>& DroppedObjects, TArray<AActor*>& OutNewActors);
	UNREALED_API virtual bool PostDropObjectsAtCoordinates(int32 MouseX, int32 MouseY, UWorld* World, FViewport* Viewport, const TArray<UObject*>& DroppedObjects);

	bool GetCanDrop() const { return bCanDrop; }
	FText GetPreviewDropHintText() const { return HintText; }
	
protected:
	bool bRunAssetFilter = true;

	/** True if it's valid to drop the object at the location queried */
	bool bCanDrop = false;

	/** Optional hint text that may be returned to the user. */
	FText HintText;
};
