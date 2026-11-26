// Created by Stalker7274

#include "Navigation/Internal/CustomNavigationSnapshot.h"

namespace CustomNavigationPrivate
{
	static const FName StageValidate(TEXT("Validate"));

	/** Recast stores span heights in 13 bits. */
	static constexpr int64 MaxHeightCells = (1 << 13) - 1;

	/** Squared length of the cross product below which a triangle counts as degenerate (area < ~0.00005 cm^2). */
	static constexpr double DegenerateCrossSizeSquared = 1e-8;

	static bool IsFiniteBox(const FBox& Box)
	{
		return Box.IsValid && !Box.Min.ContainsNaN() && !Box.Max.ContainsNaN()
			&& FMath::IsFinite(Box.Min.X) && FMath::IsFinite(Box.Min.Y) && FMath::IsFinite(Box.Min.Z)
			&& FMath::IsFinite(Box.Max.X) && FMath::IsFinite(Box.Max.Y) && FMath::IsFinite(Box.Max.Z);
	}

	static bool IsFiniteVector(const FVector& V)
	{
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
	}
}

FString FCustomNavBuildToken::ToString() const
{
	return FString::Printf(TEXT("world=%llu key=%s rev=%llu request=%llu"), WorldLifetimeId, *Key.ToString(), DesiredRevision, RequestId);
}

namespace CustomNavigation
{
	FVector3f WorldToRecast(const FVector& World, const FVector& Origin)
	{
		const FVector Local = World - Origin; // double precision before narrowing
		return FVector3f(static_cast<float>(Local.X), static_cast<float>(Local.Z), static_cast<float>(Local.Y));
	}

	FVector RecastToWorld(const FVector3f& Recast, const FVector& Origin)
	{
		return RecastToWorld(Recast.X, Recast.Y, Recast.Z, Origin);
	}

	FVector RecastToWorld(float X, float Y, float Z, const FVector& Origin)
	{
		return FVector(static_cast<double>(X), static_cast<double>(Z), static_cast<double>(Y)) + Origin;
	}

	bool ComputeGridSize(const FBox& WorldBounds, const FCustomNavBuildSettings& Settings, int64& OutWidth, int64& OutDepth, int64& OutHeightCells)
	{
		OutWidth = OutDepth = OutHeightCells = 0;
		if (!CustomNavigationPrivate::IsFiniteBox(WorldBounds) || !(Settings.CellSize > 0.f) || !(Settings.CellHeight > 0.f))
		{
			return false;
		}
		const FVector Size = WorldBounds.GetSize();
		// Same rounding as rcCalcGridSize.
		OutWidth = static_cast<int64>(Size.X / Settings.CellSize + 0.5);
		OutDepth = static_cast<int64>(Size.Y / Settings.CellSize + 0.5);
		OutHeightCells = static_cast<int64>(FMath::CeilToDouble(Size.Z / Settings.CellHeight));
		return OutWidth > 0 && OutDepth > 0 && OutHeightCells > 0;
	}

	bool ValidateSettings(const FCustomNavBuildSettings& S, FCustomNavDiagnostics& Diagnostics)
	{
		using namespace CustomNavigationPrivate;
		const int32 ErrorsBefore = Diagnostics.ErrorCount;
		auto Fail = [&Diagnostics](const FString& Message)
		{
			Diagnostics.Error(ECustomNavDiagnosticCode::InvalidSettings, StageValidate, Message);
		};

		if (!FMath::IsFinite(S.CellSize) || S.CellSize <= 0.f) Fail(FString::Printf(TEXT("CellSize must be > 0 (got %f)"), S.CellSize));
		if (!FMath::IsFinite(S.CellHeight) || S.CellHeight <= 0.f) Fail(FString::Printf(TEXT("CellHeight must be > 0 (got %f)"), S.CellHeight));
		if (!FMath::IsFinite(S.AgentRadius) || S.AgentRadius < 0.f) Fail(FString::Printf(TEXT("AgentRadius must be >= 0 (got %f)"), S.AgentRadius));
		if (!FMath::IsFinite(S.AgentMaxClimb) || S.AgentMaxClimb < 0.f) Fail(FString::Printf(TEXT("AgentMaxClimb must be >= 0 (got %f)"), S.AgentMaxClimb));
		if (!FMath::IsFinite(S.MaxSlopeDegrees) || S.MaxSlopeDegrees < 0.f || S.MaxSlopeDegrees > 89.f) Fail(FString::Printf(TEXT("MaxSlopeDegrees must be in [0, 89] (got %f)"), S.MaxSlopeDegrees));
		if (S.CellHeight > 0.f && (!FMath::IsFinite(S.AgentHeight) || S.AgentHeight < 3.f * S.CellHeight))
		{
			Fail(FString::Printf(TEXT("AgentHeight must be >= 3 * CellHeight (%f), got %f"), 3.f * S.CellHeight, S.AgentHeight));
		}
		if (FMath::IsFinite(S.AgentMaxClimb) && FMath::IsFinite(S.AgentHeight) && S.AgentMaxClimb >= S.AgentHeight)
		{
			Fail(FString::Printf(TEXT("AgentMaxClimb (%f) must be lower than AgentHeight (%f)"), S.AgentMaxClimb, S.AgentHeight));
		}
		if (S.CellHeight > 0.f && FMath::IsFinite(S.AgentHeight) && FMath::CeilToDouble(S.AgentHeight / S.CellHeight) > MaxHeightCells)
		{
			Fail(TEXT("AgentHeight / CellHeight exceeds the Recast span height limit (8191)"));
		}
		if (S.MinRegionAreaCells < 0) Fail(TEXT("MinRegionAreaCells must be >= 0"));
		if (S.MergeRegionAreaCells < 0) Fail(TEXT("MergeRegionAreaCells must be >= 0"));
		if (!FMath::IsFinite(S.MaxEdgeLength) || S.MaxEdgeLength < 0.f) Fail(TEXT("MaxEdgeLength must be >= 0"));
		if (!FMath::IsFinite(S.MaxSimplificationError) || S.MaxSimplificationError < 0.f) Fail(TEXT("MaxSimplificationError must be >= 0"));
		if (S.MaxVertsPerPoly < 3 || S.MaxVertsPerPoly > 6) Fail(FString::Printf(TEXT("MaxVertsPerPoly must be in [3, 6] (got %d)"), S.MaxVertsPerPoly));
		if (S.bBuildDetailMesh)
		{
			if (!FMath::IsFinite(S.DetailSampleDistance) || S.DetailSampleDistance < 0.f
				|| (S.DetailSampleDistance > 0.f && S.CellSize > 0.f && S.DetailSampleDistance < 0.9f * S.CellSize))
			{
				Fail(FString::Printf(TEXT("DetailSampleDistance must be 0 or >= 0.9 * CellSize (got %f)"), S.DetailSampleDistance));
			}
			if (!FMath::IsFinite(S.DetailSampleMaxError) || S.DetailSampleMaxError < 0.f) Fail(TEXT("DetailSampleMaxError must be >= 0"));
		}
		if (S.MaxGridCells < 1) Fail(TEXT("MaxGridCells must be >= 1"));
		if (S.MaxInputVertices < 3) Fail(TEXT("MaxInputVertices must be >= 3"));
		if (S.MaxInputTriangles < 1) Fail(TEXT("MaxInputTriangles must be >= 1"));

		return Diagnostics.ErrorCount == ErrorsBefore;
	}

	ECustomNavBuildStatus BuildSnapshot(const FCustomNavBuildToken& Token,
	                                    const FBox& WorldBounds,
	                                    const FCustomNavBuildSettings& Settings,
	                                    const TArray<FCustomNavGeometryChunk>& Chunks,
	                                    FCustomNavBuildSnapshot& Out,
	                                    FCustomNavDiagnostics& Diagnostics)
	{
		using namespace CustomNavigationPrivate;
		Out = FCustomNavBuildSnapshot();
		const int32 ErrorsBefore = Diagnostics.ErrorCount;

		// --- Bounds and settings ---
		const bool bSettingsOk = ValidateSettings(Settings, Diagnostics);

		if (!IsFiniteBox(WorldBounds))
		{
			Diagnostics.Error(ECustomNavDiagnosticCode::InvalidBounds, StageValidate, TEXT("WorldBounds is not a valid finite box"));
		}
		else
		{
			const FVector Size = WorldBounds.GetSize();
			if (Size.X <= 0.0 || Size.Y <= 0.0 || Size.Z <= 0.0)
			{
				Diagnostics.Error(ECustomNavDiagnosticCode::InvalidBounds, StageValidate,
				                  FString::Printf(TEXT("WorldBounds must have positive size on every axis (got %s)"), *Size.ToString()));
			}
			else if (bSettingsOk)
			{
				int64 Width = 0, Depth = 0, HeightCells = 0;
				if (!ComputeGridSize(WorldBounds, Settings, Width, Depth, HeightCells))
				{
					Diagnostics.Error(ECustomNavDiagnosticCode::InvalidBounds, StageValidate, TEXT("WorldBounds is smaller than one cell"));
				}
				else if (Width > MAX_int32 || Depth > MAX_int32 || Width * Depth > Settings.MaxGridCells)
				{
					Diagnostics.Error(ECustomNavDiagnosticCode::GridTooLarge, StageValidate,
					                  FString::Printf(TEXT("Grid %lld x %lld = %lld cells exceeds MaxGridCells %lld; increase CellSize or split the region"),
					                                  Width, Depth, Width * Depth, Settings.MaxGridCells));
				}
				else if (HeightCells > MaxHeightCells)
				{
					Diagnostics.Error(ECustomNavDiagnosticCode::GridTooLarge, StageValidate,
					                  FString::Printf(TEXT("Bounds height is %lld cells, Recast supports at most %lld; increase CellHeight or reduce the height"),
					                                  HeightCells, MaxHeightCells));
				}
			}
		}

		// --- Chunk structure (cheap checks first, so nothing is copied for broken input) ---
		int64 TotalVertices = 0;
		int64 TotalTriangles = 0;
		for (const FCustomNavGeometryChunk& Chunk : Chunks)
		{
			const FString Source = Chunk.Key.ToString();
			TotalVertices += Chunk.Vertices.Num();

			if (Chunk.Indices.Num() % 3 != 0)
			{
				Diagnostics.Error(ECustomNavDiagnosticCode::BadIndexCount, StageValidate,
				                  FString::Printf(TEXT("Index count %d is not a multiple of 3"), Chunk.Indices.Num()), Source);
				continue;
			}
			const int32 NumTris = Chunk.Indices.Num() / 3;
			TotalTriangles += NumTris;

			if (Chunk.TriangleAreas.Num() != 0 && Chunk.TriangleAreas.Num() != NumTris)
			{
				Diagnostics.Error(ECustomNavDiagnosticCode::BadAreaCount, StageValidate,
				                  FString::Printf(TEXT("%d area ids for %d triangles"), Chunk.TriangleAreas.Num(), NumTris), Source);
			}
			for (int32 TriIndex = 0; TriIndex < Chunk.TriangleAreas.Num(); ++TriIndex)
			{
				if (Chunk.TriangleAreas[TriIndex] > CustomNavigation::Area::Max)
				{
					Diagnostics.Error(ECustomNavDiagnosticCode::InvalidAreaId, StageValidate,
					                  FString::Printf(TEXT("Area id %d > %d"), Chunk.TriangleAreas[TriIndex], CustomNavigation::Area::Max), Source, TriIndex);
				}
			}
			for (int32 VertIndex = 0; VertIndex < Chunk.Vertices.Num(); ++VertIndex)
			{
				if (!IsFiniteVector(Chunk.Vertices[VertIndex]))
				{
					Diagnostics.Error(ECustomNavDiagnosticCode::NonFiniteVertex, StageValidate,
					                  FString::Printf(TEXT("Vertex %d is not finite"), VertIndex), Source);
				}
			}
			for (int32 i = 0; i < Chunk.Indices.Num(); ++i)
			{
				const int32 Index = Chunk.Indices[i];
				if (Index < 0 || Index >= Chunk.Vertices.Num())
				{
					Diagnostics.Error(ECustomNavDiagnosticCode::IndexOutOfRange, StageValidate,
					                  FString::Printf(TEXT("Index %d out of range [0, %d)"), Index, Chunk.Vertices.Num()), Source, i / 3);
				}
			}
		}

		if (TotalVertices > Settings.MaxInputVertices)
		{
			Diagnostics.Error(ECustomNavDiagnosticCode::TooManyVertices, StageValidate,
			                  FString::Printf(TEXT("%lld vertices exceed MaxInputVertices %d"), TotalVertices, Settings.MaxInputVertices));
		}
		if (TotalTriangles > Settings.MaxInputTriangles)
		{
			Diagnostics.Error(ECustomNavDiagnosticCode::TooManyTriangles, StageValidate,
			                  FString::Printf(TEXT("%lld triangles exceed MaxInputTriangles %d"), TotalTriangles, Settings.MaxInputTriangles));
		}

		if (Diagnostics.ErrorCount != ErrorsBefore)
		{
			return ECustomNavBuildStatus::InvalidInput;
		}

		// --- Freeze ---
		Out.Token = Token;
		Out.Settings = Settings;
		Out.WorldBounds = WorldBounds;
		Out.WorldOrigin = WorldBounds.GetCenter();
		Out.RecastBoundsMin = WorldToRecast(WorldBounds.Min, Out.WorldOrigin);
		Out.RecastBoundsMax = WorldToRecast(WorldBounds.Max, Out.WorldOrigin);
		// Axis swap keeps min/max order (no axis is negated), but be explicit.
		const FVector3f MinCopy = Out.RecastBoundsMin;
		Out.RecastBoundsMin = FVector3f::Min(MinCopy, Out.RecastBoundsMax);
		Out.RecastBoundsMax = FVector3f::Max(MinCopy, Out.RecastBoundsMax);

		Out.Vertices.Reserve(static_cast<int32>(TotalVertices * 3));
		Out.Indices.Reserve(static_cast<int32>(TotalTriangles * 3));
		Out.TriangleAreas.Reserve(static_cast<int32>(TotalTriangles));
		Out.Sources.Reserve(Chunks.Num());

		int32 DroppedDegenerate = 0;
		for (const FCustomNavGeometryChunk& Chunk : Chunks)
		{
			const int32 NumTris = Chunk.Indices.Num() / 3;
			if (NumTris == 0)
			{
				continue;
			}
			const int32 Base = Out.NumVertices();
			for (const FVector& V : Chunk.Vertices)
			{
				const FVector3f R = WorldToRecast(V, Out.WorldOrigin);
				Out.Vertices.Add(R.X);
				Out.Vertices.Add(R.Y);
				Out.Vertices.Add(R.Z);
			}

			int32 KeptInChunk = 0;
			for (int32 TriIndex = 0; TriIndex < NumTris; ++TriIndex)
			{
				const int32 IA = Chunk.Indices[TriIndex * 3 + 0];
				const int32 IB = Chunk.Indices[TriIndex * 3 + 1];
				const int32 IC = Chunk.Indices[TriIndex * 3 + 2];
				const bool bRepeated = IA == IB || IB == IC || IA == IC;
				const bool bZeroArea = !bRepeated && FVector::CrossProduct(Chunk.Vertices[IB] - Chunk.Vertices[IA], Chunk.Vertices[IC] - Chunk.Vertices[IA]).SizeSquared() < DegenerateCrossSizeSquared;
				if (bRepeated || bZeroArea)
				{
					++DroppedDegenerate;
					Diagnostics.Warning(ECustomNavDiagnosticCode::DegenerateTriangleDropped, StageValidate,
					                    bRepeated ? TEXT("Triangle repeats a vertex index") : TEXT("Triangle has zero area"),
					                    Chunk.Key.ToString(), TriIndex);
					continue;
				}
				// Axis swap mirrors the space: (A, B, C) -> (A, C, B) keeps "up" faces up.
				Out.Indices.Add(Base + IA);
				Out.Indices.Add(Base + IC);
				Out.Indices.Add(Base + IB);
				Out.TriangleAreas.Add(Chunk.TriangleAreas.Num() ? Chunk.TriangleAreas[TriIndex] : CustomNavigation::Area::Walkable);
				++KeptInChunk;
			}
			if (KeptInChunk > 0)
			{
				Out.Sources.Emplace(Chunk.Key, Chunk.Revision);
			}
		}

		if (Out.NumTriangles() == 0)
		{
			Diagnostics.Warning(ECustomNavDiagnosticCode::NoGeometry, StageValidate,
			                    DroppedDegenerate > 0
				                    ? FString::Printf(TEXT("No usable triangles (%d degenerate dropped)"), DroppedDegenerate)
				                    : FString(TEXT("Providers returned no triangles for the region")));
			return ECustomNavBuildStatus::NoGeometry;
		}

		return ECustomNavBuildStatus::Success;
	}
}
