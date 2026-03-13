#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"

#include "PCGPruneOverlappingPointsByOwner.generated.h"

UENUM(BlueprintType)
enum class EPCGPruneVoteAttributeType : uint8
{
	Int32 UMETA(DisplayName = "Int32"),
	Int64 UMETA(DisplayName = "Int64"),
	Double UMETA(DisplayName = "Double"),
	Name UMETA(DisplayName = "Name"),
	String UMETA(DisplayName = "String")
};

UCLASS(BlueprintType, ClassGroup = (Procedural))
class BUILDINGGENERATOR_API UPCGPruneOverlappingPointsByOwnerSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	// 要读取的属性名，默认就是你说的 ownerindex
	// 选择要参与“投票裁剪”的属性（像 Attribute Filter 一样可选）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
	FName CompareAttribute = TEXT("OwnerIndex");

	// 这个属性按什么类型解释
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
	EPCGPruneVoteAttributeType CompareAttributeType = EPCGPruneVoteAttributeType::Int32;

	// 判断“重合”的位置容差（单位：cm）
	// 会按这个值做量化，所以建议 >= 0.1
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (ClampMin = "0.001"))
	double PositionTolerance = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (ClampMin = "0.0000001"))
	double DoubleQuantization = 0.001;

	// 是否只按 XY 判断重合（忽略 Z）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
	bool bUseXYOnly = false;

	// 当多个 OwnerIndex 数量相同，是否全部保留
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
	bool bKeepAllIfTie = false;

	// 如果并列且 bKeepAllIfTie = false，则是否优先保留 OwnerIndex 更大的那组
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
	bool bPreferHigherValueOnTie = true;


public:
#if WITH_EDITOR
	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("PCGPruneOverlappingPointsByOwner", "NodeTitle", "Prune Overlapping Points By Attribute Vote");
	}

	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGPruneOverlappingPointsByOwner", "NodeTooltip",
			"Groups overlapping points by position, reads a selected attribute/property, compares value counts in each overlap group, and keeps only the winning value group.");
	}
#endif

	virtual EPCGSettingsType GetType() const override
	{
		return EPCGSettingsType::Spatial;
	}

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

class FPCGPruneOverlappingPointsByOwnerElement : public IPCGElement
{
public:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};