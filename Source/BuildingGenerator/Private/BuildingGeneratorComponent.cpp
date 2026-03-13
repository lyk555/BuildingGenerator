#include "BuildingGeneratorComponent.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

UBuildingGeneratorComponent::UBuildingGeneratorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

UInstancedStaticMeshComponent* UBuildingGeneratorComponent::EnsureISM(
	TObjectPtr<UInstancedStaticMeshComponent>& InOut,
	const FName& Name,
	UStaticMesh* Mesh)
{
	if (!GetOwner())
	{
		return nullptr;
	}

	// 如果 Mesh 为空：不创建组件（避免你不想要的子组件出现）
	// 注意：如果你希望“即使 Mesh 空也创建一个组件”，把这段 if 删掉即可。
	if (!Mesh)
	{
		return nullptr;
	}

	if (!InOut)
	{
		InOut = NewObject<UInstancedStaticMeshComponent>(GetOwner(), Name, RF_Transactional);
		InOut->CreationMethod = EComponentCreationMethod::Instance;
		GetOwner()->AddInstanceComponent(InOut);

		// 只有在 Generate 时才会走到这里，所以此时 attach/register 是你想要的
		InOut->SetupAttachment(GetOwner()->GetRootComponent());
		InOut->RegisterComponent();

		InOut->SetMobility(EComponentMobility::Movable);
		InOut->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// 每次 Generate 都同步 mesh（允许你在 Details 改 mesh 后重生成）
	InOut->SetStaticMesh(Mesh);
	return InOut;
}


int32 UBuildingGeneratorComponent::GetZPerFloor() const
{
	const float CZ = FMath::Max(1.0f, CellSizeCm.Z);
	const int32 ZPer = FMath::Max(1, FMath::CeilToInt(FloorHeightCm / CZ));
	return ZPer;
}

float UBuildingGeneratorComponent::GetActualFloorHeightCm() const
{
	// Snap actual floor height to the lattice defined by CellSizeZ and ZPerFloor
	return GetZPerFloor() * FMath::Max(1.0f, CellSizeCm.Z);
}

FFloorShrinkRule UBuildingGeneratorComponent::GetRuleForFloor(int32 FloorIndex) const
{
	if (PerFloorShrink.IsValidIndex(FloorIndex))
	{
		return PerFloorShrink[FloorIndex];
	}

	// Default: no shrink
	FFloorShrinkRule R;
	R.Axis = EBGAxis::X;
	R.Sign = 1;
	R.Strength = 0.0f;
	return R;
}

void UBuildingGeneratorComponent::ClearGenerated()
{
	if (ISM_Corner) ISM_Corner->ClearInstances();
	if (ISM_Wall1)  ISM_Wall1->ClearInstances();
	if (ISM_Wall2)  ISM_Wall2->ClearInstances();
	if (ISM_Wall4)  ISM_Wall4->ClearInstances();
	if (ISM_Floor1) ISM_Floor1->ClearInstances();
	if (ISM_Roof1)  ISM_Roof1->ClearInstances();
	if (ISM_Roof2)  ISM_Roof2->ClearInstances();
	if (ISM_Roof4)  ISM_Roof4->ClearInstances();
}

void UBuildingGeneratorComponent::Generate()
{
	if (!GetWorld() || !GetOwner())
	{
		return;
	}

	// sanity
	SizeX = FMath::Max(1, SizeX);
	SizeY = FMath::Max(1, SizeY);
	Floors = FMath::Max(1, Floors);
	FloorHeightCm = FMath::Max(1.0f, FloorHeightCm);
	CellSizeCm.X = FMath::Max(1.0f, CellSizeCm.X);
	CellSizeCm.Y = FMath::Max(1.0f, CellSizeCm.Y);
	CellSizeCm.Z = FMath::Max(1.0f, CellSizeCm.Z);

	// sort module sizes descending (ensure greedy is correct)
	WallModuleSizes.Sort([](int32 A, int32 B) { return A > B; });
	RoofPatchSizes.Sort([](int32 A, int32 B) { return A > B; });

	// clear old
	ClearGenerated();

	// 只在 Generate 时创建/注册 ISM，不要在 OnRegister/构造时做
	EnsureISM(ISM_Corner, TEXT("BG_ISM_Corner"), CornerMesh.Get());
	EnsureISM(ISM_Wall1, TEXT("BG_ISM_Wall1"), WallMesh1.Get());
	EnsureISM(ISM_Wall2, TEXT("BG_ISM_Wall2"), WallMesh2.Get());
	EnsureISM(ISM_Wall4, TEXT("BG_ISM_Wall4"), WallMesh4.Get());
	EnsureISM(ISM_Floor1, TEXT("BG_ISM_Floor1"), FloorMesh1.Get());
	EnsureISM(ISM_Roof1, TEXT("BG_ISM_Roof1"), RoofMesh1.Get());
	EnsureISM(ISM_Roof2, TEXT("BG_ISM_Roof2"), RoofMesh2.Get());
	EnsureISM(ISM_Roof4, TEXT("BG_ISM_Roof4"), RoofMesh4.Get());


	// Build occ (2D per floor) after shrink
	TArray<uint8> Occ;
	BuildOcc(Occ);

	// For each floor: compute rectangular bounds, then emit floor/corners/walls
	for (int32 f = 0; f < Floors; ++f)
	{
		int32 MinX = 0, MaxX = -1, MinY = 0, MaxY = -1;
		if (!ComputeBoundsRect(Occ, f, MinX, MaxX, MinY, MaxY))
		{
			continue;
		}

		if (bDebugDraw && bDebugDrawBounds)
		{
			DebugDrawFloor(f, MinX, MaxX, MinY, MaxY);
		}
		if (bDebugDraw && bDebugDrawCells)
		{
			DebugDrawOccCells(f, Occ);
		}

		// Floor tiles (rect)
		EmitFloorTilesRect(f, MinX, MaxX, MinY, MaxY);

		// Corners
		EmitCorners(f, MinX, MaxX, MinY, MaxY);

		// Walls: 4 faces
		EmitWallFaceSegments(f, EBGFace::North, MinX, MaxX, MinY, MaxY);
		EmitWallFaceSegments(f, EBGFace::South, MinX, MaxX, MinY, MaxY);
		EmitWallFaceSegments(f, EBGFace::East, MinX, MaxX, MinY, MaxY);
		EmitWallFaceSegments(f, EBGFace::West, MinX, MaxX, MinY, MaxY);
	}

	// Roof: exposed top surfaces across floors (supports terraces)
	EmitRoofFromOcc(Occ);
}

void UBuildingGeneratorComponent::BuildOcc(TArray<uint8>& OutOcc) const
{
	OutOcc.SetNumZeroed(Floors * SizeX * SizeY);

	// init full rect = true
	for (int32 f = 0; f < Floors; ++f)
	{
		for (int32 y = 0; y < SizeY; ++y)
		{
			for (int32 x = 0; x < SizeX; ++x)
			{
				SetOcc(OutOcc, f, x, y, true);
			}
		}
	}

	// apply per-floor shrink (rect cut => still rect)
	for (int32 f = 0; f < Floors; ++f)
	{
		const FFloorShrinkRule R = GetRuleForFloor(f);
		const float s = FMath::Clamp(R.Strength, 0.0f, 1.0f);
		const int32 Sign = (R.Sign >= 0) ? 1 : -1;

		if (s <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		if (R.Axis == EBGAxis::X)
		{
			const int32 NX = SizeX;
			for (int32 y = 0; y < SizeY; ++y)
			{
				for (int32 x = 0; x < SizeX; ++x)
				{
					const float u = (float(x) + 0.5f) / float(NX); // 0..1
					bool bKeep = true;
					if (Sign > 0)
					{
						// cut from minX side: remove u < s
						bKeep = (u >= s);
					}
					else
					{
						// cut from maxX side: remove u > 1-s
						bKeep = (u <= (1.0f - s));
					}
					if (!bKeep)
					{
						SetOcc(OutOcc, f, x, y, false);
					}
				}
			}
		}
		else // Y
		{
			const int32 NY = SizeY;
			for (int32 y = 0; y < SizeY; ++y)
			{
				const float v = (float(y) + 0.5f) / float(NY); // 0..1
				for (int32 x = 0; x < SizeX; ++x)
				{
					bool bKeep = true;
					if (Sign > 0)
					{
						// cut from minY side
						bKeep = (v >= s);
					}
					else
					{
						// cut from maxY side
						bKeep = (v <= (1.0f - s));
					}
					if (!bKeep)
					{
						SetOcc(OutOcc, f, x, y, false);
					}
				}
			}
		}
	}
}

bool UBuildingGeneratorComponent::ComputeBoundsRect(const TArray<uint8>& Occ, int32 FloorIndex, int32& OutMinX, int32& OutMaxX, int32& OutMinY, int32& OutMaxY) const
{
	int32 MinX = INT_MAX, MinY = INT_MAX;
	int32 MaxX = INT_MIN, MaxY = INT_MIN;

	for (int32 y = 0; y < SizeY; ++y)
	{
		for (int32 x = 0; x < SizeX; ++x)
		{
			if (GetOcc(Occ, FloorIndex, x, y))
			{
				MinX = FMath::Min(MinX, x);
				MinY = FMath::Min(MinY, y);
				MaxX = FMath::Max(MaxX, x);
				MaxY = FMath::Max(MaxY, y);
			}
		}
	}

	if (MaxX < MinX || MaxY < MinY)
	{
		return false;
	}

	OutMinX = MinX; OutMaxX = MaxX;
	OutMinY = MinY; OutMaxY = MaxY;
	return true;
}

FTransform UBuildingGeneratorComponent::MakeCellTransform(int32 X, int32 Y, float ZCm, const FRotator& Rot, const FVector& Scale) const
{
	const FVector LocalPos(
		(float(X) + 0.5f) * CellSizeCm.X,
		(float(Y) + 0.5f) * CellSizeCm.Y,
		ZCm
	);

	const FTransform OwnerTM = GetOwner()->GetActorTransform();
	const FVector WorldPos = OwnerTM.TransformPosition(LocalPos);
	const FQuat WorldRot = (OwnerTM.GetRotation() * Rot.Quaternion());
	return FTransform(WorldRot, WorldPos, Scale);
}

void UBuildingGeneratorComponent::EmitFloorTilesRect(int32 FloorIndex, int32 MinX, int32 MaxX, int32 MinY, int32 MaxY)
{
	if (!ISM_Floor1 || !FloorMesh1)
	{
		// still allow debug only
		return;
	}

	const float FloorBaseZ = float(FloorIndex) * GetActualFloorHeightCm();
	const float Z = FloorBaseZ; // floor at base plane

	// floor tile assumes 1 cell square; scale to cell size
	const FVector Scale(CellSizeCm.X / 100.0f, CellSizeCm.Y / 100.0f, 1.0f); // assumes mesh authored in meters (100cm)

	for (int32 y = MinY; y <= MaxY; ++y)
	{
		for (int32 x = MinX; x <= MaxX; ++x)
		{
			const FTransform T = MakeCellTransform(x, y, Z, FRotator::ZeroRotator, Scale);
			ISM_Floor1->AddInstance(T);
		}
	}
}

void UBuildingGeneratorComponent::EmitCorners(int32 FloorIndex, int32 MinX, int32 MaxX, int32 MinY, int32 MaxY)
{
	if (!ISM_Corner || !CornerMesh)
	{
		return;
	}

	const float FloorMidZ = (float(FloorIndex) * GetActualFloorHeightCm()) + (0.5f * GetActualFloorHeightCm());

	// scale XY to cell size, scale Z to actual floor height (mesh assumed 1m tall by default)
	const float HeightScale = GetActualFloorHeightCm() / 100.0f;
	const FVector Scale(CellSizeCm.X / 100.0f, CellSizeCm.Y / 100.0f, HeightScale);

	const int32 CX[4] = { MinX, MinX, MaxX, MaxX };
	const int32 CY[4] = { MinY, MaxY, MinY, MaxY };

	for (int32 i = 0; i < 4; ++i)
	{
		const FTransform T = MakeCellTransform(CX[i], CY[i], FloorMidZ, FRotator::ZeroRotator, Scale);
		ISM_Corner->AddInstance(T);
	}
}

static float FaceYawDegrees(EBGFace Face)
{
	// Assumption: wall mesh faces +X by default (forward = +X), rotate yaw to face outward.
	// North is +Y, South -Y, East +X, West -X.
	switch (Face)
	{
	case EBGFace::East:  return 0.0f;
	case EBGFace::North: return 90.0f;
	case EBGFace::West:  return 180.0f;
	case EBGFace::South: return -90.0f;
	default: return 0.0f;
	}
}

UStaticMesh* PickWallMesh(int32 LenCells, UStaticMesh* M1, UStaticMesh* M2, UStaticMesh* M4)
{
	if (LenCells >= 4 && M4) return M4;
	if (LenCells >= 2 && M2) return M2;
	return M1;
}

UInstancedStaticMeshComponent* PickWallISM(int32 LenCells, UInstancedStaticMeshComponent* I1, UInstancedStaticMeshComponent* I2, UInstancedStaticMeshComponent* I4)
{
	if (LenCells >= 4 && I4 && I4->GetStaticMesh()) return I4;
	if (LenCells >= 2 && I2 && I2->GetStaticMesh()) return I2;
	return I1;
}

void UBuildingGeneratorComponent::EmitWallFaceSegments(int32 FloorIndex, EBGFace Face, int32 MinX, int32 MaxX, int32 MinY, int32 MaxY)
{
	// Choose ISM by length (we will emit multiple segments with varying lengths)
	// If user did not assign meshes, skip.
	if (!ISM_Wall1 || !WallMesh1)
	{
		return;
	}

	const float FloorMidZ = (float(FloorIndex) * GetActualFloorHeightCm()) + (0.5f * GetActualFloorHeightCm());
	const float HeightScale = GetActualFloorHeightCm() / 100.0f;

	// Exclude corners from run length on each face:
	// North/South: along X, cells from MinX+1 .. MaxX-1 at Y boundary
	// East/West: along Y, cells from MinY+1 .. MaxY-1 at X boundary
	const int32 RunLenX = FMath::Max(0, (MaxX - MinX + 1) - 2);
	const int32 RunLenY = FMath::Max(0, (MaxY - MinY + 1) - 2);

	auto EmitSegmentAt = [&](int32 StartCellX, int32 StartCellY, int32 LenCells)
		{
			const float Yaw = FaceYawDegrees(Face);
			const FRotator Rot(0.0f, Yaw, 0.0f);

			// scale X length in cells (mesh assumed 1m per cell in X), thickness in Y set to 1 cell, height set to floor height
			// We map:
			// - segment length axis: for North/South it's X, for East/West it's Y (we'll rotate).
			const float LenScale = float(LenCells) * (CellSizeCm.X / 100.0f); // if mesh's base length is 1m
			const float ThickScale = (CellSizeCm.Y / 100.0f);

			// For East/West walls, after yaw rotation, "length" still aligns with mesh X axis; we place segment centers accordingly.
			const FVector Scale(LenScale, ThickScale, HeightScale);

			// Segment center cell:
			// For North/South: center in X = StartX + (LenCells-1)/2, Y fixed boundary
			// For East/West: center in Y = StartY + (LenCells-1)/2, X fixed boundary
			float CenterX = float(StartCellX);
			float CenterY = float(StartCellY);

			if (Face == EBGFace::North || Face == EBGFace::South)
			{
				CenterX = float(StartCellX) + (float(LenCells) * 0.5f) - 0.5f;
				CenterY = float(StartCellY);
			}
			else
			{
				CenterY = float(StartCellY) + (float(LenCells) * 0.5f) - 0.5f;
				CenterX = float(StartCellX);
			}

			// Convert center cell float -> nearest int anchor for transform builder
			// We build transform by manually placing in world space (sub-cell precision).
			const FVector LocalPos(
				(CenterX + 0.5f) * CellSizeCm.X,
				(CenterY + 0.5f) * CellSizeCm.Y,
				FloorMidZ
			);

			const FTransform OwnerTM = GetOwner()->GetActorTransform();
			const FVector WorldPos = OwnerTM.TransformPosition(LocalPos);
			const FQuat WorldRot = (OwnerTM.GetRotation() * Rot.Quaternion());

			UInstancedStaticMeshComponent* TargetISM = PickWallISM(LenCells, ISM_Wall1, ISM_Wall2, ISM_Wall4);
			if (!TargetISM)
			{
				return;
			}
			TargetISM->AddInstance(FTransform(WorldRot, WorldPos, Scale));
		};

	// Helper: greedy cut of run length with module sizes
	auto GreedyEmitRun = [&](int32 RunStartX, int32 RunStartY, int32 RunLen, bool bAlongX)
		{
			int32 Cursor = 0;
			while (Cursor < RunLen)
			{
				int32 Pick = 1;
				for (int32 S : WallModuleSizes)
				{
					if (S <= 0) continue;
					if (Cursor + S <= RunLen)
					{
						Pick = S;
						break;
					}
				}

				int32 SegX = RunStartX;
				int32 SegY = RunStartY;

				if (bAlongX)
				{
					SegX = RunStartX + Cursor;
					SegY = RunStartY;
				}
				else
				{
					SegX = RunStartX;
					SegY = RunStartY + Cursor;
				}

				EmitSegmentAt(SegX, SegY, Pick);
				Cursor += Pick;
			}
		};

	switch (Face)
	{
	case EBGFace::North:
	{
		if (RunLenX <= 0) return;
		const int32 Y = MaxY;
		const int32 StartX = MinX + 1;
		GreedyEmitRun(StartX, Y, RunLenX, true);
	} break;

	case EBGFace::South:
	{
		if (RunLenX <= 0) return;
		const int32 Y = MinY;
		const int32 StartX = MinX + 1;
		GreedyEmitRun(StartX, Y, RunLenX, true);
	} break;

	case EBGFace::East:
	{
		if (RunLenY <= 0) return;
		const int32 X = MaxX;
		const int32 StartY = MinY + 1;
		GreedyEmitRun(X, StartY, RunLenY, false);
	} break;

	case EBGFace::West:
	{
		if (RunLenY <= 0) return;
		const int32 X = MinX;
		const int32 StartY = MinY + 1;
		GreedyEmitRun(X, StartY, RunLenY, false);
	} break;

	default:
		break;
	}
}

void UBuildingGeneratorComponent::EmitRoofFromOcc(const TArray<uint8>& Occ)
{
	// Build roof tiles per floor: roofTile[f][x,y] = occ[f] && (f is top OR !occ[f+1])
	// Then greedy pack squares 4/2/1
	for (int32 f = 0; f < Floors; ++f)
	{
		// Build 2D roof tile mask
		TArray<uint8> RoofTiles;
		RoofTiles.SetNumZeroed(SizeX * SizeY);

		for (int32 y = 0; y < SizeY; ++y)
		{
			for (int32 x = 0; x < SizeX; ++x)
			{
				const bool bHere = GetOcc(Occ, f, x, y);
				const bool bCovered = (f < Floors - 1) ? GetOcc(Occ, f + 1, x, y) : false;
				const bool bRoof = bHere && !bCovered;
				RoofTiles[Idx2D(x, y)] = bRoof ? 1 : 0;
			}
		}

		// Used mask for packing
		TArray<uint8> Used;
		Used.SetNumZeroed(SizeX * SizeY);

		for (int32 Patch : RoofPatchSizes)
		{
			if (Patch <= 0) continue;
			RoofPackFloorTiles(f, RoofTiles, Used, Patch);
		}
	}
}

void UBuildingGeneratorComponent::RoofPackFloorTiles(int32 FloorIndex, const TArray<uint8>& RoofTiles, TArray<uint8>& Used, int32 PatchSizeCells)
{
	// pick mesh/ISM by patch size
	UInstancedStaticMeshComponent* TargetISM = nullptr;
	UStaticMesh* TargetMesh = nullptr;

	if (PatchSizeCells >= 4)
	{
		TargetISM = ISM_Roof4;
		TargetMesh = RoofMesh4;
	}
	else if (PatchSizeCells >= 2)
	{
		TargetISM = ISM_Roof2;
		TargetMesh = RoofMesh2;
	}
	else
	{
		TargetISM = ISM_Roof1;
		TargetMesh = RoofMesh1;
	}

	if (!TargetISM || !TargetMesh)
	{
		// If missing meshes, skip emitting instances, but still allow debug boxes via bDebugDrawCells if desired.
		return;
	}

	// Roof plane Z = top of this floor
	const float RoofZ = (float(FloorIndex) + 1.0f) * GetActualFloorHeightCm();

	// mesh assumed 1m square for size1; scale to PatchSizeCells * CellSize
	const float SX = float(PatchSizeCells) * (CellSizeCm.X / 100.0f);
	const float SY = float(PatchSizeCells) * (CellSizeCm.Y / 100.0f);
	const FVector Scale(SX, SY, 1.0f);

	for (int32 y = 0; y <= SizeY - PatchSizeCells; ++y)
	{
		for (int32 x = 0; x <= SizeX - PatchSizeCells; ++x)
		{
			// check all tiles available and unused
			bool bOk = true;
			for (int32 dy = 0; dy < PatchSizeCells && bOk; ++dy)
			{
				for (int32 dx = 0; dx < PatchSizeCells; ++dx)
				{
					const int32 Id = Idx2D(x + dx, y + dy);
					if (RoofTiles[Id] == 0 || Used[Id] != 0)
					{
						bOk = false;
						break;
					}
				}
			}
			if (!bOk)
			{
				continue;
			}

			// mark used
			for (int32 dy = 0; dy < PatchSizeCells; ++dy)
			{
				for (int32 dx = 0; dx < PatchSizeCells; ++dx)
				{
					Used[Idx2D(x + dx, y + dy)] = 1;
				}
			}

			// place at patch center
			const float CenterX = float(x) + (float(PatchSizeCells) * 0.5f) - 0.5f;
			const float CenterY = float(y) + (float(PatchSizeCells) * 0.5f) - 0.5f;

			const FVector LocalPos(
				(CenterX + 0.5f) * CellSizeCm.X,
				(CenterY + 0.5f) * CellSizeCm.Y,
				RoofZ
			);

			const FTransform OwnerTM = GetOwner()->GetActorTransform();
			const FVector WorldPos = OwnerTM.TransformPosition(LocalPos);
			const FQuat WorldRot = OwnerTM.GetRotation(); // flat

			TargetISM->AddInstance(FTransform(WorldRot, WorldPos, Scale));
		}
	}
}

void UBuildingGeneratorComponent::DebugDrawFloor(int32 FloorIndex, int32 MinX, int32 MaxX, int32 MinY, int32 MaxY) const
{
	if (!bDebugDraw || !GetWorld() || !GetOwner())
	{
		return;
	}

	const float BaseZ = float(FloorIndex) * GetActualFloorHeightCm();
	const float TopZ = float(FloorIndex + 1) * GetActualFloorHeightCm();
	const float MidZ = 0.5f * (BaseZ + TopZ);

	// draw a bounding box for footprint rect
	const FVector LocalCenter(
		((float(MinX + MaxX) + 1.0f) * 0.5f) * CellSizeCm.X,
		((float(MinY + MaxY) + 1.0f) * 0.5f) * CellSizeCm.Y,
		MidZ
	);

	const FVector LocalExtents(
		(float(MaxX - MinX + 1) * 0.5f) * CellSizeCm.X,
		(float(MaxY - MinY + 1) * 0.5f) * CellSizeCm.Y,
		0.5f * GetActualFloorHeightCm()
	);

	const FTransform OwnerTM = GetOwner()->GetActorTransform();
	const FVector WorldCenter = OwnerTM.TransformPosition(LocalCenter);
	const FQuat WorldRot = OwnerTM.GetRotation();

	DrawDebugBox(GetWorld(), WorldCenter, LocalExtents, WorldRot, FColor::Cyan, DebugDuration < 0.0f, FMath::Abs(DebugDuration), 0, 2.0f);
}

void UBuildingGeneratorComponent::DebugDrawOccCells(int32 FloorIndex, const TArray<uint8>& Occ) const
{
	if (!bDebugDraw || !GetWorld() || !GetOwner())
	{
		return;
	}

	const float BaseZ = float(FloorIndex) * GetActualFloorHeightCm();
	const float MidZ = BaseZ + 0.5f * GetActualFloorHeightCm();

	const FTransform OwnerTM = GetOwner()->GetActorTransform();
	const FQuat WorldRot = OwnerTM.GetRotation();

	const FVector Half(CellSizeCm.X * 0.5f, CellSizeCm.Y * 0.5f, GetActualFloorHeightCm() * 0.5f);

	for (int32 y = 0; y < SizeY; ++y)
	{
		for (int32 x = 0; x < SizeX; ++x)
		{
			if (!GetOcc(Occ, FloorIndex, x, y))
			{
				continue;
			}

			const FVector LocalCenter(
				(float(x) + 0.5f) * CellSizeCm.X,
				(float(y) + 0.5f) * CellSizeCm.Y,
				MidZ
			);

			const FVector WorldCenter = OwnerTM.TransformPosition(LocalCenter);
			DrawDebugBox(GetWorld(), WorldCenter, Half, WorldRot, FColor::White, DebugDuration < 0.0f, FMath::Abs(DebugDuration), 0, 0.5f);
		}
	}
}
