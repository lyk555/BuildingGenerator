#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"
#include "PCGData.h"
#include "UObject/WeakObjectPtr.h"

#include "PCGFilterOccludedRoofPoints.generated.h"

class UPCGComponent;
class UPCGNode;

UCLASS(BlueprintType, ClassGroup=(PCG))
class BUILDINGGENERATOR_API UPCGFilterOccludedRoofPointsSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("FilterOccludedRoofPoints")); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("PCG", "FilterOccludedRoofPointsTitle", "Filter Occluded Roof Points"); }
	virtual FPCGElementPtr CreateElement() const override;

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pins")
	FName RoofsPinLabel = TEXT("Roofs");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pins")
	FName FloorsPinLabel = TEXT("Floors");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pins")
	FName OutputPinLabel = TEXT("VisibleRoofs");

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes")
	FName OwnerIndexAttribute = TEXT("OwnerIndex");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes")
	FName GridExtentsAttribute = TEXT("GridExtents");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes")
	FName RotationAttribute = TEXT("Rotation"); // yaw degrees (Z轴旋转)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes")
	bool bUseRingId = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes", meta=(EditCondition="bUseRingId"))
	FName RingIdAttribute = TEXT("RingId");

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tolerance", meta=(ClampMin="0.0"))
	double SurfaceTol = 2.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tolerance", meta=(ClampMin="0.0"))
	double XYTol = 1.0;

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Robustness")
	bool bTryBuildFloorPolygon = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Robustness")
	bool bFallbackToExtentsAABB = true;

	/** GridExtents 是半尺寸(half extent) 就勾 true；如果是全尺寸(board size) 就勾 false */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Robustness")
	bool bGridExtentsIsHalfSize = true;

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Debug")
	bool bDebugLog = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Debug", meta=(ClampMin="0"))
	int32 DebugMaxFloorGroupsToLog = 12;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Debug", meta=(ClampMin="0"))
	int32 DebugMaxRoofSamplesToLog = 12;
};

class FPCGFilterOccludedRoofPointsElement : public IPCGElement
{
public:
	using IPCGElement::Initialize;

	virtual FPCGContext* Initialize(
		const FPCGDataCollection& InputData,
		TWeakObjectPtr<UPCGComponent> SourceComponent,
		const UPCGNode* Node) override;

protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};
