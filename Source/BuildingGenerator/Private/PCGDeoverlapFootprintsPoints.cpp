// PCGDeoverlapFootprintsPoints.cpp
#include "PCGDeoverlapFootprintsPoints.h"

#include "PCGContext.h"
#include "PCGElement.h"
#include "PCGPin.h"

#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"

// 关键：Union/Difference/Clipper64 等通常在这个头里
#include "clipper2/clipper.h"

namespace
{
	static const FName PinInName(TEXT("In"));
	static const FName PinOutName(TEXT("Out"));

	template<typename T>
	static const FPCGMetadataAttribute<T>* GetAttrTyped(const UPCGMetadata* Meta, const FName Name)
	{
		return Meta ? Meta->GetConstTypedAttribute<T>(Name) : nullptr;
	}

	// 兼容：同一个“逻辑整数属性”在 PCG 里可能是 int32 或 int64
	static bool ReadIntAny(const UPCGMetadata* Meta, PCGMetadataEntryKey Key, FName Name, int64& OutValue)
	{
		if (!Meta)
		{
			return false;
		}

		if (const FPCGMetadataAttribute<int64>* Attr64 = GetAttrTyped<int64>(Meta, Name))
		{
			OutValue = Attr64->GetValueFromItemKey(Key);
			return true;
		}

		if (const FPCGMetadataAttribute<int32>* Attr32 = GetAttrTyped<int32>(Meta, Name))
		{
			OutValue = (int64)Attr32->GetValueFromItemKey(Key);
			return true;
		}

		// 新增：double / float
		if (const FPCGMetadataAttribute<double>* AttrD = GetAttrTyped<double>(Meta, Name))
		{
			OutValue = FMath::RoundToInt64(AttrD->GetValueFromItemKey(Key));
			return true;
		}

		if (const FPCGMetadataAttribute<float>* AttrF = GetAttrTyped<float>(Meta, Name))
		{
			OutValue = FMath::RoundToInt64((double)AttrF->GetValueFromItemKey(Key));
			return true;
		}

		return false;
	}

	static bool ReadVectorAny(
		const UPCGMetadata* Meta,
		PCGMetadataEntryKey Key,
		const FPCGMetadataAttribute<FVector>* AttrVec,
		FVector& OutValue)
	{
		if (Meta && AttrVec)
		{
			OutValue = AttrVec->GetValueFromItemKey(Key);
			return true;
		}

		OutValue = FVector::ZeroVector;
		return false;
	}

	struct FOwnerPoly
	{
		int64 Owner = 0;
		double MinZ = 0.0;
		double MaxZ = 0.0;

		// Volume Z (preferred) derived from CenterPosition/GridExtents
		double BottomZ = 0.0;
		double TopZ = 0.0;
		bool bHasVolumeZ = false;

		FVector VolumeCenter = FVector::ZeroVector;
		FVector VolumeExtents = FVector::ZeroVector;
		bool bHasCenter = false;
		bool bHasExtents = false;

		struct FVert
		{
			FVector Pos = FVector::ZeroVector;
			int64 SortKey = 0; // PointIndex
		};
		TArray<FVert> Verts;

		FVector Extrude = FVector::ZeroVector;
		bool bHasExtrude = false;
	};


	static bool ZOverlaps(const FOwnerPoly& A, const FOwnerPoly& B, double MinOverlap)
	{
		// overlap thickness on Z
		const double Overlap = FMath::Min(A.MaxZ, B.MaxZ) - FMath::Max(A.MinZ, B.MinZ);

		// 注意：严格大于。Overlap == 0（只是贴着）=> false，不会互相 subtract
		return Overlap > MinOverlap;
	}

	static Clipper2Lib::Point64 ToP64(const FVector& P, double Scale)
	{
		const int64 X = static_cast<int64>(llround(P.X * Scale));
		const int64 Y = static_cast<int64>(llround(P.Y * Scale));
		return Clipper2Lib::Point64(X, Y);
	}

	static FVector FromP64(const Clipper2Lib::Point64& P, double InvScale, double Z)
	{
		return FVector(
			static_cast<double>(P.x) * InvScale,
			static_cast<double>(P.y) * InvScale,
			Z);
	}
}

// ---------------- UPCGDeoverlapFootprintsPointsSettings ----------------

UPCGDeoverlapFootprintsPointsSettings::UPCGDeoverlapFootprintsPointsSettings()
{
}

#if WITH_EDITOR
FName UPCGDeoverlapFootprintsPointsSettings::GetDefaultNodeName() const
{
	return TEXT("DeoverlapFootprintsPoints");
}

FText UPCGDeoverlapFootprintsPointsSettings::GetDefaultNodeTitle() const
{
	return NSLOCTEXT("PCGDeoverlapFootprintsPoints", "NodeTitle", "Deoverlap Footprints (Points)");
}

FText UPCGDeoverlapFootprintsPointsSettings::GetNodeTooltipText() const
{
	return NSLOCTEXT(
		"PCGDeoverlapFootprintsPoints",
		"NodeTooltip",
		"Subtract previously accepted 2D footprint polygons (built from point vertices) from subsequent ones, keeping output as points.");
}
#endif

TArray<FPCGPinProperties> UPCGDeoverlapFootprintsPointsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PinInName, EPCGDataType::Point);
	return Pins;
}

TArray<FPCGPinProperties> UPCGDeoverlapFootprintsPointsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PinOutName, EPCGDataType::Point);
	return Pins;
}

class FPCGDeoverlapFootprintsPointsElement final : public IPCGElement
{
public:
	virtual bool ExecuteInternal(FPCGContext* Context) const override
	{
		check(Context);

		const UPCGDeoverlapFootprintsPointsSettings* Settings =
			Context->GetInputSettings<UPCGDeoverlapFootprintsPointsSettings>();
		if (!Settings)
		{
			return true;
		}

		const double Scale = FMath::Max(1.0, Settings->IntScale);
		const double InvScale = 1.0 / Scale;
		const double ZTol = FMath::Max(0.0, Settings->ZOverlapTolerance);

		const Clipper2Lib::FillRule FillRule =
			Settings->bUseEvenOddFillRule ? Clipper2Lib::FillRule::EvenOdd : Clipper2Lib::FillRule::NonZero;

		const TArray<FPCGTaggedData> AllInputs = Context->InputData.GetInputs();

		// 只吃 In pin，避免有些情况下 GetInputs() 混进别的 pin 数据
		TArray<FPCGTaggedData> Inputs;
		Inputs.Reserve(AllInputs.Num());
		for (const FPCGTaggedData& T : AllInputs)
		{
			if (T.Pin == PinInName)
			{
				Inputs.Add(T);
			}
		}

		if (Inputs.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[DeoverlapFootprints] No inputs on pin '%s' (AllInputs=%d)."),
				*PinInName.ToString(), AllInputs.Num());
			return true;
		}

		TArray<const UPCGPointData*> InPointDatas;
		for (const FPCGTaggedData& Tagged : Inputs)
		{
			if (const UPCGPointData* PD = Cast<UPCGPointData>(Tagged.Data))
			{
				InPointDatas.Add(PD);
			}
		}

		if (InPointDatas.IsEmpty())
		{
			return true;
		}

		TMap<int64, FOwnerPoly> OwnerToPoly;

		for (const UPCGPointData* InPD : InPointDatas)
		{
			if (!InPD)
			{
				continue;
			}

			const UPCGMetadata* Meta = InPD->Metadata;
			const TArray<FPCGPoint>& Points = InPD->GetPoints();

			// 输入 Extrude（注意：读输入属性名，而不是输出属性名）
			const FPCGMetadataAttribute<FVector>* ExtrudeAttr = GetAttrTyped<FVector>(Meta, Settings->ExtrudeVectorAttribute);
			const FPCGMetadataAttribute<FVector>* CenterAttr = GetAttrTyped<FVector>(Meta, Settings->VolumeCenterAttribute);
			const FPCGMetadataAttribute<FVector>* ExtentsAttr = GetAttrTyped<FVector>(Meta, Settings->VolumeExtentsAttribute);


			for (int32 i = 0; i < Points.Num(); ++i)
			{
				const FPCGPoint& Pt = Points[i];
				const PCGMetadataEntryKey Key = Pt.MetadataEntry;

				int64 Owner = 0;
				if (!ReadIntAny(Meta, Key, Settings->OwnerIndexAttribute, Owner))
				{
					continue;
				}

				int64 SortKey = (int64)i; // 默认用点序
				(void)ReadIntAny(Meta, Key, Settings->PointIndexAttribute, SortKey);

				FOwnerPoly& Poly = OwnerToPoly.FindOrAdd(Owner);
				Poly.Owner = Owner;

				FOwnerPoly::FVert V;
				V.Pos = Pt.Transform.GetLocation();
				V.SortKey = SortKey;
				Poly.Verts.Add(V);
				// Volume Center/Extents (duplicated per-vertex in your data)
				// Volume Center/Extents (duplicated per-vertex in your data) —— 下游 Support 必须用
				if (!Poly.bHasCenter)
				{
					FVector C = FVector::ZeroVector;
					if (ReadVectorAny(Meta, Key, CenterAttr, C))
					{
						Poly.VolumeCenter = C;
						Poly.bHasCenter = true;
					}
				}

				if (!Poly.bHasExtents)
				{
					FVector E = FVector::ZeroVector;
					if (ReadVectorAny(Meta, Key, ExtentsAttr, E))
					{
						Poly.VolumeExtents = E;
						Poly.bHasExtents = true;
					}
				}


				// Extrude
				if (!Poly.bHasExtrude)
				{
					FVector Extrude = FVector::ZeroVector;
					if (ReadVectorAny(Meta, Key, ExtrudeAttr, Extrude))
					{
						Poly.Extrude = Extrude;
						Poly.bHasExtrude = true;
					}
				}

				// Z 范围（用点位 + extrude.Z 估计一个范围）
				// Z range gating for de-overlap:
				// Prefer VolumeCenter/VolumeExtents (Center.Z±Extents.Z) if available; otherwise fallback to pointZ + Extrude.Z
				double BaseZ = V.Pos.Z;
				double TopZ = V.Pos.Z + (Poly.bHasExtrude ? Poly.Extrude.Z : 0.0);

				if (Settings->bUseVolumeZForOverlap && Poly.bHasCenter && Poly.bHasExtents)
				{
					BaseZ = (double)Poly.VolumeCenter.Z - (double)Poly.VolumeExtents.Z;
					TopZ = (double)Poly.VolumeCenter.Z + (double)Poly.VolumeExtents.Z;
					Poly.BottomZ = BaseZ;
					Poly.TopZ = TopZ;
					Poly.bHasVolumeZ = true;
				}


				if (Poly.Verts.Num() == 1)
				{
					Poly.MinZ = FMath::Min(BaseZ, TopZ);
					Poly.MaxZ = FMath::Max(BaseZ, TopZ);
				}
				else
				{
					Poly.MinZ = FMath::Min(Poly.MinZ, FMath::Min(BaseZ, TopZ));
					Poly.MaxZ = FMath::Max(Poly.MaxZ, FMath::Max(BaseZ, TopZ));
				}
			}
		}

		if (OwnerToPoly.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[DeoverlapFootprints] OwnerToPoly is empty. Likely missing/typed attributes. OwnerAttr='%s' PointIndexAttr='%s' RingAttr='%s' ExtrudeAttr='%s'"),
				*Settings->OwnerIndexAttribute.ToString(),
				*Settings->PointIndexAttribute.ToString(),
				*Settings->RingIdAttribute.ToString(),
				*Settings->ExtrudeVectorAttribute.ToString());

			// 兜底输出：至少把输入透传到 Out，方便你在下游检查属性到底有没有
			FPCGTaggedData& OutTagged = Context->OutputData.TaggedData.Emplace_GetRef();
			OutTagged.Pin = PinOutName;
			OutTagged.Data = const_cast<UPCGPointData*>(InPointDatas[0]);
			return true;
		}


		TArray<int64> Owners;
		OwnerToPoly.GetKeys(Owners);
		Owners.Sort();

		struct FAccepted
		{
			double MinZ = 0.0;
			double MaxZ = 0.0;
			Clipper2Lib::Paths64 Paths;
		};
		TArray<FAccepted> Accepted;

		UPCGPointData* OutPD = NewObject<UPCGPointData>();
		OutPD->InitializeFromData(InPointDatas[0]);

		TArray<FPCGPoint> OutPoints;
		OutPoints.Reserve(1024);

		UPCGMetadata* OutMeta = OutPD->MutableMetadata();
		check(OutMeta);

		FPCGMetadataAttribute<int32>* OutOwnerAttr =
			OutMeta->FindOrCreateAttribute<int32>(Settings->OutOwnerIndexAttribute, 0, false, true);

		FPCGMetadataAttribute<int32>* OutRingAttr =
			OutMeta->FindOrCreateAttribute<int32>(Settings->OutRingIdAttribute, 0, false, true);

		FPCGMetadataAttribute<int32>* OutVtxAttr =
			OutMeta->FindOrCreateAttribute<int32>(Settings->OutVertexIndexAttribute, 0, false, true);

		FPCGMetadataAttribute<FVector>* OutExtrudeAttr =
			OutMeta->FindOrCreateAttribute<FVector>(Settings->OutExtrudeVectorAttribute, FVector::ZeroVector, true, true);
		FPCGMetadataAttribute<FVector>* OutCenterAttr =
			OutMeta->FindOrCreateAttribute<FVector>(Settings->OutVolumeCenterAttribute, FVector::ZeroVector, true, true);
		FPCGMetadataAttribute<FVector>* OutExtentsAttr =
			OutMeta->FindOrCreateAttribute<FVector>(Settings->OutVolumeExtentsAttribute, FVector::ZeroVector, true, true);

		int32 RingCounter = 0;

		for (int64 Owner : Owners)
		{
			FOwnerPoly* Poly = OwnerToPoly.Find(Owner);
			if (!Poly || Poly->Verts.Num() < 3)
			{
				continue;
			}

			Poly->Verts.Sort([](const FOwnerPoly::FVert& A, const FOwnerPoly::FVert& B)
				{
					return A.SortKey < B.SortKey;
				});

			Clipper2Lib::Path64 Subject;
			Subject.reserve(Poly->Verts.Num());
			for (const auto& V : Poly->Verts)
			{
				Subject.push_back(ToP64(V.Pos, Scale));
			}

			if (Settings->bForceCloseLoop && Subject.size() >= 3)
			{
				if (Subject.front() != Subject.back())
				{
					Subject.push_back(Subject.front());
				}
			}

			Clipper2Lib::Paths64 SubjectPaths;
			SubjectPaths.push_back(Subject);

			Clipper2Lib::Paths64 Subtrahend;
			for (const FAccepted& A : Accepted)
			{
				FOwnerPoly FakeA;
				FakeA.MinZ = A.MinZ;
				FakeA.MaxZ = A.MaxZ;

				if (ZOverlaps(*Poly, FakeA, ZTol))
				{
					Subtrahend.insert(Subtrahend.end(), A.Paths.begin(), A.Paths.end());
				}
			}

			Clipper2Lib::Paths64 Result;
			if (Subtrahend.empty())
			{
				Result = SubjectPaths;
			}
			else
			{
				Clipper2Lib::Paths64 SubU = Clipper2Lib::Union(Subtrahend, FillRule);
				Result = Clipper2Lib::Difference(SubjectPaths, SubU, FillRule);
			}

			if (Result.empty())
			{
				continue;
			}

			const double OutZ = (Settings->bWritePointZAsBottomZ && Poly->bHasVolumeZ) ? Poly->BottomZ : Poly->Verts[0].Pos.Z;
			for (const Clipper2Lib::Path64& Path : Result)
			{
				if (Path.size() < 3)
				{
					continue;
				}

				const int32 ThisRing = RingCounter++;

				for (int32 i = 0; i < static_cast<int32>(Path.size()); ++i)
				{
					FPCGPoint OutPt;
					OutPt.Transform = FTransform(FromP64(Path[i], InvScale, OutZ));
					OutPt.Density = 1.0f;

					const PCGMetadataEntryKey NewKey = OutMeta->AddEntry();
					OutPt.MetadataEntry = NewKey;

					if (OutOwnerAttr) { OutOwnerAttr->SetValue(NewKey, static_cast<int32>(Owner)); }
					if (OutRingAttr) { OutRingAttr->SetValue(NewKey, ThisRing); }
					if (OutVtxAttr) { OutVtxAttr->SetValue(NewKey, i); }
					if (OutExtrudeAttr) { OutExtrudeAttr->SetValue(NewKey, Poly->bHasExtrude ? Poly->Extrude : FVector::ZeroVector); }
					if (OutCenterAttr) { OutCenterAttr->SetValue(NewKey, Poly->bHasCenter ? Poly->VolumeCenter : FVector::ZeroVector); }
					if (OutExtentsAttr) { OutExtentsAttr->SetValue(NewKey, Poly->bHasExtents ? Poly->VolumeExtents : FVector::ZeroVector); }

					OutPoints.Add(OutPt);
				}
			}

			FAccepted NewAcc;
			NewAcc.MinZ = Poly->MinZ;
			NewAcc.MaxZ = Poly->MaxZ;
			NewAcc.Paths = Result;
			Accepted.Add(MoveTemp(NewAcc));
		}

		OutPD->SetPoints(MoveTemp(OutPoints));

		FPCGTaggedData& OutTagged = Context->OutputData.TaggedData.Emplace_GetRef();
		OutTagged.Pin = PinOutName;
		OutTagged.Data = OutPD;

		return true;
	}
};

FPCGElementPtr UPCGDeoverlapFootprintsPointsSettings::CreateElement() const
{
	return MakeShared<FPCGDeoverlapFootprintsPointsElement>();
}

