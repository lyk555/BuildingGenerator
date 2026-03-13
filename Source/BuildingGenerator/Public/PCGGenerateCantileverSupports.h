#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGGenerateCantileverSupports.generated.h"

UENUM(BlueprintType)
enum class EPCGSupportZeroYawDir : uint8
{
	PlusX  UMETA(DisplayName="+X"),
	MinusX UMETA(DisplayName="-X"),
	PlusY  UMETA(DisplayName="+Y"),
	MinusY UMETA(DisplayName="-Y")
};

UCLASS(BlueprintType, ClassGroup=(Procedural))
class BUILDINGGENERATOR_API UPCGGenerateCantileverSupportsSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return TEXT("GenerateCantileverSupports"); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("PCGGenerateCantileverSupports", "NodeTitle", "Generate Cantilever Supports"); }
	virtual FText GetNodeTooltipText() const override { return NSLOCTEXT("PCGGenerateCantileverSupports", "NodeTip", "Detect cantilever areas and spawn support points (small/large)."); }
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Spatial; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;

public:
	// ---- Required input attributes ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input|Attributes")
	FName OwnerIndexAttribute = TEXT("OwnerIndex");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input|Attributes")
	FName RingIdAttribute = TEXT("RingId");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input|Attributes")
	FName VolumeCenterAttribute = TEXT("CenterPosition");  // FVector

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input|Attributes")
	FName VolumeExtentsAttribute = TEXT("GridExtents"); // FVector

	// ---- Optional: use existing vertex order instead of angle sort ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input|Ordering")
	bool bUseVertexIndexForOrdering = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input|Ordering", meta=(EditCondition="bUseVertexIndexForOrdering"))
	FName VertexIndexAttribute = TEXT("VertexIndex");

	// ---- Detection (Z relation) ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Detection")
	double ZTolerance = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Detection")
	double MaxGap = 20.0;

	// ---- Cantilever threshold (XY) ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Detection")
	double CantileverThreshold = 200.0;

	// ---- Sampling ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sampling")
	double GridSpacing = 100.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sampling")
	double Inset = 30.0;

	// ---- Clipper scaling ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Clipper")
	double ClipperScale = 1000.0;

	// ---- Output attributes ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Attributes")
	FName SupportTypeAttribute = TEXT("SupportType"); // int32: 0 small, 1 large

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Attributes")
	FName SupportHeightAttribute = TEXT("SupportHeight"); // double

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Attributes")
	FName SupportSymbolAttribute = TEXT("SupportSymbol"); // string

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Symbols")
	FString SmallSupportSymbol = TEXT("SupportSmall");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Symbols")
	FString LargeSupportSymbol = TEXT("SupportLarge");

		// ---- Output rotation (point transform yaw) ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Rotation")
	bool bWriteRotation = true;

	// 你说的：默认 +Y 方向对应 yaw=0（可改成 +X/-X/-Y）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Rotation", meta=(EditCondition="bWriteRotation"))
	EPCGSupportZeroYawDir ZeroYawDirection = EPCGSupportZeroYawDir::PlusY;

	// 在计算出的 yaw 上再加一个偏移（度），用于素材自身前向不是严格对齐你定义的 ZeroYawDirection 的情况
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Rotation", meta=(EditCondition="bWriteRotation"))
	double YawOffsetDegrees = 0.0;

	// 可选：把最终 yaw 写到 metadata，方便你 debug/后续节点使用
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Rotation")
	bool bWriteYawAttribute = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output|Rotation", meta=(EditCondition="bWriteYawAttribute"))
	FName SupportYawAttribute = TEXT("SupportYaw");

};
