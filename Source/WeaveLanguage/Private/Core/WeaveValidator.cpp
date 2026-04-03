#include "Core/WeaveValidator.h"
#include "Core/WeaveInterpreter.h"

// ========================================================================
// 辅助函数
// ========================================================================

namespace
{
	// 构建 NodeId -> NodeDecl 查找表（针对一个 Section）
	TMap<FString, const FWeaveNodeDecl*> BuildNodeMap(const FWeaveGraphSection& Section)
	{
		TMap<FString, const FWeaveNodeDecl*> Map;
		for (const FWeaveNodeDecl& Node : Section.Nodes)
		{
			Map.Add(Node.NodeId, &Node);
		}
		return Map;
	}

	// 判断 SchemaId 是否为纯节点（无 execute/then 引脚）
	bool IsPureSchema(const FString& SchemaId)
	{
		// VariableGet 是纯节点（ValidatedGet 不是）
		if (SchemaId.StartsWith(TEXT("VariableGet.")))
			return true;

		// special.Knot 和 special.Self 是纯节点
		if (SchemaId == TEXT("special.Knot") || SchemaId == TEXT("special.Self"))
			return true;

		// KismetMathLibrary 所有函数都是纯节点
		if (SchemaId.Contains(TEXT("KismetMathLibrary.")))
			return true;

		// KismetStringLibrary 大部分函数都是纯节点
		if (SchemaId.Contains(TEXT("KismetStringLibrary.")))
			return true;

		// 常见纯 Getter 函数
		if (SchemaId.Contains(TEXT(".GetActorLocation")) ||
			SchemaId.Contains(TEXT(".GetActorRotation")) ||
			SchemaId.Contains(TEXT(".GetSplineLength")) ||
			SchemaId.Contains(TEXT(".GetWorldLocation")) ||
			SchemaId.Contains(TEXT(".GetRelativeLocation")) ||
			SchemaId.Contains(TEXT(".GetScaledCapsuleRadius")) ||
			SchemaId.Contains(TEXT(".GetDistanceTo")))
			return true;

		return false;
	}

	// 从 SchemaId 提取最后一段（通常是变量名或函数名）
	FString GetLastSegment(const FString& SchemaId)
	{
		int32 LastDot;
		if (SchemaId.FindLastChar(TEXT('.'), LastDot))
		{
			return SchemaId.Mid(LastDot + 1);
		}
		return SchemaId;
	}
}

// ========================================================================
// Rule 1: InternalSchemaFix — K2Node 内部类名转换
// ========================================================================

class FRule_InternalSchemaFix : public IWeaveValidationRule
{
public:
	virtual FString GetRuleName() const override { return TEXT("InternalSchemaFix"); }

	virtual bool Apply(FWeaveAST& AST, TArray<FWeaveCorrection>& OutCorrections) override
	{
		bool bModified = false;

		// 可翻译的映射
		static const TMap<FString, FString> TranslationMap = {
			{ TEXT("K2Node_IfThenElse"), TEXT("special.Branch") },
			{ TEXT("K2Node_ExecutionSequence"), TEXT("special.Sequence") },
			{ TEXT("K2Node_SpawnActorFromClass"), TEXT("special.SpawnActorFromClass") },
			{ TEXT("K2Node_ConstructObjectFromClass"), TEXT("special.ConstructObjectFromClass") },
			{ TEXT("K2Node_MathExpression"), TEXT("special.MathExpression") },
		};

		// 不可翻译的（缺少上下文信息，只能删除）
		static const TArray<FString> RemovableNames = {
			TEXT("K2Node_Message"),
			TEXT("K2Node_CallFunction"),
			TEXT("K2Node_VariableGet"),
			TEXT("K2Node_VariableSet"),
			TEXT("K2Node_MacroInstance"),
			TEXT("K2Node_Event"),
			TEXT("K2Node_CustomEvent"),
			TEXT("K2Node_ComponentBoundEvent"),
			TEXT("K2Node_FunctionEntry"),
			TEXT("K2Node_FunctionResult"),
			TEXT("K2Node_DynamicCast"),
			TEXT("K2Node_MakeStruct"),
			TEXT("K2Node_BreakStruct"),
			TEXT("K2Node_SwitchEnum"),
			TEXT("K2Node_SwitchName"),
			TEXT("K2Node_Timeline"),
		};

		for (FWeaveGraphSection& Section : AST.Sections)
		{
			TArray<FString> NodesToRemove;

			for (FWeaveNodeDecl& Node : Section.Nodes)
			{
				// 跳过 native.* schema
				if (Node.SchemaId.StartsWith(TEXT("native.")))
					continue;

				// 尝试翻译
				bool bHandled = false;
				for (const auto& Pair : TranslationMap)
				{
					if (Node.SchemaId.Contains(Pair.Key))
					{
						OutCorrections.Add(FWeaveCorrection(
							GetRuleName(),
							FString::Printf(TEXT("修正 schema: %s 的 %s → %s"), *Node.NodeId, *Node.SchemaId, *Pair.Value)));
						Node.SchemaId = Pair.Value;
						bModified = true;
						bHandled = true;
						break;
					}
				}
				if (bHandled) continue;

				// 尝试删除
				for (const FString& Name : RemovableNames)
				{
					if (Node.SchemaId.Contains(Name))
					{
						OutCorrections.Add(FWeaveCorrection(
							GetRuleName(),
							FString::Printf(TEXT("移除无效节点: %s 使用了 UE 内部类名 %s，无法自动转换"), *Node.NodeId, *Name)));
						NodesToRemove.Add(Node.NodeId);
						bModified = true;
						break;
					}
				}
			}

			// 删除标记的节点及其关联的 set/link
			for (const FString& NodeId : NodesToRemove)
			{
				Section.Nodes.RemoveAll([&NodeId](const FWeaveNodeDecl& N) { return N.NodeId == NodeId; });
				Section.Sets.RemoveAll([&NodeId](const FWeaveSetStmt& S) { return S.NodeId == NodeId; });
				Section.Links.RemoveAll([&NodeId](const FWeaveLinkStmt& L) { return L.FromNode == NodeId || L.ToNode == NodeId; });
			}
		}

		return bModified;
	}
};

// ========================================================================
// Rule 2: SelfLoopRemoval — 自连链接移除
// ========================================================================

class FRule_SelfLoopRemoval : public IWeaveValidationRule
{
public:
	virtual FString GetRuleName() const override { return TEXT("SelfLoopRemoval"); }

	virtual bool Apply(FWeaveAST& AST, TArray<FWeaveCorrection>& OutCorrections) override
	{
		bool bModified = false;

		for (FWeaveGraphSection& Section : AST.Sections)
		{
			for (int32 i = Section.Links.Num() - 1; i >= 0; --i)
			{
				const FWeaveLinkStmt& Link = Section.Links[i];
				if (Link.FromNode == Link.ToNode)
				{
					OutCorrections.Add(FWeaveCorrection(
						GetRuleName(),
						FString::Printf(TEXT("移除自连链接: link %s.%s -> %s.%s（节点不能连接到自身）"),
							*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin)));
					Section.Links.RemoveAt(i);
					bModified = true;
				}
			}
		}

		return bModified;
	}
};

// ========================================================================
// Rule 3: PureExecRemoval — 纯节点执行链移除
// ========================================================================

class FRule_PureExecRemoval : public IWeaveValidationRule
{
public:
	virtual FString GetRuleName() const override { return TEXT("PureExecRemoval"); }

	virtual bool Apply(FWeaveAST& AST, TArray<FWeaveCorrection>& OutCorrections) override
	{
		bool bModified = false;

		for (FWeaveGraphSection& Section : AST.Sections)
		{
			TMap<FString, const FWeaveNodeDecl*> NodeMap = BuildNodeMap(Section);

			for (int32 i = Section.Links.Num() - 1; i >= 0; --i)
			{
				const FWeaveLinkStmt& Link = Section.Links[i];

				// 检查执行链连接
				bool bIsExecLink = (Link.FromPin == TEXT("then") && Link.ToPin == TEXT("execute"));
				if (!bIsExecLink)
				{
					// 也检查 then -> execute 的变体（如 then_0 等不算）
					// 以及 FromPin == "then" 但 ToPin 不是 execute 的情况不是执行链
					// 只检查明确的 then -> execute 模式
					continue;
				}

				// 检查目标节点是否为纯节点
				if (const FWeaveNodeDecl* const* ToNodePtr = NodeMap.Find(Link.ToNode))
				{
					if (IsPureSchema((*ToNodePtr)->SchemaId))
					{
						OutCorrections.Add(FWeaveCorrection(
							GetRuleName(),
							FString::Printf(TEXT("移除无效执行链接: link %s.then -> %s.execute（%s 是纯节点，没有执行引脚）"),
								*Link.FromNode, *Link.ToNode, *Link.ToNode)));
						Section.Links.RemoveAt(i);
						bModified = true;
						continue;
					}
				}

				// 检查源节点是否为纯节点（纯节点没有 then 引脚）
				if (const FWeaveNodeDecl* const* FromNodePtr = NodeMap.Find(Link.FromNode))
				{
					if (IsPureSchema((*FromNodePtr)->SchemaId))
					{
						OutCorrections.Add(FWeaveCorrection(
							GetRuleName(),
							FString::Printf(TEXT("移除无效执行链接: link %s.then -> %s.execute（%s 是纯节点，没有 then 引脚）"),
								*Link.FromNode, *Link.ToNode, *Link.FromNode)));
						Section.Links.RemoveAt(i);
						bModified = true;
					}
				}
			}
		}

		return bModified;
	}
};

// ========================================================================
// Rule 4: VarSetPinFix — VariableSet 输出引脚名修正
// ========================================================================

class FRule_VarSetPinFix : public IWeaveValidationRule
{
public:
	virtual FString GetRuleName() const override { return TEXT("VarSetPinFix"); }

	virtual bool Apply(FWeaveAST& AST, TArray<FWeaveCorrection>& OutCorrections) override
	{
		bool bModified = false;

		for (FWeaveGraphSection& Section : AST.Sections)
		{
			TMap<FString, const FWeaveNodeDecl*> NodeMap = BuildNodeMap(Section);

			for (FWeaveLinkStmt& Link : Section.Links)
			{
				if (const FWeaveNodeDecl* const* NodePtr = NodeMap.Find(Link.FromNode))
				{
					const FString& Schema = (*NodePtr)->SchemaId;
					if (Schema.StartsWith(TEXT("VariableSet.")))
					{
						FString VarName = GetLastSegment(Schema);
						if (Link.FromPin == VarName && Link.FromPin != TEXT("then"))
						{
							OutCorrections.Add(FWeaveCorrection(
								GetRuleName(),
								FString::Printf(TEXT("修正 VariableSet 输出引脚: %s.%s → %s.Output_Get（VariableSet 的数据输出引脚名为 Output_Get）"),
									*Link.FromNode, *Link.FromPin, *Link.FromNode)));
							Link.FromPin = TEXT("Output_Get");
							bModified = true;
						}
					}
				}
			}
		}

		return bModified;
	}
};

// ========================================================================
// Rule 5: InterfaceEventSuggestion — customEvent/接口事件建议
// ========================================================================

class FRule_InterfaceEventSuggestion : public IWeaveValidationRule
{
public:
	virtual FString GetRuleName() const override { return TEXT("InterfaceEventSuggestion"); }

	virtual bool Apply(FWeaveAST& AST, TArray<FWeaveCorrection>& OutCorrections) override
	{
		// 收集所有 message 和 event 节点的函数名
		TSet<FString> InterfaceFuncNames;
		for (const FWeaveGraphSection& Section : AST.Sections)
		{
			for (const FWeaveNodeDecl& Node : Section.Nodes)
			{
				if (Node.SchemaId.StartsWith(TEXT("message.")))
				{
					InterfaceFuncNames.Add(GetLastSegment(Node.SchemaId));
				}
				else if (Node.SchemaId.StartsWith(TEXT("event.")) && !Node.SchemaId.StartsWith(TEXT("event./Script/")))
				{
					// 非引擎事件的 event 节点，可能是接口事件
					InterfaceFuncNames.Add(GetLastSegment(Node.SchemaId));
				}
			}
		}

		if (InterfaceFuncNames.Num() == 0)
			return false;

		bool bHasSuggestion = false;
		for (const FWeaveGraphSection& Section : AST.Sections)
		{
			for (const FWeaveNodeDecl& Node : Section.Nodes)
			{
				if (Node.SchemaId.StartsWith(TEXT("customEvent.")))
				{
					FString EventName = Node.SchemaId.Mid(12); // len("customEvent.")
					if (InterfaceFuncNames.Contains(EventName))
					{
						OutCorrections.Add(FWeaveCorrection(
							GetRuleName(),
							FString::Printf(TEXT("节点 %s 使用 customEvent.%s，但该名称与接口函数同名。如果是接口事件实现，建议改用 event.<BlueprintClass>.%s"),
								*Node.NodeId, *EventName, *EventName),
							true)); // bIsSuggestion
						bHasSuggestion = true;
					}
				}
			}
		}

		return bHasSuggestion;
	}
};

// ========================================================================
// Rule 6: ExecMultiTargetSequence — 执行输出多目标自动插 Sequence
// ========================================================================

class FRule_ExecMultiTargetSequence : public IWeaveValidationRule
{
public:
	virtual FString GetRuleName() const override { return TEXT("ExecMultiTargetSequence"); }

	virtual bool Apply(FWeaveAST& AST, TArray<FWeaveCorrection>& OutCorrections) override
	{
		bool bModified = false;
		int32 SeqCounter = 0;

		for (FWeaveGraphSection& Section : AST.Sections)
		{
			// 收集所有现有 NodeId 用于避免冲突
			TSet<FString> ExistingIds;
			for (const FWeaveNodeDecl& Node : Section.Nodes)
			{
				ExistingIds.Add(Node.NodeId);
			}

			// 按 (FromNode, FromPin=="then") 分组，找出多目标
			TMap<FString, TArray<int32>> ExecSourceToLinkIndices;
			for (int32 i = 0; i < Section.Links.Num(); i++)
			{
				const FWeaveLinkStmt& Link = Section.Links[i];
				if (Link.FromPin == TEXT("then") && Link.ToPin == TEXT("execute"))
				{
					ExecSourceToLinkIndices.FindOrAdd(Link.FromNode).Add(i);
				}
			}

			// 处理有多个目标的源节点
			TArray<int32> IndicesToRemove;
			TArray<FWeaveLinkStmt> LinksToAdd;
			TArray<FWeaveNodeDecl> NodesToAdd;

			for (auto& Pair : ExecSourceToLinkIndices)
			{
				const FString& SourceNode = Pair.Key;
				const TArray<int32>& LinkIndices = Pair.Value;

				if (LinkIndices.Num() <= 1)
					continue;

				// 生成唯一的 Sequence 节点 ID
				FString SeqId;
				do
				{
					SeqId = FString::Printf(TEXT("_seq_auto_%d"), SeqCounter++);
				}
				while (ExistingIds.Contains(SeqId));
				ExistingIds.Add(SeqId);

				// 确定位置：源节点位置 + (200, 0)
				FVector2D SeqPos(0, 0);
				TMap<FString, const FWeaveNodeDecl*> NodeMap = BuildNodeMap(Section);
				if (const FWeaveNodeDecl* const* SrcPtr = NodeMap.Find(SourceNode))
				{
					SeqPos = (*SrcPtr)->Position + FVector2D(200, 0);
				}

				// 创建 Sequence 节点
				FWeaveNodeDecl SeqNode;
				SeqNode.NodeId = SeqId;
				SeqNode.SchemaId = TEXT("special.Sequence");
				SeqNode.Position = SeqPos;
				NodesToAdd.Add(SeqNode);

				// 标记要删除的旧 link
				for (int32 Idx : LinkIndices)
				{
					IndicesToRemove.Add(Idx);
				}

				// 添加新 link：源 -> Sequence
				FWeaveLinkStmt SrcToSeq;
				SrcToSeq.FromNode = SourceNode;
				SrcToSeq.FromPin = TEXT("then");
				SrcToSeq.ToNode = SeqId;
				SrcToSeq.ToPin = TEXT("execute");
				LinksToAdd.Add(SrcToSeq);

				// 添加新 link：Sequence.then_N -> 各目标
				for (int32 i = 0; i < LinkIndices.Num(); i++)
				{
					const FWeaveLinkStmt& OldLink = Section.Links[LinkIndices[i]];
					FWeaveLinkStmt SeqToTarget;
					SeqToTarget.FromNode = SeqId;
					SeqToTarget.FromPin = FString::Printf(TEXT("then_%d"), i);
					SeqToTarget.ToNode = OldLink.ToNode;
					SeqToTarget.ToPin = OldLink.ToPin;
					LinksToAdd.Add(SeqToTarget);
				}

				OutCorrections.Add(FWeaveCorrection(
					GetRuleName(),
					FString::Printf(TEXT("插入 Sequence 节点: %s.then 有 %d 个执行目标，自动添加 %s 进行分发"),
						*SourceNode, LinkIndices.Num(), *SeqId)));
				bModified = true;
			}

			// 执行修改：先删后加（倒序删除）
			IndicesToRemove.Sort([](int32 A, int32 B) { return A > B; });
			for (int32 Idx : IndicesToRemove)
			{
				Section.Links.RemoveAt(Idx);
			}
			Section.Links.Append(LinksToAdd);
			Section.Nodes.Append(NodesToAdd);
		}

		return bModified;
	}
};

// ========================================================================
// FWeaveValidator 实现
// ========================================================================

FWeaveValidator::FWeaveValidator()
{
	// 按依赖顺序注册规则
	Rules.Add({ MakeShared<FRule_InternalSchemaFix>(), true });
	Rules.Add({ MakeShared<FRule_SelfLoopRemoval>(), true });
	Rules.Add({ MakeShared<FRule_PureExecRemoval>(), true });
	Rules.Add({ MakeShared<FRule_VarSetPinFix>(), true });
	Rules.Add({ MakeShared<FRule_InterfaceEventSuggestion>(), true });
	Rules.Add({ MakeShared<FRule_ExecMultiTargetSequence>(), true });
}

void FWeaveValidator::SetRuleEnabled(const FString& RuleName, bool bEnabled)
{
	for (FRuleEntry& Entry : Rules)
	{
		if (Entry.Rule->GetRuleName() == RuleName)
		{
			Entry.bEnabled = bEnabled;
			return;
		}
	}
}

bool FWeaveValidator::Validate(FWeaveAST& AST, TArray<FWeaveCorrection>& OutCorrections)
{
	bool bAnyModified = false;

	for (const FRuleEntry& Entry : Rules)
	{
		if (Entry.bEnabled)
		{
			if (Entry.Rule->Apply(AST, OutCorrections))
			{
				bAnyModified = true;
			}
		}
	}

	// 同步 Sections[0] 到顶层字段
	if (bAnyModified)
	{
		SyncTopLevelFromSections(AST);
	}

	return bAnyModified;
}

void FWeaveValidator::SyncTopLevelFromSections(FWeaveAST& AST)
{
	if (AST.Sections.Num() > 0)
	{
		const FWeaveGraphSection& First = AST.Sections[0];
		AST.GraphName = First.GraphName;
		AST.Nodes = First.Nodes;
		AST.Sets = First.Sets;
		AST.Links = First.Links;
		AST.Comments = First.Comments;
	}
}
