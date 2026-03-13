#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"
#include "Data/PCGPointData.h"
#include "PCGExtractOwnerEdges.generated.h"

UCLASS(BlueprintType, ClassGroup=(Procedural))
class BUILDINGGENERATOR_API UPCGExtractOwnerEdgesSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPCGExtractOwnerEdgesSettings();

	// ---- 可调参数 ----

	// 这里默认认为 rotation 存的是“角度（Yaw）”
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings")
	FName RotationAttribute = TEXT("rotation");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings")
	FName EdgeLengthAttribute = TEXT("EdgeLength");

	// 判断是否在边界上的容差（反旋转后的局部坐标）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings", meta=(ClampMin="0.0"))
	double EdgeTolerance = 1.0;

public:
	// ---- UPCGSettings overrides ----
	virtual FName GetDefaultNodeName() const override;
	virtual FText GetDefaultNodeTitle() const override;
	virtual EPCGSettingsType GetType() const override;

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

class FPCGExtractOwnerEdgesElement : public IPCGElement
{
public:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};