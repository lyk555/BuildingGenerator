#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGDeoverlapFootprintsPoints.generated.h"

/**
 * De-overlap 2D footprint vertex points (grouped by OwnerIndex/RingId), by subtracting previously accepted footprints.
 * Output is still point data (vertices) with attributes for downstream spline/mesh logic.
 */
UCLASS(BlueprintType, ClassGroup=(Procedural))
class BUILDINGGENERATOR_API UPCGDeoverlapFootprintsPointsSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPCGDeoverlapFootprintsPointsSettings();

	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override;
	virtual FText GetDefaultNodeTitle() const override;
	virtual FText GetNodeTooltipText() const override;
#endif
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings interface

public:
	/** Scale factor for float->int conversion when calling Clipper2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Clipper")
	double IntScale = 1000.0;

	/** Fill rule for boolean ops. NonZero is usually stable for footprints. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Clipper")
	bool bUseEvenOddFillRule = false;

	/** Z overlap tolerance (world units). Two footprints only subtract each other if their Z ranges overlap within this tolerance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Clipper")
	double ZOverlapTolerance = 1.0;

	/** If true, force close loop for each ring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	bool bForceCloseLoop = true;

	// -------------------------
	// Input attribute names
	// -------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input Attributes")
	FName OwnerIndexAttribute = TEXT("OwnerIndex");

	/** Vertex order inside one ring. 默认改成 VertexIndex（你上游就是这个）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input Attributes")
	FName PointIndexAttribute = TEXT("VertexIndex");

	/** Optional: which ring/loop this vertex belongs to (0 if absent). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input Attributes")
	FName RingIdAttribute = TEXT("RingId");

	/** Preferred extrude vector attribute (XYZ). If missing, we'll fallback to Extents.Z*2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input Attributes")
	FName ExtrudeVectorAttribute = TEXT("ExtrudeVector");

	/** Fallback extents attribute when ExtrudeVector is missing. If found, we use Extents.Z * 2 as height (e.g. GridExtents). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input Attributes")
	FName FallbackExtentsAttribute = TEXT("GridExtents");

	// ---- Volume Z source (for Z overlap gating) ----
	/** If true and VolumeCenter/VolumeExtents attrs exist, Z overlap uses Center.Z±Extents.Z instead of point Z + Extrude. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Z Overlap")
	bool bUseVolumeZForOverlap = true;

	/** If true, output footprint points' Z will be set to the volume BottomZ (Center.Z-Extents.Z) when VolumeZ is available. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Z Overlap")
	bool bWritePointZAsBottomZ = true;

	/** Volume center attribute (FVector). Used to compute BottomZ/TopZ for overlap gating. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input Attributes")
	FName VolumeCenterAttribute = TEXT("CenterPosition");

	/** Volume extents attribute (FVector). Used to compute BottomZ/TopZ for overlap gating. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input Attributes")
	FName VolumeExtentsAttribute = TEXT("GridExtents");

	// -------------------------
	// Output attribute names
	// -------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output Attributes")
	FName OutOwnerIndexAttribute = TEXT("OwnerIndex");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output Attributes")
	FName OutRingIdAttribute = TEXT("RingId");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output Attributes")
	FName OutVertexIndexAttribute = TEXT("VertexIndex");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output Attributes")
	FName OutExtrudeVectorAttribute = TEXT("ExtrudeVector");

	/** Passthrough volume center attribute so downstream nodes (supports) can read true Z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output Attributes")
	FName OutVolumeCenterAttribute = TEXT("CenterPosition");

	/** Passthrough volume extents attribute so downstream nodes (supports) can read true Z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output Attributes")
	FName OutVolumeExtentsAttribute = TEXT("GridExtents");

};
