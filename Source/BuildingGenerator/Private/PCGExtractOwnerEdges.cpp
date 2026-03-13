#include "PCGExtractOwnerEdges.h"

#include "PCGContext.h"
#include "PCGPin.h"
#include "PCGData.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"

#define LOCTEXT_NAMESPACE "PCGExtractOwnerEdges"

namespace PCGExtractOwnerEdgesConstants
{
	static const FName InputLabel = TEXT("In");
	static const FName OutPosXLabel = TEXT("+XEdge");
	static const FName OutNegXLabel = TEXT("-XEdge");
	static const FName OutPosYLabel = TEXT("+YEdge");
	static const FName OutNegYLabel = TEXT("-YEdge");
}

UPCGExtractOwnerEdgesSettings::UPCGExtractOwnerEdgesSettings()
{
}

FName UPCGExtractOwnerEdgesSettings::GetDefaultNodeName() const
{
	return TEXT("ExtractOwnerEdges");
}

FText UPCGExtractOwnerEdgesSettings::GetDefaultNodeTitle() const
{
	return LOCTEXT("NodeTitle", "Extract Owner Edges");
}

EPCGSettingsType UPCGExtractOwnerEdgesSettings::GetType() const
{
	return EPCGSettingsType::Spatial;
}

TArray<FPCGPinProperties> UPCGExtractOwnerEdgesSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGExtractOwnerEdgesConstants::InputLabel, EPCGDataType::Point);
	return Pins;
}

TArray<FPCGPinProperties> UPCGExtractOwnerEdgesSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGExtractOwnerEdgesConstants::OutPosXLabel, EPCGDataType::Point);
	Pins.Emplace(PCGExtractOwnerEdgesConstants::OutNegXLabel, EPCGDataType::Point);
	Pins.Emplace(PCGExtractOwnerEdgesConstants::OutPosYLabel, EPCGDataType::Point);
	Pins.Emplace(PCGExtractOwnerEdgesConstants::OutNegYLabel, EPCGDataType::Point);
	return Pins;
}

FPCGElementPtr UPCGExtractOwnerEdgesSettings::CreateElement() const
{
	return MakeShared<FPCGExtractOwnerEdgesElement>();
}

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

namespace
{
	template<typename T>
	const FPCGMetadataAttribute<T>* GetConstTypedAttr(const UPCGMetadata* Metadata, const FName AttrName)
	{
		if (!Metadata || AttrName.IsNone())
		{
			return nullptr;
		}

		return Metadata->GetConstTypedAttribute<T>(AttrName);
	}

	template<typename T>
	static FPCGMetadataAttribute<T>* GetOrCreateAttr(UPCGMetadata* MD, FName Name, const T& DefaultValue)
	{
		if (!MD || Name.IsNone())
		{
			return nullptr;
		}

		if (FPCGMetadataAttributeBase* Existing = MD->GetMutableAttribute(Name))
		{
			return static_cast<FPCGMetadataAttribute<T>*>(Existing);
		}

		return MD->CreateAttribute<T>(Name, DefaultValue, /*bAllowsInterpolation*/ false, /*bOverrideParent*/ true);
	}

	static double ReadRotationDegrees(const UPCGPointData* PointData, const FPCGPoint& Point, const FName AttrName, double DefaultValue = 0.0)
	{
		if (!PointData)
		{
			return DefaultValue;
		}

		const UPCGMetadata* Metadata = PointData->Metadata;
		if (!Metadata)
		{
			return DefaultValue;
		}

		// 常见情况：double / float
		if (const FPCGMetadataAttribute<double>* AttrD = GetConstTypedAttr<double>(Metadata, AttrName))
		{
			return AttrD->GetValueFromItemKey(Point.MetadataEntry);
		}

		if (const FPCGMetadataAttribute<float>* AttrF = GetConstTypedAttr<float>(Metadata, AttrName))
		{
			return static_cast<double>(AttrF->GetValueFromItemKey(Point.MetadataEntry));
		}

		// 如果 rotation 存成 Rotator，也兼容一下，取 Yaw
		if (const FPCGMetadataAttribute<FRotator>* AttrR = GetConstTypedAttr<FRotator>(Metadata, AttrName))
		{
			return static_cast<double>(AttrR->GetValueFromItemKey(Point.MetadataEntry).Yaw);
		}

		return DefaultValue;
	}

	static FVector2D Rotate2D(const FVector2D& V, double Degrees)
	{
		const double Rad = FMath::DegreesToRadians(Degrees);
		const double C = FMath::Cos(Rad);
		const double S = FMath::Sin(Rad);
		return FVector2D(
			static_cast<float>(V.X * C - V.Y * S),
			static_cast<float>(V.X * S + V.Y * C)
		);
	}

	static void AddOutput(FPCGContext* Context, const FName PinLabel, UPCGPointData* Data)
	{
		if (!Context || !Data)
		{
			return;
		}

		FPCGTaggedData& Tagged = Context->OutputData.TaggedData.Emplace_GetRef();
		Tagged.Pin = PinLabel;
		Tagged.Data = Data;
	}

	static void AppendPointWithLength(
		UPCGPointData* OutData,
		const FPCGPoint& SourcePoint,
		FName LengthAttrName,
		double LengthValue)
	{
		if (!OutData || !OutData->Metadata)
		{
			return;
		}

		FPCGMetadataAttribute<double>* LengthAttr =
			GetOrCreateAttr<double>(OutData->Metadata, LengthAttrName, 0.0);

		if (!LengthAttr)
		{
			return;
		}

		FPCGPoint NewPoint = SourcePoint;

		// 给这个输出点创建新的 metadata entry（继承源 entry）
		NewPoint.MetadataEntry = OutData->Metadata->AddEntry(SourcePoint.MetadataEntry);

		// 写入长度
		LengthAttr->SetValue(NewPoint.MetadataEntry, LengthValue);

		OutData->GetMutablePoints().Add(MoveTemp(NewPoint));
	}
}

// ------------------------------------------------------------
// Execute
// ------------------------------------------------------------

bool FPCGExtractOwnerEdgesElement::ExecuteInternal(FPCGContext* Context) const
{
	check(Context);

	const UPCGExtractOwnerEdgesSettings* Settings = Context->GetInputSettings<UPCGExtractOwnerEdgesSettings>();
	if (!Settings)
	{
		return true;
	}

	const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGExtractOwnerEdgesConstants::InputLabel);
	if (Inputs.IsEmpty())
	{
		return true;
	}

	UPCGPointData* OutPosX = NewObject<UPCGPointData>();
	UPCGPointData* OutNegX = NewObject<UPCGPointData>();
	UPCGPointData* OutPosY = NewObject<UPCGPointData>();
	UPCGPointData* OutNegY = NewObject<UPCGPointData>();

	bool bInitializedOutput = false;

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGPointData* InPointData = Cast<UPCGPointData>(Input.Data);
		if (!InPointData)
		{
			continue;
		}

		if (!bInitializedOutput)
		{
			OutPosX->InitializeFromData(InPointData);
			OutNegX->InitializeFromData(InPointData);
			OutPosY->InitializeFromData(InPointData);
			OutNegY->InitializeFromData(InPointData);
			bInitializedOutput = true;
		}

		const TArray<FPCGPoint>& InPoints = InPointData->GetPoints();
		if (InPoints.IsEmpty())
		{
			continue;
		}

		// 1) 读取整组点的整体 rotation（默认取第一个点）
		double RotationDeg = ReadRotationDegrees(InPointData, InPoints[0], Settings->RotationAttribute, 0.0);

		// 2) 计算整组中心，作为反旋转枢轴
		FVector Center3D = FVector::ZeroVector;
		for (const FPCGPoint& Pt : InPoints)
		{
			Center3D += Pt.Transform.GetLocation();
		}
		Center3D /= static_cast<double>(InPoints.Num());

		const FVector2D Pivot(Center3D.X, Center3D.Y);

		// 3) 反旋转到“未旋转空间”，统计边界
		struct FLocalPoint
		{
			const FPCGPoint* Source = nullptr;
			FVector2D Local = FVector2D::ZeroVector;
		};

		TArray<FLocalPoint> LocalPoints;
		LocalPoints.Reserve(InPoints.Num());

		double MinX = TNumericLimits<double>::Max();
		double MaxX = TNumericLimits<double>::Lowest();
		double MinY = TNumericLimits<double>::Max();
		double MaxY = TNumericLimits<double>::Lowest();

		for (const FPCGPoint& Pt : InPoints)
		{
			const FVector P3 = Pt.Transform.GetLocation();
			const FVector2D P2(P3.X, P3.Y);

			const FVector2D Delta = P2 - Pivot;
			const FVector2D Unrotated = Rotate2D(Delta, -RotationDeg);

			FLocalPoint& LP = LocalPoints.Emplace_GetRef();
			LP.Source = &Pt;
			LP.Local = Unrotated;

			MinX = FMath::Min(MinX, static_cast<double>(Unrotated.X));
			MaxX = FMath::Max(MaxX, static_cast<double>(Unrotated.X));
			MinY = FMath::Min(MinY, static_cast<double>(Unrotated.Y));
			MaxY = FMath::Max(MaxY, static_cast<double>(Unrotated.Y));
		}
		const double XEdgeLength = FMath::Max(0.0, MaxY - MinY); // 给 +X / -X
		const double YEdgeLength = FMath::Max(0.0, MaxX - MinX); // 给 +Y / -Y
		// 4) 按四条边筛点
		const double Tol = FMath::Max(0.0, Settings->EdgeTolerance);

		for (const FLocalPoint& LP : LocalPoints)
		{
			const double X = static_cast<double>(LP.Local.X);
			const double Y = static_cast<double>(LP.Local.Y);

			const bool bOnPosX = FMath::Abs(X - MaxX) <= Tol;
			const bool bOnNegX = FMath::Abs(X - MinX) <= Tol;
			const bool bOnPosY = FMath::Abs(Y - MaxY) <= Tol;
			const bool bOnNegY = FMath::Abs(Y - MinY) <= Tol;

			if (bOnPosX)
			{
				AppendPointWithLength(OutPosX, *LP.Source, Settings->EdgeLengthAttribute, XEdgeLength);
			}
			if (bOnNegX)
			{
				AppendPointWithLength(OutNegX, *LP.Source, Settings->EdgeLengthAttribute, XEdgeLength);
			}
			if (bOnPosY)
			{
				AppendPointWithLength(OutPosY, *LP.Source, Settings->EdgeLengthAttribute, YEdgeLength);
			}
			if (bOnNegY)
			{
				AppendPointWithLength(OutNegY, *LP.Source, Settings->EdgeLengthAttribute, YEdgeLength);
			}
		}
	}

	if (bInitializedOutput)
	{
		AddOutput(Context, PCGExtractOwnerEdgesConstants::OutPosXLabel, OutPosX);
		AddOutput(Context, PCGExtractOwnerEdgesConstants::OutNegXLabel, OutNegX);
		AddOutput(Context, PCGExtractOwnerEdgesConstants::OutPosYLabel, OutPosY);
		AddOutput(Context, PCGExtractOwnerEdgesConstants::OutNegYLabel, OutNegY);
	}

	return true;
}

#undef LOCTEXT_NAMESPACE