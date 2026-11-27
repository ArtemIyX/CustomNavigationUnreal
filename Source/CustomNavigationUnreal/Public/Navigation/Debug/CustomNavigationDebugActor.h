// Created by Stalker7274

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Navigation/CustomNavigationTypes.h"
#include "Navigation/CustomNavigationGeometryProvider.h"
#include "CustomNavigationDebugActor.generated.h"

class UBoxComponent;
class UCustomNavigationDebugComponent;
class UCustomNavigationWorldSubsystem;
class FCustomNavHardcodedGeometryProvider;

/**
 * Drop-in test actor: defines a build volume, optionally registers a hardcoded test scene as a geometry provider,
 * requests an async build from UCustomNavigationWorldSubsystem and draws the published result.
 *
 * Editor worlds: builds when the level is loaded (or the actor is streamed in / placed) and after every move or property change.
 * PIE / game: builds on BeginPlay. The drawing follows the Navigation show flag (P in the editor).
 * Nothing else in the world is used as geometry yet: only providers registered with the subsystem contribute.
 */
UCLASS(hidecategories = (Input, Collision, Replication, HLOD, Physics, Networking, LevelInstance, Cooking))
class CUSTOMNAVIGATIONUNREAL_API ACustomNavigationDebugActor : public AActor
{
	GENERATED_BODY()

public:
	ACustomNavigationDebugActor();

	/** Requests a build of this actor's region (async). The drawing updates when the result is published. */
	UFUNCTION(BlueprintCallable, Category = "Custom Navigation")
	void BuildNavigation();

	/** Removes the region and the drawing. */
	UFUNCTION(BlueprintCallable, Category = "Custom Navigation")
	void ClearNavigation();

	/** Region id; empty = the actor's name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation")
	FName RegionId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation")
	FName AgentProfile = TEXT("Default");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation")
	FCustomNavBuildSettings Settings;

	/** Registers a hardcoded floor with obstacles centred on the actor as a geometry provider. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation")
	bool bUseHardcodedTestScene = true;

	/** Floor half size of the test scene [cm]; 0 = 90% of the build volume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation", meta = (ClampMin = "0.0", EditCondition = "bUseHardcodedTestScene"))
	float TestSceneHalfSize = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation")
	bool bBuildOnBeginPlay = true;

	/** Build in editor worlds automatically: on level load and after every move or property change. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation")
	bool bAutoRebuildInEditor = true;

	/** Result of the last finished build (outcome, polygon count, time, first error). */
	UPROPERTY(VisibleInstanceOnly, Transient, BlueprintReadOnly, Category = "Custom Navigation")
	FString LastBuildSummary;

	UBoxComponent* GetBuildVolume() const { return BuildVolume; }
	UCustomNavigationDebugComponent* GetDebugDrawComponent() const { return DebugDraw; }
	FCustomNavRegionKey GetRegionKey() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void Destroyed() override;

private:
	UCustomNavigationWorldSubsystem* BindToSubsystem();
	void UnbindFromSubsystem();
	void RefreshTestScene(UCustomNavigationWorldSubsystem& Subsystem);
	void HandleRegionBuildFinished(const FCustomNavRegionKey& Key, const FCustomNavRegionStatus& Status);
	bool ShouldAutoBuildInEditor() const;

	UPROPERTY(VisibleAnywhere, Category = "Custom Navigation")
	TObjectPtr<UBoxComponent> BuildVolume;

	UPROPERTY(VisibleAnywhere, Category = "Custom Navigation")
	TObjectPtr<UCustomNavigationDebugComponent> DebugDraw;

	TWeakObjectPtr<UCustomNavigationWorldSubsystem> BoundSubsystem;
	FDelegateHandle BuildFinishedHandle;
	TSharedPtr<FCustomNavHardcodedGeometryProvider> TestSceneProvider;
	FCustomNavGeometryProviderHandle TestSceneHandle;
	FBox TestSceneBounds = FBox(ForceInit);
	FVector TestSceneCenter = FVector::ZeroVector;
	double TestSceneBuiltHalfSize = -1.0;
	FCustomNavRegionKey RequestedKey;
};
