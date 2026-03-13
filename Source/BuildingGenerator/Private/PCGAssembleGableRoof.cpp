#include "PCGAssembleGableRoof.h"

#include "PCGContext.h"
#include "PCGPin.h"
#include "PCGParamData.h"

#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Metadata/PCGMetadataAttributeTraits.h" // MetadataTypes<T>::Id
#include "Engine/StaticMesh.h"

static const FName PIN_POINTS = TEXT("Points");
static const FName PIN_MESHTABLE = TEXT("MeshTable");
static const FName PIN_OUT = TEXT("Out");

#define LOCTEXT_NAMESPACE "PCGAssembleGableRoof"

FText UPCGAssembleGableRoofSettings::GetDefaultNodeTitle() const
{
	return LOCTEXT("NodeTitle", "Assemble Gable Roof");
}

#undef LOCTEXT_NAMESPACE

// ------------------------
// Utilities
// ------------------------
static bool LoadMeshBounds(const FSoftObjectPath& MeshPath, FVector& OutSize)
{
	OutSize = FVector(100, 100, 100);
	if (!MeshPath.IsValid()) return false;

	UObject* Obj = MeshPath.TryLoad();
	UStaticMesh* SM = Cast<UStaticMesh>(Obj);
	if (!SM) return false;

	const FBoxSphereBounds B = SM->GetBounds();
	OutSize = B.BoxExtent * 2.0f; // full size
	return true;
}

template<typename T>
static const FPCGMetadataAttribute<T>* GetAttrTyped(const UPCGMetadata* MD, FName Name)
{
	if (!MD) return nullptr;
	const FPCGMetadataAttributeBase* Base = MD->GetConstAttribute(Name);
	if (!Base) return nullptr;

	// UE5.5: prefer IsOfTypes<T>(TypeId) to avoid direct MetadataTypes<T>::Id dependency
	if (!PCG::Private::IsOfTypes<T>(Base->GetTypeId()))
		return nullptr;

	return static_cast<const FPCGMetadataAttribute<T>*>(Base);
}

template<typename T>
static FPCGMetadataAttribute<T>* GetOrCreateAttr(UPCGMetadata* MD, FName Name, const T& DefaultValue)
{
	if (!MD) return nullptr;

	if (FPCGMetadataAttributeBase* Existing = MD->GetMutableAttribute(Name))
	{
		//  type matches -> reuse
		if (PCG::Private::IsOfTypes<T>(Existing->GetTypeId()))
		{
			return static_cast<FPCGMetadataAttribute<T>*>(Existing);
		}

		//  type mismatch -> recreate as T (safer)
		// NOTE: if you prefer not to recreate, just return nullptr here.
	}

	return MD->CreateAttribute<T>(Name, DefaultValue,
		/*bAllowsInterpolation*/false,
		/*bOverrideParent*/true);
}

static bool ReadOwnerIndex_Any(const UPCGPointData* InData, const FPCGPoint& Pt, FName Attr, int64& OutOwner)
{
	const UPCGMetadata* MD = InData ? InData->Metadata : nullptr;
	if (!MD) return false;

	if (const auto* A = GetAttrTyped<int64>(MD, Attr)) { OutOwner = A->GetValueFromItemKey(Pt.MetadataEntry); return true; }
	if (const auto* A = GetAttrTyped<int32>(MD, Attr)) { OutOwner = (int64)A->GetValueFromItemKey(Pt.MetadataEntry); return true; }
	if (const auto* A = GetAttrTyped<double>(MD, Attr)) { OutOwner = (int64)llround(A->GetValueFromItemKey(Pt.MetadataEntry)); return true; }
	if (const auto* A = GetAttrTyped<float>(MD, Attr)) { OutOwner = (int64)llround((double)A->GetValueFromItemKey(Pt.MetadataEntry)); return true; }

	return false;
}

static bool ReadRotationQuat(const UPCGPointData* InData, const FPCGPoint& Pt, FName Attr, FQuat& OutQ)
{
	const UPCGMetadata* MD = InData ? InData->Metadata : nullptr;
	if (!MD) return false;

	if (const auto* A = GetAttrTyped<FRotator>(MD, Attr)) { OutQ = A->GetValueFromItemKey(Pt.MetadataEntry).Quaternion(); return true; }
	if (const auto* A = GetAttrTyped<FVector>(MD, Attr))
	{
		const FVector V = A->GetValueFromItemKey(Pt.MetadataEntry);
		OutQ = FRotator((float)V.X, (float)V.Y, (float)V.Z).Quaternion();
		return true;
	}
	if (const auto* A = GetAttrTyped<double>(MD, Attr)) { OutQ = FRotator(0.f, (float)A->GetValueFromItemKey(Pt.MetadataEntry), 0.f).Quaternion(); return true; }
	if (const auto* A = GetAttrTyped<float>(MD, Attr)) { OutQ = FRotator(0.f, (float)A->GetValueFromItemKey(Pt.MetadataEntry), 0.f).Quaternion(); return true; }

	return false;
}

static bool ReadMeshTable(
	const UPCGAssembleGableRoofSettings* Settings,
	const FPCGDataCollection& Input,
	TMap<FName, FGableMeshRow>& OutBySymbol,
	FString& OutError)
{
	OutBySymbol.Reset();

	const UPCGParamData* Param = nullptr;
	for (const FPCGTaggedData& TD : Input.GetInputsByPin(PIN_MESHTABLE))
	{
		Param = Cast<UPCGParamData>(TD.Data);
		if (Param) break;
	}

	if (!Param || !Param->Metadata)
	{
		OutError = TEXT("MeshTable missing or not UPCGParamData with Metadata.");
		return false;
	}

	const UPCGMetadata* MD = Param->Metadata;

	// Symbol can be Name or String
	const auto* SymName = GetAttrTyped<FName>(MD, Settings->ColSymbol);
	const auto* SymStr = GetAttrTyped<FString>(MD, Settings->ColSymbol);

	// Mesh can be SoftObjectPath or String
	const auto* MeshSoft = GetAttrTyped<FSoftObjectPath>(MD, Settings->ColMesh);
	const auto* MeshStr = GetAttrTyped<FString>(MD, Settings->ColMesh);

	if ((!SymName && !SymStr) || (!MeshSoft && !MeshStr))
	{
		OutError = TEXT("MeshTable: Symbol must be Name or String; Mesh must be SoftObjectPath or String.");
		return false;
	}

	// bScalable can be bool/int32/int64
	const auto* ScaleB = GetAttrTyped<bool>(MD, Settings->ColScalable);
	const auto* ScaleI = GetAttrTyped<int32>(MD, Settings->ColScalable);
	const auto* ScaleL = GetAttrTyped<int64>(MD, Settings->ColScalable);

	// Probe row keys (0..31). Stop after 4 empties.
	int32 EmptyRun = 0;
	for (PCGMetadataEntryKey K = 0; K < 32; ++K)
	{
		FName Sym = NAME_None;
		if (SymName) Sym = SymName->GetValueFromItemKey(K);
		else
		{
			const FString S = SymStr->GetValueFromItemKey(K);
			if (!S.IsEmpty()) Sym = FName(*S);
		}

		if (Sym.IsNone())
		{
			if (++EmptyRun >= 4) break;
			continue;
		}
		EmptyRun = 0;

		FSoftObjectPath MeshPath;
		if (MeshSoft) MeshPath = MeshSoft->GetValueFromItemKey(K);
		else
		{
			const FString P = MeshStr->GetValueFromItemKey(K);
			MeshPath = FSoftObjectPath(P);
		}

		if (!MeshPath.IsValid())
			continue;

		bool bScalable = true;
		if (ScaleB)      bScalable = ScaleB->GetValueFromItemKey(K);
		else if (ScaleI) bScalable = (ScaleI->GetValueFromItemKey(K) != 0);
		else if (ScaleL) bScalable = (ScaleL->GetValueFromItemKey(K) != 0);

		FGableMeshRow Row;
		Row.Symbol = Sym;
		Row.MeshPath = MeshPath;
		Row.bScalable = bScalable;
		OutBySymbol.Add(Sym, Row);
	}

	static const FName NeedSyms[] = { TEXT("CapLeft"), TEXT("CapRight"), TEXT("RoofTileLeft"), TEXT("RoofTileRight") };
	for (const FName& S : NeedSyms)
	{
		if (!OutBySymbol.Contains(S))
		{
			OutError = FString::Printf(TEXT("MeshTable missing Symbol: %s"), *S.ToString());
			return false;
		}
	}

	return true;
}

// ------------------------
// Settings plumbing
// ------------------------
TArray<FPCGPinProperties> UPCGAssembleGableRoofSettings::InputPinProperties() const
{
	return {
		FPCGPinProperties(PIN_POINTS, EPCGDataType::Point),
		FPCGPinProperties(PIN_MESHTABLE, EPCGDataType::Param)
	};
}

TArray<FPCGPinProperties> UPCGAssembleGableRoofSettings::OutputPinProperties() const
{
	return { FPCGPinProperties(PIN_OUT, EPCGDataType::Point) };
}

FPCGElementPtr UPCGAssembleGableRoofSettings::CreateElement() const
{
	return MakeShared<FPCGAssembleGableRoofElement>().ToSharedPtr();
}

// ------------------------
// Execute
// ------------------------
bool FPCGAssembleGableRoofElement::ExecuteInternal(FPCGContext* Context) const
{
	const UPCGAssembleGableRoofSettings* Settings = Context ? Context->GetInputSettings<UPCGAssembleGableRoofSettings>() : nullptr;
	if (!Settings) return true;

	// Input points
	const UPCGPointData* InData = nullptr;
	for (const FPCGTaggedData& TD : Context->InputData.GetInputsByPin(PIN_POINTS))
	{
		InData = Cast<UPCGPointData>(TD.Data);
		if (InData) break;
	}
	if (!InData)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AssembleGableRoof] Missing Points input."));
		return true;
	}

	// Mesh table
	TMap<FName, FGableMeshRow> MeshBySym;
	FString Err;
	if (!ReadMeshTable(Settings, Context->InputData, MeshBySym, Err))
	{
		UE_LOG(LogTemp, Warning, TEXT("[AssembleGableRoof] %s"), *Err);
		return true;
	}

	const FGableMeshRow CapL = MeshBySym[TEXT("CapLeft")];
	const FGableMeshRow CapR = MeshBySym[TEXT("CapRight")];
	const FGableMeshRow TileL = MeshBySym[TEXT("RoofTileLeft")];
	const FGableMeshRow TileR = MeshBySym[TEXT("RoofTileRight")];

	FVector CapSizeL, CapSizeR, TileSizeL, TileSizeR;
	LoadMeshBounds(CapL.MeshPath, CapSizeL);
	LoadMeshBounds(CapR.MeshPath, CapSizeR);
	LoadMeshBounds(TileL.MeshPath, TileSizeL);
	LoadMeshBounds(TileR.MeshPath, TileSizeR);

	// Output
	UPCGPointData* OutData = NewObject<UPCGPointData>();
	OutData->InitializeFromData(InData);
	OutData->GetMutablePoints().Reset();

	UPCGMetadata* OutMD = OutData->Metadata;

	// IMPORTANT: store Mesh as SoftObjectPath, not FString (safer & matches PCG metadata helpers)
	auto* OutMeshAttr = GetOrCreateAttr<FSoftObjectPath>(OutMD, TEXT("Mesh"), FSoftObjectPath());
	auto* OutOwnerAttr = GetOrCreateAttr<int64>(OutMD, TEXT("OwnerIndex"), 0);
	auto* OutSymAttr = Settings->bWriteDebugAttrs ? GetOrCreateAttr<FName>(OutMD, TEXT("Symbol"), NAME_None) : nullptr;

	const TArray<FPCGPoint>& InPts = InData->GetPoints();

	// Group by owner
	TMap<int64, TArray<int32>> OwnerToIdx;
	for (int32 i = 0; i < InPts.Num(); ++i)
	{
		int64 Owner = 0;
		if (!ReadOwnerIndex_Any(InData, InPts[i], Settings->OwnerIndexAttribute, Owner))
			continue;
		OwnerToIdx.FindOrAdd(Owner).Add(i);
	}

	for (const auto& KV : OwnerToIdx)
	{
		const int64 Owner = KV.Key;
		const TArray<int32>& Idxs = KV.Value;
		if (Idxs.Num() < 4) continue;

		// Center
		FVector CenterWS(0, 0, 0);
		for (int32 idx : Idxs) CenterWS += InPts[idx].Transform.GetLocation();
		CenterWS /= (float)Idxs.Num();

		// Rotation attribute on points: "Rotation"
		FQuat OwnerRot = InPts[Idxs[0]].Transform.GetRotation();
		{
			FQuat Q;
			if (ReadRotationQuat(InData, InPts[Idxs[0]], Settings->RotationAttribute, Q))
				OwnerRot = Q;
		}

		auto ToLocal = [&](const FVector& Pws)->FVector { return OwnerRot.UnrotateVector(Pws - CenterWS); };

		// Local AABB + local positions
		TArray<FVector> LocalPos;
		LocalPos.Reserve(Idxs.Num());

		double minX = DBL_MAX, maxX = -DBL_MAX, minY = DBL_MAX, maxY = -DBL_MAX;
		for (int32 k = 0; k < Idxs.Num(); ++k)
		{
			const FVector Pl = ToLocal(InPts[Idxs[k]].Transform.GetLocation());
			LocalPos.Add(Pl);
			minX = FMath::Min(minX, (double)Pl.X); maxX = FMath::Max(maxX, (double)Pl.X);
			minY = FMath::Min(minY, (double)Pl.Y); maxY = FMath::Max(maxY, (double)Pl.Y);
		}

		const double spanX = FMath::Max(1.0, maxX - minX);
		const double spanY = FMath::Max(1.0, maxY - minY);
		const double TolX = FMath::Max(2.0, (double)Settings->EdgeTolRatio * spanX);
		const double TolY = FMath::Max(2.0, (double)Settings->EdgeTolRatio * spanY);

		TArray<int32> EdgeXMin, EdgeXMax, EdgeYMin, EdgeYMax;
		EdgeXMin.Reserve(64); EdgeXMax.Reserve(64); EdgeYMin.Reserve(64); EdgeYMax.Reserve(64);

		for (int32 i = 0; i < LocalPos.Num(); ++i)
		{
			const FVector& P = LocalPos[i];
			if (FMath::Abs((double)P.X - minX) <= TolX) EdgeXMin.Add(i);
			if (FMath::Abs((double)P.X - maxX) <= TolX) EdgeXMax.Add(i);
			if (FMath::Abs((double)P.Y - minY) <= TolY) EdgeYMin.Add(i); // Y- => Left
			if (FMath::Abs((double)P.Y - maxY) <= TolY) EdgeYMax.Add(i); // Y+ => Right
		}

		auto PickClosestOnEdge = [&](const TArray<int32>& Edge, double TargetX, double TargetY)->int32
			{
				int32 Best = INDEX_NONE; double BestD2 = DBL_MAX;
				for (int32 li : Edge)
				{
					const FVector& P = LocalPos[li];
					const double dx = (double)P.X - TargetX;
					const double dy = (double)P.Y - TargetY;
					const double d2 = dx * dx + dy * dy;
					if (d2 < BestD2) { BestD2 = d2; Best = li; }
				}
				return Best;
			};

		auto PickClosestByX = [&](const TArray<int32>& Edge, double TargetX)->int32
			{
				int32 Best = INDEX_NONE; double BestAbs = DBL_MAX;
				for (int32 li : Edge)
				{
					const double ax = FMath::Abs((double)LocalPos[li].X - TargetX);
					if (ax < BestAbs) { BestAbs = ax; Best = li; }
				}
				return Best;
			};

		// SAFE output emitter: always new key (prevents access violation)
		auto EmitPoint = [&](int32 SrcLocalIdx, const FGableMeshRow& Row, const FTransform& Xf)
			{
				const int32 SrcIdx = Idxs[SrcLocalIdx];

				FPCGPoint OutPt = InPts[SrcIdx];
				OutPt.Transform = Xf;

				const PCGMetadataEntryKey Key = OutMD->AddEntry();
				OutPt.MetadataEntry = Key;

				if (OutMeshAttr)  OutMeshAttr->SetValue(Key, Row.MeshPath);
				if (OutOwnerAttr) OutOwnerAttr->SetValue(Key, Owner);
				if (OutSymAttr)   OutSymAttr->SetValue(Key, Row.Symbol);

				OutData->GetMutablePoints().Add(OutPt);
			};

		// -------- Caps: X ends, split by Y sign --------
		const double MidY = 0.5 * (minY + maxY);
		const double HalfLeftW = MidY - minY;
		const double HalfRightW = maxY - MidY;

		auto SpawnCapHalf = [&](bool bAtXMax, bool bRightHalf)
			{
				const double Xend = bAtXMax ? maxX : minX;
				const double Yside = bRightHalf ? maxY : minY;
				const TArray<int32>& XEdge = bAtXMax ? EdgeXMax : EdgeXMin;

				const int32 LocalPick = PickClosestOnEdge(XEdge, Xend, Yside);
				if (LocalPick == INDEX_NONE) return;

				FQuat Q = OwnerRot;
				if (bAtXMax && Settings->bFlipCapAtXMax)
				{
					const FVector ZAxisWS = OwnerRot.RotateVector(FVector::UpVector);
					Q = FQuat(ZAxisWS, PI) * Q;
				}

				const FGableMeshRow& Row = bRightHalf ? CapR : CapL;
				const FVector CapSize = bRightHalf ? CapSizeR : CapSizeL;

				const double TargetW = bRightHalf ? HalfRightW : HalfLeftW;
				const float ScaleY = (CapSize.Y > 1e-3f) ? (float)(TargetW / (double)CapSize.Y) : 1.f;

				const FVector Pws = InPts[Idxs[LocalPick]].Transform.GetLocation();
				const FTransform Xf(Q, Pws, FVector(1.f, ScaleY * Settings->CapScaleYExtra, 1.f));

				EmitPoint(LocalPick, Row, Xf);
			};

		// Always 4 cap halves (both ends, both Y sides)
		SpawnCapHalf(false, false);
		SpawnCapHalf(false, true);
		SpawnCapHalf(true, false);
		SpawnCapHalf(true, true);

		// -------- Tiles: Y edges, march along X --------
		const double UsableLen = FMath::Max(0.0, maxX - minX);

		auto SpawnTilesOnYEdge = [&](bool bRightSide)
			{
				const TArray<int32>& YEdge = bRightSide ? EdgeYMax : EdgeYMin;
				if (YEdge.Num() == 0 || UsableLen <= 1.0) return;

				const FGableMeshRow& Row = bRightSide ? TileR : TileL;
				const FVector TileSize = bRightSide ? TileSizeR : TileSizeL;

				// Your convention: X=length, Y=width
				const double TileLen = FMath::Max(1.0, (double)TileSize.X);
				const double TileWidth = FMath::Max(1.0, (double)TileSize.Y);

				const int32 N = FMath::Max(1, (int32)FMath::RoundToInt((float)(UsableLen / TileLen)));
				const double RawScaleX = UsableLen / (double)N / TileLen;
				const double ScaleX = Row.bScalable
					? FMath::Clamp(RawScaleX, (double)Settings->TileScaleXMin, (double)Settings->TileScaleXMax)
					: 1.0;

				const double SideWidth = bRightSide ? (maxY - MidY) : (MidY - minY);
				const float ScaleY = (float)(SideWidth / TileWidth);

				const double OutSign = bRightSide ? +1.0 : -1.0;
				const FVector OutDirWS = OwnerRot.RotateVector(FVector(0.f, (float)OutSign, 0.f));
				const double MinorHalf = 0.5 * TileWidth;

				for (int32 i = 0; i < N; ++i)
				{
					const double Xat = minX + (i + 0.5) * TileLen * ScaleX;
					const int32 LocalPick = PickClosestByX(YEdge, Xat);
					if (LocalPick == INDEX_NONE) continue;

					FVector Pws = InPts[Idxs[LocalPick]].Transform.GetLocation();
					Pws += OutDirWS * (MinorHalf * (double)Settings->TileEaveOutOffsetScale);

					const FTransform Xf(OwnerRot, Pws, FVector((float)ScaleX, ScaleY, 1.f));
					EmitPoint(LocalPick, Row, Xf);
				}
			};

		SpawnTilesOnYEdge(false); // Y- => TileLeft
		SpawnTilesOnYEdge(true);  // Y+ => TileRight
	}

	// Output
	FPCGTaggedData& OutTD = Context->OutputData.TaggedData.Emplace_GetRef();
	OutTD.Pin = PIN_OUT;
	OutTD.Data = OutData;

	return true;
}