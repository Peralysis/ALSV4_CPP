// Copyright (C) 2020, Doga Can Yanikoglu


#include "Character/BMBaseCharacter.h"


#include "Character/BMPlayerController.h"
#include "Character/Animation/BMCharacterAnimInstance.h"
#include "Components/CapsuleComponent.h"
#include "Components/TimelineComponent.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveFloat.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/KismetMathLibrary.h"
#if DEBUG_BASECHAR
#include "DrawDebugHelpers.h"
#endif

ABMBaseCharacter::ABMBaseCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	MantleTimeline = CreateDefaultSubobject<UTimelineComponent>(FName(TEXT("MantleTimeline")));
	bUseControllerRotationYaw = 0;
}

void ABMBaseCharacter::OnBreakfall_Implementation()
{
	check(MainAnimInstance);
	MainAnimInstance->Montage_Play(GetRollAnimation(), 1.35f);
}

void ABMBaseCharacter::OnRoll_Implementation()
{
	// Roll: Simply play a Root Motion Montage.
	check(MainAnimInstance);
	MainAnimInstance->Montage_Play(GetRollAnimation(), 1.15f);
}

void ABMBaseCharacter::BeginPlay()
{
	Super::BeginPlay();

	FOnTimelineFloat TimelineUpdated;
	FOnTimelineEvent TimelineFinished;
	TimelineUpdated.BindUFunction(this, FName(TEXT("OnTimeLineUpdated")));
	TimelineFinished.BindUFunction(this, FName(TEXT("OnTimeLineFinished")));
	MantleTimeline->SetTimelineFinishedFunc(TimelineFinished);
	MantleTimeline->SetLooping(false);
	MantleTimeline->SetTimelineLengthMode(ETimelineLengthMode::TL_TimelineLength);
	MantleTimeline->AddInterpFloat(MantleTimelineCurve, TimelineUpdated);

	// Make sure the mesh and animbp update after the CharacterBP to ensure it gets the most recent values.
	GetMesh()->AddTickPrerequisiteActor(this);

	// Set Reference to the Main Anim Instance.
	if (IsValid(GetMesh()->GetAnimInstance()))
	{
		MainAnimInstance = Cast<UBMCharacterAnimInstance>(GetMesh()->GetAnimInstance());
		if (bEnableOptimization)
		{
			MainAnimInstance->EnableOptimization();
		}
	}

	// Set the Movement Model
	SetMovementModel();

	// Update states to use the initial desired values.
	SetGait(DesiredGait);
	SetRotationMode(DesiredRotationMode);
	SetOverlayState(OverlayState);

	if (Stance == EBMStance::Standing)
	{
		UnCrouch();
	}
	else if (Stance == EBMStance::Crouching)
	{
		Crouch();
	}

	// Set default rotation values.
	TargetRotation = GetActorRotation();
	LastVelocityRotation = TargetRotation;
	LastMovementInputRotation = TargetRotation;
}

void ABMBaseCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Set required values
	SetEssentialValues(DeltaTime);

	// Cache values
	PreviousVelocity = GetVelocity();
	PreviousAimYaw = GetControlRotation().Yaw;

	if (MovementState == EBMMovementState::Grounded)
	{
		UpdateCharacterMovement();
		UpdateGroundedRotation(DeltaTime);
	}
	else if (MovementState == EBMMovementState::InAir)
	{
		UpdateInAirRotation(DeltaTime);

		// Perform a mantle check if falling while movement input is pressed.
		if (bHasMovementInput)
		{
			MantleCheck(FallingTraceSettings);
		}
	}
	else if (MovementState == EBMMovementState::Ragdoll)
	{
		RagdollUpdate();
	}

	DrawDebugSpheres();
}

void ABMBaseCharacter::RagdollStart()
{
	// Step 1: Clear the Character Movement Mode and set teh Movement State to Ragdoll
	GetCharacterMovement()->SetMovementMode(EMovementMode::MOVE_None);
	SetMovementState(EBMMovementState::Ragdoll);

	// Step 2: Disable capsule collision and enable mesh physics simulation starting from the pelvis.
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetMesh()->SetCollisionObjectType(ECC_PhysicsBody);
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	GetMesh()->SetAllBodiesBelowSimulatePhysics(FName(TEXT("Pelvis")), true, true);

	// Step 3: Stop any active montages.
	MainAnimInstance->Montage_Stop(0.2f);
}

void ABMBaseCharacter::RagdollEnd()
{
	if (!IsValid(MainAnimInstance))
	{
		return;
	}

	// Step 1: Save a snapshot of the current Ragdoll Pose for use in AnimGraph to blend out of the ragdoll
	MainAnimInstance->SavePoseSnapshot(FName(TEXT("RagdollPose")));

	// Step 2: If the ragdoll is on the ground, set the movement mode to walking and play a Get Up animation.
	// If not, set the movement mode to falling and update teh character movement velocity to match the last ragdoll velocity.
	if (bRagdollOnGround)
	{
		GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		MainAnimInstance->Montage_Play(GetGetUpAnimation(bRagdollFaceUp),
		                               1.0f, EMontagePlayReturnType::MontageLength, 0.0f, true);
	}
	else
	{
		GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		GetCharacterMovement()->Velocity = LastRagdollVelocity;
	}

	// Step 3: Re-Enable capsule collision, and disable physics simulation on the mesh.
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	GetMesh()->SetCollisionObjectType(ECC_Pawn);
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	GetMesh()->SetAllBodiesSimulatePhysics(false);
}

void ABMBaseCharacter::SetMovementState(const EBMMovementState NewState)
{
	if (MovementState != NewState)
	{
		PrevMovementState = MovementState;
		MovementState = NewState;
		OnMovementStateChanged(PrevMovementState);
	}
}

void ABMBaseCharacter::SetMovementAction(const EBMMovementAction NewAction)
{
	if (MovementAction != NewAction)
	{
		EBMMovementAction Prev = MovementAction;
		MovementAction = NewAction;
		OnMovementActionChanged(Prev);
	}
}

void ABMBaseCharacter::SetStance(const EBMStance NewStance)
{
	if (Stance != NewStance)
	{
		EBMStance Prev = Stance;
		Stance = NewStance;
		OnStanceChanged(Prev);
	}
}

void ABMBaseCharacter::SetRotationMode(const EBMRotationMode NewRotationMode)
{
	if (RotationMode != NewRotationMode)
	{
		EBMRotationMode Prev = RotationMode;
		RotationMode = NewRotationMode;
		OnRotationModeChanged(Prev);
	}
}

void ABMBaseCharacter::SetGait(const EBMGait NewGait)
{
	if (Gait != NewGait)
	{
		EBMGait Prev = Gait;
		Gait = NewGait;
		OnGaitChanged(Prev);
	}
}

void ABMBaseCharacter::SetOverlayState(const EBMOverlayState NewState)
{
	if (OverlayState != NewState)
	{
		EBMOverlayState Prev = OverlayState;
		OverlayState = NewState;
		OnOverlayStateChanged(Prev);
	}
}

void ABMBaseCharacter::SetActorLocationAndTargetRotation(FVector NewLocation, FRotator NewRotation)
{
	SetActorLocationAndRotation(NewLocation, NewRotation);
	TargetRotation = NewRotation;
}

bool ABMBaseCharacter::MantleCheckGrounded()
{
	return MantleCheck(GroundedTraceSettings);
}

bool ABMBaseCharacter::MantleCheckFalling()
{
	return MantleCheck(FallingTraceSettings);
}

void ABMBaseCharacter::SetMovementModel()
{
	FString ContextString = GetFullName();
	FBMMovementStateSettings* OutRow =
		MovementModel.DataTable->FindRow<FBMMovementStateSettings>(MovementModel.RowName, ContextString);
	check(OutRow);
	MovementData = *OutRow;
}

void ABMBaseCharacter::DrawDebugSpheres()
{
#if DEBUG_BASECHAR
	UWorld* World = GetWorld();
	check(World);

	// Velocity Arrow
	FVector LineStart = GetActorLocation();
	LineStart.Z -= GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	FVector LineEnd;
	FColor ArrowColor;
	if (GetVelocity().IsNearlyZero())
	{
		LineEnd = LastVelocityRotation.Vector();
		ArrowColor = FColor::Purple;
	}
	else
	{
		LineEnd = GetVelocity();
		ArrowColor = FColor::Magenta;
	}
	LineEnd = LineStart +
		LineEnd.GetUnsafeNormal() * FMath::GetMappedRangeValueClamped(FVector2D(0.0f, GetCharacterMovement()->MaxWalkSpeed),
		                                                              FVector2D(50.0f, 75.0f), GetVelocity().Size());
	DrawDebugDirectionalArrow(World, LineStart, LineEnd, 60.0f, ArrowColor, false, 0.0f, 0, 5.0f);

	// Movement Input Arrow
	LineStart = GetActorLocation();
	LineStart.Z -= GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 3.5f;
	if (GetVelocity().IsNearlyZero())
	{
		LineEnd = LastMovementInputRotation.Vector();
		ArrowColor = FColor::Yellow;
	}
	else
	{
		LineEnd = GetCharacterMovement()->GetCurrentAcceleration();;
		ArrowColor = FColor::Orange;
	}
	LineEnd = LineStart +
		LineEnd.GetUnsafeNormal() * FMath::GetMappedRangeValueClamped(FVector2D(0.0f, 1.0f),
		                                                              FVector2D(50.0f, 75.0f),
		                                                              GetCharacterMovement()->GetCurrentAcceleration().Size() /
		                                                              GetCharacterMovement()->GetMaxAcceleration());
	DrawDebugDirectionalArrow(World, LineStart, LineEnd, 50.0f, ArrowColor, false, 0.0f, 0, 3.0f);

	// Target Rotation Arrow
	LineStart = GetActorLocation();
	LineStart.Z -= GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 7.0f;
	LineEnd = LineStart + (TargetRotation.Vector().GetUnsafeNormal() * 50.0f);
	DrawDebugDirectionalArrow(World, LineStart, LineEnd, 50.0f, FColor::Blue, false, 0.0f, 0, 3.0f);

	// Aiming Rotation Cone
	DrawDebugCone(World, GetMesh()->GetSocketLocation(FName(TEXT("FP_Camera"))), GetControlRotation().Vector().GetUnsafeNormal(), 100.0f,
	              FMath::DegreesToRadians(30.0f), FMath::DegreesToRadians(30.0f), 8, FColor::Blue, false, 0.0f, 0, 0.5f);

	// Capsule
	DrawDebugCapsule(World, GetActorLocation(), GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
	                 GetCapsuleComponent()->GetScaledCapsuleRadius(), GetActorRotation().Quaternion(), FColor::Black, false, 0.0f, 0, 0.5f);
#endif
}

FBMMovementSettings ABMBaseCharacter::GetTargetMovementSettings()
{
	if (RotationMode == EBMRotationMode::VelocityDirection)
	{
		if (Stance == EBMStance::Standing)
		{
			return MovementData.VelocityDirection.Standing;
		}
		if (Stance == EBMStance::Crouching)
		{
			return MovementData.VelocityDirection.Crouching;
		}
	}
	else if (RotationMode == EBMRotationMode::LookingDirection)
	{
		if (Stance == EBMStance::Standing)
		{
			return MovementData.LookingDirection.Standing;
		}
		if (Stance == EBMStance::Crouching)
		{
			return MovementData.LookingDirection.Crouching;
		}
	}
	else if (RotationMode == EBMRotationMode::Aiming)
	{
		if (Stance == EBMStance::Standing)
		{
			return MovementData.Aiming.Standing;
		}
		if (Stance == EBMStance::Crouching)
		{
			return MovementData.Aiming.Crouching;
		}
	}

	// Default to velocity dir standing
	return MovementData.VelocityDirection.Standing;
}

bool ABMBaseCharacter::CanSprint()
{
	// Determine if the character is currently able to sprint based on the Rotation mode and current acceleration
	// (input) rotation. If the character is in the Looking Rotation mode, only allow sprinting if there is full
	// movement input and it is faced forward relative to the camera + or - 50 degrees.

	if (!bHasMovementInput || RotationMode == EBMRotationMode::Aiming)
	{
		return false;
	}

	const bool bValidInputAmount = MovementInputAmount > 0.9f;

	if (RotationMode == EBMRotationMode::VelocityDirection)
	{
		return bValidInputAmount;
	}

	if (RotationMode == EBMRotationMode::LookingDirection)
	{
		const FRotator AccRot = GetCharacterMovement()->GetCurrentAcceleration().ToOrientationRotator();
		FRotator Delta = AccRot - GetControlRotation();
		Delta.Normalize();

		return bValidInputAmount && FMath::Abs(Delta.Yaw) < 50.0f;
	}

	return false;
}

FVector ABMBaseCharacter::GetPlayerMovementInput()
{
	ABMPlayerController* PlayerController = Cast<ABMPlayerController>(GetController());
	check(PlayerController);
	return PlayerController->GetPlayerMovementInput();
}

FVector ABMBaseCharacter::GetMovementInput()
{
	return GetCharacterMovement()->GetCurrentAcceleration();
}

float ABMBaseCharacter::GetAnimCurveValue(FName CurveName)
{
	if (MainAnimInstance)
	{
		return MainAnimInstance->GetCurveValue(CurveName);
	}

	return 0.0f;
}

ECollisionChannel ABMBaseCharacter::GetTraceParams(FVector& TraceOrigin, float& TraceRadius)
{
	TraceOrigin = GetActorLocation();
	TraceRadius = 10.0f;
	return ECC_Visibility;
}

FTransform ABMBaseCharacter::GetPivotTarget()
{
	return GetActorTransform();
}

void ABMBaseCharacter::GetCameraParameters(float& FOVOut, bool& bRightShoulderOut)
{
	FOVOut = FOV;
	bRightShoulderOut = bRightShoulder;
}

void ABMBaseCharacter::RagdollUpdate()
{
	// Set the Last Ragdoll Velocity.
	LastRagdollVelocity = GetMesh()->GetPhysicsLinearVelocity(FName(TEXT("Root")));

	// Use the Ragdoll Velocity to scale the ragdoll's joint strength for physical animation.
	const float SpringValue = FMath::GetMappedRangeValueClamped(FVector2D(0.0f, 1000.0f),
	                                                            FVector2D(0.0f, 25000.0f), LastRagdollVelocity.Size());
	GetMesh()->SetAllMotorsAngularDriveParams(SpringValue, 0.0f, 0.0f, false);

	// Disable Gravity if falling faster than -4000 to prevent continual acceleration.
	// This also prevents the ragdoll from going through the floor.
	const bool bEnableGrav = LastRagdollVelocity.Z > -4000.0f;
	GetMesh()->SetEnableGravity(bEnableGrav);

	// Update the Actor location to follow the ragdoll.
	SetActorLocationDuringRagdoll();
}

void ABMBaseCharacter::SetActorLocationDuringRagdoll()
{
	// Set the pelvis as the target location.
	const FVector TargetRagdollLocation = GetMesh()->GetSocketLocation(FName(TEXT("Pelvis")));

	// Determine wether the ragdoll is facing up or down and set the target rotation accordingly.
	const FRotator PelvisRot = GetMesh()->GetSocketRotation(FName(TEXT("Pelvis")));

	bRagdollFaceUp = PelvisRot.Roll < 0.0f;

	const FRotator TargetRagdollRotation =
		FRotator(0.0f, bRagdollFaceUp ? PelvisRot.Yaw - 180.0f : PelvisRot.Yaw, 0.0f);

	// Trace downward from the target location to offset the target location,
	// preventing the lower half of the capsule from going through the floor when the ragdoll is laying on the ground.
	const FVector TraceVect(TargetRagdollLocation.X, TargetRagdollLocation.Y,
	                        TargetRagdollLocation.Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight());

	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);

	FHitResult HitResult;
	GetWorld()->LineTraceSingleByChannel(HitResult, TargetRagdollLocation, TraceVect,
	                                     ECC_Visibility, Params);

	bRagdollOnGround = HitResult.IsValidBlockingHit();
	if (bRagdollOnGround)
	{
		const float ImpactDistZ = FMath::Abs(HitResult.ImpactPoint.Z - HitResult.TraceStart.Z);
		FVector NewRagdollLoc = TargetRagdollLocation;
		NewRagdollLoc.Z += GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - ImpactDistZ + 2.0f;
		SetActorLocationAndTargetRotation(NewRagdollLoc, TargetRagdollRotation);
	}
	else
	{
		SetActorLocationAndTargetRotation(TargetRagdollLocation, TargetRagdollRotation);
	}
}

void ABMBaseCharacter::OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PrevMovementMode, PreviousCustomMode);

	// Use the Character Movement Mode changes to set the Movement States to the right values. This allows you to have
	// a custom set of movement states but still use the functionality of the default character movement component.

	if (GetCharacterMovement()->MovementMode == MOVE_Walking ||
		GetCharacterMovement()->MovementMode == MOVE_NavWalking)
	{
		SetMovementState(EBMMovementState::Grounded);
	}
	else if (GetCharacterMovement()->MovementMode == MOVE_Falling)
	{
		SetMovementState(EBMMovementState::InAir);
	}
}

void ABMBaseCharacter::OnMovementStateChanged(const EBMMovementState PreviousState)
{
	if (MovementState == EBMMovementState::InAir)
	{
		if (MovementAction == EBMMovementAction::None)
		{
			// If the character enters the air, set the In Air Rotation and uncrouch if crouched.
			InAirRotation = GetActorRotation();
			if (Stance == EBMStance::Crouching)
			{
				UnCrouch();
			}
		}
		else if (MovementAction == EBMMovementAction::Rolling)
		{
			// If the character is currently rolling, enable the ragdoll.
			RagdollStart();
		}
	}
	else if (MovementState == EBMMovementState::Ragdoll && PreviousState == EBMMovementState::Mantling)
	{
		// Stop the Mantle Timeline if transitioning to the ragdoll state while mantling.
		MantleTimeline->Stop();
	}
}

void ABMBaseCharacter::OnMovementActionChanged(const EBMMovementAction PreviousAction)
{
	// Make the character crouch if performing a roll.
	if (MovementAction == EBMMovementAction::Rolling)
	{
		Crouch();
	}

	if (PreviousAction == EBMMovementAction::Rolling)
	{
		if (DesiredStance == EBMStance::Standing)
		{
			UnCrouch();
		}
		else if (DesiredStance == EBMStance::Crouching)
		{
			Crouch();
		}
	}
}

void ABMBaseCharacter::OnStanceChanged(const EBMStance PreviousStance)
{
}

void ABMBaseCharacter::OnRotationModeChanged(EBMRotationMode PreviousRotationMode)
{
}

void ABMBaseCharacter::OnGaitChanged(const EBMGait PreviousGait)
{
}

void ABMBaseCharacter::OnOverlayStateChanged(const EBMOverlayState PreviousState)
{
}

void ABMBaseCharacter::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

	SetStance(EBMStance::Crouching);
}

void ABMBaseCharacter::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

	SetStance(EBMStance::Standing);
}

void ABMBaseCharacter::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();

	check(MainAnimInstance);

	// Set the new In Air Rotation to the velocity rotation if speed is greater than 100.
	InAirRotation = Speed > 100.0f ? LastVelocityRotation : GetActorRotation();
	MainAnimInstance->OnJumped();
}

void ABMBaseCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	const float VelZ = FMath::Abs(GetCharacterMovement()->Velocity.Z);

	if (bHasMovementInput && VelZ >= 600.0f && VelZ <= 1000.0f)
	{
		OnBreakfall();
	}
	else if (VelZ > 1000.0f)
	{
		RagdollStart();
	}
	else
	{
		GetCharacterMovement()->BrakingFrictionFactor = bHasMovementInput ? 0.5f : 3.0f;

		// After 0.5 secs, reset braking friction factor to zero
		GetWorldTimerManager().SetTimer(OnLandedFrictionResetTimer, this,
		                                &ABMBaseCharacter::OnLandFrictionReset, 0.5f, false);
	}
}

void ABMBaseCharacter::OnLandFrictionReset()
{
	// Reset the braking friction
	GetCharacterMovement()->BrakingFrictionFactor = 0.0f;
}

void ABMBaseCharacter::OnTimelineUpdated(float BlendIn)
{
	MantleUpdate(BlendIn);
}

void ABMBaseCharacter::OnTimelineFinished()
{
	MantleEnd();
}

void ABMBaseCharacter::SetEssentialValues(float DeltaTime)
{
	// These values represent how the capsule is moving as well as how it wants to move, and therefore are essential
	// for any data driven animation system. They are also used throughout the system for various functions,
	// so I found it is easiest to manage them all in one place.

	const FVector CurrentVel = GetVelocity();

	// Set the amount of Acceleration.
	Acceleration = (CurrentVel - PreviousVelocity) / DeltaTime;

	// Determine if the character is moving by getting it's speed. The Speed equals the length of the horizontal (x y)
	// velocity, so it does not take vertical movement into account. If the character is moving, update the last
	// velocity rotation. This value is saved because it might be useful to know the last orientation of movement
	// even after the character has stopped.
	Speed = CurrentVel.Size2D();
	bIsMoving = Speed > 1.0f;
	if (bIsMoving)
	{
		LastVelocityRotation = CurrentVel.ToOrientationRotator();
	}

	// Determine if the character has movement input by getting its movement input amount.
	// The Movement Input Amount is equal to the current acceleration divided by the max acceleration so that
	// it has a range of 0-1, 1 being the maximum possible amount of input, and 0 beiung none.
	// If the character has movement input, update the Last Movement Input Rotation.
	FVector CurAcc = GetCharacterMovement()->GetCurrentAcceleration();
	MovementInputAmount = CurAcc.Size() / GetCharacterMovement()->GetMaxAcceleration();
	bHasMovementInput = MovementInputAmount > 0.0f;
	if (bHasMovementInput)
	{
		LastMovementInputRotation = CurAcc.ToOrientationRotator();
	}

	// Set the Aim Yaw rate by comparing the current and previous Aim Yaw value, divided by Delta Seconds.
	// This represents the speed the camera is rotating left to right.
	AimYawRate = FMath::Abs((GetControlRotation().Yaw - PreviousAimYaw) / DeltaTime);
}

void ABMBaseCharacter::UpdateCharacterMovement()
{
	// Set the Allowed Gait
	const EBMGait AllowedGait = GetAllowedGait();

	// Determine the Actual Gait. If it is different from the current Gait, Set the new Gait Event.
	const EBMGait ActualGait = GetActualGait(AllowedGait);

	if (ActualGait != Gait)
	{
		SetGait(ActualGait);
	}

	// Use the allowed gait to update the movement settings.
	UpdateDynamicMovementSettings(AllowedGait);
}

void ABMBaseCharacter::UpdateDynamicMovementSettings(EBMGait AllowedGait)
{
	// Get the Current Movement Settings.
	CurrentMovementSettings = GetTargetMovementSettings();

	// Update the Character Max Walk Speed to the configured speeds based on the currently Allowed Gait.
	GetCharacterMovement()->MaxWalkSpeed = CurrentMovementSettings.GetSpeedForGait(AllowedGait);
	GetCharacterMovement()->MaxWalkSpeedCrouched = GetCharacterMovement()->MaxWalkSpeed;

	// Update the Acceleration, Deceleration, and Ground Friction using the Movement Curve.
	// This allows for fine control over movement behavior at each speed (May not be suitable for replication).
	const float MappedSpeed = GetMappedSpeed();
	const FVector CurveVec = CurrentMovementSettings.MovementCurve->GetVectorValue(MappedSpeed);
	GetCharacterMovement()->MaxAcceleration = CurveVec.X;
	GetCharacterMovement()->BrakingDecelerationWalking = CurveVec.Y;
	GetCharacterMovement()->GroundFriction = CurveVec.Z;
}

void ABMBaseCharacter::UpdateGroundedRotation(float DeltaTime)
{
	if (MovementAction == EBMMovementAction::None)
	{
		const bool bCanUpdateMovingRot = ((bIsMoving && bHasMovementInput) || Speed > 150.0f) && !HasAnyRootMotion();
		if (bCanUpdateMovingRot)
		{
			const float GroundedRotationRate = CalculateGroundedRotationRate();
			if (RotationMode == EBMRotationMode::VelocityDirection)
			{
				// Velocity Direction Rotation
				SmoothCharacterRotation(FRotator(0.0f, LastVelocityRotation.Yaw, 0.0f),
				                        800.0f, GroundedRotationRate, DeltaTime);
			}
			else if (RotationMode == EBMRotationMode::LookingDirection)
			{
				// Looking Direction Rotation
				float YawValue;
				if (Gait == EBMGait::Sprinting)
				{
					YawValue = LastVelocityRotation.Yaw;
				}
				else
				{
					// Walking or Running..
					check(MainAnimInstance);
					const float YawOffsetCurveVal = MainAnimInstance->GetCurveValue(FName(TEXT("YawOffset")));
					YawValue = GetControlRotation().Yaw + YawOffsetCurveVal;
				}
				SmoothCharacterRotation(FRotator(0.0f, YawValue, 0.0f),
				                        500.0f, GroundedRotationRate, DeltaTime);
			}
			else if (RotationMode == EBMRotationMode::Aiming)
			{
				const float ControlYaw = GetControlRotation().Yaw;
				SmoothCharacterRotation(FRotator(0.0f, ControlYaw, 0.0f),
				                        1000.0f, 20.0f, DeltaTime);
			}
		}
		else
		{
			// Not Moving

			if (RotationMode == EBMRotationMode::Aiming)
			{
				LimitRotation(-100.0f, 100.0f, 20.0f, DeltaTime);
			}

			// Apply the RotationAmount curve from Turn In Place Animations.
			// The Rotation Amount curve defines how much rotation should be applied each frame,
			// and is calculated for animations that are animated at 30fps.

			check(MainAnimInstance);
			const float RotAmountCurve = MainAnimInstance->GetCurveValue(FName(TEXT("RotationAmount")));

			if (FMath::Abs(RotAmountCurve) > 0.001f)
			{
				AddActorWorldRotation(
					FRotator(0.0f, RotAmountCurve * (DeltaTime / (1.0f / 30.0f)), 0.0f));
				TargetRotation = GetActorRotation();
			}
		}
	}
	else if (MovementAction == EBMMovementAction::Rolling)
	{
		// Rolling Rotation

		if (bHasMovementInput)
		{
			SmoothCharacterRotation(FRotator(0.0f, LastMovementInputRotation.Yaw, 0.0f),
			                        0.0f, 2.0f, DeltaTime);
		}
	}

	// Other actions are ignored...
}

void ABMBaseCharacter::UpdateInAirRotation(float DeltaTime)
{
	if (RotationMode == EBMRotationMode::VelocityDirection || RotationMode == EBMRotationMode::LookingDirection)
	{
		// Velocity / Looking Direction Rotation
		SmoothCharacterRotation(FRotator(0.0f, InAirRotation.Yaw, 0.0f),
		                        0.0f, 5.0f, DeltaTime);
	}
	else if (RotationMode == EBMRotationMode::Aiming)
	{
		// Aiming Rotation
		SmoothCharacterRotation(FRotator(0.0f, GetControlRotation().Yaw, 0.0f),
		                        0.0f, 15.0f, DeltaTime);
		InAirRotation = GetActorRotation();
	}
}

static FTransform TransfromSub(const FTransform& T1, const FTransform& T2)
{
	return FTransform(T1.Rotator() - T2.Rotator(),
	                  T1.GetLocation() - T2.GetLocation(), T1.GetScale3D() - T2.GetScale3D());
}

static FTransform TransfromAdd(const FTransform& T1, const FTransform& T2)
{
	return FTransform(T1.Rotator() + T2.Rotator(),
	                  T1.GetLocation() + T2.GetLocation(), T1.GetScale3D() + T2.GetScale3D());
}

static FVector GetCapsuleBaseLocation(const float ZOffset, UCapsuleComponent* Capsule)
{
	return Capsule->GetComponentLocation() -
		Capsule->GetUpVector() * (Capsule->GetScaledCapsuleHalfHeight() + ZOffset);
}

static FVector GetCapsuleLocationFromBase(FVector BaseLocation, const float ZOffset, UCapsuleComponent* Capsule)
{
	BaseLocation.Z += Capsule->GetScaledCapsuleHalfHeight() + ZOffset;
	return BaseLocation;
}

void ABMBaseCharacter::MantleStart(float MantleHeight, const FBMComponentAndTransform& MantleLedgeWS, EBMMantleType MantleType)
{
	// Step 1: Get the Mantle Asset and use it to set the new Mantle Params.
	const FBMMantleAsset& MantleAsset = GetMantleAsset(MantleType);

	MantleParams.AnimMontage = MantleAsset.AnimMontage;
	MantleParams.PositionCorrectionCurve = MantleAsset.PositionCorrectionCurve;
	MantleParams.StartingOffset = MantleAsset.StartingOffset;
	MantleParams.StartingPosition =
		FMath::GetMappedRangeValueClamped(FVector2D(MantleAsset.LowHeight, MantleAsset.HighHeight),
		                                  FVector2D(MantleAsset.LowStartPosition, MantleAsset.HighStartPosition), MantleHeight);
	MantleParams.PlayRate =
		FMath::GetMappedRangeValueClamped(FVector2D(MantleAsset.LowHeight, MantleAsset.HighHeight),
		                                  FVector2D(MantleAsset.LowPlayRate, MantleAsset.HighPlayRate), MantleHeight);

	// Step 2: Convert the world space target to the mantle component's local space for use in moving objects.
	MantleLedgeLS.Component = MantleLedgeWS.Component;
	MantleLedgeLS.Transform = MantleLedgeWS.Transform * MantleLedgeWS.Component->GetComponentToWorld().Inverse();

	// Step 3: Set the Mantle Target and calculate the Starting Offset
	// (offset amount between the actor and target transform).
	MantleTarget = MantleLedgeWS.Transform;
	MantleActualStartOffset = TransfromSub(GetActorTransform(), MantleTarget);

	// Step 4: Calculate the Animated Start Offset from the Target Location.
	// This would be the location the actual animation starts at relative to the Target Transform.
	FVector RotatedVector = MantleTarget.GetRotation().Vector() * MantleParams.StartingOffset.Y;
	RotatedVector.Z = MantleParams.StartingOffset.Z;
	const FTransform StartOffset(MantleTarget.Rotator(), MantleTarget.GetLocation() - RotatedVector,
	                             FVector::OneVector);
	MantleAnimatedStartOffset = TransfromSub(StartOffset, MantleTarget);

	// Step 5: Clear the Character Movement Mode and set the Movement State to Mantling
	GetCharacterMovement()->SetMovementMode(MOVE_None);
	SetMovementState(EBMMovementState::Mantling);

	// Step 6: Configure the Mantle Timeline so that it is the same length as the
	// Lerp/Correction curve minus the starting position, and plays at the same speed as the animation.
	// Then start the timeline.
	float MinTime = 0.0f;
	float MaxTime = 0.0f;
	MantleParams.PositionCorrectionCurve->GetTimeRange(MinTime, MaxTime);
	MantleTimeline->SetTimelineLength(MaxTime - MantleParams.StartingPosition);
	MantleTimeline->SetPlayRate(MantleParams.PlayRate);
	MantleTimeline->PlayFromStart();

	// Step 7: Play the Anim Montaget if valid.
	if (IsValid(MantleParams.AnimMontage))
	{
		check(MainAnimInstance);
		MainAnimInstance->Montage_Play(MantleParams.AnimMontage, MantleParams.PlayRate,
		                               EMontagePlayReturnType::MontageLength, MantleParams.StartingPosition, false);
	}

	// Step 8: Prevent Incorrect Rotation
	FRotator ForcedRotation = GetCapsuleComponent()->GetComponentRotation();
	ForcedRotation.Yaw = MantleTarget.Rotator().Yaw;
	GetCapsuleComponent()->SetWorldRotation(ForcedRotation);
}

bool ABMBaseCharacter::MantleCheck(const FBMMantleTraceSettings& TraceSettings, EDrawDebugTrace::Type DebugType)
{
	if (!bAllowMantle)
	{
		return false;
	}

	// Step 1: Trace forward to find a wall / object the character cannot walk on.
	const FVector& CapsuleBaseLocation = GetCapsuleBaseLocation(2.0f, GetCapsuleComponent());
	FVector TraceStart = CapsuleBaseLocation + GetPlayerMovementInput() * -30.0f;
	TraceStart.Z += (TraceSettings.MaxLedgeHeight + TraceSettings.MinLedgeHeight) / 2.0f;
	const FVector TraceEnd = TraceStart + (GetPlayerMovementInput() * TraceSettings.ReachDistance);
	const float HalfHeight = 1.0f + ((TraceSettings.MaxLedgeHeight - TraceSettings.MinLedgeHeight) / 2.0f);

	UWorld* World = GetWorld();
	check(World);

	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);

	FHitResult HitResult;
	// ECC_GameTraceChannel2 -> Climbable
	World->SweepSingleByChannel(HitResult, TraceStart, TraceEnd, FQuat::Identity, ECC_GameTraceChannel2,
	                            FCollisionShape::MakeCapsule(TraceSettings.ForwardTraceRadius, HalfHeight), Params);

	if (!HitResult.IsValidBlockingHit() || GetCharacterMovement()->IsWalkable(HitResult))
	{
		// Not a valid surface to mantle
		return false;
	}

	const FVector InitialTraceImpactPoint = HitResult.ImpactPoint;
	const FVector InitialTraceNormal = HitResult.ImpactNormal;

	// Step 2: Trace downward from the first trace's Impact Point and determine if the hit location is walkable.
	FVector DownwardTraceEnd = InitialTraceImpactPoint;
	DownwardTraceEnd.Z = CapsuleBaseLocation.Z;
	DownwardTraceEnd += InitialTraceNormal * -15.0f;
	FVector DownwardTraceStart = DownwardTraceEnd;
	DownwardTraceStart.Z += TraceSettings.MaxLedgeHeight + TraceSettings.DownwardTraceRadius + 1.0f;

	World->SweepSingleByChannel(HitResult, DownwardTraceStart, DownwardTraceEnd, FQuat::Identity,
	                            ECC_GameTraceChannel2, FCollisionShape::MakeSphere(TraceSettings.DownwardTraceRadius), Params);


	if (!GetCharacterMovement()->IsWalkable(HitResult))
	{
		// Not a valid surface to mantle
		return false;
	}

	const FVector DownTraceLocation(HitResult.Location.X, HitResult.Location.Y, HitResult.ImpactPoint.Z);
	UPrimitiveComponent* HitComponent = HitResult.GetComponent();

	// Step 3: Check if the capsule has room to stand at the downward trace's location.
	// If so, set that location as the Target Transform and calculate the mantle height.
	const FVector& CapsuleLocationFBase = GetCapsuleLocationFromBase(DownTraceLocation, 2.0f, GetCapsuleComponent());
	const bool bCapsuleHasRoom = CapsuleHasRoomCheck(GetCapsuleComponent(), CapsuleLocationFBase, 0.0f,
	                                                 0.0f, DebugType);

	if (!bCapsuleHasRoom)
	{
		// Capsule doesn't have enough room to mantle
		return false;
	}

	const FTransform TargetTransform(
		(InitialTraceNormal * FVector(-1.0f, -1.0f, 0.0f)).ToOrientationRotator(),
		CapsuleLocationFBase,
		FVector::OneVector);

	const float MantleHeight = (CapsuleLocationFBase - GetActorLocation()).Z;

	// Step 4: Determine the Mantle Type by checking the movement mode and Mantle Height.
	EBMMantleType MantleType;
	if (MovementState == EBMMovementState::InAir)
	{
		MantleType = EBMMantleType::FallingCatch;
	}
	else
	{
		MantleType = MantleHeight > 125.0f ? EBMMantleType::HighMantle : EBMMantleType::LowMantle;
	}

	// Step 5: If everything checks out, start the Mantle
	FBMComponentAndTransform MantleWS;
	MantleWS.Component = HitComponent;
	MantleWS.Transform = TargetTransform;
	MantleStart(MantleHeight, MantleWS, MantleType);

	return true;
}

static FTransform MantleComponentLocalToWorld(FBMComponentAndTransform CompAndTransform)
{
	const FTransform& InverseTransform = CompAndTransform.Component->GetComponentToWorld().Inverse();
	const FVector Location = InverseTransform.InverseTransformPosition(CompAndTransform.Transform.GetLocation());
	const FQuat Quat = InverseTransform.InverseTransformRotation(CompAndTransform.Transform.GetRotation());
	const FVector Scale = InverseTransform.InverseTransformPosition(CompAndTransform.Transform.GetScale3D());
	return {Quat, Location, Scale};
}

void ABMBaseCharacter::MantleUpdate(float BlendIn)
{
	// Step 1: Continually update the mantle target from the stored local transform to follow along with moving objects
	MantleTarget = MantleComponentLocalToWorld(MantleLedgeLS);

	// Step 2: Update the Position and Correction Alphas using the Position/Correction curve set for each Mantle.
	const FVector CurveVec = MantleParams.PositionCorrectionCurve
	                                     ->GetVectorValue(MantleParams.StartingPosition + MantleTimeline->GetPlaybackPosition());
	const float PositionAlpha = CurveVec.X;
	const float XYCorrectionAlpha = CurveVec.Y;
	const float ZCorrectionAlpha = CurveVec.Z;

	// Step 3: Lerp multiple transforms together for independent control over the horizontal
	// and vertical blend to the animated start position, as well as the target position.

	// Blend into the animated horizontal and rotation offset using the Y value of the Position/Correction Curve.
	const FTransform TargetHzTransform(MantleAnimatedStartOffset.GetRotation(),
	                                   {
		                                   MantleAnimatedStartOffset.GetLocation().X, MantleAnimatedStartOffset.GetLocation().Y,
		                                   MantleActualStartOffset.GetLocation().Z
	                                   },
	                                   FVector::OneVector);
	const FTransform& HzLerpResult =
		UKismetMathLibrary::TLerp(MantleActualStartOffset, TargetHzTransform, XYCorrectionAlpha);

	// Blend into the animated vertical offset using the Z value of the Position/Correction Curve.
	const FTransform TargetVtTransform(MantleActualStartOffset.GetRotation(),
	                                   {
		                                   MantleActualStartOffset.GetLocation().X, MantleActualStartOffset.GetLocation().Y,
		                                   MantleAnimatedStartOffset.GetLocation().Z
	                                   },
	                                   FVector::OneVector);
	const FTransform& VtLerpResult =
		UKismetMathLibrary::TLerp(MantleActualStartOffset, TargetVtTransform, ZCorrectionAlpha);

	const FTransform ResultTransform(HzLerpResult.GetRotation(),
	                                 FVector(HzLerpResult.GetLocation().X, HzLerpResult.GetLocation().Y, VtLerpResult.GetLocation().Z),
	                                 FVector::OneVector);

	// Blend from the currently blending transforms into the final mantle target using the X
	// value of the Position/Correction Curve.
	const FTransform& ResultLerp = UKismetMathLibrary::TLerp(TransfromAdd(MantleTarget, ResultTransform), MantleTarget, PositionAlpha);

	// Initial Blend In (controlled in the timeline curve) to allow the actor to blend into the Position/Correction
	// curve at the midoint. This prevents pops when mantling an object lower than the animated mantle.
	const FTransform& LerpedTarget =
		UKismetMathLibrary::TLerp(TransfromAdd(MantleTarget, MantleActualStartOffset), ResultLerp, BlendIn);

	// Step 4: Set the actors location and rotation to the Lerped Target.
	SetActorLocationAndTargetRotation(LerpedTarget.GetLocation(), LerpedTarget.Rotator());
}

void ABMBaseCharacter::MantleEnd()
{
	// Set the Character Movement Mode to Walking
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
}

bool ABMBaseCharacter::CapsuleHasRoomCheck(UCapsuleComponent* Capsule, FVector TargetLocation, float HeightOffset,
                                           float RadiusOffset, EDrawDebugTrace::Type DebugType)
{
	// Perform a trace to see if the capsule has room to be at the target location.
	const float ZTarget = Capsule->GetScaledCapsuleHalfHeight_WithoutHemisphere() - RadiusOffset + HeightOffset;
	FVector TraceStart = TargetLocation;
	TraceStart.Z += ZTarget;
	FVector TraceEnd = TargetLocation;
	TraceEnd.Z -= ZTarget;
	const float Radius = Capsule->GetUnscaledCapsuleRadius() + RadiusOffset;

	const UWorld* World = GetWorld();
	check(World);

	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);

	FHitResult HitResult;
	World->SweepSingleByProfile(HitResult, TraceStart, TraceEnd, FQuat::Identity,
	                            FName(TEXT("ALS_Character")), FCollisionShape::MakeSphere(Radius), Params);

	return !(HitResult.bBlockingHit || HitResult.bStartPenetrating);
}

float ABMBaseCharacter::GetMappedSpeed()
{
	// Map the character's current speed to the configured movement speeds with a range of 0-3,
	// with 0 = stopped, 1 = the Walk Speed, 2 = the Run Speed, and 3 = the Sprint Speed.
	// This allows us to vary the movement speeds but still use the mapped range in calculations for consistent results

	const float LocWalkSpeed = CurrentMovementSettings.WalkSpeed;
	const float LocRunSpeed = CurrentMovementSettings.RunSpeed;
	const float LocSprintSpeed = CurrentMovementSettings.SprintSpeed;

	if (Speed > LocRunSpeed)
	{
		return FMath::GetMappedRangeValueClamped(FVector2D(LocRunSpeed, LocSprintSpeed),
		                                         FVector2D(2.0f, 3.0f), Speed);
	}

	if (Speed > LocWalkSpeed)
	{
		return FMath::GetMappedRangeValueClamped(FVector2D(LocWalkSpeed, LocRunSpeed),
		                                         FVector2D(1.0f, 2.0f), Speed);
	}

	return FMath::GetMappedRangeValueClamped(FVector2D(0.0f, LocWalkSpeed),
	                                         FVector2D(0.0f, 1.0f), Speed);
}

EBMGait ABMBaseCharacter::GetAllowedGait()
{
	// Calculate the Allowed Gait. This represents the maximum Gait the character is currently allowed to be in,
	// and can be determined by the desired gait, the rotation mode, the stance, etc. For example,
	// if you wanted to force the character into a walking state while indoors, this could be done here.

	if (Stance == EBMStance::Standing)
	{
		if (RotationMode != EBMRotationMode::Aiming)
		{
			if (DesiredGait == EBMGait::Sprinting)
			{
				return CanSprint() ? EBMGait::Sprinting : EBMGait::Running;
			}
			return DesiredGait;
		}
	}

	// Crouching stance & Aiming rot mode has same behaviour

	if (DesiredGait == EBMGait::Sprinting)
	{
		return EBMGait::Running;
	}

	return DesiredGait;
}

EBMGait ABMBaseCharacter::GetActualGait(EBMGait AllowedGait)
{
	// Get the Actual Gait. This is calculated by the actual movement of the character,  and so it can be different
	// from the desired gait or allowed gait. For instance, if the Allowed Gait becomes walking,
	// the Actual gait will still be running untill the character decelerates to the walking speed.

	const float LocWalkSpeed = CurrentMovementSettings.WalkSpeed;
	const float LocRunSpeed = CurrentMovementSettings.RunSpeed;

	if (Speed > LocRunSpeed + 10.0f)
	{
		if (AllowedGait == EBMGait::Sprinting)
		{
			return EBMGait::Sprinting;
		}
		return EBMGait::Running;
	}

	if (Speed >= LocWalkSpeed + 10.0f)
	{
		return EBMGait::Running;
	}

	return EBMGait::Walking;
}

void ABMBaseCharacter::SmoothCharacterRotation(FRotator Target, float TargetInterpSpeed, float ActorInterpSpeed,
                                               float DeltaTime)
{
	if (bEnableOptimization)
	{
		TargetRotation = Target;
		SetActorRotation(
			FMath::RInterpTo(GetActorRotation(), Target, DeltaTime, ActorInterpSpeed));
	}
	else
	{
		// Interpolate the Target Rotation for extra smooth rotation behavior
		TargetRotation =
			FMath::RInterpConstantTo(TargetRotation, Target, DeltaTime, TargetInterpSpeed);
		SetActorRotation(
			FMath::RInterpTo(GetActorRotation(), TargetRotation, DeltaTime, ActorInterpSpeed));
	}
}

float ABMBaseCharacter::CalculateGroundedRotationRate()
{
	// Calculate the rotation rate by using the current Rotation Rate Curve in the Movement Settings.
	// Using the curve in conjunction with the mapped speed gives you a high level of control over the rotation
	// rates for each speed. Increase the speed if the camera is rotating quickly for more responsive rotation.

	const float MappedSpeedVal = GetMappedSpeed();
	const float CurveVal =
		CurrentMovementSettings.RotationRateCurve->GetFloatValue(MappedSpeedVal);
	const float ClampedAimYawRate = FMath::GetMappedRangeValueClamped(FVector2D(0.0f, 300.0f),
	                                                                  FVector2D(1.0f, 3.0f), AimYawRate);
	return CurveVal * ClampedAimYawRate;
}

void ABMBaseCharacter::LimitRotation(float AimYawMin, float AimYawMax, float InterpSpeed, float DeltaTime)
{
	// Prevent the character from rotating past a certain angle.
	FRotator Delta = GetControlRotation() - GetActorRotation();
	Delta.Normalize();
	const float RangeVal = Delta.Yaw;

	if (RangeVal < AimYawMin || RangeVal > AimYawMax)
	{
		const float ControlRotYaw = GetControlRotation().Yaw;
		const float TargetYaw = ControlRotYaw + (RangeVal > 0.0f ? AimYawMin : AimYawMax);
		SmoothCharacterRotation(FRotator(0.0f, TargetYaw, 0.0f),
		                        0.0f, InterpSpeed, DeltaTime);
	}
}
