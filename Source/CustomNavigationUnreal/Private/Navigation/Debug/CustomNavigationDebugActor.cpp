// Created by Stalker7274

#include "Navigation/Debug/CustomNavigationDebugActor.h"
#include "Navigation/Debug/CustomNavigationDebugComponent.h"
#include "Navigation/CustomNavigationWorldSubsystem.h"
#include "Navigation/CustomNavigationHardcodedProvider.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(CustomNavigationDebugActor)

ACustomNavigationDebugActor::ACustomNavigationDebugActor()
{
	PrimaryActorTick.bCanEverTick = false;

	BuildVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("BuildVolume"));
	BuildVolume->SetBoxExtent(FVector(2200.0, 2200.0, 500.0));
	BuildVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BuildVolume->SetCanEverAffectNavigation(false);
	BuildVolume->ShapeColor = FColor(80, 200, 255);
	BuildVolume->bHiddenInGame = true;
	RootComponent = BuildVolume;

	DebugDraw = CreateDefaultSubobject<UCustomNavigationDebugComponent>(TEXT("DebugDraw"));
	DebugDraw->SetupAttachment(BuildVolume);
	DebugDraw->SetUsingAbsoluteLocation(true);
	DebugDraw->SetUsingAbsoluteRotation(true);
	DebugDraw->SetUsingAbsoluteScale(true);
}

FCustomNavRegionKey ACustomNavigationDebugActor::GetRegionKey() const
{
	return FCustomNavRegionKey(RegionId.IsNone() ? GetFName() : RegionId, AgentProfile);
}

UCustomNavigationWorldSubsystem* ACustomNavigationDebugActor::BindToSubsystem()
{
	UCustomNavigationWorldSubsystem* Subsystem = UCustomNavigationWorldSubsystem::Get(GetWorld());
	if (!Subsystem || Subsystem->IsClosing())
	{
		return nullptr;
	}
	if (BoundSubsystem.Get() != Subsystem)
	{
		UnbindFromSubsystem();
		BoundSubsystem = Subsystem;
		BuildFinishedHandle = Subsystem->OnRegionBuildFinished.AddUObject(this, &ACustomNavigationDebugActor::HandleRegionBuildFinished);
	}
	return Subsystem;
}

void ACustomNavigationDebugActor::UnbindFromSubsystem()
{
	if (UCustomNavigationWorldSubsystem* Subsystem = BoundSubsystem.Get())
	{
		if (!Subsystem->IsClosing())
		{
			Subsystem->OnRegionBuildFinished.Remove(BuildFinishedHandle);
			if (TestSceneHandle.IsValid())
			{
				Subsystem->UnregisterGeometryProvider(TestSceneHandle);
			}
			if (RequestedKey.RegionId != NAME_None)
			{
				Subsystem->RemoveRegion(RequestedKey);
			}
		}
	}
	BoundSubsystem.Reset();
	BuildFinishedHandle.Reset();
	TestSceneHandle.Reset();
	TestSceneProvider.Reset();
	TestSceneBounds = FBox(ForceInit);
	RequestedKey = FCustomNavRegionKey();
	TestSceneBuiltHalfSize = -1.0;
}

void ACustomNavigationDebugActor::RefreshTestScene(UCustomNavigationWorldSubsystem& Subsystem)
{
	if (!bUseHardcodedTestScene)
	{
		if (TestSceneHandle.IsValid())
		{
			Subsystem.UnregisterGeometryProvider(TestSceneHandle); // invalidates all regions
			TestSceneHandle.Reset();
			TestSceneProvider.Reset();
			TestSceneBounds = FBox(ForceInit);
			TestSceneBuiltHalfSize = -1.0;
		}
		return;
	}

	const FVector Extent = BuildVolume->GetScaledBoxExtent();
	const double HalfSize = TestSceneHalfSize > 0.f ? TestSceneHalfSize : 0.9 * FMath::Min(Extent.X, Extent.Y);
	const FVector Center = GetActorLocation();

	// Unchanged scene: keep the published mesh (level load and component re-registration call this repeatedly).
	if (TestSceneHandle.IsValid() && TestSceneBuiltHalfSize == HalfSize && TestSceneCenter.Equals(Center))
	{
		return;
	}

	if (!TestSceneProvider.IsValid())
	{
		// A unique domain per actor, so two debug actors never produce the same source keys.
		TestSceneProvider = MakeShared<FCustomNavHardcodedGeometryProvider>(FName(*FString::Printf(TEXT("DebugActor_%s"), *GetPathName())));
	}
	TestSceneProvider->Clear();
	TestSceneProvider->AddDefaultTestScene(Center, HalfSize);
	const FBox NewBounds = FBox(Center - FVector(HalfSize, HalfSize, 10.0), Center + FVector(HalfSize, HalfSize, 400.0));

	if (!TestSceneHandle.IsValid())
	{
		TestSceneHandle = Subsystem.RegisterGeometryProvider(TestSceneProvider.ToSharedRef()); // invalidates all regions
	}
	else
	{
		// Provider contract: report both the old and the new area after a change.
		Subsystem.InvalidateGeometry(TestSceneBounds);
		Subsystem.InvalidateGeometry(NewBounds);
	}
	TestSceneBounds = NewBounds;
	TestSceneCenter = Center;
	TestSceneBuiltHalfSize = HalfSize;
}

void ACustomNavigationDebugActor::BuildNavigation()
{
	UCustomNavigationWorldSubsystem* Subsystem = BindToSubsystem();
	if (!Subsystem)
	{
		LastBuildSummary = TEXT("No UCustomNavigationWorldSubsystem in this world type");
		return;
	}

	const FCustomNavRegionKey Key = GetRegionKey();
	if (RequestedKey.RegionId != NAME_None && RequestedKey != Key)
	{
		Subsystem->RemoveRegion(RequestedKey);
		DebugDraw->SetMesh(nullptr);
	}
	RequestedKey = Key;

	RefreshTestScene(*Subsystem);

	FCustomNavRegionRequest Request;
	Request.Key = Key;
	// Axis-aligned; actor rotation is ignored.
	Request.WorldBounds = FBox::BuildAABB(GetActorLocation(), BuildVolume->GetScaledBoxExtent());
	Request.Settings = Settings;
	const uint64 Revision = Subsystem->RequestRegionBuild(Request);
	LastBuildSummary = Revision ? FString::Printf(TEXT("Requested revision %llu..."), Revision) : TEXT("Request rejected");

	// Show the last good mesh (possibly stale) until the new one arrives.
	if (FCustomNavRegionMeshPtr Existing = Subsystem->GetPublishedRegion(Key, true))
	{
		if (DebugDraw->GetMesh() != Existing)
		{
			DebugDraw->SetMesh(Existing);
		}
	}
}

void ACustomNavigationDebugActor::ClearNavigation()
{
	UnbindFromSubsystem();
	DebugDraw->SetMesh(nullptr);
	LastBuildSummary.Reset();
}

void ACustomNavigationDebugActor::HandleRegionBuildFinished(const FCustomNavRegionKey& Key, const FCustomNavRegionStatus& Status)
{
	if (Key != RequestedKey)
	{
		return;
	}
	UCustomNavigationWorldSubsystem* Subsystem = BoundSubsystem.Get();
	FCustomNavRegionMeshPtr Mesh = Subsystem ? Subsystem->GetPublishedRegion(Key, /*bAllowStale*/ true) : nullptr;
	DebugDraw->SetMesh(Mesh);

	const FCustomNavDiagnostic* FirstError = Status.LastDiagnostics.FirstError();
	LastBuildSummary = FString::Printf(TEXT("%s rev %llu: %d polys, %d warnings, %d errors%s%s"),
	                                   *UEnum::GetValueAsString(Status.LastOutcome),
	                                   Status.PublishedRevision,
	                                   Mesh.IsValid() ? Mesh->NumPolygons() : 0,
	                                   Status.LastDiagnostics.WarningCount,
	                                   Status.LastDiagnostics.ErrorCount,
	                                   FirstError ? TEXT(" | ") : TEXT(""),
	                                   FirstError ? *FirstError->ToString() : TEXT(""));
}

void ACustomNavigationDebugActor::BeginPlay()
{
	Super::BeginPlay();
	if (bBuildOnBeginPlay)
	{
		BuildNavigation();
	}
}

void ACustomNavigationDebugActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindFromSubsystem();
	Super::EndPlay(EndPlayReason);
}

void ACustomNavigationDebugActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// Runs on placement, on every move (also while dragging) and on every property change.
	if (ShouldAutoBuildInEditor())
	{
		BuildNavigation();
	}
}

void ACustomNavigationDebugActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// Level load and World Partition streaming do not always rerun construction scripts; this always runs.
	// Repeated calls are cheap: an identical request and an unchanged test scene start nothing.
	if (ShouldAutoBuildInEditor())
	{
		BuildNavigation();
	}
}

bool ACustomNavigationDebugActor::ShouldAutoBuildInEditor() const
{
#if WITH_EDITOR
	const UWorld* World = GetWorld();
	// RF_Transient excludes the drag-and-drop preview actor from the content browser.
	return bAutoRebuildInEditor && World && World->WorldType == EWorldType::Editor && !IsTemplate() && !HasAnyFlags(RF_Transient) && IsValid(this);
#else
	return false;
#endif
}

void ACustomNavigationDebugActor::Destroyed()
{
	UnbindFromSubsystem();
	Super::Destroyed();
}
