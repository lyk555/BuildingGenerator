#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"                 // IPCGElement
#include "Data/PCGPointData.h"
#include "UObject/SoftObjectPath.h"
#include "PCGAssembleGableRoof.generated.h"

USTRUCT()
struct FGableMeshRow
{
	GENERATED_BODY()

	UPROPERTY()
	FName Symbol = NAME_None;

	UPROPERTY()
	FSoftObjectPath MeshPath;

	UPROPERTY()
	bool bScalable = true;
};

UCLASS(BlueprintType, ClassGroup=(Procedural), meta=(DisplayName="Assemble Gable Roof (Axis-based UE5.5)"))
class BUILDINGGENERATOR_API UPCGAssembleGableRoofSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	// -------- Attributes --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes")
	FName OwnerIndexAttribute = TEXT("OwnerIndex");

	// Volume rotation attribute on input points
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes")
	FName RotationAttribute = TEXT("Rotation");

	// -------- Mesh table columns --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeshTable")
	FName ColSymbol = TEXT("Symbol");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeshTable")
	FName ColMesh = TEXT("Mesh");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeshTable")
	FName ColScalable = TEXT("bScalable");

	// -------- Placement tuning --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Placement", meta=(ClampMin="0.0", ClampMax="0.5"))
	float EdgeTolRatio = 0.02f;

	// Tile: X is length (spacing axis), Y is slope-width axis
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tile", meta=(ClampMin="0.1"))
	float TileScaleXMin = 0.9f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tile", meta=(ClampMin="0.1"))
	float TileScaleXMax = 1.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tile", meta=(ClampMin="0.0", ClampMax="3.0"))
	float TileEaveOutOffsetScale = 1.0f;

	// Cap: extra widen on Y to ensure coverage
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Cap", meta=(ClampMin="0.5", ClampMax="2.0"))
	float CapScaleYExtra = 1.05f;

	// For Xmax end, yaw 180 around local Z (deterministic)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Cap")
	bool bFlipCapAtXMax = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Debug")
	bool bWriteDebugAttrs = true;

public:
	// -------- Node identity (fix "None") --------
	virtual FName GetDefaultNodeName() const override { return TEXT("AssembleGableRoof"); }
	virtual FText GetDefaultNodeTitle() const override;

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

class FPCGAssembleGableRoofElement : public IPCGElement
{
public:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};