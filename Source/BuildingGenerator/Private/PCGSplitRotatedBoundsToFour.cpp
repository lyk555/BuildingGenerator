#include "PCGSplitRotatedBoundsToFour.h"

#include "PCGContext.h"
#include "PCGElement.h"
#include "PCGPin.h"
#include "PCGData.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/PCGMetadataAttributeTpl.h"

#define LOCTEXT_NAMESPACE "PCGSplitRotatedBoundsToFour"

namespace PCGSplitRotatedBoundsToFour
{
	static const FName InputLabel(TEXT("In"));

	static const FName OutOverallBounds(TEXT("OverallBounds"));
	static const FName OutHalves(TEXT("Halves"));

	static const FName OutPartAEdges(TEXT("PartA_Edges"));
	static const FName OutPartAMiddle(TEXT("PartA_Middle"));
	static const FName OutPartBEdges(TEXT("PartB_Edges"));
	static const FName OutPartBMiddle(TEXT("PartB_Middle"));

	struct FLocalBox2D
	{
		FVector2D Min = FVector2D::ZeroVector;
		FVector2D Max = FVector2D::ZeroVector;

		FVector2D Size() const { return Max - Min; }
		FVector2D Center() const { return (Min + Max) * 0.5; }
		bool IsValid() const { return Min.X <= Max.X && Min.Y <= Max.Y; }
	};

	static FPCGPoint MakeBoxPoint(
		const FVector& WorldCenter,
		const FRotator& WorldRot,
		const FVector& FullSize)
	{
		FPCGPoint Pt;

		const FVector SafeSize(
			FMath::Max(FullSize.X, 1.0),
			FMath::Max(FullSize.Y, 1.0),
			FMath::Max(FullSize.Z, 1.0));

		const FVector Half = SafeSize * 0.5;

		// 不要把世界尺寸写进 Transform.Scale
		// 这里只输出“一个新的点 + 它的 bounds”
		Pt.Transform = FTransform(WorldRot, WorldCenter, FVector::OneVector);

		Pt.BoundsMin = -Half;
		Pt.BoundsMax = Half;

		Pt.Seed = GetTypeHash(WorldCenter);
		Pt.Density = 1.0f;

		return Pt;
	}

	static FPCGPoint MakeVolumePointFromLocalBox(
		const FLocalBox2D& Box2D,
		double MinZ,
		double MaxZ,
		const FVector& PivotWorld,
		double YawDeg,
		double MinThickness)
	{
		const FVector2D C2 = Box2D.Center();
		const FVector2D S2 = Box2D.Size();

		const double WorldCenterZ = (MinZ + MaxZ) * 0.5;
		const double SizeZ = FMath::Max(MaxZ - MinZ, MinThickness);

		// 只旋转 XY，Z 不参与
		const FVector RotatedXY =
			FRotator(0.0, YawDeg, 0.0).RotateVector(FVector(C2.X, C2.Y, 0.0));

		const FVector WorldCenter(
			PivotWorld.X + RotatedXY.X,
			PivotWorld.Y + RotatedXY.Y,
			WorldCenterZ);

		const FVector FullSize(
			FMath::Max(S2.X, MinThickness),
			FMath::Max(S2.Y, MinThickness),
			SizeZ);

		return MakeBoxPoint(WorldCenter, FRotator(0.0, YawDeg, 0.0), FullSize);
	}

	static void WritePointData(
		FPCGContext* Context,
		const UPCGPointData* SourcePointData,
		const FName& PinName,
		const TArray<FPCGPoint>& Points)
	{
		if (!Context) return;

		UPCGPointData* OutData = NewObject<UPCGPointData>();

		if (SourcePointData)
		{
			OutData->InitializeFromData(
				SourcePointData,
				SourcePointData->Metadata,
				/*bInheritMetadata=*/true,
				/*bInheritAttributes=*/true);
		}

		TArray<FPCGPoint>& MutablePoints = OutData->GetMutablePoints();
		MutablePoints = Points;

		// 关键修复：给每个输出点挂上 metadata entry
		if (SourcePointData && SourcePointData->Metadata && !SourcePointData->GetPoints().IsEmpty())
		{
			const PCGMetadataEntryKey ParentEntry = SourcePointData->GetPoints()[0].MetadataEntry;

			if (ParentEntry != PCGInvalidEntryKey)
			{
				for (FPCGPoint& Pt : MutablePoints)
				{
					Pt.MetadataEntry = OutData->Metadata->AddEntry(ParentEntry);
				}
			}
		}

		FPCGTaggedData& Tagged = Context->OutputData.TaggedData.Emplace_GetRef();
		Tagged.Pin = PinName;
		Tagged.Data = OutData;
	}

	static bool ReadUniformRotationDegrees(
		const UPCGPointData* InPointData,
		FName RotationAttribute,
		double& OutYawDeg)
	{
		OutYawDeg = 0.0;

		if (!InPointData)
		{
			return false;
		}

		const TArray<FPCGPoint>& Points = InPointData->GetPoints();
		if (Points.IsEmpty())
		{
			return false;
		}

		// 先尝试读 metadata 上的统一 Rotation
		if (const UPCGMetadata* MD = InPointData->Metadata)
		{
			if (const FPCGMetadataAttributeBase* AttrBase = MD->GetConstAttribute(RotationAttribute))
			{
				// double
				if (AttrBase->GetTypeId() == PCG::Private::MetadataTypes<double>::Id)
				{
					const FPCGMetadataAttribute<double>* AttrD = static_cast<const FPCGMetadataAttribute<double>*>(AttrBase);
					OutYawDeg = AttrD->GetValue(Points[0].MetadataEntry);
					return true;
				}
				// float
				else if (AttrBase->GetTypeId() == PCG::Private::MetadataTypes<float>::Id)
				{
					const FPCGMetadataAttribute<float>* AttrF = static_cast<const FPCGMetadataAttribute<float>*>(AttrBase);
					OutYawDeg = static_cast<double>(AttrF->GetValue(Points[0].MetadataEntry));
					return true;
				}
			}
		}

		// fallback：如果 metadata 没有，就直接用第一个点自己的 Transform Yaw
		OutYawDeg = static_cast<double>(Points[0].Transform.Rotator().Yaw);
		return true;
	}

	static FVector ComputeCentroid(const TArray<FPCGPoint>& Points)
	{
		if (Points.IsEmpty()) return FVector::ZeroVector;

		FVector Sum = FVector::ZeroVector;
		for (const FPCGPoint& Pt : Points)
		{
			Sum += Pt.Transform.GetLocation();
		}
		return Sum / (double)Points.Num();
	}

	static FVector ComputeWorldAABBCenterFromPointBounds(const TArray<FPCGPoint>& Points)
	{
		if (Points.IsEmpty())
		{
			return FVector::ZeroVector;
		}

		FVector MinW(0, 0, 0);
		FVector MaxW(0, 0, 0);
		bool bInit = false;

		for (const FPCGPoint& Pt : Points)
		{
			const FTransform& T = Pt.Transform;

			const FVector LocalCorners[8] =
			{
				FVector(Pt.BoundsMin.X, Pt.BoundsMin.Y, Pt.BoundsMin.Z),
				FVector(Pt.BoundsMin.X, Pt.BoundsMin.Y, Pt.BoundsMax.Z),
				FVector(Pt.BoundsMin.X, Pt.BoundsMax.Y, Pt.BoundsMin.Z),
				FVector(Pt.BoundsMin.X, Pt.BoundsMax.Y, Pt.BoundsMax.Z),
				FVector(Pt.BoundsMax.X, Pt.BoundsMin.Y, Pt.BoundsMin.Z),
				FVector(Pt.BoundsMax.X, Pt.BoundsMin.Y, Pt.BoundsMax.Z),
				FVector(Pt.BoundsMax.X, Pt.BoundsMax.Y, Pt.BoundsMin.Z),
				FVector(Pt.BoundsMax.X, Pt.BoundsMax.Y, Pt.BoundsMax.Z)
			};

			for (const FVector& C : LocalCorners)
			{
				const FVector W = T.TransformPosition(C);

				if (!bInit)
				{
					MinW = W;
					MaxW = W;
					bInit = true;
				}
				else
				{
					MinW.X = FMath::Min(MinW.X, W.X);
					MinW.Y = FMath::Min(MinW.Y, W.Y);
					MinW.Z = FMath::Min(MinW.Z, W.Z);

					MaxW.X = FMath::Max(MaxW.X, W.X);
					MaxW.Y = FMath::Max(MaxW.Y, W.Y);
					MaxW.Z = FMath::Max(MaxW.Z, W.Z);
				}
			}
		}

		return (MinW + MaxW) * 0.5;
	}

	static FLocalBox2D ComputeUnrotatedXYBoxFromPointBounds(
		const TArray<FPCGPoint>& InPoints,
		const FVector& PivotWorld,
		double YawDeg)
	{
		FLocalBox2D Box;
		if (InPoints.IsEmpty())
		{
			return Box;
		}

		const FQuat InvWorldRot = FRotator(0.0, -YawDeg, 0.0).Quaternion();

		TArray<double> Xs;
		TArray<double> Ys;
		Xs.Reserve(InPoints.Num());
		Ys.Reserve(InPoints.Num());

		bool bInit = false;

		for (const FPCGPoint& Pt : InPoints)
		{
			// 只用点中心来算点阵范围
			const FVector WorldPos = Pt.Transform.GetLocation();
			const FVector Local = InvWorldRot.RotateVector(WorldPos - PivotWorld);

			const double X = Local.X;
			const double Y = Local.Y;

			Xs.Add(X);
			Ys.Add(Y);

			if (!bInit)
			{
				Box.Min = FVector2D(X, Y);
				Box.Max = FVector2D(X, Y);
				bInit = true;
			}
			else
			{
				Box.Min.X = FMath::Min(Box.Min.X, X);
				Box.Min.Y = FMath::Min(Box.Min.Y, Y);
				Box.Max.X = FMath::Max(Box.Max.X, X);
				Box.Max.Y = FMath::Max(Box.Max.Y, Y);
			}
		}

		// 用半个格距把“点中心范围”扩成“点阵整体覆盖范围”
		auto EstimateStep = [](TArray<double>& Values) -> double
			{
				if (Values.Num() < 2)
				{
					return 0.0;
				}

				Values.Sort();

				double Best = TNumericLimits<double>::Max();
				for (int32 i = 1; i < Values.Num(); ++i)
				{
					const double D = Values[i] - Values[i - 1];
					if (D > KINDA_SMALL_NUMBER)
					{
						Best = FMath::Min(Best, D);
					}
				}

				return (Best == TNumericLimits<double>::Max()) ? 0.0 : Best;
			};

		const double StepX = EstimateStep(Xs);
		const double StepY = EstimateStep(Ys);

		const double PadX = (StepX > KINDA_SMALL_NUMBER) ? (0.5 * StepX) : 0.0;
		const double PadY = (StepY > KINDA_SMALL_NUMBER) ? (0.5 * StepY) : 0.0;

		Box.Min.X -= PadX;
		Box.Max.X += PadX;
		Box.Min.Y -= PadY;
		Box.Max.Y += PadY;

		return Box;
	}

	static void ComputeFixedHeightZBoundsFromInputPoints(
		const TArray<FPCGPoint>& InPoints,
		double InHeight,
		double MinThickness,
		double& OutCenterZ,
		double& OutMinZ,
		double& OutMaxZ)
	{
		if (InPoints.IsEmpty())
		{
			OutCenterZ = 0.0;

			const double Height = FMath::Max(InHeight, MinThickness);
			const double HalfH = 0.5 * Height;

			OutMinZ = -HalfH;
			OutMaxZ = HalfH;
			return;
		}

		// 整体 cube 的中心 Z：跟输入点阵一致
		// 这里取输入点位置的平均 Z，最稳
		double SumZ = 0.0;
		for (const FPCGPoint& Pt : InPoints)
		{
			SumZ += Pt.Transform.GetLocation().Z;
		}

		OutCenterZ = SumZ / static_cast<double>(InPoints.Num());

		const double Height = FMath::Max(InHeight, MinThickness);
		const double HalfH = 0.5 * Height;

		OutMinZ = OutCenterZ - HalfH;
		OutMaxZ = OutCenterZ + HalfH;
	}

	static void MakeTwoHalves(
		const FLocalBox2D& FullBox,
		EPCGBoundsSplitAxis SplitAxis,
		FLocalBox2D& OutA,
		FLocalBox2D& OutB)
	{
		OutA = FullBox;
		OutB = FullBox;

		if (SplitAxis == EPCGBoundsSplitAxis::X)
		{
			const double MidX = (FullBox.Min.X + FullBox.Max.X) * 0.5;
			OutA.Max.X = MidX;
			OutB.Min.X = MidX;
		}
		else
		{
			const double MidY = (FullBox.Min.Y + FullBox.Max.Y) * 0.5;
			OutA.Max.Y = MidY;
			OutB.Min.Y = MidY;
		}
	}

	// 对某个 half box，沿“次轴”切成：起始 / 中间 / 末尾
	// 最终输出：Edges(首+尾 合并成两个点) + Middle(一个点)
	static void SplitHalfToEdgesAndMiddle(
		const FLocalBox2D& HalfBox,
		EPCGBoundsSplitAxis SplitAxis,
		double InWidthA,
		double InWidthB,
		bool bAutoScaleWidthsIfNeeded,
		double MinThickness,
		double MinZ,
		double MaxZ,
		const FVector& PivotWorld,
		double YawDeg,
		TArray<FPCGPoint>& OutEdges,
		TArray<FPCGPoint>& OutMiddle)
	{
		const bool bSplitAlongX = (SplitAxis == EPCGBoundsSplitAxis::X);

		// 整体沿 X 二分（左右 halves）时，每个 half 内沿 Y 分段
		// 整体沿 Y 二分（上下 halves）时，每个 half 内沿 X 分段
		const double SecMin = bSplitAlongX ? HalfBox.Min.Y : HalfBox.Min.X;
		const double SecMax = bSplitAlongX ? HalfBox.Max.Y : HalfBox.Max.X;
		const double SecLen = FMath::Max(SecMax - SecMin, MinThickness);

		const double WidthA = FMath::Max(InWidthA, MinThickness);
		const double WidthB = FMath::Max(InWidthB, MinThickness);

		// 先固定首尾两个 A
		double HeadSize = WidthA;
		double TailSize = WidthA;

		// 中间剩余长度
		double Remaining = SecLen - HeadSize - TailSize;

		// 如果连两个 A 都放不下：
		// - 开启 AutoScale：把两个 A 平分整个长度，中间没有 B
		// - 关闭 AutoScale：也至少压到能放下（否则会反向）
		if (Remaining < 0.0)
		{
			const double Half = 0.5 * SecLen;
			HeadSize = FMath::Max(Half, MinThickness);
			TailSize = FMath::Max(Half, MinThickness);
			Remaining = 0.0;
		}

		// 计算中间能放多少个 B 单元
		int32 MiddleCount = 0;
		double MiddleUnitSize = WidthB;
		double MiddleStart = SecMin + HeadSize;
		double MiddleTotal = 0.0;

		if (Remaining > KINDA_SMALL_NUMBER)
		{
			// 至少放 1 个中间单元
			MiddleCount = FMath::Max(1, FMath::FloorToInt(Remaining / WidthB));

			if (MiddleCount <= 0)
			{
				MiddleCount = 1;
			}

			if (bAutoScaleWidthsIfNeeded)
			{
				// 开启 AutoScale：
				// 用 MiddleCount 个 B 铺满整个剩余区间（每个 B 可能被缩放）
				MiddleUnitSize = Remaining / static_cast<double>(MiddleCount);
				MiddleTotal = Remaining;
				MiddleStart = SecMin + HeadSize;
			}
			else
			{
				// 不开启 AutoScale：
				// 每个 B 保持 WidthB，整体居中放在剩余区间里
				MiddleUnitSize = WidthB;
				MiddleTotal = MiddleUnitSize * static_cast<double>(MiddleCount);

				// 如果 Remaining < WidthB，仍然强行放 1 个，并压到 Remaining
				if (MiddleTotal > Remaining)
				{
					MiddleCount = 1;
					MiddleUnitSize = Remaining;
					MiddleTotal = Remaining;
				}

				const double Leftover = FMath::Max(0.0, Remaining - MiddleTotal);
				MiddleStart = SecMin + HeadSize + 0.5 * Leftover;
			}
		}

		auto MakeLocalSubBox = [&](double SubSecMin, double SubSecMax) -> FLocalBox2D
			{
				FLocalBox2D B = HalfBox;
				if (bSplitAlongX)
				{
					B.Min.Y = SubSecMin;
					B.Max.Y = SubSecMax;
				}
				else
				{
					B.Min.X = SubSecMin;
					B.Max.X = SubSecMax;
				}
				return B;
			};

		auto LocalBoxToPoint = [&](const FLocalBox2D& B) -> FPCGPoint
			{
				const FVector2D C2 = B.Center();
				const FVector2D S2 = B.Size();

				const double WorldCenterZ = (MinZ + MaxZ) * 0.5;
				const double SizeZ = FMath::Max(MaxZ - MinZ, MinThickness);

				const FVector RotatedXY =
					FRotator(0.0, YawDeg, 0.0).RotateVector(FVector(C2.X, C2.Y, 0.0));

				const FVector WorldCenter(
					PivotWorld.X + RotatedXY.X,
					PivotWorld.Y + RotatedXY.Y,
					WorldCenterZ);

				const FVector FullSize(
					FMath::Max(S2.X, MinThickness),
					FMath::Max(S2.Y, MinThickness),
					SizeZ);

				return MakeBoxPoint(WorldCenter, FRotator(0.0, YawDeg, 0.0), FullSize);
			};

		// 首块 A
		const double HeadMin = SecMin;
		const double HeadMax = SecMin + HeadSize;
		OutEdges.Add(LocalBoxToPoint(MakeLocalSubBox(HeadMin, HeadMax)));

		// 中间多个 B
		for (int32 Index = 0; Index < MiddleCount; ++Index)
		{
			const double BMin = MiddleStart + static_cast<double>(Index) * MiddleUnitSize;
			const double BMax = BMin + MiddleUnitSize;
			OutMiddle.Add(LocalBoxToPoint(MakeLocalSubBox(BMin, BMax)));
		}

		// 尾块 A
		const double TailMax = SecMax;
		const double TailMin = SecMax - TailSize;
		OutEdges.Add(LocalBoxToPoint(MakeLocalSubBox(TailMin, TailMax)));

		UE_LOG(LogTemp, Warning,
			TEXT("[SplitHalf] SecLen=%.2f HeadA=%.2f TailA=%.2f Remaining=%.2f WidthB=%.2f MiddleCount=%d MiddleUnit=%.2f"),
			SecLen, HeadSize, TailSize, Remaining, WidthB, MiddleCount, MiddleUnitSize);
	}
}

class FPCGSplitRotatedBoundsToFourElement final : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override
	{
		if (!Context) return true;

		const UPCGSplitRotatedBoundsToFourSettings* Settings = Context->GetInputSettings<UPCGSplitRotatedBoundsToFourSettings>();
		if (!Settings) return true;

		const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGSplitRotatedBoundsToFour::InputLabel);
		if (Inputs.IsEmpty())
		{
			return true;
		}

		// 这里只处理第一个 PointData；如果你要支持多输入合并，可再扩展
		const UPCGPointData* InPointData = Cast<UPCGPointData>(Inputs[0].Data);
		if (!InPointData)
		{
			UE_LOG(LogTemp, Warning, TEXT("SplitRotatedBoundsToFour: Input must be Point Data."));
			return true;
		}

		const TArray<FPCGPoint>& InPoints = InPointData->GetPoints();
		if (InPoints.IsEmpty())
		{
			return true;
		}

		double YawDeg = 0.0;
		PCGSplitRotatedBoundsToFour::ReadUniformRotationDegrees(InPointData, Settings->RotationAttribute, YawDeg);

		const FVector RawPivotWorld = Settings->bUseCentroidAsPivot
			? PCGSplitRotatedBoundsToFour::ComputeWorldAABBCenterFromPointBounds(InPoints)
			: FVector::ZeroVector;

		// 旋转 pivot 只用 XY，Z 不参与
		const FVector PivotWorld(RawPivotWorld.X, RawPivotWorld.Y, 0.0);

		const PCGSplitRotatedBoundsToFour::FLocalBox2D FullBox =
			PCGSplitRotatedBoundsToFour::ComputeUnrotatedXYBoxFromPointBounds(InPoints, PivotWorld, YawDeg);

		double CenterZ = 0.0;
		double MinZ = 0.0;
		double MaxZ = 0.0;

		PCGSplitRotatedBoundsToFour::ComputeFixedHeightZBoundsFromInputPoints(
			InPoints,
			Settings->InputHeight,
			Settings->MinThickness,
			CenterZ,
			MinZ,
			MaxZ);

		// 先生成完整 OverallVolume（完整大盒子）
		TArray<FPCGPoint> OverallBounds;
		OverallBounds.Add(
			PCGSplitRotatedBoundsToFour::MakeVolumePointFromLocalBox(
				FullBox,
				MinZ,
				MaxZ,
				PivotWorld,
				YawDeg,
				Settings->MinThickness));

		PCGSplitRotatedBoundsToFour::FLocalBox2D HalfA, HalfB;
		PCGSplitRotatedBoundsToFour::MakeTwoHalves(FullBox, Settings->SplitAxis, HalfA, HalfB);

		TArray<FPCGPoint> Halves;
		Halves.Add(
			PCGSplitRotatedBoundsToFour::MakeVolumePointFromLocalBox(
				HalfA,
				MinZ,
				MaxZ,
				PivotWorld,
				YawDeg,
				Settings->MinThickness));

		Halves.Add(
			PCGSplitRotatedBoundsToFour::MakeVolumePointFromLocalBox(
				HalfB,
				MinZ,
				MaxZ,
				PivotWorld,
				YawDeg,
				Settings->MinThickness));

		TArray<FPCGPoint> PartAEdges;
		TArray<FPCGPoint> PartAMiddle;
		TArray<FPCGPoint> PartBEdges;
		TArray<FPCGPoint> PartBMiddle;

		PCGSplitRotatedBoundsToFour::SplitHalfToEdgesAndMiddle(
			HalfA,
			Settings->SplitAxis,
			Settings->WidthA,
			Settings->WidthB,
			Settings->bAutoScaleWidthsIfNeeded,
			Settings->MinThickness,
			MinZ,
			MaxZ,
			PivotWorld,
			YawDeg,
			PartAEdges,
			PartAMiddle);

		PCGSplitRotatedBoundsToFour::SplitHalfToEdgesAndMiddle(
			HalfB,
			Settings->SplitAxis,
			Settings->WidthA,
			Settings->WidthB,
			Settings->bAutoScaleWidthsIfNeeded,
			Settings->MinThickness,
			MinZ,
			MaxZ,
			PivotWorld,
			YawDeg,
			PartBEdges,
			PartBMiddle);

		UE_LOG(LogTemp, Warning, TEXT("==== SplitRotatedBoundsToFour ===="));
		UE_LOG(LogTemp, Warning, TEXT("YawDeg = %.3f"), YawDeg);
		UE_LOG(LogTemp, Warning, TEXT("PivotWorld(XY only) = (%.2f, %.2f, %.2f)"), PivotWorld.X, PivotWorld.Y, PivotWorld.Z);
		UE_LOG(LogTemp, Warning, TEXT("FullBox Min=(%.2f, %.2f) Max=(%.2f, %.2f)"),
			FullBox.Min.X, FullBox.Min.Y, FullBox.Max.X, FullBox.Max.Y);
		UE_LOG(LogTemp, Warning, TEXT("CenterZ=%.2f MinZ=%.2f MaxZ=%.2f InputHeight=%.2f"),
			CenterZ, MinZ, MaxZ, Settings->InputHeight);
		UE_LOG(LogTemp, Warning, TEXT("OverallBounds=%d Halves=%d | PartAEdges=%d PartAMiddle=%d PartBEdges=%d PartBMiddle=%d"),
			OverallBounds.Num(), Halves.Num(),
			PartAEdges.Num(), PartAMiddle.Num(), PartBEdges.Num(), PartBMiddle.Num());

		PCGSplitRotatedBoundsToFour::WritePointData(Context, InPointData, PCGSplitRotatedBoundsToFour::OutOverallBounds, OverallBounds);
		PCGSplitRotatedBoundsToFour::WritePointData(Context, InPointData, PCGSplitRotatedBoundsToFour::OutHalves, Halves);

		PCGSplitRotatedBoundsToFour::WritePointData(Context, InPointData, PCGSplitRotatedBoundsToFour::OutPartAEdges, PartAEdges);
		PCGSplitRotatedBoundsToFour::WritePointData(Context, InPointData, PCGSplitRotatedBoundsToFour::OutPartAMiddle, PartAMiddle);
		PCGSplitRotatedBoundsToFour::WritePointData(Context, InPointData, PCGSplitRotatedBoundsToFour::OutPartBEdges, PartBEdges);
		PCGSplitRotatedBoundsToFour::WritePointData(Context, InPointData, PCGSplitRotatedBoundsToFour::OutPartBMiddle, PartBMiddle);

		return true;
	}
};

TArray<FPCGPinProperties> UPCGSplitRotatedBoundsToFourSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGSplitRotatedBoundsToFour::InputLabel, EPCGDataType::Point);
	return Pins;
}

TArray<FPCGPinProperties> UPCGSplitRotatedBoundsToFourSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGSplitRotatedBoundsToFour::OutOverallBounds, EPCGDataType::Point);
	Pins.Emplace(PCGSplitRotatedBoundsToFour::OutHalves, EPCGDataType::Point);

	Pins.Emplace(PCGSplitRotatedBoundsToFour::OutPartAEdges, EPCGDataType::Point);
	Pins.Emplace(PCGSplitRotatedBoundsToFour::OutPartAMiddle, EPCGDataType::Point);
	Pins.Emplace(PCGSplitRotatedBoundsToFour::OutPartBEdges, EPCGDataType::Point);
	Pins.Emplace(PCGSplitRotatedBoundsToFour::OutPartBMiddle, EPCGDataType::Point);
	return Pins;
}

FPCGElementPtr UPCGSplitRotatedBoundsToFourSettings::CreateElement() const
{
	return MakeShared<FPCGSplitRotatedBoundsToFourElement>();
}

#undef LOCTEXT_NAMESPACE