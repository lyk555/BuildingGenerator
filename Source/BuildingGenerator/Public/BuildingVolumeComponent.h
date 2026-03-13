#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BuildingVolumeTypes.h"
#include "BuildingVolumeComponent.generated.h"

/**
 * 挂在 PCGActor 上，供自定义 PCG 节点读取 TArray<FVolumeData>
 */
UCLASS(ClassGroup=(Procedural), meta=(BlueprintSpawnableComponent))
class UBuildingVolumeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	/** 体块序列：若干 Cube 组合成一个 building massing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
	TArray<FVolumeData> Volumes;

	/** 体素化/合并时使用的 voxel size（越小越精细，成本越高） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Massing", meta=(ClampMin="1.0"))
	float VoxelSize = 50.0f;

	/** 楼层高度（用于 footprint / 支撑高度推断） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Floors", meta=(ClampMin="1.0"))
	float FloorHeight = 400.0f;

	/** 生成 footprint 时的 2D 采样间距（通常=VoxelSize 或其倍数） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Floors", meta=(ClampMin="1.0"))
	float FootprintStep = 50.0f;

	/** Overhang 边界点沿边采样的间距（支撑间距可在后续节点覆盖） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building|Supports", meta=(ClampMin="1.0"))
	float BoundaryStep = 100.0f;
};
