#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Engine/DataAsset.h"
#include "BuildingVolumeTypes.generated.h"

/**
 * 单个体块（Cube/OBB）描述：用于“先体量合并、再贴皮/grammar”的 building 生成
 */
USTRUCT(BlueprintType)
struct FVolumeData
{
	GENERATED_BODY()

	/** 体块中心（世界坐标） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Volume")
	FVector CenterWS = FVector::ZeroVector;

	/** 体块半尺寸（世界单位，Half Extents） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Volume", meta=(ClampMin="0.0"))
	FVector ExtentsWS = FVector(200, 200, 200);

	/** 体块朝向（世界空间） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Volume")
	FRotator RotationWS = FRotator::ZeroRotator;

	/** 楼层索引（0=首层） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Volume")
	int32 FloorIndex = 0;

	/** 该体块用于立面/楼板/屋顶的 ShapeGrammar 字符串（你可自己约定语法） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grammar")
	FString FacadeGrammar;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grammar")
	FString RoofGrammar;

	/** 若该体块形成悬挑，其支撑的 grammar（柱/拱/梁） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grammar")
	FString SupportGrammar;

	/**
	 * 缩放策略：
	 * - bAllowNonUniformScale=false：强制不缩放（推荐优先靠 filler 模块补缝）
	 * - true：允许作为 Plan-B 缩放（受 clamp 限制）
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ScalePolicy")
	bool bAllowNonUniformScale = false;

	/** 非均匀缩放允许区间（例如 0.92~1.08） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ScalePolicy", meta=(ClampMin="0.1"))
	FVector2D ScaleClamp = FVector2D(0.92f, 1.08f);

	/** 可选：体块类别/风格标签（用于在 PCG 里过滤或选不同 grammar/mesh） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tags")
	FName StyleTag = NAME_None;

	/** 可选：优先级（例如核心体块 > 次体块，用于冲突消解时的规则） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tags")
	int32 Priority = 0;
};

USTRUCT(BlueprintType)
struct FGrammarMeshEntry
{
	GENERATED_BODY()

	// grammar token, e.g. "P", "A", "Wall_01"
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GrammarMesh")
	FName Symbol = NAME_None;

	// mesh to spawn
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GrammarMesh")
	TSoftObjectPtr<UStaticMesh> Mesh;

	// full display name for this module
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GrammarMesh")
	FString FullName;

	// optional: syntax/help text for this symbol/module
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GrammarMesh", meta=(MultiLine="true"))
	FString GrammarSyntax;

	// optional: allow remainder scaling for this symbol (some modules must be exact)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GrammarMesh")
	bool bAllowRemainderScale = true;
};

UCLASS(BlueprintType)
class UGrammarMeshLibraryDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GrammarMesh")
	TArray<FGrammarMeshEntry> Entries;
};