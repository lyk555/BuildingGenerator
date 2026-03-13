#include "PCGFilterOccludedRoofPoints.h"

#include "PCGContext.h"
#include "PCGPin.h"

#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"

#include "Math/Box2D.h"
#include "Templates/TypeHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogPCGFilterOccludedRoofPoints, Log, All);

#define ROOFDBG(Verbosity, Format, ...) \
	UE_LOG(LogPCGFilterOccludedRoofPoints, Verbosity, TEXT("[FilterOccludedRoofPoints] " Format), ##__VA_ARGS__)

namespace PCGFilterOccludedRoofPoints
{
	struct FRingKey
	{
		int32 Owner = 0;
		int32 Ring = 0;

		friend bool operator==(const FRingKey& A, const FRingKey& B)
		{
			return A.Owner == B.Owner && A.Ring == B.Ring;
		}
	};

	inline uint32 GetTypeHash(const FRingKey& K)
	{
		return HashCombine(::GetTypeHash(K.Owner), ::GetTypeHash(K.Ring));
	}

	// ---- Robust attribute readers ----
	static bool GetIntAttrRobust(const UPCGMetadata* Meta, FName Name, int64 Key, int32& OutVal)
	{
		if (!Meta || Name.IsNone()) return false;

		if (const FPCGMetadataAttribute<int32>* A32 = Meta->GetConstTypedAttribute<int32>(Name))
		{
			OutVal = A32->GetValueFromItemKey(Key);
			return true;
		}
		if (const FPCGMetadataAttribute<int64>* A64 = Meta->GetConstTypedAttribute<int64>(Name))
		{
			const int64 V = A64->GetValueFromItemKey(Key);
			OutVal = (int32)V;
			return true;
		}
		if (const FPCGMetadataAttribute<uint32>* AU32 = Meta->GetConstTypedAttribute<uint32>(Name))
		{
			OutVal = (int32)AU32->GetValueFromItemKey(Key);
			return true;
		}
		if (const FPCGMetadataAttribute<uint64>* AU64 = Meta->GetConstTypedAttribute<uint64>(Name))
		{
			OutVal = (int32)AU64->GetValueFromItemKey(Key);
			return true;
		}
		// 兼容：OwnerIndex 可能被写成 float/double（PCG 运算链路很常见）
		if (const FPCGMetadataAttribute<float>* AF = Meta->GetConstTypedAttribute<float>(Name))
		{
			OutVal = (int32)FMath::RoundToInt(AF->GetValueFromItemKey(Key));
			return true;
		}
		if (const FPCGMetadataAttribute<double>* AD = Meta->GetConstTypedAttribute<double>(Name))
		{
			OutVal = (int32)FMath::RoundToInt(AD->GetValueFromItemKey(Key));
			return true;
		}

		return false;
	}

	static bool GetVecAttrRobust(const UPCGMetadata* Meta, FName Name, int64 Key, FVector& OutVal)
	{
		if (!Meta || Name.IsNone()) return false;

		// PCG MetadataTraits 支持 FVector (double)，不支持 FVector3f :contentReference[oaicite:1]{index=1}
		if (const FPCGMetadataAttribute<FVector>* AV = Meta->GetConstTypedAttribute<FVector>(Name))
		{
			OutVal = AV->GetValueFromItemKey(Key);
			return true;
		}

		// 可选：如果你有些属性实际是 FVector2D（例如只关心 XY）
		if (const FPCGMetadataAttribute<FVector2D>* AV2 = Meta->GetConstTypedAttribute<FVector2D>(Name))
		{
			const FVector2D V = AV2->GetValueFromItemKey(Key);
			OutVal = FVector(V.X, V.Y, 0.0);
			return true;
		}

		return false;
	}
	static bool GetDoubleAttrRobust(const UPCGMetadata* Meta, FName Name, int64 Key, double& OutVal)
	{
		if (!Meta || Name.IsNone()) return false;

		if (const FPCGMetadataAttribute<double>* AD = Meta->GetConstTypedAttribute<double>(Name))
		{
			OutVal = AD->GetValueFromItemKey(Key);
			return true;
		}
		if (const FPCGMetadataAttribute<float>* AF = Meta->GetConstTypedAttribute<float>(Name))
		{
			OutVal = (double)AF->GetValueFromItemKey(Key);
			return true;
		}
		if (const FPCGMetadataAttribute<int32>* AI = Meta->GetConstTypedAttribute<int32>(Name))
		{
			OutVal = (double)AI->GetValueFromItemKey(Key);
			return true;
		}
		if (const FPCGMetadataAttribute<int64>* A64 = Meta->GetConstTypedAttribute<int64>(Name))
		{
			OutVal = (double)A64->GetValueFromItemKey(Key);
			return true;
		}

		return false;
	}


	static void RemoveNearlyDuplicateXY(TArray<FVector2D>& InOut, double Eps)
	{
		if (InOut.Num() <= 1) return;
		const double EpsSq = Eps * Eps;

		TArray<FVector2D> Out;
		Out.Reserve(InOut.Num());
		for (const FVector2D& P : InOut)
		{
			bool bDup = false;
			for (const FVector2D& Q : Out)
			{
				if ((P - Q).SizeSquared() <= EpsSq)
				{
					bDup = true;
					break;
				}
			}
			if (!bDup) Out.Add(P);
		}
		InOut = MoveTemp(Out);
	}

	static bool BuildPolygonByAngleSort(const TArray<FVector2D>& InPts, TArray<FVector2D>& OutPoly, FBox2D& OutBounds)
	{
		OutPoly.Reset();
		OutBounds.Init();

		if (InPts.Num() < 3) return false;

		FVector2D C(0, 0);
		for (const FVector2D& P : InPts) C += P;
		C /= (double)InPts.Num();

		struct FAngPt { double A; FVector2D P; };
		TArray<FAngPt> Tmp;
		Tmp.Reserve(InPts.Num());
		for (const FVector2D& P : InPts)
		{
			const FVector2D D = P - C;
			Tmp.Add({ FMath::Atan2(D.Y, D.X), P });
		}
		Tmp.Sort([](const FAngPt& L, const FAngPt& R) { return L.A < R.A; });

		OutPoly.Reserve(Tmp.Num());
		for (const FAngPt& AP : Tmp)
		{
			OutPoly.Add(AP.P);
			OutBounds += AP.P;
		}

		// 去重后至少要有3个点
		TArray<FVector2D> Dedup = OutPoly;
		RemoveNearlyDuplicateXY(Dedup, 0.01);
		return Dedup.Num() >= 3;
	}

	static double PointSegDistSq(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const double Den = AB.SizeSquared();
		if (Den <= KINDA_SMALL_NUMBER) return (P - A).SizeSquared();
		const double T = FMath::Clamp(((P - A) | AB) / Den, 0.0, 1.0);
		const FVector2D Q = A + AB * T;
		return (P - Q).SizeSquared();
	}

	static bool PointInPoly2D(const FVector2D& P, const TArray<FVector2D>& Poly, double XYTol)
	{
		const int32 N = Poly.Num();
		if (N < 3) return false;

		const double TolSq = XYTol * XYTol;

		// 边界容差：点到边距离很近也算 inside
		for (int32 i = 0; i < N; ++i)
		{
			const FVector2D A = Poly[i];
			const FVector2D B = Poly[(i + 1) % N];
			if (PointSegDistSq(P, A, B) <= TolSq)
			{
				return true;
			}
		}

		// 射线法
		bool bInside = false;
		for (int32 i = 0, j = N - 1; i < N; j = i++)
		{
			const FVector2D Pi = Poly[i];
			const FVector2D Pj = Poly[j];

			const bool bIntersect = ((Pi.Y > P.Y) != (Pj.Y > P.Y)) &&
				(P.X < (Pj.X - Pi.X) * (P.Y - Pi.Y) / (Pj.Y - Pi.Y + 1e-12) + Pi.X);

			if (bIntersect) bInside = !bInside;
		}
		return bInside;
	}

	static bool PointInAABB2D(const FVector2D& P, const FVector2D& Center, const FVector2D& HalfExt, double XYTol)
	{
		return (P.X >= Center.X - HalfExt.X - XYTol) && (P.X <= Center.X + HalfExt.X + XYTol) &&
			(P.Y >= Center.Y - HalfExt.Y - XYTol) && (P.Y <= Center.Y + HalfExt.Y + XYTol);
	}

	static FVector2D Rotate2D(const FVector2D& V, double Rad)
	{
		const double C = FMath::Cos(Rad);
		const double S = FMath::Sin(Rad);
		return FVector2D(V.X * C - V.Y * S, V.X * S + V.Y * C);
	}

	static bool PointInOBB2D(const FVector2D& P, const FVector2D& Center, const FVector2D& HalfExt, double YawDeg, double XYTol)
	{
		const double Rad = FMath::DegreesToRadians(YawDeg);

		// 世界点 -> 盒体局部：平移到 Center，再反向旋转 -Yaw
		const FVector2D Local = Rotate2D(P - Center, -Rad);

		return (FMath::Abs(Local.X) <= HalfExt.X + XYTol) &&
			(FMath::Abs(Local.Y) <= HalfExt.Y + XYTol);
	}

	struct FFloorGroup
	{
		int32 Owner = 0;
		int32 Ring = 0;
		bool bOwnerValid = false;
		TArray<FVector2D> XY;
		double FloorZSum = 0.0;
		int32  ZCount = 0;

		FBox2D XYBounds;
		bool   bBoundsInit = false;

		FVector GridExtents = FVector::ZeroVector;
		bool    bHasGridExtents = false;

		double  YawDeg = 0.0;
		bool    bHasYaw = false;

		TArray<FVector2D> Poly;
		FBox2D PolyBounds;
		bool   bPolyValid = false;

		double GetFloorZ() const { return (ZCount > 0) ? (FloorZSum / (double)ZCount) : 0.0; }

		FVector2D GetCenterXY() const
		{
			if (!bBoundsInit) return FVector2D::ZeroVector;
			return (XYBounds.Min + XYBounds.Max) * 0.5;
		}

		FVector2D GetDerivedHalfExtentsXY() const
		{
			if (!bBoundsInit) return FVector2D::ZeroVector;
			return (XYBounds.Max - XYBounds.Min) * 0.5;
		}
	};
}

using namespace PCGFilterOccludedRoofPoints;

FPCGElementPtr UPCGFilterOccludedRoofPointsSettings::CreateElement() const
{
	return MakeShareable(new FPCGFilterOccludedRoofPointsElement());
}

TArray<FPCGPinProperties> UPCGFilterOccludedRoofPointsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	{
		FPCGPinProperties P;
		P.Label = RoofsPinLabel;
		P.AllowedTypes = EPCGDataType::Point;
		P.SetAllowMultipleConnections(true); // UE5.5+ 正确 API :contentReference[oaicite:2]{index=2}
		Pins.Add(P);
	}

	{
		FPCGPinProperties P;
		P.Label = FloorsPinLabel;
		P.AllowedTypes = EPCGDataType::Point;
		P.SetAllowMultipleConnections(true); // UE5.5+ 正确 API :contentReference[oaicite:3]{index=3}
		Pins.Add(P);
	}

	return Pins;
}

TArray<FPCGPinProperties> UPCGFilterOccludedRoofPointsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	{
		FPCGPinProperties P;
		P.Label = OutputPinLabel;
		P.AllowedTypes = EPCGDataType::Point;
		Pins.Add(P);
	}

	return Pins;
}

FPCGContext* FPCGFilterOccludedRoofPointsElement::Initialize(
	const FPCGDataCollection& InputData,
	TWeakObjectPtr<UPCGComponent> SourceComponent,
	const UPCGNode* Node)
{
	FPCGContext* Context = new FPCGContext();
	Context->InputData = InputData;
	Context->SourceComponent = SourceComponent;
	Context->Node = Node;
	return Context;
}

bool FPCGFilterOccludedRoofPointsElement::ExecuteInternal(FPCGContext* Context) const
{
	check(Context);

	const UPCGFilterOccludedRoofPointsSettings* Settings = Context->GetInputSettings<UPCGFilterOccludedRoofPointsSettings>();
	if (!Settings) return true;

	ROOFDBG(Log, TEXT("Settings: SurfaceTol=%.3f XYTol=%.3f TryPoly=%d FallbackAABB=%d GridHalf=%d UseRingId=%d OwnerAttr=%s ExtAttr=%s"),
		Settings->SurfaceTol,
		Settings->XYTol,
		Settings->bTryBuildFloorPolygon ? 1 : 0,
		Settings->bFallbackToExtentsAABB ? 1 : 0,
		Settings->bGridExtentsIsHalfSize ? 1 : 0,
		Settings->bUseRingId ? 1 : 0,
		*Settings->OwnerIndexAttribute.ToString(),
		*Settings->GridExtentsAttribute.ToString());

	// ---- 1) Floors: group by (OwnerIndex, RingId optional) ----
	TMap<FRingKey, FFloorGroup> FloorGroups;

	int64 FloorTotalPts = 0;
	int64 FloorOwnerReadFail = 0;
	int64 FloorExtReadFail = 0;
	TSet<int32> FloorOwners;

	const TArray<FPCGTaggedData> FloorInputs = Context->InputData.GetInputsByPin(Settings->FloorsPinLabel);
	for (const FPCGTaggedData& Tagged : FloorInputs)
	{
		const UPCGPointData* FloorData = Cast<UPCGPointData>(Tagged.Data);
		if (!FloorData) continue;

		const UPCGMetadata* Meta = FloorData->Metadata;
		const TArray<FPCGPoint>& Pts = FloorData->GetPoints();

		for (const FPCGPoint& Pt : Pts)
		{
			const int64 Key = Pt.MetadataEntry;

			int32 Owner = 0;
			const bool bOwnerOk = GetIntAttrRobust(Meta, Settings->OwnerIndexAttribute, Key, Owner);
			if (!bOwnerOk)
			{
				++FloorOwnerReadFail;
				// 不要强行置 0（会导致后面“同Owner跳过”把所有floor跳掉）
				Owner = 0;
			}

			int32 Ring = 0;
			if (Settings->bUseRingId && !Settings->RingIdAttribute.IsNone())
			{
				GetIntAttrRobust(Meta, Settings->RingIdAttribute, Key, Ring);
			}

			FRingKey K{ Owner, Ring };
			FFloorGroup& G = FloorGroups.FindOrAdd(K);
			G.bOwnerValid = G.bOwnerValid || bOwnerOk;
			G.Owner = Owner;
			G.Ring = Ring;

			const FVector L = Pt.Transform.GetLocation();
			const FVector2D Pxy(L.X, L.Y);

			++FloorTotalPts;
			FloorOwners.Add(Owner);

			G.XY.Add(Pxy);
			G.FloorZSum += L.Z;
			G.ZCount += 1;

			if (!G.bBoundsInit)
			{
				G.XYBounds = FBox2D(Pxy, Pxy);
				G.bBoundsInit = true;
			}
			else
			{
				G.XYBounds += Pxy;
			}

			if (!G.bHasGridExtents)
			{
				FVector Ext;
				if (GetVecAttrRobust(Meta, Settings->GridExtentsAttribute, Key, Ext))
				{
					G.GridExtents = Ext;
					G.bHasGridExtents = true;
				}
				else
				{
					++FloorExtReadFail;
				}
			}
			if (!G.bHasYaw)
			{
				double Yaw = 0.0;
				if (GetDoubleAttrRobust(Meta, Settings->RotationAttribute, Key, Yaw))
				{
					G.YawDeg = Yaw;
					G.bHasYaw = true;
				}
			}

		}
	}

	ROOFDBG(Log, TEXT("Floors: Inputs=%d TotalPts=%lld Groups=%d UniqueOwners=%d OwnerReadFail=%lld ExtReadFail=%lld"),
		FloorInputs.Num(),
		(long long)FloorTotalPts,
		FloorGroups.Num(),
		FloorOwners.Num(),
		(long long)FloorOwnerReadFail,
		(long long)FloorExtReadFail);

	// ---- 1.5) Build floor polygon (optional) ----
	if (Settings->bTryBuildFloorPolygon)
	{
		for (auto& It : FloorGroups)
		{
			FFloorGroup& G = It.Value;
			if (G.XY.Num() < 3) continue;

			TArray<FVector2D> Pts = G.XY;
			RemoveNearlyDuplicateXY(Pts, 0.01);

			FBox2D Bounds;
			if (BuildPolygonByAngleSort(Pts, G.Poly, Bounds))
			{
				G.PolyBounds = Bounds;
				G.bPolyValid = true;
			}
		}
	}

	if (Settings->bDebugLog)
	{
		int32 Printed = 0;
		for (const auto& It : FloorGroups)
		{
			if (Printed >= Settings->DebugMaxFloorGroupsToLog) break;
			const FFloorGroup& G = It.Value;

			const FVector2D C = G.GetCenterXY();
			const FVector2D HalfDer = G.GetDerivedHalfExtentsXY();

			UE_LOG(LogPCGFilterOccludedRoofPoints, Log,
				TEXT("[FilterOccludedRoofPoints] FloorGroup[%d]: Owner=%d Ring=%d Pts=%d Z=%.3f Center=(%.1f,%.1f) HalfDer=(%.1f,%.1f) GridExt=(%.1f,%.1f,%.1f) HasGrid=%d PolyValid=%d"),
				Printed, G.Owner, G.Ring, G.XY.Num(),
				G.GetFloorZ(),
				C.X, C.Y,
				HalfDer.X, HalfDer.Y,
				G.GridExtents.X, G.GridExtents.Y, G.GridExtents.Z,
				G.bHasGridExtents ? 1 : 0,
				G.bPolyValid ? 1 : 0);

			++Printed;
		}
	}

	// ---- 2) Roofs: filter occluded points ----
	const TArray<FPCGTaggedData> RoofInputs = Context->InputData.GetInputsByPin(Settings->RoofsPinLabel);

	int64 RoofTotalPts = 0;
	int64 RoofOwnerReadFail = 0;
	int64 RoofOccluded = 0;
	int64 RoofNoZCandidate = 0;
	int64 RoofHasZButNoXY = 0;

	double GlobalMinAbsZDiff = TNumericLimits<double>::Max();
	int32 DebugSamplePrinted = 0;

	for (const FPCGTaggedData& Tagged : RoofInputs)
	{
		const UPCGPointData* RoofData = Cast<UPCGPointData>(Tagged.Data);
		if (!RoofData) continue;

		const UPCGMetadata* InMeta = RoofData->Metadata;
		const TArray<FPCGPoint>& InPts = RoofData->GetPoints();

		UPCGPointData* OutData = FPCGContext::NewObject_AnyThread<UPCGPointData>(Context);
		// 复制元数据结构(Parent/Attributes) + 复制空间数据，保证 AddEntry(parentKey) 能正确拷贝值 :contentReference[oaicite:4]{index=4}
		OutData->InitializeFromData(RoofData, nullptr, true, true);

		UPCGMetadata* OutMeta = OutData->MutableMetadata();
		TArray<FPCGPoint>& OutPts = OutData->GetMutablePoints();
		OutPts.Reserve(InPts.Num());

		for (const FPCGPoint& Pt : InPts)
		{
			const int64 Key = Pt.MetadataEntry;

			int32 OwnerR = 0;
			const bool bRoofOwnerOk = GetIntAttrRobust(InMeta, Settings->OwnerIndexAttribute, Key, OwnerR);
			if (!bRoofOwnerOk)
			{
				++RoofOwnerReadFail;
				OwnerR = 0;
			}

			const FVector L = Pt.Transform.GetLocation();
			const double RoofZ = L.Z;
			const FVector2D RoofXY(L.X, L.Y);

			++RoofTotalPts;

			bool bOccluded = false;
			bool bAnyZCandidate = false;
			bool bAnyXYCandidate = false;

			for (const auto& FGIt : FloorGroups)
			{
				const FFloorGroup& F = FGIt.Value;

				// 只有当 roof owner 和 floor owner 都是“有效读到”的情况下才跳过同 owner
				if (bRoofOwnerOk && F.bOwnerValid && (F.Owner == OwnerR))
				{
					continue;
				}
				if (F.ZCount <= 0) continue;

				const double AbsZ = FMath::Abs(RoofZ - F.GetFloorZ());
				GlobalMinAbsZDiff = FMath::Min(GlobalMinAbsZDiff, AbsZ);

				if (AbsZ > Settings->SurfaceTol) continue;
				bAnyZCandidate = true;

				// 优先 polygon
				if (F.bPolyValid)
				{
					FBox2D B = F.PolyBounds;
					B.Min -= FVector2D(Settings->XYTol, Settings->XYTol);
					B.Max += FVector2D(Settings->XYTol, Settings->XYTol);

					if (!B.IsInside(RoofXY)) continue;

					if (PointInPoly2D(RoofXY, F.Poly, Settings->XYTol))
					{
						bAnyXYCandidate = true;
						bOccluded = true;
						break;
					}
				}
				else if (Settings->bFallbackToExtentsAABB)
				{
					FVector2D HalfExt = FVector2D::ZeroVector;

					if (F.bHasGridExtents)
					{
						HalfExt = FVector2D(F.GridExtents.X, F.GridExtents.Y);
						if (!Settings->bGridExtentsIsHalfSize)
						{
							HalfExt *= 0.5; // full size -> half extent
						}
					}
					else
					{
						HalfExt = F.GetDerivedHalfExtentsXY();
					}

					const FVector2D Center = F.GetCenterXY();

					const bool bInside =
						(F.bHasYaw ? PointInOBB2D(RoofXY, Center, HalfExt, F.YawDeg, Settings->XYTol)
							: PointInAABB2D(RoofXY, Center, HalfExt, Settings->XYTol));

					if (bInside)

					{
						bAnyXYCandidate = true;
						bOccluded = true;
						break;
					}
				}
			}

			if (bOccluded)
			{
				++RoofOccluded;
			}
			else
			{
				if (!bAnyZCandidate) ++RoofNoZCandidate;
				else if (!bAnyXYCandidate)
				{
					++RoofHasZButNoXY;

					if (Settings->bDebugLog && DebugSamplePrinted < Settings->DebugMaxRoofSamplesToLog)
					{
						UE_LOG(LogPCGFilterOccludedRoofPoints, Log,
							TEXT("[FilterOccludedRoofPoints] RoofSample(HasZ_NoXY): Owner=%d Pos=(%.1f,%.1f,%.1f) SurfaceTol=%.3f XYTol=%.3f"),
							OwnerR, L.X, L.Y, L.Z, Settings->SurfaceTol, Settings->XYTol);
						++DebugSamplePrinted;
					}
				}

				FPCGPoint NewPt = Pt;
				if (OutMeta)
				{
					// 按 parentKey 拷贝该点的所有属性值 :contentReference[oaicite:5]{index=5}
					NewPt.MetadataEntry = OutMeta->AddEntry(Pt.MetadataEntry);
				}
				OutPts.Add(NewPt);
			}
		}

		FPCGTaggedData& OutTagged = Context->OutputData.TaggedData.Emplace_GetRef();
		OutTagged.Data = OutData;
		OutTagged.Pin = Settings->OutputPinLabel;
		OutTagged.Tags = Tagged.Tags;
	}

	ROOFDBG(Log, TEXT("Roofs: TotalPts=%lld Occluded=%lld Kept=%lld NoZ=%lld HasZNoXY=%lld OwnerReadFail=%lld MinAbsZDiff=%.3f"),
		(long long)RoofTotalPts,
		(long long)RoofOccluded,
		(long long)(RoofTotalPts - RoofOccluded),
		(long long)RoofNoZCandidate,
		(long long)RoofHasZButNoXY,
		(long long)RoofOwnerReadFail,
		(GlobalMinAbsZDiff == TNumericLimits<double>::Max() ? -1.0 : GlobalMinAbsZDiff));
	
	if (FloorTotalPts > 0 && FloorOwnerReadFail == FloorTotalPts)
	{
		UE_LOG(LogPCGFilterOccludedRoofPoints, Warning,
			TEXT("[FilterOccludedRoofPoints] OwnerIndex ('%s') is unreadable on ALL Floor points. "
				"Likely not present on @point domain or stored as a different numeric type."),
			*Settings->OwnerIndexAttribute.ToString());
	}

	return true;
}
