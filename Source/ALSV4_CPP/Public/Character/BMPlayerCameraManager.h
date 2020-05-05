// Copyright (C) 2020, Doga Can Yanikoglu

#pragma once

#include "CoreMinimal.h"
#include "Camera/PlayerCameraManager.h"
#include "BMPlayerCameraManager.generated.h"

#define DEBUG_CAMERA 0

class ABMBaseCharacter;

/**
 * Player camera manager class
 */
UCLASS(Blueprintable, BlueprintType)
class ALSV4_CPP_API ABMPlayerCameraManager : public APlayerCameraManager
{
	GENERATED_BODY()

	ABMPlayerCameraManager();

public:
	UFUNCTION(BlueprintCallable)
	void OnPossess(ABMBaseCharacter* NewCharacter);

	UFUNCTION(BlueprintCallable)
	float GetCameraBehaviorParam(FName CurveName);

protected:
	virtual void UpdateViewTargetInternal(FTViewTarget& OutVT, float DeltaTime);

	UFUNCTION(BlueprintCallable)
	FVector CalculateAxisIndependentLag(
		FVector CurrentLocation, FVector TargetLocation, FRotator CameraRotation, FVector LagSpeeds, float DeltaTime);

	UFUNCTION(BlueprintCallable)
	bool CustomCameraBehavior(float DeltaTime, FVector& Location, FRotator& Rotation, float& FOV);

private:
	void DrawDebugTargets(FVector PivotTargetLocation);
	
public:
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite)
	ABMBaseCharacter* ControlledCharacter = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite)
	USkeletalMeshComponent* CameraBehavior = nullptr;

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite)
	FName CustomTag = FName(TEXT("BM_Character"));

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector RootLocation;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FTransform SmoothedPivotTarget;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector PivotLocation;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector TargetCameraLocation;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FRotator TargetCameraRotation;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite)
	FRotator DebugViewRotation;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite)
	FVector DebugViewOffset;
};
