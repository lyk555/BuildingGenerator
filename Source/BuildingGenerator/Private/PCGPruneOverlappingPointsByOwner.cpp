#include "PCGPruneOverlappingPointsByOwner.h"

#include "PCGContext.h"
#include "PCGPin.h"
#include "PCGData.h"

#include "Data/PCGPointData.h"
#include "Data/PCGSpatialData.h"

#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"

#define LOCTEXT_NAMESPACE "PCGPruneOverlappingPointsByOwner"

namespace PCGPruneOverlappingPointsByOwner
{
	struct FQuantizedPositionKey
	{
		int64 X = 0;
		int64 Y = 0;
		int64 Z = 0;

		bool operator==(const FQuantizedPositionKey& Other) const
		{
			return X == Other.X && Y == Other.Y && Z == Other.Z;
		}
	};

	FORCEINLINE uint32 GetTypeHash(const FQuantizedPositionKey& Key)
	{
		uint32 H = ::GetTypeHash(Key.X);
		H = HashCombine(H, ::GetTypeHash(Key.Y));
		H = HashCombine(H, ::GetTypeHash(Key.Z));
		return H;
	}

	struct FGroupStats
	{
		TArray<int32> PointIndices;
		TMap<FString, int32> ValueCounts;
	};

	static FQuantizedPositionKey MakeQuantizedKey(const FVector& InPos, double Tolerance, bool bUseXYOnly)
	{
		const double SafeTol = FMath::Max(Tolerance, 0.001);

		FQuantizedPositionKey Key;
		Key.X = FMath::RoundToInt64(InPos.X / SafeTol);
		Key.Y = FMath::RoundToInt64(InPos.Y / SafeTol);
		Key.Z = bUseXYOnly ? 0 : FMath::RoundToInt64(InPos.Z / SafeTol);
		return Key;
	}

	static bool GetVoteKeys(
		const UPCGPointData* InPointData,
		const FName InAttributeName,
		EPCGPruneVoteAttributeType InType,
		double InDoubleQuantization,
		TArray<FString>& OutVoteKeys)
	{
		OutVoteKeys.Reset();

		if (!InPointData || InAttributeName.IsNone())
		{
			return false;
		}

		const TArray<FPCGPoint>& InPoints = InPointData->GetPoints();
		const int32 NumPoints = InPoints.Num();
		if (NumPoints <= 0)
		{
			return true;
		}

		const UPCGMetadata* Metadata = InPointData->ConstMetadata();
		if (!Metadata)
		{
			return false;
		}

		// 关键：FString 是非 POD，必须用 SetNum，不要用 SetNumUninitialized
		OutVoteKeys.SetNum(NumPoints);

		switch (InType)
		{
		case EPCGPruneVoteAttributeType::Int32:
		{
			const FPCGMetadataAttribute<int32>* Attr = Metadata->GetConstTypedAttribute<int32>(InAttributeName);
			if (!Attr)
			{
				OutVoteKeys.Reset();
				return false;
			}

			for (int32 i = 0; i < NumPoints; ++i)
			{
				OutVoteKeys[i] = LexToString(Attr->GetValueFromItemKey(InPoints[i].MetadataEntry));
			}
			return true;
		}

		case EPCGPruneVoteAttributeType::Int64:
		{
			const FPCGMetadataAttribute<int64>* Attr = Metadata->GetConstTypedAttribute<int64>(InAttributeName);
			if (!Attr)
			{
				OutVoteKeys.Reset();
				return false;
			}

			for (int32 i = 0; i < NumPoints; ++i)
			{
				OutVoteKeys[i] = LexToString(Attr->GetValueFromItemKey(InPoints[i].MetadataEntry));
			}
			return true;
		}

		case EPCGPruneVoteAttributeType::Double:
		{
			const FPCGMetadataAttribute<double>* Attr = Metadata->GetConstTypedAttribute<double>(InAttributeName);
			if (!Attr)
			{
				OutVoteKeys.Reset();
				return false;
			}

			const double SafeQ = FMath::Max(InDoubleQuantization, 0.0000001);

			for (int32 i = 0; i < NumPoints; ++i)
			{
				const double Raw = Attr->GetValueFromItemKey(InPoints[i].MetadataEntry);
				const double Quantized = FMath::RoundToDouble(Raw / SafeQ) * SafeQ;
				OutVoteKeys[i] = FString::Printf(TEXT("%.15g"), Quantized);
			}
			return true;
		}

		case EPCGPruneVoteAttributeType::Name:
		{
			const FPCGMetadataAttribute<FName>* Attr = Metadata->GetConstTypedAttribute<FName>(InAttributeName);
			if (!Attr)
			{
				OutVoteKeys.Reset();
				return false;
			}

			for (int32 i = 0; i < NumPoints; ++i)
			{
				OutVoteKeys[i] = Attr->GetValueFromItemKey(InPoints[i].MetadataEntry).ToString();
			}
			return true;
		}

		case EPCGPruneVoteAttributeType::String:
		{
			const FPCGMetadataAttribute<FString>* Attr = Metadata->GetConstTypedAttribute<FString>(InAttributeName);
			if (!Attr)
			{
				OutVoteKeys.Reset();
				return false;
			}

			for (int32 i = 0; i < NumPoints; ++i)
			{
				OutVoteKeys[i] = Attr->GetValueFromItemKey(InPoints[i].MetadataEntry);
			}
			return true;
		}

		default:
			OutVoteKeys.Reset();
			return false;
		}
	}

	static void SelectWinningValues(
		const FGroupStats& Group,
		EPCGPruneVoteAttributeType CompareType,
		bool bKeepAllIfTie,
		bool bPreferHigherValueOnTie,
		TSet<FString>& OutWinningValues)
	{
		OutWinningValues.Reset();

		if (Group.PointIndices.Num() <= 0 || Group.ValueCounts.Num() <= 0)
		{
			return;
		}

		int32 BestCount = INDEX_NONE;

		for (const TPair<FString, int32>& Pair : Group.ValueCounts)
		{
			BestCount = FMath::Max(BestCount, Pair.Value);
		}

		if (BestCount == INDEX_NONE)
		{
			return;
		}

		TArray<FString> TiedValues;
		TiedValues.Reserve(Group.ValueCounts.Num());

		for (const TPair<FString, int32>& Pair : Group.ValueCounts)
		{
			if (Pair.Value == BestCount)
			{
				TiedValues.Add(Pair.Key);
			}
		}

		if (TiedValues.Num() == 0)
		{
			return;
		}

		if (TiedValues.Num() == 1)
		{
			OutWinningValues.Add(TiedValues[0]);
			return;
		}

		if (bKeepAllIfTie)
		{
			for (const FString& Value : TiedValues)
			{
				OutWinningValues.Add(Value);
			}
			return;
		}

		if (CompareType == EPCGPruneVoteAttributeType::Int32)
		{
			int32 Winner = FCString::Atoi(*TiedValues[0]);

			for (const FString& Value : TiedValues)
			{
				const int32 Candidate = FCString::Atoi(*Value);
				if (bPreferHigherValueOnTie ? (Candidate > Winner) : (Candidate < Winner))
				{
					Winner = Candidate;
				}
			}

			OutWinningValues.Add(LexToString(Winner));
			return;
		}

		if (CompareType == EPCGPruneVoteAttributeType::Int64)
		{
			int64 Winner = FCString::Atoi64(*TiedValues[0]);

			for (const FString& Value : TiedValues)
			{
				const int64 Candidate = FCString::Atoi64(*Value);
				if (bPreferHigherValueOnTie ? (Candidate > Winner) : (Candidate < Winner))
				{
					Winner = Candidate;
				}
			}

			OutWinningValues.Add(LexToString(Winner));
			return;
		}

		if (CompareType == EPCGPruneVoteAttributeType::Double)
		{
			double Winner = FCString::Atod(*TiedValues[0]);

			for (const FString& Value : TiedValues)
			{
				const double Candidate = FCString::Atod(*Value);
				if (bPreferHigherValueOnTie ? (Candidate > Winner) : (Candidate < Winner))
				{
					Winner = Candidate;
				}
			}

			OutWinningValues.Add(FString::Printf(TEXT("%.15g"), Winner));
			return;
		}

		// Name/String：稳定地取第一个
		OutWinningValues.Add(TiedValues[0]);
	}
}



TArray<FPCGPinProperties> UPCGPruneOverlappingPointsByOwnerSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultInputLabel, EPCGDataType::Point);
	return Pins;
}

TArray<FPCGPinProperties> UPCGPruneOverlappingPointsByOwnerSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);
	return Pins;
}

FPCGElementPtr UPCGPruneOverlappingPointsByOwnerSettings::CreateElement() const
{
	return MakeShared<FPCGPruneOverlappingPointsByOwnerElement>();
}

bool FPCGPruneOverlappingPointsByOwnerElement::ExecuteInternal(FPCGContext* Context) const
{
	if (!Context)
	{
		return true;
	}

	const UPCGPruneOverlappingPointsByOwnerSettings* Settings =
		Context->GetInputSettings<UPCGPruneOverlappingPointsByOwnerSettings>();

	if (!Settings)
	{
		return true;
	}

	const TArray<FPCGTaggedData>& Inputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
	if (Inputs.IsEmpty())
	{
		return true;
	}

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGPointData* InputPointData = Cast<UPCGPointData>(Input.Data);
		if (!InputPointData)
		{
			// 不是点数据，直接跳过（你也可以改成 passthrough）
			continue;
		}

		const TArray<FPCGPoint>& InPoints = InputPointData->GetPoints();
		const int32 NumPoints = InPoints.Num();

		// 空输入直接输出空点集
		if (NumPoints == 0)
		{
			UPCGPointData* EmptyOut = NewObject<UPCGPointData>();
			EmptyOut->InitializeFromData(InputPointData);

			FPCGTaggedData& Output = Context->OutputData.TaggedData.Add_GetRef(Input);
			Output.Data = EmptyOut;
			continue;
		}

		// 读取 OwnerIndex
		TArray<FString> VoteKeys;
		const bool bHasVoteKeys = PCGPruneOverlappingPointsByOwner::GetVoteKeys(
			InputPointData,
			Settings->CompareAttribute,
			Settings->CompareAttributeType,
			Settings->DoubleQuantization,
			VoteKeys);

		if (!bHasVoteKeys || VoteKeys.Num() != NumPoints)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[PruneOverlapping] Failed to read attribute '%s' as type %d. Passing through unchanged."),
				*Settings->CompareAttribute.ToString(),
				static_cast<int32>(Settings->CompareAttributeType));

			FPCGTaggedData& Output = Context->OutputData.TaggedData.Add_GetRef(Input);
			Output.Data = Input.Data;
			continue;
		}

		// 1) 按位置聚类
		TMap<PCGPruneOverlappingPointsByOwner::FQuantizedPositionKey, PCGPruneOverlappingPointsByOwner::FGroupStats> Groups;

		for (int32 PointIndex = 0; PointIndex < NumPoints; ++PointIndex)
		{
			const FVector Pos = InPoints[PointIndex].Transform.GetLocation();

			const auto Key = PCGPruneOverlappingPointsByOwner::MakeQuantizedKey(
				Pos,
				Settings->PositionTolerance,
				Settings->bUseXYOnly);

			PCGPruneOverlappingPointsByOwner::FGroupStats& Group = Groups.FindOrAdd(Key);
			Group.PointIndices.Add(PointIndex);

			const FString VoteKey = VoteKeys[PointIndex];
			int32& CountRef = Group.ValueCounts.FindOrAdd(VoteKey);
			CountRef += 1;
		}

		// 2) 在每个重合组里决定保留哪些 owner
		TBitArray<> bKeepFlags(false, NumPoints);

		for (const TPair<PCGPruneOverlappingPointsByOwner::FQuantizedPositionKey, PCGPruneOverlappingPointsByOwner::FGroupStats>& Pair : Groups)
		{
			const PCGPruneOverlappingPointsByOwner::FGroupStats& Group = Pair.Value;

			// 只有一个点，不算冲突，直接保留
			if (Group.PointIndices.Num() <= 1)
			{
				bKeepFlags[Group.PointIndices[0]] = true;
				continue;
			}

			TSet<FString> WinningValues;
			PCGPruneOverlappingPointsByOwner::SelectWinningValues(
				Group,
				Settings->CompareAttributeType,
				Settings->bKeepAllIfTie,
				Settings->bPreferHigherValueOnTie,
				WinningValues);

			for (int32 PointIndex : Group.PointIndices)
			{
				const FString VoteKey = VoteKeys[PointIndex];
				if (WinningValues.Contains(VoteKey))
				{
					bKeepFlags[PointIndex] = true;
				}
			}
		}

		// 3) 输出保留下来的点
		UPCGPointData* OutputPointData = NewObject<UPCGPointData>();
		OutputPointData->InitializeFromData(InputPointData);

		TArray<FPCGPoint>& OutPoints = OutputPointData->GetMutablePoints();
		OutPoints.Reserve(NumPoints);

		for (int32 i = 0; i < NumPoints; ++i)
		{
			if (bKeepFlags[i])
			{
				OutPoints.Add(InPoints[i]);
			}
		}

		// 扁平化 metadata，避免保留无用父链
		OutputPointData->Flatten();

		FPCGTaggedData& Output = Context->OutputData.TaggedData.Add_GetRef(Input);
		Output.Data = OutputPointData;
	}

	return true;
}

#undef LOCTEXT_NAMESPACE