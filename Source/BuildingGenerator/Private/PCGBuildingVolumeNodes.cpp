#include "PCGBuildingVolumeNodes.h"

#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGParamData.h"
#include "PCGElement.h"
#include "Engine/StaticMesh.h"
#include "UObject/SoftObjectPath.h"// UE5.5：FPCGElement / IPCGElement 在这里 :contentReference[oaicite:5]{index=5}
#include "Data/PCGPointData.h"

#include "Metadata/PCGMetadata.h"          // UE5.5：UPCGMetadata API（Create/FindOrCreate/GetConstTypedAttribute 等）:contentReference[oaicite:6]{index=6}
#include "Metadata/PCGMetadataAttributeTpl.h" // UE5.5：FPCGMetadataAttribute<T>::SetValue / GetValuesFromItemKeys :contentReference[oaicite:7]{index=7}

#include "GameFramework/Actor.h"

// ------------------------------
// helpers
// ------------------------------
static UBuildingVolumeComponent* FindBuildingVolumeComponent(AActor* Owner, FName ComponentName)
{
	if (!Owner) return nullptr;

	if (ComponentName != NAME_None)
	{
		TArray<UActorComponent*> Comps;
		Owner->GetComponents(Comps);
		for (UActorComponent* C : Comps)
		{
			if (C && C->GetFName() == ComponentName)
			{
				return Cast<UBuildingVolumeComponent>(C);
			}
		}
	}

	return Owner->FindComponentByClass<UBuildingVolumeComponent>();
}

static int64 Pack3(int32 X, int32 Y, int32 Z)
{
	const int64 Ox = (int64)(X & 0x1FFFFF);
	const int64 Oy = (int64)(Y & 0x1FFFFF);
	const int64 Oz = (int64)(Z & 0x1FFFFF);
	return (Ox << 42) | (Oy << 21) | Oz;
}

static FIntVector Unpack3(int64 K)
{
	auto SignExtend21 = [](int32 V)->int32
		{
			if (V & (1 << 20)) V |= ~0x1FFFFF;
			return V;
		};

	const int32 Oz = (int32)(K & 0x1FFFFF);
	const int32 Oy = (int32)((K >> 21) & 0x1FFFFF);
	const int32 Ox = (int32)((K >> 42) & 0x1FFFFF);
	return FIntVector(SignExtend21(Ox), SignExtend21(Oy), SignExtend21(Oz));
}

// ---- Metadata helpers (UE5.5) ----
template<typename T>
static FPCGMetadataAttribute<T>* FindOrCreateAttr(UPCGMetadata* MD, const FName Name, const T& DefaultValue, bool bAllowsInterpolation)
{
	if (!MD) return nullptr;
	// UE5.5: FindOrCreateAttribute/CreateAttribute 都在 UPCGMetadata 上 :contentReference[oaicite:8]{index=8}
	return MD->FindOrCreateAttribute<T>(Name, DefaultValue, bAllowsInterpolation, /*bOverrideParent*/ true);
}

template<typename T>
static bool GetAttrValueSingle(const UPCGMetadata* MD, const FName Name, PCGMetadataEntryKey EntryKey, T& OutValue)
{
	if (!MD) return false;

	const FPCGMetadataAttribute<T>* Attr = MD->GetConstTypedAttribute<T>(Name);
	if (!Attr) return false;

	// UE5.5: 直接读取单个 ItemKey 的值，避免 GetValuesFromItemKeys 的重载歧义
	OutValue = Attr->GetValueFromItemKey(EntryKey);
	return true;
}


// ------------------------------
// Node 1: VolumeArrayToPoints
// ------------------------------
class FPCGVolumeArrayToPointsElement : public IPCGElement
{
public:

	virtual bool IsCacheable(const UPCGSettings* InSettings) const override
	{
		return false; // 依赖 Actor 上的 Volumes 数组（PCG 不追踪），必须禁用缓存
	}

	virtual bool ExecuteInternal(FPCGContext* Context) const override // UE5.5 元素执行入口 :contentReference[oaicite:11]{index=11}
	{
		check(Context);
		const UPCGVolumeArrayToPointsSettings* Settings = Context->GetInputSettings<UPCGVolumeArrayToPointsSettings>();
		if (!Settings) return true;

		UPCGComponent* SourceComp = Context->SourceComponent.Get();
		AActor* Owner = SourceComp ? SourceComp->GetOwner() : nullptr;
		UBuildingVolumeComponent* BVC = FindBuildingVolumeComponent(Owner, Settings->ComponentName);

		UPCGPointData* Out = NewObject<UPCGPointData>();
		Out->InitializeFromData(nullptr);

		// Create attrs
		UPCGMetadata* MD = Out->Metadata;
		FPCGMetadataAttribute<FString>* FacadeAttr = FindOrCreateAttr<FString>(MD, TEXT("FacadeGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<FString>* RoofAttr = FindOrCreateAttr<FString>(MD, TEXT("RoofGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<FString>* SupportAttr = FindOrCreateAttr<FString>(MD, TEXT("SupportGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<int32>* FloorAttr = FindOrCreateAttr<int32>(MD, TEXT("FloorIndex"), 0, true);
		FPCGMetadataAttribute<bool>* AllowScale = FindOrCreateAttr<bool>(MD, TEXT("AllowNonUniformScale"), false, true);
		FPCGMetadataAttribute<FVector>* ExtentsAttr = FindOrCreateAttr<FVector>(MD, TEXT("ExtentsWS"), FVector::ZeroVector, true);
		FPCGMetadataAttribute<FName>* StyleAttr = FindOrCreateAttr<FName>(MD, TEXT("StyleTag"), NAME_None, false);
		FPCGMetadataAttribute<int32>* PriAttr = FindOrCreateAttr<int32>(MD, TEXT("Priority"), 0, true);
		FPCGMetadataAttribute<FVector2D>* ScaleClampAttr = FindOrCreateAttr<FVector2D>(MD, TEXT("ScaleClamp"), FVector2D(0.92f, 1.08f), true);
		FPCGMetadataAttribute<int32>* VolumeIndexAttr = FindOrCreateAttr<int32>(MD, TEXT("VolumeIndex"), -1, true);


		if (BVC)
		{
			TArray<FPCGPoint>& Points = Out->GetMutablePoints();
			Points.Reserve(BVC->Volumes.Num());

			for (int32 VolumeIndex = 0; VolumeIndex < BVC->Volumes.Num(); ++VolumeIndex)
			{
				const FVolumeData& V = BVC->Volumes[VolumeIndex];

				FPCGPoint P;
				P.Transform = FTransform(V.RotationWS, V.CenterWS, FVector::OneVector);
				P.BoundsMin = -V.ExtentsWS;
				P.BoundsMax = V.ExtentsWS;
				P.Density = Settings->Density;

				const PCGMetadataEntryKey Entry = MD->AddEntry();
				P.MetadataEntry = Entry;

				if (FacadeAttr)     FacadeAttr->SetValue(Entry, V.FacadeGrammar);
				if (RoofAttr)       RoofAttr->SetValue(Entry, V.RoofGrammar);
				if (SupportAttr)    SupportAttr->SetValue(Entry, V.SupportGrammar);
				if (FloorAttr)      FloorAttr->SetValue(Entry, V.FloorIndex);
				if (AllowScale)     AllowScale->SetValue(Entry, V.bAllowNonUniformScale);
				if (ScaleClampAttr) ScaleClampAttr->SetValue(Entry, V.ScaleClamp);
				if (ExtentsAttr)    ExtentsAttr->SetValue(Entry, V.ExtentsWS);
				if (StyleAttr)      StyleAttr->SetValue(Entry, V.StyleTag);
				if (PriAttr)        PriAttr->SetValue(Entry, V.Priority);
				if (VolumeIndexAttr)VolumeIndexAttr->SetValue(Entry, VolumeIndex);

				Points.Add(P);
			}
		}

		FPCGTaggedData& Tag = Context->OutputData.TaggedData.Emplace_GetRef();
		Tag.Pin = PCGBuildingPins::VolumesOut;
		Tag.Data = Out;

		return true;
	}
};

TArray<FPCGPinProperties> UPCGVolumeArrayToPointsSettings::InputPinProperties() const
{
	return {};
}

TArray<FPCGPinProperties> UPCGVolumeArrayToPointsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> OutPins;
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::VolumesOut;
	P.AllowedTypes = EPCGDataType::Point;
	OutPins.Add(P);
	return OutPins;
}

FPCGElementPtr UPCGVolumeArrayToPointsSettings::CreateElement() const
{
	// MakeShared 返回 TSharedRef，这里转成 TSharedPtr 以匹配 FPCGElementPtr
	return MakeShared<FPCGVolumeArrayToPointsElement>().ToSharedPtr();
}

// ------------------------------
// Node 2: VoxelUnionMassing
// ------------------------------
class FPCGVoxelUnionMassingElement : public IPCGElement
{
public:
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override
	{
		return false; // 依赖 Actor 上的 Volumes 数组（PCG 不追踪），必须禁用缓存
	}

	virtual bool ExecuteInternal(FPCGContext* Context) const override
	{
		check(Context);
		const UPCGVoxelUnionMassingSettings* Settings = Context->GetInputSettings<UPCGVoxelUnionMassingSettings>();
		if (!Settings) return true;

		UPCGComponent* SourceComp = Context->SourceComponent.Get();
		AActor* Owner = SourceComp ? SourceComp->GetOwner() : nullptr;
		UBuildingVolumeComponent* BVC = FindBuildingVolumeComponent(Owner, NAME_None);

		const float VoxelSize = (Settings->VoxelSizeOverride > 0.0f) ? Settings->VoxelSizeOverride : (BVC ? BVC->VoxelSize : 50.0f);
		const float FloorH = (Settings->FloorHeightOverride > 0.0f) ? Settings->FloorHeightOverride : (BVC ? BVC->FloorHeight : 400.0f);

		// collect input volume points
		TArray<const UPCGPointData*> Inputs;
		for (const FPCGTaggedData& In : Context->InputData.TaggedData)
		{
			if (const UPCGPointData* PD = Cast<UPCGPointData>(In.Data))
			{
				Inputs.Add(PD);
			}
		}

		// Prepare outputs
		UPCGPointData* ShellOut = NewObject<UPCGPointData>(); ShellOut->InitializeFromData(nullptr);
		UPCGPointData* FootprintOut = NewObject<UPCGPointData>(); FootprintOut->InitializeFromData(nullptr);
		UPCGPointData* BoundaryOut = NewObject<UPCGPointData>(); BoundaryOut->InitializeFromData(nullptr);

		// metadata attrs
		FPCGMetadataAttribute<int32>* ShellFloorAttr = FindOrCreateAttr<int32>(ShellOut->Metadata, TEXT("FloorIndex"), 0, true);
		FPCGMetadataAttribute<FVector>* VoxelCAttr = FindOrCreateAttr<FVector>(ShellOut->Metadata, TEXT("VoxelCenterWS"), FVector::ZeroVector, true);

		FPCGMetadataAttribute<int32>* FPFloorAttr = FindOrCreateAttr<int32>(FootprintOut->Metadata, TEXT("FloorIndex"), 0, true);
		FPCGMetadataAttribute<FVector>* CellCAttr = FindOrCreateAttr<FVector>(FootprintOut->Metadata, TEXT("CellCenterWS"), FVector::ZeroVector, true);

		FPCGMetadataAttribute<int32>* BFloorAttr = FindOrCreateAttr<int32>(BoundaryOut->Metadata, TEXT("FloorIndex"), 0, true);
		FPCGMetadataAttribute<FString>* BSupportAttr = FindOrCreateAttr<FString>(BoundaryOut->Metadata, TEXT("SupportGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<FVector>* BCAttr = FindOrCreateAttr<FVector>(BoundaryOut->Metadata, TEXT("BoundaryCenterWS"), FVector::ZeroVector, true);
		FPCGMetadataAttribute<int32>* ShellOwnerAttr = FindOrCreateAttr<int32>(ShellOut->Metadata, TEXT("OwnerIndex"), -1, true);
		FPCGMetadataAttribute<int32>* FPOwnerAttr = FindOrCreateAttr<int32>(FootprintOut->Metadata, TEXT("OwnerIndex"), -1, true);
		FPCGMetadataAttribute<int32>* BOwnerAttr = FindOrCreateAttr<int32>(BoundaryOut->Metadata, TEXT("OwnerIndex"), -1, true);
		// ---- pass-through attrs (so union outputs still carry per-volume grammar/policy) ----
		FPCGMetadataAttribute<FString>* ShellFacadeAttr = FindOrCreateAttr<FString>(ShellOut->Metadata, TEXT("FacadeGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<FString>* ShellRoofAttr = FindOrCreateAttr<FString>(ShellOut->Metadata, TEXT("RoofGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<bool>* ShellAllowScale = FindOrCreateAttr<bool>(ShellOut->Metadata, TEXT("AllowNonUniformScale"), false, true);
		FPCGMetadataAttribute<FVector2D>* ShellScaleClamp = FindOrCreateAttr<FVector2D>(ShellOut->Metadata, TEXT("ScaleClamp"), FVector2D(0.92f, 1.08f), true);
		FPCGMetadataAttribute<FName>* ShellStyleAttr = FindOrCreateAttr<FName>(ShellOut->Metadata, TEXT("StyleTag"), NAME_None, false);
		FPCGMetadataAttribute<int32>* ShellPriAttr = FindOrCreateAttr<int32>(ShellOut->Metadata, TEXT("Priority"), 0, true);
		FPCGMetadataAttribute<int32>* ShellVolIdxAttr = FindOrCreateAttr<int32>(ShellOut->Metadata, TEXT("VolumeIndex"), -1, true);

		FPCGMetadataAttribute<FString>* FpFacadeAttr = FindOrCreateAttr<FString>(FootprintOut->Metadata, TEXT("FacadeGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<FString>* FpRoofAttr = FindOrCreateAttr<FString>(FootprintOut->Metadata, TEXT("RoofGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<bool>* FpAllowScale = FindOrCreateAttr<bool>(FootprintOut->Metadata, TEXT("AllowNonUniformScale"), false, true);
		FPCGMetadataAttribute<FVector2D>* FpScaleClamp = FindOrCreateAttr<FVector2D>(FootprintOut->Metadata, TEXT("ScaleClamp"), FVector2D(0.92f, 1.08f), true);
		FPCGMetadataAttribute<FName>* FpStyleAttr = FindOrCreateAttr<FName>(FootprintOut->Metadata, TEXT("StyleTag"), NAME_None, false);
		FPCGMetadataAttribute<int32>* FpPriAttr = FindOrCreateAttr<int32>(FootprintOut->Metadata, TEXT("Priority"), 0, true);
		FPCGMetadataAttribute<int32>* FpVolIdxAttr = FindOrCreateAttr<int32>(FootprintOut->Metadata, TEXT("VolumeIndex"), -1, true);

		FPCGMetadataAttribute<FString>* BFacadeAttr = FindOrCreateAttr<FString>(BoundaryOut->Metadata, TEXT("FacadeGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<FString>* BRoofAttr = FindOrCreateAttr<FString>(BoundaryOut->Metadata, TEXT("RoofGrammar"), TEXT(""), false);
		FPCGMetadataAttribute<bool>* BAllowScale = FindOrCreateAttr<bool>(BoundaryOut->Metadata, TEXT("AllowNonUniformScale"), false, true);
		FPCGMetadataAttribute<FVector2D>* BScaleClamp = FindOrCreateAttr<FVector2D>(BoundaryOut->Metadata, TEXT("ScaleClamp"), FVector2D(0.92f, 1.08f), true);
		FPCGMetadataAttribute<FName>* BStyleAttr = FindOrCreateAttr<FName>(BoundaryOut->Metadata, TEXT("StyleTag"), NAME_None, false);
		FPCGMetadataAttribute<int32>* BPriAttr = FindOrCreateAttr<int32>(BoundaryOut->Metadata, TEXT("Priority"), 0, true);
		FPCGMetadataAttribute<int32>* BVolIdxAttr = FindOrCreateAttr<int32>(BoundaryOut->Metadata, TEXT("VolumeIndex"), -1, true);


		if (Inputs.Num() == 0)
		{
			Emit(Context, ShellOut, PCGBuildingPins::ShellOut);
			Emit(Context, FootprintOut, PCGBuildingPins::FootprintOut);
			Emit(Context, BoundaryOut, PCGBuildingPins::OverhangBoundaryOut);
			return true;
		}

		
		// ---------- OBB voxelization (true rotated shape) + owner mapping ----------
		// ---------- per-volume voxelization (grid rotates with each Volume RotationWS) ----------
		struct FSrc
		{
			FTransform T;
			FTransform InvT;
			FVector Ext;            // local half extents
			FQuat RotQ;

			FString FacadeGrammar;
			FString RoofGrammar;
			FString SupportGrammar;

			bool bAllowNonUniformScale = false;
			FVector2D ScaleClamp = FVector2D(0.92f, 1.08f);

			FName StyleTag = NAME_None;
			int32 Priority = 0;

			int32 VolumeIndex = -1; // from Node1 (stable link back to actor array index)

			bool bHasFloorBase = false;
			int32 FloorBase = 0;
			float BottomZWorld = 0.0f;
		};



		TArray<FSrc> Sources;
		Sources.Reserve(128);

		// maps for "who owns this voxel/cell"
		TMap<int64, int32> VoxelOwner; VoxelOwner.Reserve(2048); // Pack3(x,y,z) -> src index
		TMap<int64, int32> CellOwner;  CellOwner.Reserve(2048);  // Pack3(x,y,floor) -> src index

		auto ReadIntAttr = [&](const UPCGPointData* PD, const FPCGPoint& P, const FName Name, int32& Out)->bool
			{
				if (!PD || !PD->Metadata || P.MetadataEntry == PCGInvalidEntryKey) return false;
				return GetAttrValueSingle<int32>(PD->Metadata, Name, P.MetadataEntry, Out);
			};

		auto ReadStrAttr = [&](const UPCGPointData* PD, const FPCGPoint& P, const FName Name, FString& Out)->bool
			{
				if (!PD || !PD->Metadata || P.MetadataEntry == PCGInvalidEntryKey) return false;
				return GetAttrValueSingle<FString>(PD->Metadata, Name, P.MetadataEntry, Out);
			};
		auto ReadBoolAttr = [&](const UPCGPointData* PD, const FPCGPoint& P, const FName Name, bool& Out)->bool
			{
				if (!PD || !PD->Metadata || P.MetadataEntry == PCGInvalidEntryKey) return false;
				return GetAttrValueSingle<bool>(PD->Metadata, Name, P.MetadataEntry, Out);
			};

		auto ReadVec2Attr = [&](const UPCGPointData* PD, const FPCGPoint& P, const FName Name, FVector2D& Out)->bool
			{
				if (!PD || !PD->Metadata || P.MetadataEntry == PCGInvalidEntryKey) return false;
				return GetAttrValueSingle<FVector2D>(PD->Metadata, Name, P.MetadataEntry, Out);
			};

		auto ReadNameAttr = [&](const UPCGPointData* PD, const FPCGPoint& P, const FName Name, FName& Out)->bool
			{
				if (!PD || !PD->Metadata || P.MetadataEntry == PCGInvalidEntryKey) return false;
				return GetAttrValueSingle<FName>(PD->Metadata, Name, P.MetadataEntry, Out);
			};

		// owner pick rule: higher priority wins; if equal, later volume overwrites (so your "V2覆盖V1"能稳定生效)
		auto ChooseOwner = [&](TMap<int64, int32>& OwnerMap, int64 Key, int32 NewOwner)
			{
				int32* OldOwner = OwnerMap.Find(Key);
				if (!OldOwner) { OwnerMap.Add(Key, NewOwner); return; }

				const int32 OldPri = Sources[*OldOwner].Priority;
				const int32 NewPri = Sources[NewOwner].Priority;

				if (NewPri > OldPri || (NewPri == OldPri))
				{
					*OldOwner = NewOwner;
				}
			};

		// point-in-OBB test: inverse transform to local, then abs(local) <= extents
		auto IsInsideOBB = [&](const FSrc& S, const FVector& WorldPoint)->bool
			{
				const FVector L = S.InvT.TransformPosition(WorldPoint);

				const float VoxelHalf = VoxelSize * 0.5f;
				const float ExpandXY = VoxelHalf * 1.41421356f; // sqrt(2)

				const FVector E = S.Ext + FVector(ExpandXY, ExpandXY, VoxelHalf) + FVector(0.001f);
				return FMath::Abs(L.X) <= E.X && FMath::Abs(L.Y) <= E.Y && FMath::Abs(L.Z) <= E.Z;
			};


		// collect all sources (input points)
		for (const UPCGPointData* PD : Inputs)
		{
			for (const FPCGPoint& P : PD->GetPoints())
			{
				FSrc S;
				S.T = P.Transform;
				S.InvT = S.T.Inverse();
				S.Ext = (P.BoundsMax - P.BoundsMin) * 0.5f;
				S.RotQ = S.T.GetRotation();

				// priority (optional)
				int32 Pri = 0;
				if (ReadIntAttr(PD, P, TEXT("Priority"), Pri)) S.Priority = Pri;

				// SupportGrammar (optional)
				FString SG;
				if (ReadStrAttr(PD, P, TEXT("SupportGrammar"), SG))
				{
					S.SupportGrammar = SG;
				}

				// Facade/Roof grammar (optional)
				FString FG;
				if (ReadStrAttr(PD, P, TEXT("FacadeGrammar"), FG))
				{
					S.FacadeGrammar = FG;
				}

				FString RG;
				if (ReadStrAttr(PD, P, TEXT("RoofGrammar"), RG))
				{
					S.RoofGrammar = RG;
				}

				// scale policy (optional)
				bool bAllow = false;
				if (ReadBoolAttr(PD, P, TEXT("AllowNonUniformScale"), bAllow))
				{
					S.bAllowNonUniformScale = bAllow;
				}

				FVector2D Clamp = FVector2D(0.92f, 1.08f);
				if (ReadVec2Attr(PD, P, TEXT("ScaleClamp"), Clamp))
				{
					S.ScaleClamp = Clamp;
				}

				// style + stable volume index (optional)
				FName Style = NAME_None;
				if (ReadNameAttr(PD, P, TEXT("StyleTag"), Style))
				{
					S.StyleTag = Style;
				}

				int32 VolIdx = -1;
				if (ReadIntAttr(PD, P, TEXT("VolumeIndex"), VolIdx))
				{
					S.VolumeIndex = VolIdx;
				}

				// FloorIndex base (optional)
				if (Settings->bUseFloorIndexAttribute)
				{
					int32 FloorBase = 0;
					if (ReadIntAttr(PD, P, TEXT("FloorIndex"), FloorBase))
					{
						S.bHasFloorBase = true;
						S.FloorBase = FloorBase;
					}
				}

				// bottom z in world (local (0,0,-Ext.Z) transformed)
				float MinZ = TNumericLimits<float>::Max();
				for (int sx : {-1, 1})
					for (int sy : {-1, 1})
						for (int sz : {-1, 1})
						{
							const FVector CornerLocal(sx * S.Ext.X, sy * S.Ext.Y, sz * S.Ext.Z);
							MinZ = FMath::Min(MinZ, S.T.TransformPosition(CornerLocal).Z);
						}
				S.BottomZWorld = MinZ;

				Sources.Add(S);
			}
		}

		// ================================
// ✅ Fix3: Per-Volume local voxel grid
// 解释：你现在是在 Details 里改 Volumes[Index].RotationWS（不是旋转 Actor）。
// 如果仍用 Root(Actor/FirstSource) 的轴对齐网格去采样旋转 OBB，就会出现你截图那种“棋盘镂空/隔一格一个点”的 aliasing。
// 方案：每个 Volume 在自己的 Local 空间里做轴对齐体素化（保证无镂空），再用 S.T 变换到 World（格子整体跟着 RotationWS 旋转）。
// ================================

		TArray<FPCGPoint>& ShellPts = ShellOut->GetMutablePoints();
		TArray<FPCGPoint>& FPts = FootprintOut->GetMutablePoints();
		TArray<FPCGPoint>& BPts = BoundaryOut->GetMutablePoints();

		ShellPts.Reset();
		FPts.Reset();
		BPts.Reset();

		// 预留一点容量（粗略）
		ShellPts.Reserve(4096);
		FPts.Reserve(4096);
		BPts.Reserve(4096);

		// ---- Global occupancy for supports (across sources/floors) ----
		const FVector GridOriginWS = Owner ? Owner->GetActorLocation() : FVector::ZeroVector;
		const float WorldFloor0Z = GridOriginWS.Z; // 方案2：FloorIndex 的绝对高度基准（跟随 Actor/组件所在高度）

		TMap<int32, TSet<int64>> GlobalFloorOcc;   // floor -> set(Pack3(gx,gy,0))
		TMap<int64, int32>       GlobalCellOwner;  // Pack3(gx,gy,floor) -> owner src
		TMap<int64, FVector>     GlobalCellPos;    // Pack3(gx,gy,floor) -> representative world pos

		GlobalCellOwner.Reserve(4096);
		GlobalCellPos.Reserve(4096);

		auto WorldToGXY = [&](const FVector& W)->FIntPoint
			{
				const FVector D = W - GridOriginWS;
				return FIntPoint(
					FMath::FloorToInt(D.X / VoxelSize),
					FMath::FloorToInt(D.Y / VoxelSize)
				);
			};

		auto SetGlobalCell = [&](int32 Floor, const FVector& W, int32 OwnerIdx)
			{
				const FIntPoint G = WorldToGXY(W);

				// 2D presence for difference test
				GlobalFloorOcc.FindOrAdd(Floor).Add(Pack3(G.X, G.Y, 0));

				// owner + pos for output attributes
				const int64 CK = Pack3(G.X, G.Y, Floor);

				int32* PrevOwner = GlobalCellOwner.Find(CK);
				if (!PrevOwner || Sources[OwnerIdx].Priority >= Sources[*PrevOwner].Priority)
				{
					GlobalCellOwner.Add(CK, OwnerIdx);
					GlobalCellPos.Add(CK, W);
				}
			};

		// 每个 Source 独立生成：shell / footprint / boundary
		for (int32 SrcIdx = 0; SrcIdx < Sources.Num(); ++SrcIdx)
		{
			const FSrc& S = Sources[SrcIdx];

			// -------- 1) local occupancy --------
			auto LocalToVoxelMin = [&](float v)->int32 { return FMath::FloorToInt(v / VoxelSize); };
			auto LocalToVoxelMax = [&](float v)->int32 { return FMath::CeilToInt(v / VoxelSize) - 1; };

			const int32 X0 = LocalToVoxelMin(-S.Ext.X);
			const int32 X1 = LocalToVoxelMax(+S.Ext.X);
			const int32 Y0 = LocalToVoxelMin(-S.Ext.Y);
			const int32 Y1 = LocalToVoxelMax(+S.Ext.Y);
			const int32 Z0 = LocalToVoxelMin(-S.Ext.Z);
			const int32 Z1 = LocalToVoxelMax(+S.Ext.Z);

			TSet<int64> OccLocal;   OccLocal.Reserve((X1 - X0 + 1) * (Y1 - Y0 + 1) * FMath::Max(1, (Z1 - Z0 + 1)));
			TSet<int64> Occ2DLocal; Occ2DLocal.Reserve((X1 - X0 + 1) * (Y1 - Y0 + 1));

			for (int32 z = Z0; z <= Z1; ++z)
			{
				for (int32 y = Y0; y <= Y1; ++y)
				{
					for (int32 x = X0; x <= X1; ++x)
					{
						OccLocal.Add(Pack3(x, y, z));

						// local voxel center (axis-aligned in local)
						const FVector L((x + 0.5f) * VoxelSize, (y + 0.5f) * VoxelSize, (z + 0.5f) * VoxelSize);
						const FVector C = S.T.TransformPosition(L); // world center (grid rotates with RotationWS)

						// floor slicing（方案2：UseFloorIndexAttribute 时，只用“local-from-bottom”切层，不用 BottomZWorld）
						int32 FloorIndexZ = 0;

						if (Settings->bUseFloorIndexAttribute && S.bHasFloorBase)
						{
							// L.Z 是体素中心的 local Z（相对 volume 中心）
							// volume local bottom = -S.Ext.Z，所以从底部起算：L.Z + S.Ext.Z
							const float LocalZFromBottom = L.Z + S.Ext.Z;
							FloorIndexZ = S.FloorBase + FMath::FloorToInt(LocalZFromBottom / FloorH);
						}
						else
						{
							// 没用 FloorIndexAttribute：用世界Z（可选以 WorldFloor0Z 为基准更稳定）
							FloorIndexZ = FMath::FloorToInt((C.Z - WorldFloor0Z) / FloorH);
						}

						FloorIndexZ = FMath::Max(0, FloorIndexZ);

						Occ2DLocal.Add(Pack3(x, y, FloorIndexZ));
					}
				}
			}

			auto HasLocal = [&](int32 x, int32 y, int32 z)->bool { return OccLocal.Contains(Pack3(x, y, z)); };

			// -------- 2) shell points (neighbor test in LOCAL grid, no aliasing holes) --------
			for (int64 K : OccLocal)
			{
				const FIntVector V = Unpack3(K);

				bool bIsShell = false;
				bIsShell |= !HasLocal(V.X + 1, V.Y, V.Z);
				bIsShell |= !HasLocal(V.X - 1, V.Y, V.Z);
				bIsShell |= !HasLocal(V.X, V.Y + 1, V.Z);
				bIsShell |= !HasLocal(V.X, V.Y - 1, V.Z);
				bIsShell |= !HasLocal(V.X, V.Y, V.Z + 1);
				bIsShell |= !HasLocal(V.X, V.Y, V.Z - 1);

				if (Settings->bShellOnly && !bIsShell) continue;

				const FVector L((V.X + 0.5f) * VoxelSize, (V.Y + 0.5f) * VoxelSize, (V.Z + 0.5f) * VoxelSize);
				const FVector C = S.T.TransformPosition(L);

				FPCGPoint Pt;
				Pt.Transform = FTransform(S.RotQ, C, FVector::OneVector); // ✅ 保留旋转
				Pt.BoundsMin = FVector(-VoxelSize * 0.5f);
				Pt.BoundsMax = FVector(+VoxelSize * 0.5f);
				Pt.Density = Settings->Density;

				const PCGMetadataEntryKey Entry = ShellOut->Metadata->AddEntry();
				Pt.MetadataEntry = Entry;

				int32 FloorIndex = 0;

				if (Settings->bUseFloorIndexAttribute && S.bHasFloorBase)
				{
					// V.Z 在 Shell 这儿是 local voxel z index；对应体素中心 localZ = (V.Z+0.5)*VoxelSize
					const float LocalZFromBottom = ((V.Z + 0.5f) * VoxelSize) + S.Ext.Z;
					FloorIndex = S.FloorBase + FMath::FloorToInt(LocalZFromBottom / FloorH);
				}
				else
				{
					FloorIndex = FMath::FloorToInt((C.Z - WorldFloor0Z) / FloorH);
				}

				FloorIndex = FMath::Max(0, FloorIndex);

				if (ShellFloorAttr) ShellFloorAttr->SetValue(Entry, FloorIndex);
				if (ShellOwnerAttr) ShellOwnerAttr->SetValue(Entry, SrcIdx);
				if (VoxelCAttr)     VoxelCAttr->SetValue(Entry, C);
				if (ShellFacadeAttr) ShellFacadeAttr->SetValue(Entry, S.FacadeGrammar);
				if (ShellRoofAttr)   ShellRoofAttr->SetValue(Entry, S.RoofGrammar);
				if (ShellAllowScale) ShellAllowScale->SetValue(Entry, S.bAllowNonUniformScale);
				if (ShellScaleClamp) ShellScaleClamp->SetValue(Entry, S.ScaleClamp);
				if (ShellStyleAttr)  ShellStyleAttr->SetValue(Entry, S.StyleTag);
				if (ShellPriAttr)    ShellPriAttr->SetValue(Entry, S.Priority);
				if (ShellVolIdxAttr) ShellVolIdxAttr->SetValue(Entry, S.VolumeIndex);

				ShellPts.Add(Pt);
			}

			// -------- 3) Footprint2D points (per-volume) --------
			for (int64 K : Occ2DLocal)
			{
				const FIntVector V = Unpack3(K); // (x,y,floor)

				const FVector Lxy((V.X + 0.5f) * VoxelSize, (V.Y + 0.5f) * VoxelSize, 0.0f);
				FVector C = S.T.TransformPosition(Lxy);

				// 把 footprint 放到“楼层中心高度”(world) —— 避免被 S.T 的平移再叠一次
				if (S.bHasFloorBase)
				{
					C.Z = WorldFloor0Z + ((float)V.Z + 0.5f) * FloorH;
				}
				else
				{
					C.Z = ((float)V.Z + 0.5f) * FloorH;
				}

				FPCGPoint Pt;
				Pt.Transform = FTransform(S.RotQ, C, FVector::OneVector);
				Pt.BoundsMin = FVector(-VoxelSize * 0.5f, -VoxelSize * 0.5f, -1.0f);
				Pt.BoundsMax = FVector(+VoxelSize * 0.5f, +VoxelSize * 0.5f, +1.0f);
				Pt.Density = Settings->Density;

				const PCGMetadataEntryKey Entry = FootprintOut->Metadata->AddEntry();
				Pt.MetadataEntry = Entry;

				if (FPFloorAttr)  FPFloorAttr->SetValue(Entry, V.Z);
				if (CellCAttr)    CellCAttr->SetValue(Entry, C);
				if (FPOwnerAttr)  FPOwnerAttr->SetValue(Entry, SrcIdx);
				if (FpFacadeAttr) FpFacadeAttr->SetValue(Entry, S.FacadeGrammar);
				if (FpRoofAttr)   FpRoofAttr->SetValue(Entry, S.RoofGrammar);
				if (FpAllowScale) FpAllowScale->SetValue(Entry, S.bAllowNonUniformScale);
				if (FpScaleClamp) FpScaleClamp->SetValue(Entry, S.ScaleClamp);
				if (FpStyleAttr)  FpStyleAttr->SetValue(Entry, S.StyleTag);
				if (FpPriAttr)    FpPriAttr->SetValue(Entry, S.Priority);
				if (FpVolIdxAttr) FpVolIdxAttr->SetValue(Entry, S.VolumeIndex);

				SetGlobalCell(V.Z, C, SrcIdx);
				FPts.Add(Pt);
			}
		}

		// -------- 4) OverhangBoundary (GLOBAL across sources) --------
		for (auto& It : GlobalFloorOcc)
		{
			const int32 Floor = It.Key;
			const TSet<int64>& CurXY = It.Value;

			const TSet<int64>* NextPtr = GlobalFloorOcc.Find(Floor + 1);
			if (!NextPtr) continue;
			const TSet<int64>& NextXY = *NextPtr;

			auto IsOver = [&](int32 gx, int32 gy)->bool
				{
					return NextXY.Contains(Pack3(gx, gy, 0)) && !CurXY.Contains(Pack3(gx, gy, 0));
				};

			for (int64 XYK : NextXY)
			{
				if (CurXY.Contains(XYK)) continue; // next 有、cur 没有 => overhang

				const FIntVector V = Unpack3(XYK); // (gx,gy,0)

				const bool n0 = IsOver(V.X + 1, V.Y);
				const bool n1 = IsOver(V.X - 1, V.Y);
				const bool n2 = IsOver(V.X, V.Y + 1);
				const bool n3 = IsOver(V.X, V.Y - 1);

				const int32 NeighborCount = (int32)n0 + (int32)n1 + (int32)n2 + (int32)n3;
				if (NeighborCount == 4) continue;

				const int64 CellK = Pack3(V.X, V.Y, Floor + 1);

				const FVector* PosPtr = GlobalCellPos.Find(CellK);
				if (!PosPtr) continue;

				int32 OwnerIdx = 0;
				if (const int32* O = GlobalCellOwner.Find(CellK)) OwnerIdx = *O;

				FVector C = *PosPtr;

				//  Support 需要落在“上层 Volume 的底面(MinZ)”，而不是楼层分界平面
				if (Sources.IsValidIndex(OwnerIdx))
				{
					C.Z = Sources[OwnerIdx].BottomZWorld;   // == VolumeMinZ（上层体块的底面）
				}
				else if (Settings->bUseFloorIndexAttribute)
				{
					// fallback：如果 OwnerIdx 异常，才回退到楼层平面
					C.Z = WorldFloor0Z + (float)(Floor + 1) * FloorH;
				}
				else
				{
					C.Z = (float)(Floor + 1) * FloorH;
				}


				const FQuat RotQ = Sources.IsValidIndex(OwnerIdx) ? Sources[OwnerIdx].RotQ : FQuat::Identity;

				FPCGPoint Pt;
				Pt.Transform = FTransform(RotQ, C, FVector::OneVector);
				Pt.BoundsMin = FVector(-VoxelSize * 0.5f, -VoxelSize * 0.5f, -1.0f);
				Pt.BoundsMax = FVector(+VoxelSize * 0.5f, +VoxelSize * 0.5f, +1.0f);
				Pt.Density = Settings->Density;

				const PCGMetadataEntryKey Entry = BoundaryOut->Metadata->AddEntry();
				Pt.MetadataEntry = Entry;

				if (BFloorAttr)   BFloorAttr->SetValue(Entry, Floor + 1);
				if (BCAttr)       BCAttr->SetValue(Entry, C);
				if (BOwnerAttr)   BOwnerAttr->SetValue(Entry, OwnerIdx);

				if (Sources.IsValidIndex(OwnerIdx))
				{
					const FSrc& OS = Sources[OwnerIdx];

					if (BSupportAttr) BSupportAttr->SetValue(Entry, OS.SupportGrammar);

					if (BFacadeAttr)  BFacadeAttr->SetValue(Entry, OS.FacadeGrammar);
					if (BRoofAttr)    BRoofAttr->SetValue(Entry, OS.RoofGrammar);
					if (BAllowScale)  BAllowScale->SetValue(Entry, OS.bAllowNonUniformScale);
					if (BScaleClamp)  BScaleClamp->SetValue(Entry, OS.ScaleClamp);
					if (BStyleAttr)   BStyleAttr->SetValue(Entry, OS.StyleTag);
					if (BPriAttr)     BPriAttr->SetValue(Entry, OS.Priority);
					if (BVolIdxAttr)  BVolIdxAttr->SetValue(Entry, OS.VolumeIndex);
				}

				BPts.Add(Pt);
			}
		}

		Emit(Context, ShellOut, PCGBuildingPins::ShellOut);
		Emit(Context, FootprintOut, PCGBuildingPins::FootprintOut);
		Emit(Context, BoundaryOut, PCGBuildingPins::OverhangBoundaryOut);
		return true;
	}

private:
	static void Emit(FPCGContext* Context, UPCGData* Data, const FName Pin)
	{
		FPCGTaggedData& Tag = Context->OutputData.TaggedData.Emplace_GetRef();
		Tag.Pin = Pin;
		Tag.Data = Data;
	}
};

TArray<FPCGPinProperties> UPCGVoxelUnionMassingSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::VolumesIn;
	P.AllowedTypes = EPCGDataType::Point;
	Pins.Add(P);
	return Pins;
}

TArray<FPCGPinProperties> UPCGVoxelUnionMassingSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	{
		FPCGPinProperties P;
		P.Label = PCGBuildingPins::ShellOut;
		P.AllowedTypes = EPCGDataType::Point;
		Pins.Add(P);
	}
	{
		FPCGPinProperties P;
		P.Label = PCGBuildingPins::FootprintOut;
		P.AllowedTypes = EPCGDataType::Point;
		Pins.Add(P);
	}
	{
		FPCGPinProperties P;
		P.Label = PCGBuildingPins::OverhangBoundaryOut;
		P.AllowedTypes = EPCGDataType::Point;
		Pins.Add(P);
	}

	return Pins;
}

FPCGElementPtr UPCGVoxelUnionMassingSettings::CreateElement() const
{
	return MakeShared<FPCGVoxelUnionMassingElement>().ToSharedPtr();
}

// ------------------------------
// Node 3: OverhangSupports
// ------------------------------
class FPCGOverhangSupportsElement : public IPCGElement
{
public:
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override
	{
		return false; // 依赖 Actor 上的 Volumes 数组（PCG 不追踪），必须禁用缓存
	}

	virtual bool ExecuteInternal(FPCGContext* Context) const override
	{
		check(Context);
		const UPCGOverhangSupportsSettings* Settings = Context->GetInputSettings<UPCGOverhangSupportsSettings>();
		if (!Settings) return true;

		UPCGComponent* SourceComp = Context->SourceComponent.Get();
		AActor* Owner = SourceComp ? SourceComp->GetOwner() : nullptr;
		UBuildingVolumeComponent* BVC = FindBuildingVolumeComponent(Owner, NAME_None);

		const float FloorH = (BVC ? BVC->FloorHeight : 400.0f);
		const float SpacingRaw = (Settings->SupportSpacingOverride > 0.0f) ? Settings->SupportSpacingOverride : (BVC ? BVC->BoundaryStep : 100.0f);
		const float Spacing = FMath::Max(1.0f, SpacingRaw);

		const UPCGPointData* BoundaryPD = nullptr;
		for (const FPCGTaggedData& In : Context->InputData.TaggedData)
		{
			if (In.Pin == PCGBuildingPins::BoundaryIn)
			{
				BoundaryPD = Cast<UPCGPointData>(In.Data);
				break;
			}
		}

		UPCGPointData* Out = NewObject<UPCGPointData>();
		Out->InitializeFromData(nullptr);

		UPCGMetadata* MD = Out->Metadata;
		FPCGMetadataAttribute<FString>* SupportAttr = FindOrCreateAttr<FString>(MD, TEXT("SupportGrammar"), Settings->DefaultSupportGrammar, false);
		FPCGMetadataAttribute<int32>* FloorAttr = FindOrCreateAttr<int32>(MD, TEXT("FloorIndex"), 0, true);

		if (!BoundaryPD)
		{
			Emit(Context, Out, PCGBuildingPins::SupportsOut);
			return true;
		}

		TSet<int64> UsedCells;
		TArray<FPCGPoint>& Pts = Out->GetMutablePoints();
		Pts.Reserve(BoundaryPD->GetPoints().Num() * 2);

		for (const FPCGPoint& B : BoundaryPD->GetPoints())
		{
			const FVector C = B.Transform.GetLocation();
			const FQuat BoundaryRotQ = B.Transform.GetRotation(); // 继承二层/Owner 的旋转（你图1里就是 60）

			const int32 cx = FMath::FloorToInt(C.X / Spacing);
			const int32 cy = FMath::FloorToInt(C.Y / Spacing);
			const int64 CellKey = Pack3(cx, cy, 0);
			if (UsedCells.Contains(CellKey)) continue;
			UsedCells.Add(CellKey);

			int32 FloorIndex = FMath::Max(0, FMath::FloorToInt(C.Z / FloorH));
			if (BoundaryPD->Metadata && B.MetadataEntry != PCGInvalidEntryKey)
			{
				int32 Tmp = 0;
				if (GetAttrValueSingle<int32>(BoundaryPD->Metadata, TEXT("FloorIndex"), B.MetadataEntry, Tmp))
				{
					FloorIndex = Tmp;
				}
			}

			FString Grammar = Settings->DefaultSupportGrammar;
			if (BoundaryPD->Metadata && B.MetadataEntry != PCGInvalidEntryKey)
			{
				FString TmpG;
				if (GetAttrValueSingle<FString>(BoundaryPD->Metadata, TEXT("SupportGrammar"), B.MetadataEntry, TmpG))
				{
					if (!TmpG.IsEmpty()) Grammar = TmpG;
				}
			}

			const float MaxDrop = (Settings->MaxDropHeightOverride > 0.0f) ? Settings->MaxDropHeightOverride : ((FloorIndex + 1) * FloorH);
			const int32 Steps = FMath::Max(1, FMath::RoundToInt(MaxDrop / FloorH));

			for (int32 i = 0; i < Steps; ++i)
			{
				const FVector P0 = FVector(C.X, C.Y, C.Z - i * FloorH);

				FPCGPoint P;
				P.Transform = FTransform(BoundaryRotQ, P0, FVector::OneVector);
				P.BoundsMin = FVector(-10.0f);
				P.BoundsMax = FVector(+10.0f);
				P.Density = Settings->Density;

				const PCGMetadataEntryKey Entry = MD->AddEntry();
				P.MetadataEntry = Entry;

				if (SupportAttr) SupportAttr->SetValue(Entry, Grammar);
				if (FloorAttr)   FloorAttr->SetValue(Entry, FloorIndex);

				Pts.Add(P);
			}
		}

		Emit(Context, Out, PCGBuildingPins::SupportsOut);
		return true;
	}

private:
	static void Emit(FPCGContext* Context, UPCGData* Data, const FName Pin)
	{
		FPCGTaggedData& Tag = Context->OutputData.TaggedData.Emplace_GetRef();
		Tag.Pin = Pin;
		Tag.Data = Data;
	}
};

TArray<FPCGPinProperties> UPCGOverhangSupportsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::BoundaryIn;
	P.AllowedTypes = EPCGDataType::Point;
	Pins.Add(P);
	return Pins;
}

TArray<FPCGPinProperties> UPCGOverhangSupportsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::SupportsOut;
	P.AllowedTypes = EPCGDataType::Point;
	Pins.Add(P);
	return Pins;
}

FPCGElementPtr UPCGOverhangSupportsSettings::CreateElement() const
{
	return MakeShared<FPCGOverhangSupportsElement>().ToSharedPtr();
}

// ------------------------------
// Node 4: GrammarToMesh
// ------------------------------
static FString ExtractFirstTokenFromGrammar(const FString& G)
{
	// 简单解析：找第一个 [...] 的内容；若没有，则取第一个连续字母数字串
	int32 L = INDEX_NONE, R = INDEX_NONE;
	if (G.FindChar('[', L))
	{
		R = G.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromStart, L + 1);
		if (R != INDEX_NONE && R > L + 1)
		{
			FString Tok = G.Mid(L + 1, R - L - 1);
			Tok.TrimStartAndEndInline();
			return Tok;
		}
	}

	// fallback：扫描首个 token
	FString Tok;
	for (int32 i = 0; i < G.Len(); ++i)
	{
		const TCHAR C = G[i];
		if (FChar::IsAlnum(C) || C == '_' )
		{
			Tok += C;
		}
		else if (!Tok.IsEmpty())
		{
			break;
		}
	}
	return Tok;
}

class FPCGGrammarToMeshElement : public IPCGElement
{
public:
	virtual bool ExecuteInternal(FPCGContext* Context) const override
	{
		check(Context);
		const UPCGGrammarToMeshSettings* Settings = Context->GetInputSettings<UPCGGrammarToMeshSettings>();
		if (!Settings) return true;

		// gather input point data
		const UPCGPointData* InPD = nullptr;
		for (const FPCGTaggedData& In : Context->InputData.TaggedData)
		{
			if (const UPCGPointData* PD = Cast<UPCGPointData>(In.Data))
			{
				InPD = PD;
				break;
			}
		}

		UPCGPointData* Out = NewObject<UPCGPointData>();
		Out->InitializeFromData(InPD);
		Out->Metadata = NewObject<UPCGMetadata>(Out);

		// 先准备输出点数组（OutPts），后面 EntryKeys 才能用
		TArray<FPCGPoint>& OutPts = Out->GetMutablePoints();
		OutPts = InPD ? InPD->GetPoints() : TArray<FPCGPoint>();

		// 只复制输出点会用到的 entry keys（可选但更严谨）
		TArray<int64> EntryKeys;
		EntryKeys.Reserve(OutPts.Num());
		for (const FPCGPoint& P : OutPts)
		{
			if (P.MetadataEntry != PCGInvalidEntryKey)
			{
				EntryKeys.Add(P.MetadataEntry);
			}
		}

		Out->Metadata->InitializeAsCopy(InPD ? InPD->Metadata : nullptr, &EntryKeys);

		// Output attribute: SoftObjectPath mesh
		FPCGMetadataAttribute<FSoftObjectPath>* MeshAttr =
			FindOrCreateAttr<FSoftObjectPath>(Out->Metadata, Settings->MeshAttributeName, FSoftObjectPath(), false);

		// Optional: read ExtentsWS (HalfExtents)
		const FPCGMetadataAttribute<FVector>* ExtAttr = nullptr;
		if (InPD && InPD->Metadata)
		{
			ExtAttr = InPD->Metadata->GetConstTypedAttribute<FVector>(Settings->ExtentsAttributeName);
		}


		for (FPCGPoint& P : OutPts)
		{
			if (P.MetadataEntry == PCGInvalidEntryKey) continue;

			// read grammar string
			FString Grammar;
			bool bHasGrammar = false;
			if (InPD && InPD->Metadata)
			{
				bHasGrammar = GetAttrValueSingle<FString>(InPD->Metadata, Settings->GrammarAttributeName, P.MetadataEntry, Grammar);
			}

			const FString Tok = bHasGrammar ? ExtractFirstTokenFromGrammar(Grammar) : FString();
			TSoftObjectPtr<UStaticMesh> MeshPtr = nullptr;

			if (!Tok.IsEmpty())
			{
				if (const TSoftObjectPtr<UStaticMesh>* Found = Settings->SymbolToMesh.Find(Tok))
				{
					MeshPtr = *Found;
				}
			}

			if (!MeshPtr.IsValid() && Settings->DefaultMesh.IsValid())
			{
				MeshPtr = Settings->DefaultMesh;
			}

			if (MeshAttr)
			{
				const FSoftObjectPath Path = MeshPtr.IsNull() ? FSoftObjectPath() : MeshPtr.ToSoftObjectPath();
				MeshAttr->SetValue(P.MetadataEntry, Path);
			}

			if (Settings->bFitScaleToTarget && !MeshPtr.IsNull())
			{
				UStaticMesh* SM = MeshPtr.LoadSynchronous();
				if (SM)
				{
					const FVector MeshSize = SM->GetBounds().BoxExtent * 2.0f;
					FVector TargetSize = (P.BoundsMax - P.BoundsMin); // 默认用点 bounds

					// 若有 ExtentsWS，则用 ExtentsWS*2 更稳定（Volume 点适配）
					if (ExtAttr)
					{
						FVector Ext;
						TArray<FVector> TmpV; TmpV.SetNum(1);
						TArray<PCGMetadataEntryKey> Keys; Keys.Add(P.MetadataEntry);
						const TArrayView<const PCGMetadataEntryKey> KV(Keys.GetData(), Keys.Num());
						TArrayView<FVector> VV(TmpV.GetData(), TmpV.Num());
						ExtAttr->GetValuesFromItemKeys(KV, VV);
						Ext = TmpV[0];
						if (!Ext.IsNearlyZero())
						{
							TargetSize = Ext * 2.0f;
						}
					}

					FVector Scale(
						MeshSize.X > KINDA_SMALL_NUMBER ? TargetSize.X / MeshSize.X : 1.0f,
						MeshSize.Y > KINDA_SMALL_NUMBER ? TargetSize.Y / MeshSize.Y : 1.0f,
						MeshSize.Z > KINDA_SMALL_NUMBER ? TargetSize.Z / MeshSize.Z : 1.0f
					);

					if (Settings->bForceUniformScale)
					{
						const float S = FMath::Min3(Scale.X, Scale.Y, Scale.Z);
						Scale = FVector(S);
					}

					Scale.X = FMath::Clamp(Scale.X, Settings->ScaleClamp.X, Settings->ScaleClamp.Y);
					Scale.Y = FMath::Clamp(Scale.Y, Settings->ScaleClamp.X, Settings->ScaleClamp.Y);
					Scale.Z = FMath::Clamp(Scale.Z, Settings->ScaleClamp.X, Settings->ScaleClamp.Y);

					P.Transform.SetScale3D(Scale);
				}
			}
		}

		FPCGTaggedData& Tag = Context->OutputData.TaggedData.Emplace_GetRef();
		Tag.Pin = PCGBuildingPins::PointsOut;
		Tag.Data = Out;
		return true;
	}
};

TArray<FPCGPinProperties> UPCGGrammarToMeshSettings::InputPinProperties() const
{
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::PointsIn;
	P.AllowedTypes = EPCGDataType::Point;
	return { P };
}

TArray<FPCGPinProperties> UPCGGrammarToMeshSettings::OutputPinProperties() const
{
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::PointsOut;
	P.AllowedTypes = EPCGDataType::Point;
	return { P };
}

FPCGElementPtr UPCGGrammarToMeshSettings::CreateElement() const
{
	TSharedRef<FPCGGrammarToMeshElement, ESPMode::ThreadSafe> Ref = MakeShared<FPCGGrammarToMeshElement, ESPMode::ThreadSafe>();
	return StaticCastSharedPtr<IPCGElement>(Ref.ToSharedPtr());
}

// ------------------------------
// Node 5: ResolveMeshFromLibrary (Tile, Single-Scale Remainder)
// ------------------------------

static EAxis::Type ToAxis(EPCGTileAxis A)
{
	switch (A)
	{
	case EPCGTileAxis::Y: return EAxis::Y;
	case EPCGTileAxis::Z: return EAxis::Z;
	default: return EAxis::X;
	}
}

static EPCGTileAxis PickLongestLocalAxis(const FPCGPoint& P)
{
	const FVector Size = (P.BoundsMax - P.BoundsMin).GetAbs();
	if (Size.Y >= Size.X && Size.Y >= Size.Z) return EPCGTileAxis::Y;
	if (Size.Z >= Size.X && Size.Z >= Size.Y) return EPCGTileAxis::Z;
	return EPCGTileAxis::X;
}

static float GetTargetLengthAlongAxis(
	const UPCGPointData* InPD,
	const FPCGPoint& P,
	const FName ExtentsAttrName,
	EAxis::Type Axis)
{
	// prefer ExtentsWS*2 if available
	if (InPD && InPD->Metadata && ExtentsAttrName != NAME_None && P.MetadataEntry != PCGInvalidEntryKey)
	{
		FVector Ext = FVector::ZeroVector;
		if (GetAttrValueSingle<FVector>(InPD->Metadata, ExtentsAttrName, P.MetadataEntry, Ext) && !Ext.IsNearlyZero())
		{
			const FVector TargetSize = Ext * 2.0f;
			return (Axis == EAxis::X) ? TargetSize.X : (Axis == EAxis::Y ? TargetSize.Y : TargetSize.Z);
		}
	}

	// fallback to local bounds
	const FVector LocalSize = (P.BoundsMax - P.BoundsMin).GetAbs();
	return (Axis == EAxis::X) ? LocalSize.X : (Axis == EAxis::Y ? LocalSize.Y : LocalSize.Z);
}

static float GetMeshLengthAlongAxis(UStaticMesh* SM, EAxis::Type Axis)
{
	if (!SM) return 0.0f;
	const FVector MeshSize = SM->GetBounds().BoxExtent * 2.0f; // size = 2*extent :contentReference[oaicite:3]{index=3} (logic)
	return (Axis == EAxis::X) ? MeshSize.X : (Axis == EAxis::Y ? MeshSize.Y : MeshSize.Z);
}

static const FGrammarMeshEntry* FindEntryBySymbol(const UGrammarMeshLibraryDataAsset* Lib, FName Sym)
{
	if (!Lib || Sym.IsNone()) return nullptr;
	for (const FGrammarMeshEntry& E : Lib->Entries)
	{
		if (E.Symbol == Sym) return &E;
	}
	return nullptr;
}

class FPCGResolveMeshFromLibraryElement : public IPCGElement
{
public:
	virtual bool ExecuteInternal(FPCGContext* Context) const override
	{
		check(Context);
		const UPCGResolveMeshFromLibrarySettings* Settings = Context->GetInputSettings<UPCGResolveMeshFromLibrarySettings>();
		if (!Settings) return true;

		// input
		const UPCGPointData* InPD = nullptr;
		for (const FPCGTaggedData& In : Context->InputData.TaggedData)
		{
			if (const UPCGPointData* PD = Cast<UPCGPointData>(In.Data))
			{
				InPD = PD;
				break;
			}
		}
		if (!InPD)
		{
			return true;
		}

		// output (we will expand points, so don't InitializeFromData(InPD))
		UPCGPointData* Out = NewObject<UPCGPointData>();
		Out->InitializeFromData(nullptr);

		UPCGMetadata* OutMD = Out->Metadata;

		FPCGMetadataAttribute<FSoftObjectPath>* MeshAttr =
			FindOrCreateAttr<FSoftObjectPath>(OutMD, Settings->MeshAttributeName, FSoftObjectPath(), false);

		FPCGMetadataAttribute<FString>* FullNameAttr =
			FindOrCreateAttr<FString>(OutMD, Settings->MeshFullNameAttributeName, TEXT(""), false);

		FPCGMetadataAttribute<FName>* OutSymAttr =
			FindOrCreateAttr<FName>(OutMD, Settings->OutSymbolAttributeName, NAME_None, false);

		FPCGMetadataAttribute<int32>* SegIdxAttr =
			FindOrCreateAttr<int32>(OutMD, Settings->SegmentIndexAttributeName, 0, true);

		FPCGMetadataAttribute<int32>* SegCntAttr =
			FindOrCreateAttr<int32>(OutMD, Settings->SegmentCountAttributeName, 0, true);

		const TArray<FPCGPoint>& InPts = InPD->GetPoints();
		TArray<FPCGPoint>& OutPts = Out->GetMutablePoints();
		OutPts.Reserve(InPts.Num()); // may expand more; reserve baseline

		for (const FPCGPoint& InP : InPts)
		{
			// 1) get token/symbol
			FString TokStr;
			FName Sym = NAME_None;

			if (Settings->bUseGrammarAttribute)
			{
				FString Grammar;
				if (InP.MetadataEntry != PCGInvalidEntryKey &&
					GetAttrValueSingle<FString>(InPD->Metadata, Settings->GrammarAttributeName, InP.MetadataEntry, Grammar))
				{
					TokStr = ExtractFirstTokenFromGrammar(Grammar);
					if (!TokStr.IsEmpty())
					{
						Sym = FName(*TokStr);
					}
				}
			}
			else
			{
				// prefer FName; fallback to FString
				if (InP.MetadataEntry != PCGInvalidEntryKey &&
					!GetAttrValueSingle<FName>(InPD->Metadata, Settings->SymbolAttributeName, InP.MetadataEntry, Sym))
				{
					FString S;
					if (GetAttrValueSingle<FString>(InPD->Metadata, Settings->SymbolAttributeName, InP.MetadataEntry, S) && !S.IsEmpty())
					{
						Sym = FName(*S);
					}
				}
			}

			const FGrammarMeshEntry* Entry = FindEntryBySymbol(Settings->Library, Sym);

			TSoftObjectPtr<UStaticMesh> MeshPtr = Entry ? Entry->Mesh : TSoftObjectPtr<UStaticMesh>();
			FString FullName = Entry ? Entry->FullName : FString();

			if (MeshPtr.IsNull() && Settings->DefaultMesh.IsValid())
			{
				MeshPtr = Settings->DefaultMesh;
				if (FullName.IsEmpty()) FullName = TEXT("DefaultMesh");
			}

			UStaticMesh* SM = MeshPtr.IsNull() ? nullptr : MeshPtr.LoadSynchronous();
			if (!SM)
			{
				// still output one point but empty mesh path (lets you debug missing)
				FPCGPoint P = InP;
				P.MetadataEntry = OutMD->AddEntry();
				if (MeshAttr) MeshAttr->SetValue(P.MetadataEntry, FSoftObjectPath());
				if (FullNameAttr) FullNameAttr->SetValue(P.MetadataEntry, FullName);
				if (OutSymAttr) OutSymAttr->SetValue(P.MetadataEntry, Sym);
				if (SegIdxAttr) SegIdxAttr->SetValue(P.MetadataEntry, 0);
				if (SegCntAttr) SegCntAttr->SetValue(P.MetadataEntry, 1);
				OutPts.Add(P);
				continue;
			}

			// 2) decide axis
			EPCGTileAxis AxisPick = Settings->TileAxis;
			if (AxisPick == EPCGTileAxis::LongestLocalAxis)
			{
				AxisPick = PickLongestLocalAxis(InP);
			}
			const EAxis::Type Axis = ToAxis(AxisPick);

			// 3) compute tiling
			const float TargetLen = GetTargetLengthAlongAxis(InPD, InP, Settings->ExtentsAttributeName, Axis);
			const float StdLen = GetMeshLengthAlongAxis(SM, Axis);

			const bool bAllowScale = Entry ? Entry->bAllowRemainderScale : true;

			if (!Settings->bEnableTiling || TargetLen <= KINDA_SMALL_NUMBER || StdLen <= KINDA_SMALL_NUMBER)
			{
				// single point, no tiling
				FPCGPoint P = InP;
				P.MetadataEntry = OutMD->AddEntry();

				if (MeshAttr) MeshAttr->SetValue(P.MetadataEntry, MeshPtr.ToSoftObjectPath());
				if (FullNameAttr) FullNameAttr->SetValue(P.MetadataEntry, FullName);
				if (OutSymAttr) OutSymAttr->SetValue(P.MetadataEntry, Sym);
				if (SegIdxAttr) SegIdxAttr->SetValue(P.MetadataEntry, 0);
				if (SegCntAttr) SegCntAttr->SetValue(P.MetadataEntry, 1);

				OutPts.Add(P);
				continue;
			}

			// n full segments + optional one scaled remainder (or merge remainder into last)
			int32 NFull = FMath::FloorToInt(TargetLen / StdLen);
			float R = TargetLen - (float)NFull * StdLen;

			if (NFull <= 0)
			{
				// only one segment, scaled to fit (allowed: only 1 scaled)
				const float ScaleA = bAllowScale ? FMath::Clamp(TargetLen / StdLen, Settings->RemainderScaleClamp.X, Settings->RemainderScaleClamp.Y) : 1.0f;

				FPCGPoint P = InP;
				P.Transform.SetScale3D(FVector::OneVector);
				FVector S = P.Transform.GetScale3D();
				if (Axis == EAxis::X) S.X *= ScaleA;
				else if (Axis == EAxis::Y) S.Y *= ScaleA;
				else S.Z *= ScaleA;
				P.Transform.SetScale3D(S);

				// bounds: set axis half-length = TargetLen/2
				P.BoundsMin = InP.BoundsMin;
				P.BoundsMax = InP.BoundsMax;
				if (Axis == EAxis::X) { P.BoundsMin.X = -TargetLen * 0.5f; P.BoundsMax.X = TargetLen * 0.5f; }
				if (Axis == EAxis::Y) { P.BoundsMin.Y = -TargetLen * 0.5f; P.BoundsMax.Y = TargetLen * 0.5f; }
				if (Axis == EAxis::Z) { P.BoundsMin.Z = -TargetLen * 0.5f; P.BoundsMax.Z = TargetLen * 0.5f; }

				P.MetadataEntry = OutMD->AddEntry();
				if (MeshAttr) MeshAttr->SetValue(P.MetadataEntry, MeshPtr.ToSoftObjectPath());
				if (FullNameAttr) FullNameAttr->SetValue(P.MetadataEntry, FullName);
				if (OutSymAttr) OutSymAttr->SetValue(P.MetadataEntry, Sym);
				if (SegIdxAttr) SegIdxAttr->SetValue(P.MetadataEntry, 0);
				if (SegCntAttr) SegCntAttr->SetValue(P.MetadataEntry, 1);

				OutPts.Add(P);
				continue;
			}

			// if remainder is tiny -> merge into last full segment (still only 1 scaled segment)
			const bool bMergeRemainder = (R > KINDA_SMALL_NUMBER && R < Settings->MinRemainderWorld);
			const int32 OutCount = bMergeRemainder ? NFull : (R > KINDA_SMALL_NUMBER ? (NFull + 1) : NFull);

			// axis direction in world
			const FVector AxisDirWS =
				(Axis == EAxis::X) ? InP.Transform.GetUnitAxis(EAxis::X) :
				(Axis == EAxis::Y) ? InP.Transform.GetUnitAxis(EAxis::Y) :
				InP.Transform.GetUnitAxis(EAxis::Z);

			float Cursor = -TargetLen * 0.5f;

			for (int32 i = 0; i < OutCount; ++i)
			{
				const bool bIsLast = (i == OutCount - 1);

				float SegLen = StdLen;
				float ScaleA = 1.0f;

				if (bMergeRemainder && bIsLast)
				{
					SegLen = StdLen + R;
					if (bAllowScale)
					{
						ScaleA = FMath::Clamp(SegLen / StdLen, Settings->RemainderScaleClamp.X, Settings->RemainderScaleClamp.Y);
					}
				}
				else if (!bMergeRemainder && bIsLast && R > KINDA_SMALL_NUMBER)
				{
					SegLen = R;
					if (bAllowScale)
					{
						ScaleA = FMath::Clamp(SegLen / StdLen, Settings->RemainderScaleClamp.X, Settings->RemainderScaleClamp.Y);
					}
				}

				const float CenterOffset = Cursor + SegLen * 0.5f;
				Cursor += SegLen;

				FPCGPoint P = InP;
				P.Transform.SetScale3D(FVector::OneVector);

				// translate along axis
				const FVector NewLoc = InP.Transform.GetLocation() + AxisDirWS * CenterOffset;
				P.Transform.SetLocation(NewLoc);

				// apply scale only along tile axis (at most one segment will be scaled)
				FVector S = P.Transform.GetScale3D();
				if (Axis == EAxis::X) S.X *= ScaleA;
				else if (Axis == EAxis::Y) S.Y *= ScaleA;
				else S.Z *= ScaleA;
				P.Transform.SetScale3D(S);

				// bounds: set axis half-length = SegLen/2; keep other axes as original
				P.BoundsMin = InP.BoundsMin;
				P.BoundsMax = InP.BoundsMax;
				if (Axis == EAxis::X) { P.BoundsMin.X = -SegLen * 0.5f; P.BoundsMax.X = SegLen * 0.5f; }
				if (Axis == EAxis::Y) { P.BoundsMin.Y = -SegLen * 0.5f; P.BoundsMax.Y = SegLen * 0.5f; }
				if (Axis == EAxis::Z) { P.BoundsMin.Z = -SegLen * 0.5f; P.BoundsMax.Z = SegLen * 0.5f; }

				P.MetadataEntry = OutMD->AddEntry();
				if (MeshAttr) MeshAttr->SetValue(P.MetadataEntry, MeshPtr.ToSoftObjectPath());
				if (FullNameAttr) FullNameAttr->SetValue(P.MetadataEntry, FullName);
				if (OutSymAttr) OutSymAttr->SetValue(P.MetadataEntry, Sym);
				if (SegIdxAttr) SegIdxAttr->SetValue(P.MetadataEntry, i);
				if (SegCntAttr) SegCntAttr->SetValue(P.MetadataEntry, OutCount);

				OutPts.Add(P);
			}
		}

		FPCGTaggedData& Tag = Context->OutputData.TaggedData.Emplace_GetRef();
		Tag.Pin = PCGBuildingPins::PointsOut;
		Tag.Data = Out;
		return true;
	}
};

TArray<FPCGPinProperties> UPCGResolveMeshFromLibrarySettings::InputPinProperties() const
{
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::PointsIn;
	P.AllowedTypes = EPCGDataType::Point;
	return { P };
}

TArray<FPCGPinProperties> UPCGResolveMeshFromLibrarySettings::OutputPinProperties() const
{
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::PointsOut;
	P.AllowedTypes = EPCGDataType::Point;
	return { P };
}

FPCGElementPtr UPCGResolveMeshFromLibrarySettings::CreateElement() const
{
	return MakeShared<FPCGResolveMeshFromLibraryElement>().ToSharedPtr();
}

// ------------------------------
// Node 6: GrammarSegmentsFromLibrary (Full Grammar, Pivot Fix, Single-Scale Tail)
// ------------------------------

struct FGrammarGroup
{
	TArray<FName> Options;
	bool bRepeat = false;
};

static void ParseBracketGrammar(const FString& G, TArray<FGrammarGroup>& OutGroups)
{
	OutGroups.Reset();

	const int32 N = G.Len();
	int32 i = 0;
	while (i < N)
	{
		const TCHAR C = G[i];
		if (C == '[')
		{
			int32 j = i + 1;
			while (j < N && G[j] != ']') ++j;
			if (j >= N) break;

			const FString Inner = G.Mid(i + 1, j - i - 1);
			FGrammarGroup Group;

			TArray<FString> Parts;
			Inner.ParseIntoArray(Parts, TEXT(","), true);
			for (FString& P : Parts)
			{
				P.TrimStartAndEndInline();
				if (!P.IsEmpty())
				{
					Group.Options.Add(FName(*P));
				}
			}

			i = j + 1;
			// optional repeat marker
			if (i < N && G[i] == '*')
			{
				Group.bRepeat = true;
				++i;
			}

			if (Group.Options.Num() > 0)
			{
				OutGroups.Add(Group);
			}
			continue;
		}

		++i;
	}
}

struct FResolved
{
	FName Symbol = NAME_None;
	TSoftObjectPtr<UStaticMesh> Mesh;
	FString FullName;
	bool bAllowTailScale = true;
	float StdLen = 0.0f; // along tile axis in local
	FVector BoundsOrigin = FVector::ZeroVector; // local bounds origin (pivot-relative)
	FVector BoundsExtent = FVector::ZeroVector; // local bounds half extents
};

static bool ResolveSymbol(
	const UGrammarMeshLibraryDataAsset* Lib,
	FName Sym,
	const TSoftObjectPtr<UStaticMesh>& DefaultMesh,
	EAxis::Type Axis,
	TMap<FName, FResolved>& Cache,
	FResolved& OutR)
{
	if (Sym.IsNone()) return false;

	if (const FResolved* Found = Cache.Find(Sym))
	{
		OutR = *Found;
		return true;
	}

	FResolved R;
	R.Symbol = Sym;

	const FGrammarMeshEntry* E = FindEntryBySymbol(Lib, Sym);
	if (E)
	{
		R.Mesh = E->Mesh;
		R.FullName = E->FullName;
		R.bAllowTailScale = E->bAllowRemainderScale;
	}
	else
	{
		R.Mesh = DefaultMesh;
		R.FullName = TEXT("");
		R.bAllowTailScale = true;
	}

	UStaticMesh* SM = R.Mesh.LoadSynchronous();
	if (SM)
	{
		const FBoxSphereBounds B = SM->GetBounds();
		R.BoundsOrigin = B.Origin;
		R.BoundsExtent = B.BoxExtent;

		const FVector Size = B.BoxExtent * 2.0f;
		R.StdLen = (Axis == EAxis::X) ? Size.X : (Axis == EAxis::Y ? Size.Y : Size.Z);
	}

	Cache.Add(Sym, R);
	OutR = R;
	return true;
}

static int32 FindRepeatIndex(const TArray<FGrammarGroup>& Groups)
{
	for (int32 i = 0; i < Groups.Num(); ++i)
	{
		if (Groups[i].bRepeat) return i;
	}
	return INDEX_NONE;
}

static bool PickBestFitOption(
	const UGrammarMeshLibraryDataAsset* Lib,
	const TArray<FName>& Options,
	const TSoftObjectPtr<UStaticMesh>& DefaultMesh,
	EAxis::Type Axis,
	float MaxLen,
	TMap<FName, FResolved>& Cache,
	FResolved& OutBest)
{
	bool bFound = false;
	float BestLen = -1.0f;

	for (const FName& Sym : Options)
	{
		FResolved R;
		if (!ResolveSymbol(Lib, Sym, DefaultMesh, Axis, Cache, R)) continue;
		if (R.StdLen <= KINDA_SMALL_NUMBER) continue;

		if (R.StdLen <= MaxLen + KINDA_SMALL_NUMBER && R.StdLen > BestLen)
		{
			BestLen = R.StdLen;
			OutBest = R;
			bFound = true;
		}
	}

	// if nothing fits, still return the first resolvable (caller may scale tail)
	if (!bFound)
	{
		for (const FName& Sym : Options)
		{
			FResolved R;
			if (ResolveSymbol(Lib, Sym, DefaultMesh, Axis, Cache, R) && R.StdLen > KINDA_SMALL_NUMBER)
			{
				OutBest = R;
				return true;
			}
		}
	}

	return bFound;
}

class FPCGGrammarSegmentsFromLibraryElement : public IPCGElement
{
public:
	virtual bool ExecuteInternal(FPCGContext* Context) const override
	{
		check(Context);
		const UPCGGrammarSegmentsFromLibrarySettings* Settings = Context->GetInputSettings<UPCGGrammarSegmentsFromLibrarySettings>();
		if (!Settings) return true;

		const UPCGPointData* InPD = nullptr;
		for (const FPCGTaggedData& In : Context->InputData.TaggedData)
		{
			if (In.Pin == PCGBuildingPins::PointsIn || In.Pin == PCGBuildingPins::ShellOut || In.Pin == PCGBuildingPins::FootprintOut || In.Pin == PCGBuildingPins::OverhangBoundaryOut)
			{
				InPD = Cast<UPCGPointData>(In.Data);
				if (InPD) break;
			}
		}

		if (!InPD)
		{
			return true;
		}

		UPCGPointData* Out = NewObject<UPCGPointData>();
		Out->InitializeFromData(nullptr);
		UPCGMetadata* OutMD = Out->Metadata;

		// output attrs
		FPCGMetadataAttribute<FSoftObjectPath>* MeshAttr = FindOrCreateAttr<FSoftObjectPath>(OutMD, Settings->MeshAttributeName, FSoftObjectPath(), false);
		FPCGMetadataAttribute<FString>* FullNameAttr = FindOrCreateAttr<FString>(OutMD, Settings->MeshFullNameAttributeName, TEXT(""), false);
		FPCGMetadataAttribute<FName>* SymAttr = FindOrCreateAttr<FName>(OutMD, Settings->OutSymbolAttributeName, NAME_None, false);
		FPCGMetadataAttribute<int32>* SegIdxAttr = FindOrCreateAttr<int32>(OutMD, Settings->SegmentIndexAttributeName, 0, true);
		FPCGMetadataAttribute<int32>* SegCntAttr = FindOrCreateAttr<int32>(OutMD, Settings->SegmentCountAttributeName, 0, true);

		// pass-through commonly used attrs (optional)
		auto CopyInt = [&](const FName Name, const FPCGPoint& InP, PCGMetadataEntryKey OutEntry)
			{
				int32 V = 0;
				if (InPD->Metadata && InP.MetadataEntry != PCGInvalidEntryKey && GetAttrValueSingle<int32>(InPD->Metadata, Name, InP.MetadataEntry, V))
				{
					auto* A = FindOrCreateAttr<int32>(OutMD, Name, 0, true);
					if (A) A->SetValue(OutEntry, V);
				}
			};
		auto CopyName = [&](const FName Name, const FPCGPoint& InP, PCGMetadataEntryKey OutEntry)
			{
				FName V = NAME_None;
				if (InPD->Metadata && InP.MetadataEntry != PCGInvalidEntryKey && GetAttrValueSingle<FName>(InPD->Metadata, Name, InP.MetadataEntry, V))
				{
					auto* A = FindOrCreateAttr<FName>(OutMD, Name, NAME_None, false);
					if (A) A->SetValue(OutEntry, V);
				}
			};
		auto CopyStr = [&](const FName Name, const FPCGPoint& InP, PCGMetadataEntryKey OutEntry)
			{
				FString V;
				if (InPD->Metadata && InP.MetadataEntry != PCGInvalidEntryKey && GetAttrValueSingle<FString>(InPD->Metadata, Name, InP.MetadataEntry, V))
				{
					auto* A = FindOrCreateAttr<FString>(OutMD, Name, TEXT(""), false);
					if (A) A->SetValue(OutEntry, V);
				}
			};

		TArray<FPCGPoint>& OutPts = Out->GetMutablePoints();
		OutPts.Reserve(InPD->GetPoints().Num() * 4);

		TMap<FName, FResolved> ResCache;

		for (const FPCGPoint& InP : InPD->GetPoints())
		{
			// grammar string
			FString Grammar;
			if (!InPD->Metadata || InP.MetadataEntry == PCGInvalidEntryKey ||
				!GetAttrValueSingle<FString>(InPD->Metadata, Settings->GrammarAttributeName, InP.MetadataEntry, Grammar) ||
				Grammar.IsEmpty())
			{
				continue;
			}

			// tile axis
			EPCGTileAxis TA = Settings->TileAxis;
			if (TA == EPCGTileAxis::LongestLocalAxis)
			{
				TA = PickLongestLocalAxis(InP);
			}
			const EAxis::Type Axis = ToAxis(TA);

			// target length in world
			const float TargetLen = GetTargetLengthAlongAxis(InPD, InP, Settings->ExtentsAttributeName, Axis);
			if (TargetLen <= KINDA_SMALL_NUMBER) continue;

			// per-point scale policy
			bool bAllowScale = false;
			FVector2D ScaleClamp = FVector2D(0.92f, 1.08f);

			if (InPD->Metadata && InP.MetadataEntry != PCGInvalidEntryKey)
			{
				GetAttrValueSingle<bool>(InPD->Metadata, Settings->AllowScaleAttributeName, InP.MetadataEntry, bAllowScale);
				GetAttrValueSingle<FVector2D>(InPD->Metadata, Settings->ScaleClampAttributeName, InP.MetadataEntry, ScaleClamp);
			}

			// parse grammar groups
			TArray<FGrammarGroup> Groups;
			ParseBracketGrammar(Grammar, Groups);
			if (Groups.Num() == 0) continue;

			const int32 RepIdx = FindRepeatIndex(Groups);

			TArray<FResolved> Prefix, RepeatOptions, Suffix;

			// resolve prefix groups (one symbol each: pick first option)
			auto ResolveFirst = [&](const FGrammarGroup& G, FResolved& OutR)->bool
				{
					if (G.Options.Num() == 0) return false;
					FResolved R;
					ResolveSymbol(Settings->Library, G.Options[0], Settings->DefaultMesh, Axis, ResCache, R);
					OutR = R;
					return (R.StdLen > KINDA_SMALL_NUMBER && !R.Mesh.IsNull());
				};

			for (int32 gi = 0; gi < Groups.Num(); ++gi)
			{
				if (gi == RepIdx) continue;

				FResolved R;
				if (!ResolveFirst(Groups[gi], R)) continue;

				if (RepIdx != INDEX_NONE && gi < RepIdx) Prefix.Add(R);
				else Suffix.Add(R);
			}

			// repeat option set
			if (RepIdx != INDEX_NONE)
			{
				for (const FName& Sym : Groups[RepIdx].Options)
				{
					FResolved R;
					if (ResolveSymbol(Settings->Library, Sym, Settings->DefaultMesh, Axis, ResCache, R) && R.StdLen > KINDA_SMALL_NUMBER && !R.Mesh.IsNull())
					{
						RepeatOptions.Add(R);
					}
				}
			}

			// compute fixed suffix length (using their std lens)
			float PrefixLen = 0.f; for (const FResolved& R : Prefix) PrefixLen += R.StdLen;
			float SuffixLen = 0.f; for (const FResolved& R : Suffix) SuffixLen += R.StdLen;

			// build segment list (resolved per segment)
			struct FSeg { FResolved R; float Len = 0.f; float ScaleA = 1.f; };
			TArray<FSeg> Segs;

			// add prefix
			for (const FResolved& R : Prefix)
			{
				FSeg S; S.R = R; S.Len = R.StdLen; Segs.Add(S);
			}

			// remaining for repeat zone
			float Rem = TargetLen - PrefixLen - SuffixLen;
			if (Rem < 0.f) Rem = 0.f;

			// fill repeat with best-fit options, leave remainder to be merged/scaled at tail (max one scaled)
			if (RepeatOptions.Num() > 0 && Rem > KINDA_SMALL_NUMBER)
			{
				while (Rem > KINDA_SMALL_NUMBER)
				{
					// if we are very close to end, stop and handle as tail
					if (Rem < Settings->MinRemainderWorld) break;

					// pick best fit option that fits into Rem (greedy)
					FResolved Best;
					if (!PickBestFitOption(Settings->Library, Groups[RepIdx].Options, Settings->DefaultMesh, Axis, Rem, ResCache, Best))
					{
						break;
					}

					// if best is bigger than remaining, break (tail will scale one segment)
					if (Best.StdLen > Rem + KINDA_SMALL_NUMBER)
					{
						break;
					}

					FSeg S; S.R = Best; S.Len = Best.StdLen;
					Segs.Add(S);
					Rem -= Best.StdLen;

					// avoid infinite loop on tiny mesh
					if (Best.StdLen <= KINDA_SMALL_NUMBER) break;
				}
			}

			// add suffix
			for (const FResolved& R : Suffix)
			{
				FSeg S; S.R = R; S.Len = R.StdLen; Segs.Add(S);
			}

			if (Segs.Num() == 0) continue;

			// now adjust tail to fit exact TargetLen by merging remainder into the last segment (only last may scale)
			float SumLen = 0.f; for (const FSeg& S : Segs) SumLen += S.Len;
			float TailR = TargetLen - SumLen;

			// if tiny remainder, just ignore
			if (FMath::Abs(TailR) < Settings->MinRemainderWorld)
			{
				TailR = 0.f;
			}

			if (TailR != 0.f)
			{
				FSeg& Last = Segs.Last();
				const float NewLen = FMath::Max(1.0f, Last.Len + TailR);
				if (bAllowScale && Last.R.bAllowTailScale && Last.Len > KINDA_SMALL_NUMBER)
				{
					float S = NewLen / Last.Len;
					S = FMath::Clamp(S, Settings->RemainderScaleClamp.X, Settings->RemainderScaleClamp.Y);
					// also clamp by per-point policy
					S = FMath::Clamp(S, ScaleClamp.X, ScaleClamp.Y);

					Last.ScaleA = S;
					Last.Len = NewLen; // for placement
				}
				else
				{
					// not allowed to scale -> keep len, accept slight mismatch
				}
			}

			// place segments along axis in world
			const FVector AxisDirWS =
				(Axis == EAxis::X) ? InP.Transform.GetUnitAxis(EAxis::X) :
				(Axis == EAxis::Y) ? InP.Transform.GetUnitAxis(EAxis::Y) :
				InP.Transform.GetUnitAxis(EAxis::Z);

			float Cursor = -TargetLen * 0.5f;

			const int32 SegCount = Segs.Num();
			for (int32 si = 0; si < SegCount; ++si)
			{
				const FSeg& Sg = Segs[si];
				if (Sg.R.Mesh.IsNull() || Sg.R.StdLen <= KINDA_SMALL_NUMBER) continue;

				const float CenterOffset = Cursor + Sg.Len * 0.5f;
				Cursor += Sg.Len;

				FPCGPoint P = InP;
				P.MetadataEntry = OutMD->AddEntry();

				// segment center (we align to bounds origin center, not pivot)
				const FVector BoundsCenterWS = InP.Transform.GetLocation() + AxisDirWS * CenterOffset;

				// compute segment scale (only along tile axis)
				FVector Scale = FVector::OneVector;
				if (Axis == EAxis::X) Scale.X *= Sg.ScaleA;
				else if (Axis == EAxis::Y) Scale.Y *= Sg.ScaleA;
				else Scale.Z *= Sg.ScaleA;

				// rotation same as input
				const FQuat RQ = InP.Transform.GetRotation();

				// pivot fix: place pivot so that mesh bounds origin lands at BoundsCenterWS
				FVector PivotWS = BoundsCenterWS;
				if (Settings->bPivotFixByBoundsOrigin)
				{
					const FVector LocalOffset = Sg.R.BoundsOrigin * Scale; // scale in local space
					PivotWS = BoundsCenterWS - RQ.RotateVector(LocalOffset);
				}

				P.Transform = FTransform(RQ, PivotWS, Scale);

				// optional: bounds for pruning/debug (approx)
				P.BoundsMin = InP.BoundsMin;
				P.BoundsMax = InP.BoundsMax;
				{
					const float Half = Sg.Len * 0.5f;
					if (Axis == EAxis::X) { P.BoundsMin.X = -Half; P.BoundsMax.X = Half; }
					else if (Axis == EAxis::Y) { P.BoundsMin.Y = -Half; P.BoundsMax.Y = Half; }
					else { P.BoundsMin.Z = -Half; P.BoundsMax.Z = Half; }
				}

				// write output attrs
				if (MeshAttr) MeshAttr->SetValue(P.MetadataEntry, Sg.R.Mesh.ToSoftObjectPath());
				if (FullNameAttr) FullNameAttr->SetValue(P.MetadataEntry, Sg.R.FullName);
				if (SymAttr) SymAttr->SetValue(P.MetadataEntry, Sg.R.Symbol);
				if (SegIdxAttr) SegIdxAttr->SetValue(P.MetadataEntry, si);
				if (SegCntAttr) SegCntAttr->SetValue(P.MetadataEntry, SegCount);

				// copy some useful attrs so downstream can filter/debug
				CopyInt(TEXT("FloorIndex"), InP, P.MetadataEntry);
				CopyInt(TEXT("OwnerIndex"), InP, P.MetadataEntry);
				CopyInt(TEXT("Priority"), InP, P.MetadataEntry);
				CopyInt(TEXT("VolumeIndex"), InP, P.MetadataEntry);
				CopyName(TEXT("StyleTag"), InP, P.MetadataEntry);
				CopyStr(TEXT("FacadeGrammar"), InP, P.MetadataEntry);
				CopyStr(TEXT("RoofGrammar"), InP, P.MetadataEntry);
				CopyStr(TEXT("SupportGrammar"), InP, P.MetadataEntry);

				OutPts.Add(P);
			}
		}

		FPCGTaggedData& Tag = Context->OutputData.TaggedData.Emplace_GetRef();
		Tag.Pin = PCGBuildingPins::PointsOut;
		Tag.Data = Out;

		return true;
	}
};

TArray<FPCGPinProperties> UPCGGrammarSegmentsFromLibrarySettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> InPins;
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::PointsIn;
	P.AllowedTypes = EPCGDataType::Point;
	InPins.Add(P);
	return InPins;
}

TArray<FPCGPinProperties> UPCGGrammarSegmentsFromLibrarySettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> OutPins;
	FPCGPinProperties P;
	P.Label = PCGBuildingPins::PointsOut;
	P.AllowedTypes = EPCGDataType::Point;
	OutPins.Add(P);
	return OutPins;
}

FPCGElementPtr UPCGGrammarSegmentsFromLibrarySettings::CreateElement() const
{
	return MakeShared<FPCGGrammarSegmentsFromLibraryElement>();
}
