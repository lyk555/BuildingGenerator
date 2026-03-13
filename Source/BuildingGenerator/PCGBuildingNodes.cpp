#include "PCGBuildingNodes.h"

#include "PCGPin.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Containers/Set.h"
#include "Metadata/PCGMetadataCommon.h"
#include "PCGComponent.h"
#include "GameFramework/Actor.h"


static bool ParseDir(const FString& S, int32& OutAxis, int32& OutSign)
{
	const FString T = S.TrimStartAndEnd().ToUpper();
	if (T == TEXT("+X") || T == TEXT("X+")) { OutAxis = 0; OutSign = +1; return true; }
	if (T == TEXT("-X") || T == TEXT("X-")) { OutAxis = 0; OutSign = -1; return true; }
	if (T == TEXT("+Y") || T == TEXT("Y+")) { OutAxis = 1; OutSign = +1; return true; }
	if (T == TEXT("-Y") || T == TEXT("Y-")) { OutAxis = 1; OutSign = -1; return true; }
	return false;
}

static int32 CeilDivInt(int32 A, int32 B)
{
	return (B <= 0) ? 0 : (A + B - 1) / B;
}

// ============================================================
// GenerateGrid Settings
// ============================================================

TArray<FPCGPinProperties> UPCGBuildingGenerateGridSettings::InputPinProperties() const
{
	// generator node: no input pins
	return {};
}

TArray<FPCGPinProperties> UPCGBuildingGenerateGridSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);
	return Pins;
}

FPCGElementPtr UPCGBuildingGenerateGridSettings::CreateElement() const
{
	return MakeShared<FPCGBuildingGenerateGridElement>();
}

bool FPCGBuildingGenerateGridElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGBuildingGenerateGridElement::Execute);
	check(Context);

	const UPCGBuildingGenerateGridSettings* Settings = Context->GetInputSettings<UPCGBuildingGenerateGridSettings>();
	check(Settings);

	const int32 FloorCount = FMath::Max(1, Settings->Floor);
	const int32 SX = FMath::Max(1, Settings->SizeX);
	const int32 SY = FMath::Max(1, Settings->SizeY);

	const FVector Cell = FVector(
		FMath::Max(1.0f, Settings->CellSize.X),
		FMath::Max(1.0f, Settings->CellSize.Y),
		FMath::Max(1.0f, Settings->CellSize.Z)
	);

	// layers per floor: ceil(FloorHeight / CellZ)
	const int32 FH = FMath::Max(1, FMath::RoundToInt(Settings->FloorHeight));
	const int32 CZ = FMath::Max(1, FMath::RoundToInt(Cell.Z));
	const int32 LayersPerFloor = FMath::Max(1, CeilDivInt(FH, CZ));
	const int32 TotalLayers = FloorCount * LayersPerFloor;

	const int32 TotalPoints = TotalLayers * SX * SY;
	if (TotalPoints <= 0)
	{
		return true;
	}

	FRandomStream R;
	{
		const int32 BaseSeed = (int32)Context->GetSeed();
		R.Initialize(BaseSeed + Settings->SeedOffset);
	}

	UPCGPointData* OutData = NewObject<UPCGPointData>();
	check(OutData);

	UPCGMetadata* Meta = OutData->MutableMetadata();
	check(Meta);

	// attributes
	auto* AFloor = Meta->FindOrCreateAttribute<int32>(PCGBuildingAttr::Floor, 0, false, true);
	auto* ALayer = Meta->FindOrCreateAttribute<int32>(PCGBuildingAttr::LayerInFloor, 0, false, true);
	auto* AGridX = Meta->FindOrCreateAttribute<int32>(PCGBuildingAttr::GridX, 0, false, true);
	auto* AGridY = Meta->FindOrCreateAttribute<int32>(PCGBuildingAttr::GridY, 0, false, true);
	auto* AGridZ = Meta->FindOrCreateAttribute<int32>(PCGBuildingAttr::GridZ, 0, false, true);
	auto* ASizeX = Meta->FindOrCreateAttribute<int32>(PCGBuildingAttr::SizeX, SX, false, true);
	auto* ASizeY = Meta->FindOrCreateAttribute<int32>(PCGBuildingAttr::SizeY, SY, false, true);
	auto* ALPF = Meta->FindOrCreateAttribute<int32>(PCGBuildingAttr::LayersPerFloor, LayersPerFloor, false, true);
	auto* AFCnt = Meta->FindOrCreateAttribute<int32>(PCGBuildingAttr::FloorCount, FloorCount, false, true);

	TArray<FPCGPoint>& Points = OutData->GetMutablePoints();
	Points.Reserve(TotalPoints);


	// -----------------------------
	// Coordinate Space (match Create Points Grid semantics):
	// - LocalComponent: point transforms are authored in local space (no bake). Moving the PCG actor moves spawned artifacts via attachment.
	// - World: bake OwnerActor transform into each point at generation time.
	// -----------------------------
	const UPCGComponent* SourceComp = Context ? Context->SourceComponent.Get() : nullptr;
	const AActor* OwnerActor = SourceComp ? SourceComp->GetOwner() : nullptr;

	// Recipient of artifacts generated from this data (helps spawners attach to the right actor)
	if (OwnerActor)
	{
		OutData->TargetActor = const_cast<AActor*>(OwnerActor);
	}

	const bool bLocalComponentSpace = (Settings->CoordinateSpace == EPCGCoordinateSpace::LocalComponent);
	const FTransform OwnerXf = OwnerActor ? OwnerActor->GetActorTransform() : FTransform::Identity;

	for (int32 L = 0; L < TotalLayers; ++L)
	{
		const int32 FloorIndex = L / LayersPerFloor;
		const int32 LayerInFloor = L - FloorIndex * LayersPerFloor;

		for (int32 Y = 0; Y < SY; ++Y)
		{
			for (int32 X = 0; X < SX; ++X)
			{
				FPCGPoint P;
				P.Density = 1.0f;

				// cell centers
				const float WX = (X + 0.5f) * Cell.X;
				const float WY = (Y + 0.5f) * Cell.Y;
				const float WZ = (L + 0.5f) * Cell.Z;
				const FTransform LocalXf(FQuat::Identity, FVector(WX, WY, WZ), FVector::OneVector);

				// LocalComponent: local space transforms (no bake)
				// World: bake OwnerXf into point transforms
				P.Transform = bLocalComponentSpace ? LocalXf : (LocalXf * OwnerXf);


				// CellSize = 2*Extent  ()
				const FVector Extent = 0.5f * Cell;
				P.BoundsMin = -Extent;
				P.BoundsMax = Extent;

				P.SetLocalBounds(FBox(-Extent, Extent));

				const PCGMetadataEntryKey Key = Meta->AddEntry();
				P.MetadataEntry = Key;

				AFloor->SetValue(Key, FloorIndex);
				ALayer->SetValue(Key, LayerInFloor);
				AGridX->SetValue(Key, X);
				AGridY->SetValue(Key, Y);
				AGridZ->SetValue(Key, L);

				ASizeX->SetValue(Key, SX);
				ASizeY->SetValue(Key, SY);
				ALPF->SetValue(Key, LayersPerFloor);
				AFCnt->SetValue(Key, FloorCount);

				Points.Add(P);
			}
		}
	}

	FPCGTaggedData& OutTagged = Context->OutputData.TaggedData.Emplace_GetRef();
	OutTagged.Pin = PCGPinConstants::DefaultOutputLabel;
	OutTagged.Data = OutData;
	return true;
}

// ============================================================
// ShrinkByFloor Settings
// ============================================================

TArray<FPCGPinProperties> UPCGBuildingShrinkByFloorSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultInputLabel, EPCGDataType::Point);
	return Pins;
}

TArray<FPCGPinProperties> UPCGBuildingShrinkByFloorSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);
	return Pins;
}

FPCGElementPtr UPCGBuildingShrinkByFloorSettings::CreateElement() const
{
	return MakeShared<FPCGBuildingShrinkByFloorElement>();
}

bool FPCGBuildingShrinkByFloorElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGBuildingShrinkByFloorElement::Execute);
	check(Context);

	const UPCGBuildingShrinkByFloorSettings* Settings = Context->GetInputSettings<UPCGBuildingShrinkByFloorSettings>();
	check(Settings);

	const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
	if (Inputs.IsEmpty())
	{
		return true;
	}

	const UPCGPointData* InPointData = Cast<UPCGPointData>(Inputs[0].Data);
	if (!InPointData)
	{
		return true;
	}

	const UPCGMetadata* InMeta = InPointData->Metadata;
	if (!InMeta)
	{
		return true;
	}

	// Required attrs
	const FPCGMetadataAttribute<int32>* AFloor = InMeta->GetConstTypedAttribute<int32>(PCGBuildingAttr::Floor);
	const FPCGMetadataAttribute<int32>* AGridX = InMeta->GetConstTypedAttribute<int32>(PCGBuildingAttr::GridX);
	const FPCGMetadataAttribute<int32>* AGridY = InMeta->GetConstTypedAttribute<int32>(PCGBuildingAttr::GridY);
	const FPCGMetadataAttribute<int32>* ASizeX = InMeta->GetConstTypedAttribute<int32>(PCGBuildingAttr::SizeX);
	const FPCGMetadataAttribute<int32>* ASizeY = InMeta->GetConstTypedAttribute<int32>(PCGBuildingAttr::SizeY);

	if (!AFloor || !AGridX || !AGridY)
	{
		// Pass-through (cannot shrink without Floor/GridX/GridY)
		UPCGPointData* OutData = NewObject<UPCGPointData>();
		check(OutData);

		UPCGMetadata* OutMeta = OutData->MutableMetadata();
		check(OutMeta);
		OutMeta->Initialize(InMeta);

		TArray<FPCGPoint>& OutPoints = OutData->GetMutablePoints();
		OutPoints = InPointData->GetPoints();

		FPCGTaggedData& OutTagged = Context->OutputData.TaggedData.Emplace_GetRef();
		OutTagged.Pin = PCGPinConstants::DefaultOutputLabel;
		OutTagged.Data = OutData;

		return true;
	}

	// Optional per-floor attrs (authored upstream in PCG graph)
	const FPCGMetadataAttribute<int32>* ADirX = InMeta->GetConstTypedAttribute<int32>(PCGBuildingAttr::ShrinkDirectionX);
	const FPCGMetadataAttribute<int32>* ADirY = InMeta->GetConstTypedAttribute<int32>(PCGBuildingAttr::ShrinkDirectionY);
	const FPCGMetadataAttribute<float>* APerX = InMeta->GetConstTypedAttribute<float>(PCGBuildingAttr::ShrinkPercentageX);
	const FPCGMetadataAttribute<float>* APerY = InMeta->GetConstTypedAttribute<float>(PCGBuildingAttr::ShrinkPercentageY);

	// Legacy mode switch: if user filled legacy arrays OR explicitly enabled random legacy behavior.
	const bool bUseLegacy =
		(Settings->DirectionByFloor.Num() > 0) ||
		(Settings->StrengthByFloor.Num() > 0) ||
		(Settings->bEnableRandom);

	// ------------------------
	// Legacy behavior (kept): 1-axis shrink using DirectionByFloor/StrengthByFloor in [-1,+1]
	// ------------------------
	if (bUseLegacy)
	{
		FRandomStream R;
		R.Initialize((int32)Context->GetSeed() ^ (Settings->SeedOffset * 1337));

		// Compute floor count
		int32 FloorCount = 0;
		for (const FPCGPoint& P : InPointData->GetPoints())
		{
			FloorCount = FMath::Max(FloorCount, AFloor->GetValue(P.MetadataEntry) + 1);
		}
		FloorCount = FMath::Max(1, FloorCount);

		TArray<int32> AxisPerFloor;
		TArray<int32> SignPerFloor;
		TArray<float> StrengthPerFloor;
		AxisPerFloor.SetNum(FloorCount);
		SignPerFloor.SetNum(FloorCount);
		StrengthPerFloor.SetNum(FloorCount);

		for (int32 F = 0; F < FloorCount; ++F)
		{
			int32 Axis = 0;
			int32 Sign = +1;
			bool bHasDir = false;

			if (Settings->DirectionByFloor.IsValidIndex(F))
			{
				bHasDir = ParseDir(Settings->DirectionByFloor[F], Axis, Sign);
			}

			float Strength = 0.0f;
			bool bHasStr = Settings->StrengthByFloor.IsValidIndex(F);

			if (bHasStr)
			{
				Strength = Settings->StrengthByFloor[F];
			}
			else if (Settings->bEnableRandom)
			{
				Strength = R.FRandRange(-Settings->MaxAbsStrength, Settings->MaxAbsStrength);
			}

			Strength = FMath::Clamp(Strength, -Settings->MaxAbsStrength, Settings->MaxAbsStrength);

			if (!bHasDir)
			{
				if (Settings->bEnableRandom)
				{
					Axis = (R.FRand() < 0.5f) ? 0 : 1; // X or Y
					Sign = (R.FRand() < 0.5f) ? +1 : -1;
				}
			}

			AxisPerFloor[F] = Axis;
			SignPerFloor[F] = Sign;
			StrengthPerFloor[F] = Strength;
		}

		// compute per-floor bounds in grid space (min/max)
		struct FBounds2D { int32 MinX, MaxX, MinY, MaxY; bool bInit = false; };
		TArray<FBounds2D> Bounds;
		Bounds.SetNum(FloorCount);

		for (const FPCGPoint& P : InPointData->GetPoints())
		{
			const int32 F = AFloor->GetValue(P.MetadataEntry);
			const int32 X = AGridX->GetValue(P.MetadataEntry);
			const int32 Y = AGridY->GetValue(P.MetadataEntry);

			FBounds2D& B = Bounds[F];
			if (!B.bInit)
			{
				B.bInit = true;
				B.MinX = B.MaxX = X;
				B.MinY = B.MaxY = Y;
			}
			else
			{
				B.MinX = FMath::Min(B.MinX, X);
				B.MaxX = FMath::Max(B.MaxX, X);
				B.MinY = FMath::Min(B.MinY, Y);
				B.MaxY = FMath::Max(B.MaxY, Y);
			}
		}

		// Prefer declared size if present
		const int32 DeclSX = (ASizeX ? ASizeX->GetValue(InPointData->GetPoints()[0].MetadataEntry) : 0);
		const int32 DeclSY = (ASizeY ? ASizeY->GetValue(InPointData->GetPoints()[0].MetadataEntry) : 0);

		// output
		UPCGPointData* OutData = NewObject<UPCGPointData>();
		check(OutData);

		UPCGMetadata* OutMeta = OutData->MutableMetadata();
		check(OutMeta);
		OutMeta->Initialize(InMeta);

		auto* AShrinkAxis = OutMeta->FindOrCreateAttribute<int32>(PCGBuildingAttr::ShrinkAxis, 0, false, true);
		auto* AShrinkSign = OutMeta->FindOrCreateAttribute<int32>(PCGBuildingAttr::ShrinkSign, +1, false, true);
		auto* AShrinkStr = OutMeta->FindOrCreateAttribute<float>(PCGBuildingAttr::ShrinkStrength, 0.0f, false, true);

		TArray<FPCGPoint>& OutPoints = OutData->GetMutablePoints();
		OutPoints.Reserve(InPointData->GetPoints().Num());

		for (const FPCGPoint& InP : InPointData->GetPoints())
		{
			const int32 F = AFloor->GetValue(InP.MetadataEntry);
			const int32 X = AGridX->GetValue(InP.MetadataEntry);
			const int32 Y = AGridY->GetValue(InP.MetadataEntry);

			const FBounds2D& B = Bounds[F];
			if (!B.bInit) { continue; }

			const int32 Axis = AxisPerFloor[F];
			const int32 Sign = SignPerFloor[F];
			const float Strength = StrengthPerFloor[F];

			const int32 BaseLenX = (DeclSX > 0) ? DeclSX : (B.MaxX - B.MinX + 1);
			const int32 BaseLenY = (DeclSY > 0) ? DeclSY : (B.MaxY - B.MinY + 1);

			const int32 AxisLen = (Axis == 0) ? BaseLenX : BaseLenY;
			const int32 Cut = FMath::Clamp(FMath::RoundToInt(FMath::Abs(Strength) * (float)AxisLen), 0, AxisLen - 1);
			const int32 Remain = FMath::Max(1, AxisLen - Cut);

			int32 MinX = B.MinX, MaxX = B.MaxX, MinY = B.MinY, MaxY = B.MaxY;

			if (Axis == 0)
			{
				if (Sign > 0) { MaxX = MinX + Remain - 1; }
				else { MinX = MaxX - Remain + 1; }
			}
			else
			{
				if (Sign > 0) { MaxY = MinY + Remain - 1; }
				else { MinY = MaxY - Remain + 1; }
			}

			if (X < MinX || X > MaxX || Y < MinY || Y > MaxY)
			{
				continue;
			}

			FPCGPoint OutP = InP;
			const PCGMetadataEntryKey NewKey = OutMeta->AddEntry(InP.MetadataEntry);
			OutP.MetadataEntry = NewKey;

			AShrinkAxis->SetValue(NewKey, Axis);
			AShrinkSign->SetValue(NewKey, Sign);
			AShrinkStr->SetValue(NewKey, Strength);

			OutPoints.Add(OutP);
		}

		FPCGTaggedData& OutTagged = Context->OutputData.TaggedData.Emplace_GetRef();
		OutTagged.Pin = PCGPinConstants::DefaultOutputLabel;
		OutTagged.Data = OutData;

		return true;
	}

	// ------------------------
	// Doc-matching behavior (recommended):
	// per-floor X/Y direction (0/1 = front/back) and percentage (0..1),
	// scaled by ShrinkStrength and stepped by Steps.
	//
	// If per-floor attrs are missing, we generate deterministic noise per floor using Seed+Frequency.
	// ------------------------

	// Compute floor count
	int32 FloorCount = 0;
	for (const FPCGPoint& P : InPointData->GetPoints())
	{
		FloorCount = FMath::Max(FloorCount, AFloor->GetValue(P.MetadataEntry) + 1);
	}
	FloorCount = FMath::Max(1, FloorCount);

	// Per-floor bounds
	struct FBounds2D { int32 MinX, MaxX, MinY, MaxY; bool bInit = false; };
	TArray<FBounds2D> Bounds;
	Bounds.SetNum(FloorCount);

	for (const FPCGPoint& P : InPointData->GetPoints())
	{
		const int32 F = AFloor->GetValue(P.MetadataEntry);
		const int32 X = AGridX->GetValue(P.MetadataEntry);
		const int32 Y = AGridY->GetValue(P.MetadataEntry);

		FBounds2D& B = Bounds[F];
		if (!B.bInit)
		{
			B.bInit = true;
			B.MinX = B.MaxX = X;
			B.MinY = B.MaxY = Y;
		}
		else
		{
			B.MinX = FMath::Min(B.MinX, X);
			B.MaxX = FMath::Max(B.MaxX, X);
			B.MinY = FMath::Min(B.MinY, Y);
			B.MaxY = FMath::Max(B.MaxY, Y);
		}
	}

	const int32 DeclSX = (ASizeX ? ASizeX->GetValue(InPointData->GetPoints()[0].MetadataEntry) : 0);
	const int32 DeclSY = (ASizeY ? ASizeY->GetValue(InPointData->GetPoints()[0].MetadataEntry) : 0);

	const float Macro = FMath::Clamp(Settings->ShrinkStrength, 0.0f, 1.0f);
	const int32 StepsQ = FMath::Clamp(Settings->Steps, 1, 64);
	const float Freq = FMath::Max(0.0f, Settings->ShrinkFrequency);

	auto Quantize01 = [&](float V01) -> float
		{
			const float V = FMath::Clamp(V01, 0.0f, 1.0f);
			return (float)FMath::RoundToInt(V * (float)StepsQ) / (float)StepsQ;
		};

	auto ResolveDirBit = [&](EPCGBuildingShrinkDirMode Mode, int32 FloorIdx, bool bIsX) -> int32
		{
			// returns 0=Front, 1=Back
			if (Mode == EPCGBuildingShrinkDirMode::Front) { return 0; }
			if (Mode == EPCGBuildingShrinkDirMode::Back) { return 1; }

			// None: doesn't matter (will disable shrink on that axis)
			if (Mode == EPCGBuildingShrinkDirMode::None) { return 0; }

			// Both: use attribute if present, else noise fallback
			if (bIsX && ADirX)
			{
				// take first point on this floor
				for (const FPCGPoint& P : InPointData->GetPoints())
				{
					if (AFloor->GetValue(P.MetadataEntry) == FloorIdx)
					{
						return (ADirX->GetValue(P.MetadataEntry) != 0) ? 1 : 0;
					}
				}
			}
			if (!bIsX && ADirY)
			{
				for (const FPCGPoint& P : InPointData->GetPoints())
				{
					if (AFloor->GetValue(P.MetadataEntry) == FloorIdx)
					{
						return (ADirY->GetValue(P.MetadataEntry) != 0) ? 1 : 0;
					}
				}
			}

			// Deterministic noise fallback (PerlinNoise2D returns [-1,1])
			const float X = (float)FloorIdx * (Freq * 0.173f + 0.001f) + (float)(Settings->ShrinkSeed + Settings->SeedOffset) * 0.01f;
			const float Y = (bIsX ? 11.37f : 23.73f) + (float)(Settings->ShrinkSeed ^ (Settings->SeedOffset * 1337)) * 0.001f;
			const float N = FMath::PerlinNoise2D(FVector2D(X, Y));
			return (N >= 0.0f) ? 1 : 0;
		};

	auto ResolvePct = [&](int32 FloorIdx, bool bIsX) -> float
		{
			// returns 0..1 raw percentage (before macro/quantize)
			if (bIsX && APerX)
			{
				for (const FPCGPoint& P : InPointData->GetPoints())
				{
					if (AFloor->GetValue(P.MetadataEntry) == FloorIdx)
					{
						return FMath::Clamp(APerX->GetValue(P.MetadataEntry), 0.0f, 1.0f);
					}
				}
			}
			if (!bIsX && APerY)
			{
				for (const FPCGPoint& P : InPointData->GetPoints())
				{
					if (AFloor->GetValue(P.MetadataEntry) == FloorIdx)
					{
						return FMath::Clamp(APerY->GetValue(P.MetadataEntry), 0.0f, 1.0f);
					}
				}
			}

			// Noise fallback (0..1)
			const float X = (float)FloorIdx * (Freq * 0.137f + 0.001f) + (float)(Settings->ShrinkSeed + Settings->SeedOffset) * 0.02f;
			const float Y = (bIsX ? 101.9f : 202.3f) + (float)(Settings->ShrinkSeed ^ (Settings->SeedOffset * 92821)) * 0.001f;
			const float N = FMath::PerlinNoise2D(FVector2D(X, Y)); // [-1,1]
			return 0.5f * (N + 1.0f); // [0,1]
		};

	struct FTrimRect
	{
		bool bInit = false;
		int32 MinX = 0, MaxX = 0, MinY = 0, MaxY = 0;
		int32 DirX = 0; // 0=Front,1=Back
		int32 DirY = 0;
		float PctX = 0.0f;
		float PctY = 0.0f;
	};

	TArray<FTrimRect> Trim;
	Trim.SetNum(FloorCount);

	for (int32 F = 0; F < FloorCount; ++F)
	{
		const FBounds2D& B = Bounds[F];
		if (!B.bInit) { continue; }

		// Axis sizes
		const int32 W = (DeclSX > 0) ? DeclSX : (B.MaxX - B.MinX + 1);
		const int32 H = (DeclSY > 0) ? DeclSY : (B.MaxY - B.MinY + 1);

		// Resolve per-floor attrs / fallback
		int32 DirXBit = ResolveDirBit(Settings->DirectionX, F, true);
		int32 DirYBit = ResolveDirBit(Settings->DirectionY, F, false);

		float PctX = ResolvePct(F, true);
		float PctY = ResolvePct(F, false);

		// Apply macro multiplier + quantize
		PctX = Quantize01(PctX * Macro);
		PctY = Quantize01(PctY * Macro);

		// Disable axis if mode == None
		if (Settings->DirectionX == EPCGBuildingShrinkDirMode::None) { PctX = 0.0f; DirXBit = 0; }
		if (Settings->DirectionY == EPCGBuildingShrinkDirMode::None) { PctY = 0.0f; DirYBit = 0; }

		const int32 CutX = FMath::Clamp(FMath::RoundToInt(PctX * (float)W), 0, W - 1);
		const int32 CutY = FMath::Clamp(FMath::RoundToInt(PctY * (float)H), 0, H - 1);

		FTrimRect& R = Trim[F];
		R.bInit = true;
		R.DirX = DirXBit;
		R.DirY = DirYBit;
		R.PctX = PctX;
		R.PctY = PctY;

		R.MinX = B.MinX;
		R.MaxX = B.MaxX;
		R.MinY = B.MinY;
		R.MaxY = B.MaxY;

		// Apply trims.
		// DirBit: 0=Front => trim from negative side (increase Min), 1=Back => trim from positive side (decrease Max)
		if (CutX > 0)
		{
			if (DirXBit == 0) { R.MinX += CutX; }
			else { R.MaxX -= CutX; }
		}
		if (CutY > 0)
		{
			if (DirYBit == 0) { R.MinY += CutY; }
			else { R.MaxY -= CutY; }
		}

		// Safety: keep at least 1 cell on each axis
		if (R.MinX > R.MaxX)
		{
			const int32 Mid = (B.MinX + B.MaxX) / 2;
			R.MinX = Mid;
			R.MaxX = Mid;
		}
		if (R.MinY > R.MaxY)
		{
			const int32 Mid = (B.MinY + B.MaxY) / 2;
			R.MinY = Mid;
			R.MaxY = Mid;
		}
	}

	// Output
	UPCGPointData* OutData = NewObject<UPCGPointData>();
	check(OutData);

	UPCGMetadata* OutMeta = OutData->MutableMetadata();
	check(OutMeta);

	OutMeta->Initialize(InMeta);

	// Ensure the doc-matching attrs exist on output (so downstream can read them even if they were generated here)
	auto* OutDirX = OutMeta->FindOrCreateAttribute<int32>(PCGBuildingAttr::ShrinkDirectionX, 0, false, true);
	auto* OutDirY = OutMeta->FindOrCreateAttribute<int32>(PCGBuildingAttr::ShrinkDirectionY, 0, false, true);
	auto* OutPerX = OutMeta->FindOrCreateAttribute<float>(PCGBuildingAttr::ShrinkPercentageX, 0.0f, false, true);
	auto* OutPerY = OutMeta->FindOrCreateAttribute<float>(PCGBuildingAttr::ShrinkPercentageY, 0.0f, false, true);

	// Keep legacy attrs as well (best-effort)
	auto* AShrinkAxis = OutMeta->FindOrCreateAttribute<int32>(PCGBuildingAttr::ShrinkAxis, 0, false, true);
	auto* AShrinkSign = OutMeta->FindOrCreateAttribute<int32>(PCGBuildingAttr::ShrinkSign, +1, false, true);
	auto* AShrinkStr = OutMeta->FindOrCreateAttribute<float>(PCGBuildingAttr::ShrinkStrength, 0.0f, false, true);

	TArray<FPCGPoint>& OutPoints = OutData->GetMutablePoints();
	OutPoints.Reserve(InPointData->GetPoints().Num());

	for (const FPCGPoint& InP : InPointData->GetPoints())
	{
		const int32 F = AFloor->GetValue(InP.MetadataEntry);
		if (!Trim.IsValidIndex(F) || !Trim[F].bInit) { continue; }

		const int32 X = AGridX->GetValue(InP.MetadataEntry);
		const int32 Y = AGridY->GetValue(InP.MetadataEntry);

		const FTrimRect& R = Trim[F];
		if (X < R.MinX || X > R.MaxX || Y < R.MinY || Y > R.MaxY)
		{
			continue;
		}

		FPCGPoint OutP = InP;
		const PCGMetadataEntryKey NewKey = OutMeta->AddEntry(InP.MetadataEntry);
		OutP.MetadataEntry = NewKey;

		OutDirX->SetValue(NewKey, R.DirX);
		OutDirY->SetValue(NewKey, R.DirY);
		OutPerX->SetValue(NewKey, R.PctX);
		OutPerY->SetValue(NewKey, R.PctY);

		// Legacy fill (cannot represent both axes; we pick X if active else Y)
		if (R.PctX > 0.0f)
		{
			AShrinkAxis->SetValue(NewKey, 0);
			AShrinkSign->SetValue(NewKey, (R.DirX == 0) ? -1 : +1);
			AShrinkStr->SetValue(NewKey, R.PctX);
		}
		else
		{
			AShrinkAxis->SetValue(NewKey, 1);
			AShrinkSign->SetValue(NewKey, (R.DirY == 0) ? -1 : +1);
			AShrinkStr->SetValue(NewKey, R.PctY);
		}

		OutPoints.Add(OutP);
	}

	FPCGTaggedData& OutTagged = Context->OutputData.TaggedData.Emplace_GetRef();
	OutTagged.Pin = PCGPinConstants::DefaultOutputLabel;
	OutTagged.Data = OutData;

	return true;
}
