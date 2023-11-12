// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Commandlets/Commandlet.h"
#include "LandscapeGrassTypeCommandlet.generated.h"

/** Commandlet  */
UCLASS()
class ULandscapeGrassTypeCommandlet : public UCommandlet
{
	GENERATED_UCLASS_BODY()

public:
	//~ Begin UCommandlet Interface
	virtual int32 Main(const FString& Params) override;

	//~ End UCommandlet Interface
};