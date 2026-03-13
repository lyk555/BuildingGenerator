#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h" // IPCGElement, FPCGElementPtr, FPCGContext
#include "PCGAssignDoorGrammar.generated.h"

UENUM(BlueprintType)
enum class EPCGDoorInjectPolicy : uint8
{
	CornerFillCorner UMETA(DisplayName="Corner-Fill-Corner (mirror fill)"),
	InsertBeforeLast UMETA(DisplayName="Insert Before Last Token"),
	Append UMETA(DisplayName="Append Door Token")
};

UCLASS(BlueprintType, ClassGroup=(Procedural))
class BUILDINGGENERATOR_API UPCGAssignDoorGrammarSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return TEXT("AssignDoorGrammarPerOwner"); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("PCGDoor", "Title", "Assign Door Grammar Per Owner"); }
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGDoor", "Tooltip",
			"Groups segment points by OwnerIndex, chooses exactly one edge per group, injects Door token into Grammar for that edge only.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Spatial; }
#endif

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;

protected:
	virtual FPCGElementPtr CreateElement() const override;

public:
	// ---- input attributes ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes", meta=(PCG_Overridable))
	FName OwnerIndexAttribute = TEXT("OwnerIndex");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes", meta=(PCG_Overridable))
	FName HaveDoorAttribute = TEXT("HaveDoor");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes", meta=(PCG_Overridable))
	FName DoorXAttribute = TEXT("Door_X");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes", meta=(PCG_Overridable))
	FName DoorYAttribute = TEXT("Door_Y");

	// Tangent attribute from Spline to Segment (enable Extract Tangents)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Attributes", meta=(PCG_Overridable))
	FName TangentAttribute = TEXT("Tangent");

	// ---- grammar attributes ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grammar", meta=(PCG_Overridable))
	FName GrammarAttribute = TEXT("Grammar");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grammar", meta=(PCG_Overridable))
	FName OutputGrammarAttribute = TEXT("Grammar");

	// Door token (must match your Modules Info symbol mapping)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grammar", meta=(PCG_Overridable))
	FString DoorToken = TEXT("[D]");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grammar", meta=(PCG_Overridable))
	EPCGDoorInjectPolicy InjectPolicy = EPCGDoorInjectPolicy::CornerFillCorner;

	// Remove DoorToken from non-door edges
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grammar", meta=(PCG_Overridable))
	bool bStripDoorOnNonDoorEdges = true;
};

//  直接继承 IPCGElement；不要写你版本里不存在的 SupportsBasePointDataInputs
class FPCGAssignDoorGrammarPerOwnerElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};
