// Created by Stalker7274

#include "Navigation/CustomNavigationTypes.h"

DEFINE_LOG_CATEGORY(LogCustomNavigation);

bool FCustomNavBuildSettings::Equals(const FCustomNavBuildSettings& Other) const
{
	return StaticStruct()->CompareScriptStruct(this, &Other, PPF_None);
}

FString FCustomNavDiagnostic::ToString() const
{
	FString Result = FString::Printf(TEXT("[%s] %s (%s): %s"),
		*UEnum::GetDisplayValueAsText(Severity).ToString(),
		*UEnum::GetValueAsName(Code).ToString(),
		*Stage.ToString(),
		*Message);
	if (!Source.IsEmpty())
	{
		Result += FString::Printf(TEXT(" source=%s"), *Source);
	}
	if (TriangleIndex != INDEX_NONE)
	{
		Result += FString::Printf(TEXT(" triangle=%d"), TriangleIndex);
	}
	return Result;
}

void FCustomNavDiagnostics::Add(ECustomNavDiagnosticSeverity Severity, ECustomNavDiagnosticCode Code, FName Stage, FString Message,
                                FString Source, int32 TriangleIndex)
{
	if (Severity == ECustomNavDiagnosticSeverity::Error)
	{
		++ErrorCount;
	}
	else if (Severity == ECustomNavDiagnosticSeverity::Warning)
	{
		++WarningCount;
	}

	if (Entries.Num() >= MaxEntries)
	{
		++DroppedCount;
		return;
	}

	FCustomNavDiagnostic& Entry = Entries.AddDefaulted_GetRef();
	Entry.Severity = Severity;
	Entry.Code = Code;
	Entry.Stage = Stage;
	Entry.Message = MoveTemp(Message);
	Entry.Source = MoveTemp(Source);
	Entry.TriangleIndex = TriangleIndex;
}

void FCustomNavDiagnostics::Append(const FCustomNavDiagnostics& Other)
{
	for (const FCustomNavDiagnostic& Entry : Other.Entries)
	{
		Add(Entry.Severity, Entry.Code, Entry.Stage, Entry.Message, Entry.Source, Entry.TriangleIndex);
	}
	// Counts of entries the other list already dropped.
	DroppedCount += Other.DroppedCount;
	const int32 OtherStoredErrors = Other.Entries.FilterByPredicate([](const FCustomNavDiagnostic& E) { return E.Severity == ECustomNavDiagnosticSeverity::Error; }).Num();
	const int32 OtherStoredWarnings = Other.Entries.FilterByPredicate([](const FCustomNavDiagnostic& E) { return E.Severity == ECustomNavDiagnosticSeverity::Warning; }).Num();
	ErrorCount += Other.ErrorCount - OtherStoredErrors;
	WarningCount += Other.WarningCount - OtherStoredWarnings;
}

bool FCustomNavDiagnostics::Contains(ECustomNavDiagnosticCode Code) const
{
	return Entries.ContainsByPredicate([Code](const FCustomNavDiagnostic& Entry) { return Entry.Code == Code; });
}

const FCustomNavDiagnostic* FCustomNavDiagnostics::FirstError() const
{
	return Entries.FindByPredicate([](const FCustomNavDiagnostic& Entry) { return Entry.Severity == ECustomNavDiagnosticSeverity::Error; });
}

FString FCustomNavDiagnostics::ToString() const
{
	FString Result;
	for (const FCustomNavDiagnostic& Entry : Entries)
	{
		Result += Entry.ToString();
		Result += TEXT("\n");
	}
	if (DroppedCount > 0)
	{
		Result += FString::Printf(TEXT("... %d more diagnostics dropped\n"), DroppedCount);
	}
	return Result;
}

double FCustomNavRegionMesh::ComputeProjectedArea() const
{
	double Total = 0.0;
	for (int32 PolyIndex = 0; PolyIndex < NumPolygons(); ++PolyIndex)
	{
		const int32 Start = PolyOffsets[PolyIndex];
		const int32 Count = NumPolygonVerts(PolyIndex);
		double Twice = 0.0;
		for (int32 i = 0; i < Count; ++i)
		{
			const FVector& A = PolyVertices[PolyIndices[Start + i]];
			const FVector& B = PolyVertices[PolyIndices[Start + (i + 1) % Count]];
			Twice += A.X * B.Y - B.X * A.Y;
		}
		Total += FMath::Abs(Twice) * 0.5;
	}
	return Total;
}

SIZE_T FCustomNavRegionMesh::GetAllocatedSize() const
{
	return PolyVertices.GetAllocatedSize() + PolyIndices.GetAllocatedSize() + PolyOffsets.GetAllocatedSize()
		+ PolyAreas.GetAllocatedSize() + PolyRegions.GetAllocatedSize() + DetailVertices.GetAllocatedSize()
		+ DetailTriangles.GetAllocatedSize() + DetailTrianglePolys.GetAllocatedSize() + StageTimingsMs.GetAllocatedSize();
}
