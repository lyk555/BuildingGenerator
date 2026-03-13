#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BuildingGeneratorComponent.generated.h"

UENUM(BlueprintType)
enum class EBGAxis : uint8
{
	X UMETA(DisplayName="X"),
	Y UMETA(DisplayName="Y"),
};

UENUM(BlueprintType)
enum class EBGFace : uint8
{
	North UMETA(DisplayName="North(+Y)"),
	South UMETA(DisplayName="South(-Y)"),
	East  UMETA(DisplayName="East(+X)"),
	West  UMETA(DisplayName="West(-X)"),
};

USTRUCT(BlueprintType)
struct FFloorShrinkRule
{
	GENERATED_BODY()

	// X or Y axis shrink
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Shrink")
	EBGAxis Axis = EBGAxis::X;

	// +1 => cut from Min side, -1 => cut from Max side
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Shrink", meta=(ClampMin="-1", ClampMax="1"))
	int32 Sign = 1;

	// 0..1 continuous
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Shrink", meta=(ClampMin="0.0", ClampMax="1.0"))
	float Strength = 0.0f;
};

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class BUILDINGGENERATOR_API UBuildingGeneratorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBuildingGeneratorComponent();

	// =========================
	// User Params
	// =========================

	// footprint cells (X/Y)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Params", meta=(ClampMin="1"))
	int32 SizeX = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Params", meta=(ClampMin="1"))
	int32 SizeY = 5;

	// Floors count
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Params", meta=(ClampMin="1"))
	int32 Floors = 3;

	// Per-floor height in centimeters
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Params", meta=(ClampMin="1.0"))
	float FloorHeightCm = 200.0f;

	// cell size in centimeters (X/Y/Z)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Params")
	FVector CellSizeCm = FVector(100.0f, 100.0f, 100.0f);

	// If empty, uses default rule for all floors
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Shrink")
	TArray<FFloorShrinkRule> PerFloorShrink;

	// Wall segment sizes in cells (greedy). Default {4,2,1}
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Modules")
	TArray<int32> WallModuleSizes = { 4, 2, 1 };

	// Roof patch sizes in cells (greedy square). Default {4,2,1}
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Modules")
	TArray<int32> RoofPatchSizes = { 4, 2, 1 };

	// =========================
	// Meshes (optional, can be null; debug still works)
	// Assumption: Meshes are authored in "cell units":
	// - Wall meshes length in X axis equals N cells for WallN meshes
	// - Wall meshes thickness roughly 1 cell in Y
	// - Wall meshes height roughly 1 "floor" (we scale Z to match)
	// - Floor/Roof meshes are 1 cell square (we scale for patches)
	// =========================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Meshes")
	TObjectPtr<UStaticMesh> CornerMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Meshes")
	TObjectPtr<UStaticMesh> WallMesh1 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Meshes")
	TObjectPtr<UStaticMesh> WallMesh2 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Meshes")
	TObjectPtr<UStaticMesh> WallMesh4 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Meshes")
	TObjectPtr<UStaticMesh> FloorMesh1 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Meshes")
	TObjectPtr<UStaticMesh> RoofMesh1 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Meshes")
	TObjectPtr<UStaticMesh> RoofMesh2 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Meshes")
	TObjectPtr<UStaticMesh> RoofMesh4 = nullptr;

	// =========================
	// Debug
	// =========================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Debug")
	bool bDebugDraw = true;

	// Debug draw duration (seconds). 0 => single frame, <0 => persistent
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Debug")
	float DebugDuration = -1.0f;

	// Draw each occupied cell box (can be heavy)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Debug")
	bool bDebugDrawCells = false;

	// Draw derived bounds (min/max per floor)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Debug")
	bool bDebugDrawBounds = true;

	// =========================
	// Editor / Blueprint Controls
	// =========================

	UFUNCTION(CallInEditor, BlueprintCallable, Category="Building|Generate")
	void Generate();

	UFUNCTION(CallInEditor, BlueprintCallable, Category="Building|Generate")
	void ClearGenerated();

private:
	// Instanced components
	UPROPERTY(Transient)
	TObjectPtr<class UInstancedStaticMeshComponent> ISM_Corner = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UInstancedStaticMeshComponent> ISM_Wall1 = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UInstancedStaticMeshComponent> ISM_Wall2 = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UInstancedStaticMeshComponent> ISM_Wall4 = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UInstancedStaticMeshComponent> ISM_Floor1 = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UInstancedStaticMeshComponent> ISM_Roof1 = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UInstancedStaticMeshComponent> ISM_Roof2 = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UInstancedStaticMeshComponent> ISM_Roof4 = nullptr;

	// Internal helpers
	UInstancedStaticMeshComponent* EnsureISM(TObjectPtr<class UInstancedStaticMeshComponent>& InOut, const FName& Name, UStaticMesh* Mesh);

	int32 GetZPerFloor() const;
	float GetActualFloorHeightCm() const;

	FFloorShrinkRule GetRuleForFloor(int32 FloorIndex) const;

	// 2D occ per floor: Occ[Floor][x + y*SizeX] is 0/1
	void BuildOcc(TArray<uint8>& OutOcc) const;
	FORCEINLINE int32 Idx2D(int32 X, int32 Y) const { return X + Y * SizeX; }
	FORCEINLINE bool GetOcc(const TArray<uint8>& Occ, int32 FloorIndex, int32 X, int32 Y) const
	{
		const int32 Base = FloorIndex * SizeX * SizeY;
		return Occ.IsValidIndex(Base + Idx2D(X,Y)) ? (Occ[Base + Idx2D(X,Y)] != 0) : false;
	}
	FORCEINLINE void SetOcc(TArray<uint8>& Occ, int32 FloorIndex, int32 X, int32 Y, bool bVal) const
	{
		const int32 Base = FloorIndex * SizeX * SizeY;
		if (Occ.IsValidIndex(Base + Idx2D(X,Y)))
		{
			Occ[Base + Idx2D(X,Y)] = bVal ? 1 : 0;
		}
	}

	// For each floor, compute rectangular bounds of occupied cells after shrink
	// Returns false if the floor has no occupied cells.
	bool ComputeBoundsRect(const TArray<uint8>& Occ, int32 FloorIndex, int32& OutMinX, int32& OutMaxX, int32& OutMinY, int32& OutMaxY) const;

	// Emit instances
	FTransform MakeCellTransform(int32 X, int32 Y, float ZCm, const FRotator& Rot, const FVector& Scale) const;

	void EmitFloorTilesRect(int32 FloorIndex, int32 MinX, int32 MaxX, int32 MinY, int32 MaxY);
	void EmitCorners(int32 FloorIndex, int32 MinX, int32 MaxX, int32 MinY, int32 MaxY);

	void EmitWallFaceSegments(int32 FloorIndex, EBGFace Face, int32 MinX, int32 MaxX, int32 MinY, int32 MaxY);
	void EmitRoofFromOcc(const TArray<uint8>& Occ);

	// Roof helper: greedy square packing for tiles in a given floor
	void RoofPackFloorTiles(
		int32 FloorIndex,
		const TArray<uint8>& RoofTiles, // size SizeX*SizeY, 0/1
		TArray<uint8>& Used,            // size SizeX*SizeY, 0/1
		int32 PatchSizeCells);

	// Debug draw
	void DebugDrawFloor(int32 FloorIndex, int32 MinX, int32 MaxX, int32 MinY, int32 MaxY) const;
	void DebugDrawOccCells(int32 FloorIndex, const TArray<uint8>& Occ) const;
};
