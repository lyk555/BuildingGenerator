#include "PCGGenerateCantileverSupports.h"
#include "Logging/LogMacros.h"
#include "PCGContext.h"
#include "PCGElement.h"
#include "PCGPin.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Utils/PCGLogErrors.h"
#include "clipper2/clipper.h"

#define LOCTEXT_NAMESPACE "PCGGenerateCantileverSupports"

using namespace Clipper2Lib;

namespace
{
	static Point64 ToP64(const FVector2D& P, double Scale)
	{
		return Point64((int64)llround(P.X * Scale), (int64)llround(P.Y * Scale));
	}

	static void ComputeBounds64(const Paths64& P, double InvScale, FVector2D& OutMin, FVector2D& OutMax)
	{
		OutMin = FVector2D(DBL_MAX, DBL_MAX);
		OutMax = FVector2D(-DBL_MAX, -DBL_MAX);

		for (const Path64& Path : P)
		{
			for (const Point64& Q : Path)
			{
				const double X = (double)Q.x / InvScale;
				const double Y = (double)Q.y / InvScale;
				OutMin.X = FMath::Min(OutMin.X, (float)X);
				OutMin.Y = FMath::Min(OutMin.Y, (float)Y);
				OutMax.X = FMath::Max(OutMax.X, (float)X);
				OutMax.Y = FMath::Max(OutMax.Y, (float)Y);
			}
		}
	}

	static void ComputeBounds(const TArray<FVector2D>& Pts, FVector2D& OutMin, FVector2D& OutMax)
	{
		OutMin = FVector2D(DBL_MAX, DBL_MAX);
		OutMax = FVector2D(-DBL_MAX, -DBL_MAX);
		for (const FVector2D& P : Pts)
		{
			OutMin.X = FMath::Min(OutMin.X, P.X);
			OutMin.Y = FMath::Min(OutMin.Y, P.Y);
			OutMax.X = FMath::Max(OutMax.X, P.X);
			OutMax.Y = FMath::Max(OutMax.Y, P.Y);
		}
	}

	// Fallback ring ordering: angle sort around centroid
	static void SortRingByAngle(TArray<FVector2D>& Pts)
	{
		if (Pts.Num() < 3) return;

		FVector2D C(0, 0);
		for (const FVector2D& P : Pts) C += P;
		C /= (double)Pts.Num();

		Pts.Sort([&](const FVector2D& A, const FVector2D& B)
			{
				const double AA = FMath::Atan2(A.Y - C.Y, A.X - C.X);
				const double BB = FMath::Atan2(B.Y - C.Y, B.X - C.X);
				return AA < BB;
			});

		// light dedupe
		const double Eps2 = 1e-6;
		TArray<FVector2D> Clean;
		Clean.Reserve(Pts.Num());
		for (const FVector2D& P : Pts)
		{
			if (Clean.Num() == 0 || (Clean.Last() - P).SizeSquared() > Eps2)
			{
				Clean.Add(P);
			}
		}
		if (Clean.Num() >= 3 && (Clean[0] - Clean.Last()).SizeSquared() <= Eps2)
		{
			Clean.Pop();
		}

		Pts = MoveTemp(Clean);
	}

	static bool IsInsideAny(const Point64& Pt, const Paths64& Paths)
	{
		for (const Path64& Path : Paths)
		{
			const PointInPolygonResult R = PointInPolygon(Pt, Path);
			if (R != PointInPolygonResult::IsOutside)
			{
				return true;
			}
		}
		return false;
	}

	static FVector2D ZeroYawDirToVec2(EPCGSupportZeroYawDir D)
	{
		switch (D)
		{
		case EPCGSupportZeroYawDir::PlusX:  return FVector2D(1, 0);
		case EPCGSupportZeroYawDir::MinusX: return FVector2D(-1, 0);
		case EPCGSupportZeroYawDir::PlusY:  return FVector2D(0, 1);
		case EPCGSupportZeroYawDir::MinusY: return FVector2D(0, -1);
		default: return FVector2D(0, 1);
		}
	}

	static FVector2D ClosestPointOnSeg2D(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const double Den = (double)AB.SizeSquared();
		if (Den <= 1e-12) return A;

		const double T = FMath::Clamp((double)FVector2D::DotProduct(P - A, AB) / Den, 0.0, 1.0);
		return A + (float)T * AB;
	}

	// 在 Clipper Paths64 的边界上找距离点 P 最近的点（返回世界坐标）
	static bool ClosestPointOnPaths64(const Paths64& Paths, double InvScale, const FVector2D& P, FVector2D& OutClosest)
	{
		double BestD2 = DBL_MAX;
		bool bFound = false;

		for (const Path64& Path : Paths)
		{
			const int32 N = (int32)Path.size();
			if (N < 2) continue;

			for (int32 i = 0; i < N; ++i)
			{
				const Point64& A0 = Path[i];
				const Point64& B0 = Path[(i + 1) % N];

				const FVector2D A((double)A0.x / InvScale, (double)A0.y / InvScale);
				const FVector2D B((double)B0.x / InvScale, (double)B0.y / InvScale);

				const FVector2D C = ClosestPointOnSeg2D(P, A, B);
				const double D2 = (double)(C - P).SizeSquared();
				if (D2 < BestD2)
				{
					BestD2 = D2;
					OutClosest = C;
					bFound = true;
				}
			}
		}

		return bFound;
	}

	struct FRingKey
	{
		int32 Owner = 0;
		int32 RingId = 0;

		friend bool operator==(const FRingKey& A, const FRingKey& B)
		{
			return A.Owner == B.Owner && A.RingId == B.RingId;
		}
	};

	FORCEINLINE uint32 GetTypeHash(const FRingKey& K)
	{
		return HashCombine(::GetTypeHash(K.Owner), ::GetTypeHash(K.RingId));
	}

	struct FRingPoly
	{
		FRingKey Key;
		TArray<FVector2D> RingPts;
		TArray<int32> OrderKeys; // optional ordering key per point (e.g. VertexIndex)

		FVector Center = FVector::ZeroVector;
		FVector Extents = FVector::ZeroVector;
		double BottomZ = 0.0;
		double TopZ = 0.0;

		FVector2D BMin, BMax;
		Paths64 Paths; // 1 ring -> 1 path
	};
}

class FPCGGenerateCantileverSupportsElement : public IPCGElement
{
public:
	virtual bool ExecuteInternal(FPCGContext* Context) const override
	{
		const UPCGGenerateCantileverSupportsSettings* Settings = Context->GetInputSettings<UPCGGenerateCantileverSupportsSettings>();
		if (!Settings) return true;

		const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
		if (Inputs.IsEmpty()) return true;

		const UPCGPointData* InPointData = Cast<UPCGPointData>(Inputs[0].Data);
		if (!InPointData) return true;

		const UPCGMetadata* InMeta = InPointData->ConstMetadata();
		if (!InMeta) return true;

		auto FindI32 = [&](FName Name) -> const FPCGMetadataAttribute<int32>*
			{
				return InMeta->GetConstTypedAttribute<int32>(Name);
			};
		auto FindV3 = [&](FName Name) -> const FPCGMetadataAttribute<FVector>*
			{
				return InMeta->GetConstTypedAttribute<FVector>(Name);
			};

		// probe: 如果某个 FVector attribute 在前 N 个有效点上全是 0，基本可以判定“只有定义没值”
		auto ProbeNonZeroV3 = [&](const FPCGMetadataAttribute<FVector>* A, const TArray<FPCGPoint>& Pts) -> bool
			{
				if (!A) return false;
				int32 Checked = 0;
				for (const FPCGPoint& P : Pts)
				{
					const PCGMetadataEntryKey K = P.MetadataEntry;
					if (K == PCGInvalidEntryKey) continue;

					const FVector V = A->GetValueFromItemKey(K);
					if (FMath::Abs(V.X) > KINDA_SMALL_NUMBER || FMath::Abs(V.Y) > KINDA_SMALL_NUMBER || FMath::Abs(V.Z) > KINDA_SMALL_NUMBER)
					{
						return true;
					}

					if (++Checked >= 16) break;
				}
				return false;
			};

		const TArray<FPCGPoint>& InPts = InPointData->GetPoints();

		// ---- Resolve attributes with fallback to your pipeline names ----
		FName OwnerName = Settings->OwnerIndexAttribute;
		const auto* AttrOwner = FindI32(OwnerName);
		if (!AttrOwner) { OwnerName = TEXT("OwnerIndex"); AttrOwner = FindI32(OwnerName); }

		FName RingName = Settings->RingIdAttribute;
		const auto* AttrRingId = FindI32(RingName);
		if (!AttrRingId) { RingName = TEXT("RingId"); AttrRingId = FindI32(RingName); }

		FName CenterName = Settings->VolumeCenterAttribute;
		const auto* AttrCenter = FindV3(CenterName);
		if (!AttrCenter)
		{
			CenterName = TEXT("CenterPosition");
			AttrCenter = FindV3(CenterName);
		}
		else
		{
			// 如果读出来全是默认 0，优先切换到 CenterPosition
			if (!ProbeNonZeroV3(AttrCenter, InPts))
			{
				const auto* Alt = FindV3(TEXT("CenterPosition"));
				if (ProbeNonZeroV3(Alt, InPts))
				{
					CenterName = TEXT("CenterPosition");
					AttrCenter = Alt;
				}
			}
		}

		FName ExtName = Settings->VolumeExtentsAttribute;
		const auto* AttrExt = FindV3(ExtName);
		if (!AttrExt)
		{
			ExtName = TEXT("GridExtents");
			AttrExt = FindV3(ExtName);
		}
		else
		{
			if (!ProbeNonZeroV3(AttrExt, InPts))
			{
				const auto* Alt = FindV3(TEXT("GridExtents"));
				if (ProbeNonZeroV3(Alt, InPts))
				{
					ExtName = TEXT("GridExtents");
					AttrExt = Alt;
				}
			}
		}

		FName VtxName = Settings->VertexIndexAttribute;
		const FPCGMetadataAttribute<int32>* AttrVtx = nullptr;
		if (Settings->bUseVertexIndexForOrdering)
		{
			AttrVtx = FindI32(VtxName);
			if (!AttrVtx) { VtxName = TEXT("VertexIndex"); AttrVtx = FindI32(VtxName); }
		}

		UE_LOG(LogTemp, Warning, TEXT("[Cantilever] Using AttrNames Owner=%s Ring=%s Center=%s Ext=%s Vtx=%s"),
			*OwnerName.ToString(), *RingName.ToString(), *CenterName.ToString(), *ExtName.ToString(), *VtxName.ToString());

		if (!AttrOwner || !AttrRingId || !AttrCenter || !AttrExt || (Settings->bUseVertexIndexForOrdering && !AttrVtx))
		{
			PCGLog::LogErrorOnGraph(
				LOCTEXT("MissingAttrs",
					"GenerateCantileverSupports: Missing required attrs OwnerIndex/RingId/Center/Extents (and VertexIndex when ordering is enabled) on input points."),
				Context);
			return true;
		}

		// ---- DIAG: check metadata entry validity + first few attribute values ----
		int32 InvalidKeyCount = 0;
		TSet<int32> UniqueOwners;
		TSet<int32> UniqueRings;

		for (int32 i = 0; i < InPts.Num(); ++i)
		{
			const FPCGPoint& P = InPts[i];
			const PCGMetadataEntryKey K = P.MetadataEntry;

			// PCGInvalidEntryKey is the sentinel for "no metadata"
			if (K == PCGInvalidEntryKey)
			{
				++InvalidKeyCount;
				if (i < 5)
				{
					UE_LOG(LogTemp, Warning, TEXT("[Cantilever][DIAG] Pt%d has INVALID MetadataEntry"), i);
				}
				continue;
			}

			const int32 O = AttrOwner->GetValueFromItemKey(K);
			const int32 R = AttrRingId->GetValueFromItemKey(K);
			const FVector C = AttrCenter->GetValueFromItemKey(K);
			const FVector E = AttrExt->GetValueFromItemKey(K);

			UniqueOwners.Add(O);
			UniqueRings.Add(R);

			if (i < 5)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Cantilever][DIAG] Pt%d K=%lld Owner=%d Ring=%d CenterZ=%.1f ExtZ=%.1f"),
					i, (long long)K, O, R, (double)C.Z, (double)E.Z);
			}
		}

		UE_LOG(LogTemp, Warning, TEXT("[Cantilever][DIAG] InPts=%d InvalidKeys=%d UniqueOwners=%d UniqueRings=%d"),
			InPts.Num(), InvalidKeyCount, UniqueOwners.Num(), UniqueRings.Num());

		// group by (Owner,RingId)
		TMap<FRingKey, FRingPoly> Rings;
		for (const FPCGPoint& P : InPts)
		{
			const PCGMetadataEntryKey K = P.MetadataEntry;

			const int32 Owner = AttrOwner->GetValueFromItemKey(K);
			const int32 RingId = AttrRingId->GetValueFromItemKey(K);

			FRingKey Key{ Owner, RingId };
			FRingPoly& R = Rings.FindOrAdd(Key);
			R.Key = Key;

			R.RingPts.Add(FVector2D(P.Transform.GetLocation().X, P.Transform.GetLocation().Y));

			if (Settings->bUseVertexIndexForOrdering)
			{
				R.OrderKeys.Add(AttrVtx->GetValueFromItemKey(K));
			}

			// volume info once per ring
			if (R.RingPts.Num() == 1)
			{
				R.Center = AttrCenter->GetValueFromItemKey(K);
				R.Extents = AttrExt->GetValueFromItemKey(K);
				R.BottomZ = (double)R.Center.Z - (double)R.Extents.Z;
				R.TopZ = (double)R.Center.Z + (double)R.Extents.Z;
			}
		}

		// build clipper paths
		TArray<FRingPoly*> RingList;
		RingList.Reserve(Rings.Num());

		for (auto& It : Rings)
		{
			FRingPoly& R = It.Value;

			// Order ring vertices. Prefer explicit VertexIndex ordering if available; fallback to angle sort.
			if (Settings->bUseVertexIndexForOrdering && R.OrderKeys.Num() == R.RingPts.Num())
			{
				TArray<int32> Idx;
				Idx.SetNum(R.RingPts.Num());
				for (int32 ii = 0; ii < Idx.Num(); ++ii) Idx[ii] = ii;

				Idx.Sort([&](int32 A, int32 B) { return R.OrderKeys[A] < R.OrderKeys[B]; });

				TArray<FVector2D> Ordered;
				Ordered.Reserve(R.RingPts.Num());
				for (int32 ii : Idx) Ordered.Add(R.RingPts[ii]);

				R.RingPts = MoveTemp(Ordered);
			}
			else
			{
				SortRingByAngle(R.RingPts);
			}

			if (R.RingPts.Num() < 3) continue;

			ComputeBounds(R.RingPts, R.BMin, R.BMax);

			Path64 Path;
			Path.reserve((size_t)R.RingPts.Num());
			for (const FVector2D& P : R.RingPts)
			{
				Path.push_back(ToP64(P, Settings->ClipperScale));
			}

			R.Paths.clear();
			R.Paths.push_back(std::move(Path));
			RingList.Add(&R);
		}

		// output point data
		UPCGPointData* OutData = NewObject<UPCGPointData>();
		OutData->InitializeFromData(InPointData);

		UPCGMetadata* OutMeta = OutData->MutableMetadata();
		TArray<FPCGPoint>& OutPts = OutData->GetMutablePoints();

		auto* AttrSupportType = OutMeta->CreateAttribute<int32>(Settings->SupportTypeAttribute, 0, false, true);
		auto* AttrSupportH = OutMeta->CreateAttribute<double>(Settings->SupportHeightAttribute, 0.0, true, true);
		auto* AttrSymbol = OutMeta->CreateAttribute<FString>(Settings->SupportSymbolAttribute, FString(), false, true);
		FPCGMetadataAttribute<float>* AttrYaw = nullptr;
		if (Settings->bWriteYawAttribute && !Settings->SupportYawAttribute.IsNone())
		{
			AttrYaw = OutMeta->CreateAttribute<float>(Settings->SupportYawAttribute, 0.0f, true, true);
		}

		int32 NumUpper = 0;
		int32 NumHasSupport = 0;
		int32 NumHasPO = 0;
		int32 NumSampleHit = 0;

		for (FRingPoly* Upper : RingList)
		{
			if (!Upper) continue;
			++NumUpper;

			Paths64 SupportUnion;
			double BestLowerTopZ = -DBL_MAX;
			double SupportBaseZ = -DBL_MAX; // where supports start (typically lower volume bottom)

			for (FRingPoly* Lower : RingList)
			{
				if (!Lower) continue;
				if (Lower == Upper) continue; // don't let a ring support itself

				const double Gap = Upper->BottomZ - Lower->TopZ;
				if (Gap < -Settings->ZTolerance) continue;
				if (Gap > Settings->MaxGap) continue;

				const bool bOverlap =
					!(Lower->BMax.X < Upper->BMin.X || Lower->BMin.X > Upper->BMax.X ||
						Lower->BMax.Y < Upper->BMin.Y || Lower->BMin.Y > Upper->BMax.Y);

				if (!bOverlap) continue;

				for (const Path64& P : Lower->Paths)
				{
					SupportUnion.push_back(P);
				}

				// pick the lower "supporting level" that is closest to upper bottom
				if (Lower->TopZ > BestLowerTopZ)
				{
					BestLowerTopZ = Lower->TopZ;
					SupportBaseZ = Lower->BottomZ;
				}
			}

			if (SupportUnion.empty())
			{
				UE_LOG(LogTemp, Warning, TEXT("[Cantilever] Upper(Owner=%d Ring=%d) SupportUnion EMPTY. UpperBottom=%.1f UpperTop=%.1f"),
					Upper->Key.Owner, Upper->Key.RingId, Upper->BottomZ, Upper->TopZ);
				continue;
			}
			++NumHasSupport;

			SupportUnion = Union(SupportUnion, FillRule::NonZero);

			const Paths64 PU = Upper->Paths;
			Paths64 PO = Difference(PU, SupportUnion, FillRule::NonZero);
			if (PO.empty())
			{
				UE_LOG(LogTemp, Warning, TEXT("[Cantilever] Upper(Owner=%d Ring=%d) PO EMPTY (no overhang after diff)."),
					Upper->Key.Owner, Upper->Key.RingId);
				continue;
			}
			++NumHasPO;

			const Paths64 PSth = InflatePaths(
				SupportUnion,
				Settings->CantileverThreshold * Settings->ClipperScale,
				JoinType::Miter,
				EndType::Polygon);

			const Paths64 PO_big = Difference(PU, PSth, FillRule::NonZero);
			const double Sp = FMath::Max(1.0, (double)Settings->GridSpacing);
			Paths64 PO_in = PO;
			if (Settings->Inset > 0.0)
			{
				PO_in = InflatePaths(PO_in, -Settings->Inset * Settings->ClipperScale, JoinType::Miter, EndType::Polygon);
				if (PO_in.empty()) PO_in = PO;
				if (!PO_in.empty())
				{
					FVector2D OMin, OMax;
					ComputeBounds64(PO_in, Settings->ClipperScale, OMin, OMax); // 你若没有这个函数，我下面给替代版
					UE_LOG(LogTemp, Warning, TEXT("[Cantilever] Upper(Owner=%d Ring=%d) PO_in bbox=(%.1f,%.1f)-(%.1f,%.1f) Sp=%.1f Inset=%.1f"),
						Upper->Key.Owner, Upper->Key.RingId, OMin.X, OMin.Y, OMax.X, OMax.Y, Sp, Settings->Inset);
				}

			}

			if (SupportBaseZ <= -DBL_MAX / 2) continue;

			// IMPORTANT: support height should go to upper bottom from lower MIN Z (BottomZ), not from lower TopZ
			const double Height = FMath::Max(0.0, Upper->BottomZ - SupportBaseZ);
			if (Height <= 0.0) continue;

			const int32 Nx = FMath::Max(1, FMath::FloorToInt((Upper->BMax.X - Upper->BMin.X) / Sp));
			const int32 Ny = FMath::Max(1, FMath::FloorToInt((Upper->BMax.Y - Upper->BMin.Y) / Sp));

			for (int32 ix = 0; ix < Nx; ++ix)
			{
				for (int32 iy = 0; iy < Ny; ++iy)
				{
					const double X = Upper->BMin.X + (ix + 0.5) * Sp;
					const double Y = Upper->BMin.Y + (iy + 0.5) * Sp;

					const Point64 Q = ToP64(FVector2D(X, Y), Settings->ClipperScale);
					if (!IsInsideAny(Q, PO_in)) continue;
					const bool bBig = !PO_big.empty() && IsInsideAny(Q, PO_big);

					FPCGPoint S;
					// --- Compute yaw so that "ZeroYawDirection" corresponds to yaw=0 ---
					FRotator R = FRotator::ZeroRotator;

					if (Settings->bWriteRotation)
					{
						const FVector2D P2((double)X, (double)Y);
						const FVector2D ZeroDir = ZeroYawDirToVec2(Settings->ZeroYawDirection);

						// Prefer facing away from SupportUnion boundary (core). If not available, fallback.
						FVector2D Closest;
						FVector2D Dir2 = FVector2D::ZeroVector;

						if (!SupportUnion.empty() && ClosestPointOnPaths64(SupportUnion, Settings->ClipperScale, P2, Closest))
						{
							Dir2 = P2 - Closest; // outward (away from core boundary)
						}
						else if (!PO_in.empty() && ClosestPointOnPaths64(PO_in, Settings->ClipperScale, P2, Closest))
						{
							Dir2 = P2 - Closest; // fallback: away from overhang boundary point
						}
						else
						{
							Dir2 = P2 - FVector2D((double)Upper->Center.X, (double)Upper->Center.Y);
						}

						if (!Dir2.Normalize())
						{
							Dir2 = ZeroDir; // degenerate case
						}

						// angle(Target) - angle(ZeroYawDir)
						const double AngT = FMath::RadiansToDegrees(FMath::Atan2((double)Dir2.Y, (double)Dir2.X));
						const double Ang0 = FMath::RadiansToDegrees(FMath::Atan2((double)ZeroDir.Y, (double)ZeroDir.X));
						const float Yaw = (float)FRotator::NormalizeAxis((float)(AngT - Ang0 + Settings->YawOffsetDegrees));

						R = FRotator(0.0f, Yaw, 0.0f);

						// optional yaw attribute
						if (AttrYaw)
						{
							AttrYaw->SetValue(S.MetadataEntry, Yaw);
						}
					}

					S.Transform = FTransform(R, FVector((float)X, (float)Y, (float)SupportBaseZ));
					S.Density = 1.0f;
					S.MetadataEntry = OutMeta->AddEntry();
					S.Density = 1.0f;
					S.MetadataEntry = OutMeta->AddEntry();

					AttrSupportType->SetValue(S.MetadataEntry, bBig ? 1 : 0);
					AttrSupportH->SetValue(S.MetadataEntry, Height);
					AttrSymbol->SetValue(S.MetadataEntry, bBig ? Settings->LargeSupportSymbol : Settings->SmallSupportSymbol);

					OutPts.Add(S);
					++NumSampleHit;
				}
			}
		}

		FPCGTaggedData& OutTagged = Context->OutputData.TaggedData.Emplace_GetRef();
		OutTagged.Data = OutData;
		OutTagged.Pin = PCGPinConstants::DefaultOutputLabel;

		UE_LOG(LogTemp, Warning, TEXT("[Cantilever] Summary: InPts=%d Rings=%d Upper=%d HasSupport=%d HasPO=%d SampleHits=%d OutPts=%d"),
			InPts.Num(), RingList.Num(), NumUpper, NumHasSupport, NumHasPO, NumSampleHit, OutPts.Num());

		return true;
	}
};

TArray<FPCGPinProperties> UPCGGenerateCantileverSupportsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	FPCGPinProperties& In = Pins.Emplace_GetRef();
	In.Label = PCGPinConstants::DefaultInputLabel;
	In.AllowedTypes = EPCGDataType::Point;
	In.bAllowMultipleData = false;
	return Pins;
}

TArray<FPCGPinProperties> UPCGGenerateCantileverSupportsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	FPCGPinProperties& Out = Pins.Emplace_GetRef();
	Out.Label = PCGPinConstants::DefaultOutputLabel;
	Out.AllowedTypes = EPCGDataType::Point;
	Out.bAllowMultipleData = false;
	return Pins;
}

FPCGElementPtr UPCGGenerateCantileverSupportsSettings::CreateElement() const
{
	return StaticCastSharedPtr<IPCGElement>(
		MakeShared<FPCGGenerateCantileverSupportsElement, ESPMode::ThreadSafe>().ToSharedPtr()
	);
}

#undef LOCTEXT_NAMESPACE
