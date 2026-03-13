#pragma once

#include "CoreMinimal.h"

#include "PCGSettings.h"
#include "PCGElement.h"
#include "PCGContext.h"
#include "Elements/PCGCreatePointsGrid.h" // for EPCGCoordinateSpace

#include "PCGBuildingNodes.generated.h"

// ------------------------------
// Common attribute names
// ------------------------------
namespace PCGBuildingAttr
{
	static const FName Floor(TEXT("Floor"));
	static const FName LayerInFloor(TEXT("LayerInFloor"));
	static const FName GridX(TEXT("GridX"));
	static const FName GridY(TEXT("GridY"));
	static const FName GridZ(TEXT("GridZ"));

	static const FName SizeX(TEXT("SizeX"));
	static const FName SizeY(TEXT("SizeY"));
	static const FName LayersPerFloor(TEXT("LayersPerFloor"));
	static const FName FloorCount(TEXT("FloorCount"));

	static const FName ShrinkAxis(TEXT("ShrinkAxis"));         // 0=X,1=Y
	static const FName ShrinkSign(TEXT("ShrinkSign"));         // -1 or +1
	static const FName ShrinkStrength(TEXT("ShrinkStrength")); // -1..+1

	// Doc-matching / talk-matching attributes
	// Direction is bool/int 0 or 1:
	//   0 = Front shrink (trim from negative side, keep back)
	//   1 = Back  shrink (trim from positive side, keep front)
	static const FName ShrinkDirectionX(TEXT("shrink_Direction_X"));
	static const FName ShrinkDirectionY(TEXT("Shrink_Direction_Y"));
	// Percentage strength per axis (0..1)
	static const FName ShrinkPercentageX(TEXT("shrink_Percentage_X"));
	static const FName ShrinkPercentageY(TEXT("shrink_Percentage_Y"));

}


UENUM(BlueprintType)
enum class EPCGBuildingShrinkDirMode : uint8
{
	None UMETA(DisplayName="None"),
	Front UMETA(DisplayName="Front"),
	Back UMETA(DisplayName="Back"),
	Both UMETA(DisplayName="Both") // Accept both; use per-floor attribute or noise to choose front/back
};


// ============================================================
// Step 1: Generate Grid Points for the whole building volume
// ============================================================

UCLASS(BlueprintType, ClassGroup=(PCG), meta=(DisplayName="PCG Building | 01 Generate Grid"))
class BUILDINGGENERATOR_API UPCGBuildingGenerateGridSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	// --- user inputs ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(PCG_Overridable, ClampMin="1"))
	int32 Floor = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(PCG_Overridable, ClampMin="1"))
	int32 SizeX = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(PCG_Overridable, ClampMin="1"))
	int32 SizeY = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(PCG_Overridable, ClampMin="1.0"))
	float FloorHeight = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(PCG_Overridable))
	FVector CellSize = FVector(100, 100, 100);

	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(PCG_Overridable))
	EPCGCoordinateSpace CoordinateSpace = EPCGCoordinateSpace::LocalComponent;

	// optional randomness
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Random", meta=(PCG_Overridable))
	bool bEnableRandom = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Random", meta=(PCG_Overridable))
	int32 SeedOffset = 0;

	// --- UPCGSettings ---
	virtual FName GetDefaultNodeName() const override { return TEXT("PCGBuilding_GenerateGrid"); }
	virtual FText GetDefaultNodeTitle() const override { return FText::FromString(TEXT("PCG Building | Generate Grid")); }
	virtual FText GetNodeTooltipText() const override { return FText::FromString(TEXT("Generate building volume points with Floor/SizeX/SizeY/FloorHeight/CellSize and tags (Floor/GridX/GridY/GridZ/LayerInFloor).")); }

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;

protected:
	virtual FPCGElementPtr CreateElement() const override;
};

class FPCGBuildingGenerateGridElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};

// ============================================================
// Step 2: PCG - Shape - Shrink (doc-matching)
//
// Inputs are points with at least: Floor, GridX, GridY (+ optional SizeX/SizeY).
//
// Per-floor input attributes (recommended, authored upstream in PCG graph):
//  - shrink_Direction_X (int/bool 0=Front, 1=Back)
//  - Shrink_Direction_Y (int/bool 0=Front, 1=Back)
//  - shrink_Percentage_X (float 0..1)
//  - shrink_Percentage_Y (float 0..1)
//
// Node parameters:
//  - ShrinkStrength: macro multiplier applied to per-floor percentages (0..1)
//  - Steps: stepping/quantization of the final percentage (round(p*Steps)/Steps)
//  - Frequency + SeedOffset: used only when the per-floor attrs are missing (noise fallback)
// ============================================================

UCLASS(BlueprintType, ClassGroup=(PCG), meta=(DisplayName="PCG - Shape - Shrink"))
class BUILDINGGENERATOR_API UPCGBuildingShrinkByFloorSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	// --- UI params (match the talk / your screenshots) ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PCG - Shape - Shrink", meta=(PCG_Overridable, DisplayName="Shrink - Seed"))
	int32 ShrinkSeed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PCG - Shape - Shrink", meta=(PCG_Overridable, DisplayName="Shrink - Direction - X"))
	EPCGBuildingShrinkDirMode DirectionX = EPCGBuildingShrinkDirMode::Both;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PCG - Shape - Shrink", meta=(PCG_Overridable, DisplayName="Shrink - Direction - Y"))
	EPCGBuildingShrinkDirMode DirectionY = EPCGBuildingShrinkDirMode::Both;

	// Macro multiplier for per-floor percentages (0..1)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PCG - Shape - Shrink", meta=(PCG_Overridable, ClampMin="0.0", ClampMax="1.0", DisplayName="Shrink - Strength"))
	float ShrinkStrength = 0.4f;

	// Used only when input per-floor attrs are missing (noise fallback)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PCG - Shape - Shrink", meta=(PCG_Overridable, ClampMin="0.0", DisplayName="Shrink - Frequency"))
	float ShrinkFrequency = 5.0f;

	// Stepping (quantization) count. Example: Steps=2 => 0,0.5,1.0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PCG - Shape - Shrink", meta=(PCG_Overridable, ClampMin="1", ClampMax="64", DisplayName="Shrink - Steps"))
	int32 Steps = 6;

	// --- Legacy controls (optional; kept for backward compatibility) ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Legacy", meta=(PCG_Overridable))
	TArray<FString> DirectionByFloor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Legacy", meta=(PCG_Overridable))
	TArray<float> StrengthByFloor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Legacy", meta=(PCG_Overridable))
	bool bEnableRandom = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Legacy", meta=(PCG_Overridable))
	int32 SeedOffset = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Legacy", meta=(PCG_Overridable, ClampMin="0.0", ClampMax="1.0"))
	float MaxAbsStrength = 0.95f;

	virtual FName GetDefaultNodeName() const override { return TEXT("PCGShape_Shrink"); }
	virtual FText GetDefaultNodeTitle() const override { return FText::FromString(TEXT("PCG - Shape - Shrink")); }
	virtual FText GetNodeTooltipText() const override { return FText::FromString(TEXT("Doc-matching shrink: uses per-floor shrink_Direction_X/Y (0=Front,1=Back) and shrink_Percentage_X/Y (0..1), scaled by ShrinkStrength and quantized by Steps. Falls back to deterministic noise if attrs missing.")); }

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;

protected:
	virtual FPCGElementPtr CreateElement() const override;
};

class FPCGBuildingShrinkByFloorElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};

