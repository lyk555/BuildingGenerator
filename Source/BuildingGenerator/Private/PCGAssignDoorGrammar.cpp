#include "PCGAssignDoorGrammar.h"

#include "PCGContext.h"
#include "PCGPin.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"

#define LOCTEXT_NAMESPACE "PCGDoor"

namespace PCGDoorPriv
{
	static FString TrimCopy(FString S)
	{
		S.TrimStartAndEndInline();
		return S;
	}

	static bool ParseFirstToken(const FString& S, int32& OutStart, int32& OutEndExcl)
	{
		int32 Start = 0;
		while (Start < S.Len() && FChar::IsWhitespace(S[Start])) ++Start;
		if (Start >= S.Len()) return false;

		if (S[Start] == TCHAR('['))
		{
			const int32 Close = S.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			if (Close == INDEX_NONE) return false;
			OutStart = Start;
			OutEndExcl = Close + 1;
			return true;
		}

		int32 End = Start;
		while (End < S.Len() && (FChar::IsAlnum(S[End]) || S[End] == TCHAR('_')))
			++End;
		if (End <= Start) return false;

		OutStart = Start;
		OutEndExcl = End;
		return true;
	}

	static bool ParseLastToken(const FString& S, int32& OutStart, int32& OutEndExcl)
	{
		int32 End = S.Len();
		while (End > 0 && FChar::IsWhitespace(S[End - 1])) --End;
		if (End <= 0) return false;

		const int32 Last = End - 1;
		if (S[Last] == TCHAR(']'))
		{
			const int32 Open = S.Find(TEXT("["), ESearchCase::CaseSensitive, ESearchDir::FromEnd, Last);
			if (Open == INDEX_NONE) return false;
			OutStart = Open;
			OutEndExcl = End;
			return true;
		}

		int32 Start = Last;
		while (Start >= 0 && (FChar::IsAlnum(S[Start]) || S[Start] == TCHAR('_')))
			--Start;

		OutStart = Start + 1;
		OutEndExcl = End;
		return OutEndExcl > OutStart;
	}

	static FString StripDoorToken(const FString& Grammar, const FString& DoorToken)
	{
		if (DoorToken.IsEmpty()) return Grammar;
		FString Out = Grammar;
		Out.ReplaceInline(*DoorToken, TEXT(""), ESearchCase::CaseSensitive);
		Out.ReplaceInline(TEXT("  "), TEXT(" "));
		return TrimCopy(Out);
	}

	static FString InjectDoor_CornerFillCorner(const FString& Grammar, const FString& DoorToken)
	{
		if (DoorToken.IsEmpty()) return Grammar;
		if (Grammar.Contains(DoorToken)) return Grammar;

		int32 L0 = 0, L1 = 0, R0 = 0, R1 = 0;
		if (!ParseFirstToken(Grammar, L0, L1) || !ParseLastToken(Grammar, R0, R1) || R0 <= L1)
			return Grammar;

		const FString Left = Grammar.Mid(L0, L1 - L0);
		const FString Right = Grammar.Mid(R0, R1 - R0);
		const FString Mid = TrimCopy(Grammar.Mid(L1, R0 - L1));

		if (Mid.IsEmpty())
			return Left + DoorToken + Right;

		return Left + Mid + DoorToken + Mid + Right;
	}

	static FString InjectDoor_InsertBeforeLast(const FString& Grammar, const FString& DoorToken)
	{
		if (DoorToken.IsEmpty()) return Grammar;
		if (Grammar.Contains(DoorToken)) return Grammar;

		int32 R0 = 0, R1 = 0;
		if (!ParseLastToken(Grammar, R0, R1))
			return Grammar + DoorToken;

		return Grammar.Left(R0) + DoorToken + Grammar.Mid(R0);
	}

	static FString InjectDoor(const FString& Grammar, const FString& DoorToken, EPCGDoorInjectPolicy Policy)
	{
		switch (Policy)
		{
		case EPCGDoorInjectPolicy::CornerFillCorner: return InjectDoor_CornerFillCorner(Grammar, DoorToken);
		case EPCGDoorInjectPolicy::InsertBeforeLast: return InjectDoor_InsertBeforeLast(Grammar, DoorToken);
		case EPCGDoorInjectPolicy::Append:
		default: return Grammar + DoorToken;
		}
	}

	static FVector2D OutwardNormalCW(const FVector2D& Tangent)
	{
		// 2D 左手/右手你项目里只要一致即可；这里是 (x,y) 的顺时针法线
		return FVector2D(-Tangent.Y, Tangent.X);
	}

	static FVector2D SignDir(bool bXPos, bool bYPos)
	{
		return FVector2D(bXPos ? 1.f : -1.f, bYPos ? 1.f : -1.f).GetSafeNormal();
	}
}

TArray<FPCGPinProperties> UPCGAssignDoorGrammarSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultInputLabel, EPCGDataType::Point);
	return Pins;
}

TArray<FPCGPinProperties> UPCGAssignDoorGrammarSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);
	return Pins;
}

FPCGElementPtr UPCGAssignDoorGrammarSettings::CreateElement() const
{
	//  UE5.5：FPCGElementPtr == TSharedPtr<IPCGElement>
	return MakeShared<FPCGAssignDoorGrammarPerOwnerElement>().ToSharedPtr();
}

bool FPCGAssignDoorGrammarPerOwnerElement::ExecuteInternal(FPCGContext* Context) const
{
	const UPCGAssignDoorGrammarSettings* S = Context->GetInputSettings<UPCGAssignDoorGrammarSettings>();
	check(S);

	const TArray<FPCGTaggedData> InData = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
	TArray<FPCGTaggedData>& OutData = Context->OutputData.TaggedData;

	for (const FPCGTaggedData& In : InData)
	{
		const UPCGPointData* InPD = Cast<UPCGPointData>(In.Data);
		if (!InPD || !InPD->Metadata)
		{
			OutData.Add(In);
			continue;
		}

		UPCGData* Dup = InPD->DuplicateData(Context, /*bInitializeMetadata=*/true);
		UPCGPointData* PD = Cast<UPCGPointData>(Dup);
		if (!PD || !PD->Metadata)
		{
			OutData.Add(In);
			continue;
		}

		UPCGMetadata* Meta = PD->Metadata;
		TArray<FPCGPoint>& Points = PD->GetMutablePoints();

		// Read attributes
		const auto* AHaveDoor = Meta->GetConstTypedAttribute<bool>(S->HaveDoorAttribute);
		const auto* ADoorX = Meta->GetConstTypedAttribute<bool>(S->DoorXAttribute);
		const auto* ADoorY = Meta->GetConstTypedAttribute<bool>(S->DoorYAttribute);

		const auto* ATangent = Meta->GetConstTypedAttribute<FVector>(S->TangentAttribute);
		const auto* AGrammar = Meta->GetConstTypedAttribute<FString>(S->GrammarAttribute);

		// Owner can be int64 or int32 upstream; support both
		const auto* AOwner64 = Meta->GetConstTypedAttribute<int64>(S->OwnerIndexAttribute);
		const auto* AOwner32 = Meta->GetConstTypedAttribute<int32>(S->OwnerIndexAttribute);

		// Output attribute
		FPCGMetadataAttribute<FString>* OutGrammar =
			Meta->FindOrCreateAttribute<FString>(
				S->OutputGrammarAttribute,
				FString(),
				/*bAllowsInterpolation=*/false,
				/*bOverrideParent=*/true);

		// Group points by Owner
		TMap<int64, TArray<int32>> Groups;
		Groups.Reserve(64);

		for (int32 i = 0; i < Points.Num(); ++i)
		{
			const PCGMetadataEntryKey K = Points[i].MetadataEntry;

			int64 Owner = 0;
			if (AOwner64) Owner = AOwner64->GetValue(K);
			else if (AOwner32) Owner = (int64)AOwner32->GetValue(K);

			Groups.FindOrAdd(Owner).Add(i);
		}

		// For each group: pick exactly one edge to be the door edge
		for (const auto& Pair : Groups)
		{
			const TArray<int32>& Idxs = Pair.Value;

			int32 DoorIdx = INDEX_NONE;

			// ---------- NEW SEMANTICS ----------
			// Door_X / Door_Y choose axis; HaveDoor chooses sign on that axis.
			// Door_X == false && Door_Y == false => no door.
			if (Idxs.Num() > 0)
			{
				const PCGMetadataEntryKey K0 = Points[Idxs[0]].MetadataEntry;

				// IMPORTANT: default missing Door_X/Door_Y to false => no door
				const bool bAxisX = ADoorX ? ADoorX->GetValue(K0) : false;
				const bool bAxisY = ADoorY ? ADoorY->GetValue(K0) : false;

				// HaveDoor now means sign (true=positive, false=negative). Default true is harmless.
				const bool bPos = AHaveDoor ? AHaveDoor->GetValue(K0) : true;

				if (bAxisX || bAxisY)
				{
					FVector2D Dir(0, 0);

					if (bAxisX && !bAxisY)
					{
						Dir = FVector2D(bPos ? 1.f : -1.f, 0.f);
					}
					else if (bAxisY && !bAxisX)
					{
						Dir = FVector2D(0.f, bPos ? 1.f : -1.f);
					}
					else
					{
						// both true: ambiguous in the new semantics. Treat as diagonal.
						const float Sgn = bPos ? 1.f : -1.f;
						Dir = FVector2D(Sgn, Sgn).GetSafeNormal();
					}

					float Best = -FLT_MAX;

					// Candidate set is ALL edges in this owner group.
					for (int32 i : Idxs)
					{
						const PCGMetadataEntryKey K = Points[i].MetadataEntry;

						FVector T3 = FVector::ForwardVector;
						if (ATangent) T3 = ATangent->GetValue(K);
						else T3 = Points[i].Transform.GetRotation().GetForwardVector();

						const FVector2D T = FVector2D(T3.X, T3.Y).GetSafeNormal();
						const FVector2D N = PCGDoorPriv::OutwardNormalCW(T);

						const float Score = FVector2D::DotProduct(N, Dir);
						if (Score > Best)
						{
							Best = Score;
							DoorIdx = i;
						}
					}
				}
			}
			// ---------- NEW SEMANTICS ----------

			// Write grammar
			for (int32 i : Idxs)
			{
				const PCGMetadataEntryKey K = Points[i].MetadataEntry;
				FString G = AGrammar ? AGrammar->GetValue(K) : FString();

				if (i == DoorIdx)
				{
					G = PCGDoorPriv::InjectDoor(G, S->DoorToken, S->InjectPolicy);
				}
				else if (S->bStripDoorOnNonDoorEdges)
				{
					G = PCGDoorPriv::StripDoorToken(G, S->DoorToken);
				}

				OutGrammar->SetValue(K, G);
			}
		}

		FPCGTaggedData Out = In;
		Out.Data = PD;
		OutData.Add(Out);
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
