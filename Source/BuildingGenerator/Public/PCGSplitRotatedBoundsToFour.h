#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGPoint.h"
#include "PCGSplitRotatedBoundsToFour.generated.h"

class UPCGPointData;
struct FPCGContext;
class IPCGElement;

UENUM(BlueprintType)
enum class EPCGBoundsSplitAxis : uint8
{
	X UMETA(DisplayName="X"),
	Y UMETA(DisplayName="Y")
};

UCLASS(BlueprintType, ClassGroup=(Procedural))
class BUILDINGGENERATOR_API UPCGSplitRotatedBoundsToFourSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	// 读取输入点上的整体旋转属性名（float，单位度）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings", meta=(PCG_Overridable))
	FName RotationAttribute = TEXT("Rotation");

	// 按哪个轴先一分为二
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings", meta=(PCG_Overridable))
	EPCGBoundsSplitAxis SplitAxis = EPCGBoundsSplitAxis::X;

	// 每列（或每行）内部首尾两段的尺寸（A 段）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings", meta=(ClampMin="0.0", PCG_Overridable))
	double WidthA = 100.0;

	// 每列（或每行）内部中间那一段的尺寸（B 段）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings", meta=(ClampMin="0.0", PCG_Overridable))
	double WidthB = 100.0;

	// 由输入点阵 XY 范围生成整体 bounds 时使用的固定高度（Z 尺寸）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings", meta=(ClampMin="0.0", PCG_Overridable))
	double InputHeight = 300.0;

	// 逆旋转/正旋转时使用的 pivot...
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings", meta=(PCG_Overridable))
	bool bUseCentroidAsPivot = true;

	// 当 2*WidthA + WidthB 与半区尺寸不一致时，是否自动按比例缩放到正好填满
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings", meta=(PCG_Overridable))
	bool bAutoScaleWidthsIfNeeded = true;

	// 给输出 box 一个微小厚度，避免某一轴尺寸为 0 时 bounds 异常
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings", meta=(ClampMin="0.0", PCG_Overridable))
	double MinThickness = 1.0;

public:
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("SplitRotatedBoundsToFour")); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("PCGSplitRotatedBoundsToFour", "NodeTitle", "Split Rotated Bounds To Four"); }
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGSplitRotatedBoundsToFour", "Tooltip",
			"Restore a rotated point set to its unrotated bounds, build a boundary box, split it into two by a main axis, then subdivide each half along the perpendicular axis into edge bands and a middle block. Outputs 4 point sets.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Spatial; }

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};