#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGContext.h"
#include "PCGPin.h"
#include "Data/PCGPointData.h"
#include "BuildingVolumeComponent.h"
#include "PCGBuildingVolumeNodes.generated.h"

namespace PCGBuildingPins
{
	static const FName VolumesOut(TEXT("Volumes"));
	static const FName VolumesIn(TEXT("Volumes"));

	static const FName ShellOut(TEXT("ShellVoxels"));
	static const FName FootprintOut(TEXT("Footprint2D"));
	static const FName OverhangBoundaryOut(TEXT("OverhangBoundary"));

	static const FName SupportsOut(TEXT("SupportPoints"));
	static const FName BoundaryIn(TEXT("OverhangBoundary"));

	static const FName PointsIn(TEXT("Points"));
	static const FName PointsOut(TEXT("Points"));
}

/**
 * Node 1: 从 OwnerActor 上的 UBuildingVolumeComponent 读取 TArray<FVolumeData> 并输出 PointData
 */
UCLASS(BlueprintType, ClassGroup=(PCG))
class UPCGVolumeArrayToPointsSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	/** 允许从 OwnerActor 的指定 Component 名称读取（为空则自动找第一个 UBuildingVolumeComponent） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	FName ComponentName = NAME_None;

	/** 输出点的默认 density */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output", meta=(ClampMin="0.0"))
	float Density = 1.0f;

	virtual FName GetDefaultNodeName() const override { return TEXT("VolumeArrayToPoints"); }
	virtual FText GetDefaultNodeTitle() const override { return FText::FromString(TEXT("Building: VolumeArray → Points")); }
	virtual FText GetNodeTooltipText() const override { return FText::FromString(TEXT("Read UBuildingVolumeComponent::Volumes from OwnerActor and output them as PointData with metadata.")); }

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

/**
 * Node 2: 体素化 Union + 外壳提取 + footprint(按楼层) + overhang boundary(上层-下层差分的边界点)
 */
UCLASS(BlueprintType, ClassGroup=(PCG))
class UPCGVoxelUnionMassingSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	/** voxel size（覆盖 component 的默认值；<=0 表示用 component 里的 VoxelSize） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Massing", meta=(ClampMin="0.0"))
	float VoxelSizeOverride = 0.0f;

	/** 楼层高度（覆盖 component 的默认值；<=0 表示用 component 的 FloorHeight） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Floors", meta=(ClampMin="0.0"))
	float FloorHeightOverride = 0.0f;

	/** 只输出“外壳体素”（内部体素丢弃） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Massing")
	bool bShellOnly = true;

	/** 输出点 density */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output", meta=(ClampMin="0.0"))
	float Density = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Massing")
	bool bUseFloorIndexAttribute = false;


	virtual FName GetDefaultNodeName() const override { return TEXT("VoxelUnionMassing"); }
	virtual FText GetDefaultNodeTitle() const override { return FText::FromString(TEXT("Building: Voxel Union Massing")); }
	virtual FText GetNodeTooltipText() const override { return FText::FromString(TEXT("Voxelize volume points, union them, extract shell voxels and floor footprints, compute overhang boundary points.")); }

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

/**
 * Node 3: 从 OverhangBoundary 生成支撑点（柱/拱/梁由 SupportGrammar attribute 控制）
 */
UCLASS(BlueprintType, ClassGroup=(PCG))
class UPCGOverhangSupportsSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	/** 支撑间距（<=0 表示用 component BoundaryStep） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Supports", meta=(ClampMin="0.0"))
	float SupportSpacingOverride = 0.0f;

	/** 支撑向下延伸的最大高度（<=0 表示使用 FloorHeight * (FloorIndex+1)） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Supports", meta=(ClampMin="0.0"))
	float MaxDropHeightOverride = 0.0f;

	/** 支撑点 density */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output", meta=(ClampMin="0.0"))
	float Density = 1.0f;

	/** 若输入点没有 SupportGrammar，则用该默认值 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Supports")
	FString DefaultSupportGrammar = TEXT("[P][A]*[P]");

	virtual FName GetDefaultNodeName() const override { return TEXT("OverhangSupports"); }
	virtual FText GetDefaultNodeTitle() const override { return FText::FromString(TEXT("Building: Overhang → Supports")); }
	virtual FText GetNodeTooltipText() const override { return FText::FromString(TEXT("Generate vertical support points under overhang boundary points, carries SupportGrammar attribute for downstream ShapeGrammar.")); }

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

/**
 * Node 4: Grammar(字符串) → Mesh(SoftObjectPath) + 自动 Fit Scale 到点 Bounds/Extents
 *
 * 用法：
 * 1) 输入点上要有 SupportGrammar 或 FacadeGrammar（你在设置里选 GrammarAttributeName）
 * 2) 在该节点 settings 里配置 SymbolToMesh：例如 "P"->柱子mesh, "A"->拱mesh
 * 3) 输出点会新增/覆盖 MeshAttributeName（默认 "Mesh"），StaticMeshSpawner 选 PCGMeshSelectorByAttribute 即可。:contentReference[oaicite:2]{index=2}
 */
UCLASS(BlueprintType, ClassGroup=(PCG))
class UPCGGrammarToMeshSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	/** 输入点上的 grammar 属性名（SupportGrammar / FacadeGrammar / RoofGrammar ...） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grammar")
	FName GrammarAttributeName = TEXT("SupportGrammar");

	/** 输出 mesh 属性名（SoftObjectPath），建议固定用 "Mesh" */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output")
	FName MeshAttributeName = TEXT("Mesh");

	/** 可选：输入点上的 ExtentsWS（HalfExtents）属性名；为空则用点 Bounds 作为目标尺寸 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Fit")
	FName ExtentsAttributeName = TEXT("ExtentsWS");

	/** true：按目标尺寸/mesh bounds 计算点 Transform.Scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Fit")
	bool bFitScaleToTarget = true;

	/** true：不允许非均匀缩放（uniform scale） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Fit")
	bool bForceUniformScale = true;

	/** uniform/non-uniform scale clamp */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Fit", meta=(ClampMin="0.01"))
	FVector2D ScaleClamp = FVector2D(0.92f, 1.08f);

	/** grammar token/symbol → mesh */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mapping")
	TMap<FString, TSoftObjectPtr<UStaticMesh>> SymbolToMesh;

	/** 找不到时用这个 mesh（可为空） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mapping")
	TSoftObjectPtr<UStaticMesh> DefaultMesh;

	virtual FName GetDefaultNodeName() const override { return TEXT("GrammarToMesh"); }
	virtual FText GetDefaultNodeTitle() const override { return FText::FromString(TEXT("Building: Grammar → Mesh Attribute (Fit Bounds)")); }
	virtual FText GetNodeTooltipText() const override { return FText::FromString(TEXT("Parse grammar string, map first token to a mesh SoftObjectPath attribute, optionally fit point scale to bounds/extents.")); }

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

UENUM(BlueprintType)
enum class EPCGTileAxis : uint8
{
	X UMETA(DisplayName="X"),
	Y UMETA(DisplayName="Y"),
	Z UMETA(DisplayName="Z"),
	LongestLocalAxis UMETA(DisplayName="Longest (Local Bounds)")
};

/**
 * Node 5: Resolve Symbol/Grammar -> Mesh Attribute from DataAsset,
 *         and tile along axis with "only one scaled remainder" policy.
 */
UCLASS(BlueprintType, ClassGroup=(PCG))
class UPCGResolveMeshFromLibrarySettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	// ----- Input -----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	bool bUseGrammarAttribute = true;

	// when bUseGrammarAttribute=true, read this string attr and extract first token like Node4
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input", meta=(EditCondition="bUseGrammarAttribute"))
	FName GrammarAttributeName = TEXT("SupportGrammar");

	// when bUseGrammarAttribute=false, read Symbol directly (FName preferred; FString also supported)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input", meta=(EditCondition="!bUseGrammarAttribute"))
	FName SymbolAttributeName = TEXT("Symbol");

	// optional: target half extents (world) for computing L; empty => use point Bounds
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	FName ExtentsAttributeName = TEXT("ExtentsWS");

	// ----- Library -----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Library")
	TObjectPtr<UGrammarMeshLibraryDataAsset> Library = nullptr;

	// fallback if not found in Library
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Library")
	TSoftObjectPtr<UStaticMesh> DefaultMesh;

	// ----- Output -----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output")
	FName MeshAttributeName = TEXT("Mesh"); // FSoftObjectPath

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output")
	FName MeshFullNameAttributeName = TEXT("MeshFullName"); // FString

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output")
	FName OutSymbolAttributeName = TEXT("Symbol"); // FName

	// ----- Tiling policy -----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tiling")
	bool bEnableTiling = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tiling")
	EPCGTileAxis TileAxis = EPCGTileAxis::LongestLocalAxis;

	// small remainder will be merged into the last full segment (still only one scaled mesh)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tiling", meta=(ClampMin="0.0"))
	float MinRemainderWorld = 1.0f; // cm

	// clamp remainder scale along tile axis
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tiling", meta=(ClampMin="0.01"))
	FVector2D RemainderScaleClamp = FVector2D(0.25f, 1.25f);

	// debug attrs
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tiling")
	FName SegmentIndexAttributeName = TEXT("SegmentIndex");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tiling")
	FName SegmentCountAttributeName = TEXT("SegmentCount");

	virtual FName GetDefaultNodeName() const override { return TEXT("ResolveMeshFromLibrary"); }
	virtual FText GetDefaultNodeTitle() const override { return FText::FromString(TEXT("Building: Resolve Mesh From Library (Tile, Single-Scale Remainder)")); }
	virtual FText GetNodeTooltipText() const override { return FText::FromString(TEXT("Resolve grammar/symbol using a DataAsset library and tile meshes along axis. Guarantees at most one scaled segment.")); }

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

/**
 * Node 6: 解析完整 bracket grammar（prefix + repeat* + suffix），从 DataAsset 查 mesh，
 *        动态读 mesh bounds 做分段，最多只允许最后一段缩放；并做 pivot/bounds-origin 修正。
 */
UCLASS(BlueprintType, ClassGroup=(PCG))
class UPCGGrammarSegmentsFromLibrarySettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	FName GrammarAttributeName = TEXT("FacadeGrammar");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	FName ExtentsAttributeName = TEXT("ExtentsWS");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	FName AllowScaleAttributeName = TEXT("AllowNonUniformScale");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	FName ScaleClampAttributeName = TEXT("ScaleClamp");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Library")
	TObjectPtr<UGrammarMeshLibraryDataAsset> Library = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Library")
	TSoftObjectPtr<UStaticMesh> DefaultMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tiling")
	EPCGTileAxis TileAxis = EPCGTileAxis::LongestLocalAxis;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tiling", meta=(ClampMin="0.0"))
	float MinRemainderWorld = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tiling", meta=(ClampMin="0.01"))
	FVector2D RemainderScaleClamp = FVector2D(0.25f, 1.25f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Placement")
	bool bPivotFixByBoundsOrigin = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output")
	FName MeshAttributeName = TEXT("Mesh");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output")
	FName MeshFullNameAttributeName = TEXT("MeshFullName");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output")
	FName OutSymbolAttributeName = TEXT("Symbol");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output")
	FName SegmentIndexAttributeName = TEXT("SegmentIndex");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Output")
	FName SegmentCountAttributeName = TEXT("SegmentCount");

	virtual FName GetDefaultNodeName() const override { return TEXT("GrammarSegmentsFromLibrary"); }
	virtual FText GetDefaultNodeTitle() const override { return FText::FromString(TEXT("Building: Grammar Segments From Library (Full Grammar, Pivot Fix, Single-Scale Tail)")); }
	virtual FText GetNodeTooltipText() const override { return FText::FromString(TEXT("Parse full bracket grammar ([A][B,C]*[D]) and expand to mesh segment points using library mesh bounds. At most one scaled tail segment. Pivot fixed by bounds origin.")); }

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};