#include "Core/WeaveInterpreter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Message.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MathExpression.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Core/DynamicPins/SequenceDynamicPinHandler.h"
#include "Core/DynamicPins/SwitchEnumDynamicPinHandler.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_Timeline.h"
#include "K2Node_MakeArray.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "EdGraphNode_Comment.h"
#include "ScopedTransaction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UObjectIterator.h"

// 通过名称查找 UClass，支持完整路径名和短名称
static UClass* FindClassByShortName(const FString& ClassName)
{
	// 如果是完整路径（如 /Script/ModuleName.ClassName），优先用 LoadObject 加载
	if (ClassName.StartsWith(TEXT("/")))
	{
		UClass* Result = LoadObject<UClass>(nullptr, *ClassName);
		if (Result) return Result;
	}

	// 尝试 TryFindTypeSlow（适用于完整路径名或已加载的短名称）
	UClass* Result = UClass::TryFindTypeSlow<UClass>(*ClassName);
	if (Result) return Result;

	// 尝试加前缀
	Result = UClass::TryFindTypeSlow<UClass>(*(TEXT("A") + ClassName));
	if (Result) return Result;
	Result = UClass::TryFindTypeSlow<UClass>(*(TEXT("U") + ClassName));
	if (Result) return Result;

	// TObjectIterator 兜底：按 GetName() 匹配（仅对已加载的类有效）
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (Candidate->GetName() == ClassName)
		{
			return Candidate;
		}
	}

	return nullptr;
}

namespace
{
	template <typename T>
	T* SpawnEditorNode(UEdGraph* Graph)
	{
		T* Node = Graph->CreateIntermediateNode<T>();
		if (Node)
		{
			Node->CreateNewGuid();
		}
		return Node;
	}
}

// 自动纠错：修复常见的 Weave 语法错误
FString FWeaveInterpreter::AutoFixWeaveCode(const FString& WeaveCode, TArray<FString>& OutFixes)
{
	FString FixedCode = WeaveCode;
	OutFixes.Empty();

	// 无效的 UE 内部类名列表（这些不能作为 schema ID）
	static const TArray<FString> InvalidClassNames = {
		TEXT("K2Node_Message"),
		TEXT("K2Node_CallFunction"),
		TEXT("K2Node_VariableGet"),
		TEXT("K2Node_VariableSet"),
		TEXT("K2Node_IfThenElse"),
		TEXT("K2Node_ExecutionSequence"),
		TEXT("K2Node_MacroInstance"),
		TEXT("K2Node_Event"),
		TEXT("K2Node_CustomEvent"),
		TEXT("K2Node_ComponentBoundEvent"),
		TEXT("K2Node_FunctionEntry"),
		TEXT("K2Node_FunctionResult"),
		TEXT("K2Node_DynamicCast"),
		TEXT("K2Node_MakeStruct"),
		TEXT("K2Node_BreakStruct"),
		TEXT("K2Node_SpawnActorFromClass"),
		TEXT("K2Node_SwitchEnum"),
		TEXT("K2Node_SwitchName"),
		TEXT("K2Node_Timeline"),
		TEXT("K2Node_MathExpression"),
	};

	// 逐行处理
	TArray<FString> Lines;
	FixedCode.ParseIntoArrayLines(Lines);

	TArray<FString> OutputLines;
	for (const FString& Line : Lines)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();
		bool bShouldRemove = false;

		// 检查是否是 node 声明行
		if (TrimmedLine.StartsWith(TEXT("node")))
		{
			// native.* schema 中的 K2Node_ 名称是合法的，跳过检查
			bool bIsNativeSchema = TrimmedLine.Contains(TEXT("native."));
			if (!bIsNativeSchema)
			{
				// 检查是否包含无效的类名
				for (const FString& InvalidName : InvalidClassNames)
				{
					if (TrimmedLine.Contains(InvalidName))
					{
						bShouldRemove = true;
						OutFixes.Add(FString::Printf(TEXT("移除无效节点: %s"), *InvalidName));
						break;
					}
				}
			}
		}

		// 只保留非删除行
		if (!bShouldRemove)
		{
			OutputLines.Add(Line);
		}
	}

	FixedCode = FString::Join(OutputLines, TEXT("\n"));

	if (OutFixes.Num() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[Weaver AutoFix] Applied %d fix(es):"), OutFixes.Num());
		for (const FString& Fix : OutFixes)
		{
			UE_LOG(LogTemp, Log, TEXT("  - %s"), *Fix);
		}
	}

	return FixedCode;
}

// 自动纠错：修复缺失的连接和错误的 set 值（通用版本）
FString FWeaveInterpreter::AutoFixLinksAndSets(const FString& WeaveCode, TArray<FString>& OutFixes)
{
	FString FixedCode = WeaveCode;
	bool bModified = false;

	TArray<FString> Lines;
	FixedCode.ParseIntoArrayLines(Lines);

	// 节点信息收集
	struct FNodeInfo
	{
		FString NodeId;
		FString SchemaId;
		FString VarName;
		bool bIsComponentGet = false;
		bool bIsSplineFunc = false;
	};
	TArray<FNodeInfo> Nodes;
	TArray<FString> GetComponentIDs;

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("node"))) continue;

		FNodeInfo NodeInfo;
		FString NodeIdStr = Trimmed.Mid(5);
		int32 SpaceOrColonIdx = NodeIdStr.Find(TEXT(" "));
		if (SpaceOrColonIdx == INDEX_NONE) SpaceOrColonIdx = NodeIdStr.Find(TEXT(":"));
		if (SpaceOrColonIdx != INDEX_NONE)
			NodeIdStr = NodeIdStr.Left(SpaceOrColonIdx);
		NodeInfo.NodeId = NodeIdStr.TrimStartAndEnd();
		if (NodeInfo.NodeId.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver AutoFix] Skipping node with empty NodeId: %s"), *Line);
			continue;
		}

		int32 ColonIdx = Trimmed.Find(TEXT(":"));
		if (ColonIdx == INDEX_NONE) continue;
		NodeInfo.SchemaId = Trimmed.Mid(ColonIdx + 2).TrimStart();
		// 去掉 @ (X, Y) 坐标部分
		int32 AtIdx = NodeInfo.SchemaId.Find(TEXT("@"));
		if (AtIdx != INDEX_NONE)
		{
			NodeInfo.SchemaId = NodeInfo.SchemaId.Left(AtIdx).TrimEnd();
		}

		NodeInfo.bIsComponentGet = NodeInfo.SchemaId.Contains(TEXT("GetComponentByClass"));
		NodeInfo.bIsSplineFunc = NodeInfo.SchemaId.Contains(TEXT("GetLocationAtDistanceAlongSpline")) ||
		                       NodeInfo.SchemaId.Contains(TEXT("GetSplineLength")) ||
		                       NodeInfo.SchemaId.Contains(TEXT("GetSplineDataAtDistanceAlongSpline"));

		// 提取 VariableGet 的变量名（取最后一个 '.' 之后的内容）
		if (NodeInfo.SchemaId.StartsWith(TEXT("VariableGet")))
		{
			int32 LastDotPos = NodeInfo.SchemaId.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (LastDotPos != INDEX_NONE)
			{
				NodeInfo.VarName = NodeInfo.SchemaId.Mid(LastDotPos + 1);
			}
		}

		Nodes.Add(NodeInfo);
		if (NodeInfo.bIsComponentGet) GetComponentIDs.Add(NodeInfo.NodeId);
	}

	// 收集已有的连接
	TSet<FString> ExistingLinks;
	for (const FString& Line : Lines)
	{
		if (Line.TrimStartAndEnd().StartsWith(TEXT("link")))
		{
			ExistingLinks.Add(Line);
		}
	}

	// 检测是否有 Spline 上下文
	bool bHasSplineContext = false;
	for (const FNodeInfo& Node : Nodes)
	{
		if (Node.bIsSplineFunc || Node.SchemaId.Contains(TEXT("SplineComponent")))
		{
			bHasSplineContext = true;
			break;
		}
	}

	TArray<FString> OutputLines;
	TSet<FString> FixedSetLines; // 记录已修复的 set 行，避免重复

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();

		// 修复1：替换错误的 ComponentClass set（而不是添加新行）
		if (Trimmed.Contains(TEXT(".ComponentClass = \"/Script/Engine.ActorComponent\"")))
		{
			if (bHasSplineContext && GetComponentIDs.Num() > 0)
			{
				// 提取节点 ID
				FString SetNodeId;
				int32 DotIdx = Trimmed.Find(TEXT("."));
				if (DotIdx != INDEX_NONE)
				{
					SetNodeId = Trimmed.Left(DotIdx).Mid(4).TrimStartAndEnd(); // "set " 之后到 "." 之前
				}

				// 替换为正确的格式
				FString FixedLine = FString::Printf(TEXT("set %s.ComponentClass = class:USplineComponent"), *SetNodeId);
				OutputLines.Add(FixedLine);

				FString FixKey = FString::Printf(TEXT("ComponentClass:%s"), *SetNodeId);
				if (!FixedSetLines.Contains(FixKey))
				{
					OutFixes.Add(FString::Printf(TEXT("修复: %s.ComponentClass 设为 class:USplineComponent（上下文推断）"), *SetNodeId));
					FixedSetLines.Add(FixKey);
					bModified = true;
				}
				continue; // 跳过原始行，使用修复后的行
			}
		}

		OutputLines.Add(Line);
	}

	// 修复1.5：将 GetComponentByClass 节点的 Target 引脚引用重映射为 self
	// LLM 常错误地写 "link X -> getComp.Target"，实际引脚名是 "self"
	{
		TArray<FString> FixedOutputLines;
		for (const FString& Line : OutputLines)
		{
			FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("link")))
			{
				bool bFixed = false;
				for (const FString& GetComponentID : GetComponentIDs)
				{
					FString WrongTarget = FString::Printf(TEXT("-> %s.Target"), *GetComponentID);
					if (Trimmed.Contains(WrongTarget))
					{
						FString FixedLine = Line.Replace(*WrongTarget, *FString::Printf(TEXT("-> %s.self"), *GetComponentID));
						FixedOutputLines.Add(FixedLine);
						OutFixes.Add(FString::Printf(TEXT("修复: %s.Target -> %s.self（GetComponentByClass 使用 self 引脚）"), *GetComponentID, *GetComponentID));
						bModified = true;
						bFixed = true;

						// 同步更新 ExistingLinks 以便后续修复检测
						ExistingLinks.Remove(Line);
						ExistingLinks.Add(FixedLine);
						break;
					}
				}
				if (!bFixed)
				{
					FixedOutputLines.Add(Line);
				}
			}
			else
			{
				FixedOutputLines.Add(Line);
			}
		}
		OutputLines = MoveTemp(FixedOutputLines);
	}

	// 修复2：自动添加 GetComponentByClass.self 连接
	for (const FString& GetComponentID : GetComponentIDs)
	{
		// 安全检查：确保 GetComponentID 有效
		if (GetComponentID.IsEmpty() || GetComponentID.Contains(TEXT(".")) || GetComponentID.Contains(TEXT(" ")))
		{
			continue;
		}

		// 检查 self 是否已连接（同时兼容 Target 已被 Fix 1.5 重映射的情况）
		bool bSelfConnected = false;
		for (const FString& Link : ExistingLinks)
		{
			if (Link.Contains(FString::Printf(TEXT("-> %s.self"), *GetComponentID)) ||
				Link.Contains(FString::Printf(TEXT("-> %s.Target"), *GetComponentID)))
			{
				bSelfConnected = true;
				break;
			}
		}

		if (bSelfConnected) continue;

		// 寻找可能的目标：未连接的 VariableGet
		for (const FNodeInfo& Node : Nodes)
		{
			if (!Node.SchemaId.StartsWith(TEXT("VariableGet"))) continue;
			if (Node.VarName.IsEmpty() || Node.VarName == TEXT(".") || Node.VarName == TEXT("ReturnValue")) continue;

			// 安全检查：确保 NodeId 有效
			if (Node.NodeId.IsEmpty() || Node.NodeId.Contains(TEXT(".")) || Node.NodeId.Contains(TEXT(" ")))
			{
				continue;
			}

			// 检查这个 VariableGet 是否已连接
			bool bVarGetConnected = false;
			for (const FString& Link : ExistingLinks)
			{
				if (Link.Contains(FString::Printf(TEXT("%s."), *Node.NodeId)))
				{
					bVarGetConnected = true;
					break;
				}
			}

			// 如果未连接，连接到 GetComponentByClass.self
			if (!bVarGetConnected)
			{
				FString NewLink = FString::Printf(TEXT("link %s.%s -> %s.self"),
				                                 *Node.NodeId, *Node.VarName, *GetComponentID);
				OutputLines.Add(NewLink);
				OutFixes.Add(FString::Printf(TEXT("添加: %s.%s -> %s.self"),
				                         *Node.NodeId, *Node.VarName, *GetComponentID));
				bModified = true;
				break;
			}
		}
	}

	// 修复3：自动添加 GetComponentByClass.ReturnValue -> 函数.self
	for (const FString& GetComponentID : GetComponentIDs)
	{
		// 安全检查：确保 GetComponentID 有效
		if (GetComponentID.IsEmpty() || GetComponentID.Contains(TEXT(".")) || GetComponentID.Contains(TEXT(" ")))
		{
			continue;
		}

		for (const FNodeInfo& Node : Nodes)
		{
			if (!Node.bIsSplineFunc) continue;

			// 安全检查：确保 NodeId 有效
			if (Node.NodeId.IsEmpty() || Node.NodeId.Contains(TEXT(".")) || Node.NodeId.Contains(TEXT(" ")))
			{
				continue;
			}

			// 检查是否已有 self 连接
			bool bHasSelfLink = false;
			for (const FString& Link : ExistingLinks)
			{
				if (Link.Contains(FString::Printf(TEXT("-> %s.self"), *Node.NodeId)))
				{
					bHasSelfLink = true;
					break;
				}
			}

			if (!bHasSelfLink)
			{
				FString NewLink = FString::Printf(TEXT("link %s.ReturnValue -> %s.self"), *GetComponentID, *Node.NodeId);
				OutputLines.Add(NewLink);
				OutFixes.Add(FString::Printf(TEXT("添加: %s.ReturnValue -> %s.self"), *GetComponentID, *Node.NodeId));
				bModified = true;
			}
		}
	}

	if (bModified)
	{
		FixedCode = FString::Join(OutputLines, TEXT("\n"));
		UE_LOG(LogTemp, Log, TEXT("[Weaver AutoFix] Applied %d link/set fix(es)"), OutFixes.Num());
	}

	return FixedCode;
}

bool FWeaveInterpreter::Parse(const FString& WeaveCode, FWeaveAST& OutAST, FString& OutError)
{
	// 自动纠错预处理
	TArray<FString> AutoFixes;
	FString ProcessedCode = AutoFixWeaveCode(WeaveCode, AutoFixes);
	TArray<FString> LinkFixes;
	ProcessedCode = AutoFixLinksAndSets(ProcessedCode, LinkFixes);
	AutoFixes.Append(LinkFixes);

	TArray<FString> Tokens = Tokenize(ProcessedCode);
	if (Tokens.Num() == 0)
	{
		OutError = TEXT("Empty code");
		return false;
	}

	int32 Index = 0;


	if (Index < Tokens.Num() && Tokens[Index] == TEXT("graphset"))
	{
		Index++;
		if (Index >= Tokens.Num())
		{
			OutError = TEXT("Missing blueprint name after graphset");
			return false;
		}
		Index++;


		FString Path;
		while (Index < Tokens.Num()
			&& Tokens[Index] != TEXT("graph")
			&& Tokens[Index] != TEXT("var")
			&& Tokens[Index] != TEXT("node")
			&& Tokens[Index] != TEXT("set")
			&& Tokens[Index] != TEXT("link")
			&& Tokens[Index] != TEXT("comment")
			&& Tokens[Index] != TEXT("bubble"))
		{
			Path += Tokens[Index++];
		}
		OutAST.BlueprintPath = Path;
	}

	// 解析 graph 之前出现的 var 声明
	while (Index < Tokens.Num() && Tokens[Index] == TEXT("var"))
	{
		FWeaveVarDecl Var;
		if (!ParseVar(Tokens, Index, Var))
		{
			OutError = FString::Printf(TEXT("Failed to parse var at token %d"), Index);
			return false;
		}
		OutAST.Vars.Add(Var);
	}

	TArray<FWeaveParamDecl> FirstInputParams, FirstOutputParams;
	if (!ParseGraph(Tokens, Index, OutAST.GraphName, FirstInputParams, FirstOutputParams))
	{
		OutError = TEXT("Failed to parse graph declaration");
		return false;
	}

	// 当前正在填充的 Section
	FWeaveGraphSection CurrentSection;
	CurrentSection.GraphName = OutAST.GraphName;
	CurrentSection.InputParams = MoveTemp(FirstInputParams);
	CurrentSection.OutputParams = MoveTemp(FirstOutputParams);

	while (Index < Tokens.Num())
	{
		const FString& Token = Tokens[Index];

		if (Token == TEXT("graph"))
		{
			// 遇到新的 graph 声明 —— 保存当前 section，开始新 section
			OutAST.Sections.Add(MoveTemp(CurrentSection));
			CurrentSection = FWeaveGraphSection();

			FString NewGraphName;
			TArray<FWeaveParamDecl> NewInputParams, NewOutputParams;
			if (!ParseGraph(Tokens, Index, NewGraphName, NewInputParams, NewOutputParams))
			{
				OutError = FString::Printf(TEXT("Failed to parse graph declaration at token %d"), Index);
				return false;
			}
			CurrentSection.GraphName = NewGraphName;
			CurrentSection.InputParams = MoveTemp(NewInputParams);
			CurrentSection.OutputParams = MoveTemp(NewOutputParams);
		}
		else if (Token == TEXT("node"))
		{
			FWeaveNodeDecl Node;
			if (!ParseNode(Tokens, Index, Node))
			{
				OutError = FString::Printf(TEXT("Failed to parse node at token %d"), Index);
				return false;
			}
			CurrentSection.Nodes.Add(Node);
		}
		else if (Token == TEXT("set"))
		{
			FWeaveSetStmt Set;
			if (!ParseSet(Tokens, Index, Set))
			{
				OutError = FString::Printf(TEXT("Failed to parse set at token %d"), Index);
				return false;
			}
			CurrentSection.Sets.Add(Set);
		}
		else if (Token == TEXT("link"))
		{
			FWeaveLinkStmt Link;
			if (!ParseLink(Tokens, Index, Link))
			{
				// 输出解析失败位置附近的 token 上下文
				FString Context;
				int32 CtxStart = FMath::Max(0, Index - 3);
				int32 CtxEnd = FMath::Min(Tokens.Num(), Index + 4);
				for (int32 c = CtxStart; c < CtxEnd; ++c)
				{
					if (c == Index) Context += TEXT("[>>]");
					Context += Tokens[c] + TEXT(" ");
				}
				OutError = FString::Printf(TEXT("Failed to parse link at token %d. 附近 token: %s（详细原因见 OutputLog）"), Index, *Context);
				return false;
			}
			if (Link.FromNode == Link.ToNode)
			{
				OutError = FString::Printf(
					TEXT(
						"自连死循环：节点 '%s' 的输出引脚 '%s' 连接到自身的输入引脚 '%s'。执行引脚自连会让蓝图在该节点永远循环，永远无法继续执行后续逻辑。请改为连接到其他节点，或删除此 link 语句。"),
					*Link.FromNode, *Link.FromPin, *Link.ToPin);
				return false;
			}
			CurrentSection.Links.Add(Link);
		}
		else if (Token == TEXT("var"))
		{
			FWeaveVarDecl Var;
			if (!ParseVar(Tokens, Index, Var))
			{
				OutError = FString::Printf(TEXT("Failed to parse var at token %d"), Index);
				return false;
			}
			OutAST.Vars.Add(Var); // var 始终是全局的
		}
		else if (Token == TEXT("comment"))
		{
			FWeaveCommentDecl Comment;
			if (!ParseComment(Tokens, Index, Comment))
			{
				OutError = FString::Printf(TEXT("Failed to parse comment at token %d"), Index);
				return false;
			}
			CurrentSection.Comments.Add(Comment);
		}
		else if (Token == TEXT("bubble"))
		{
			FWeaveBubbleStmt Bubble;
			if (!ParseBubble(Tokens, Index, Bubble))
			{
				OutError = FString::Printf(TEXT("Failed to parse bubble at token %d"), Index);
				return false;
			}
			CurrentSection.Bubbles.Add(Bubble);
		}
		else
		{
			Index++;
		}
	}

	// 保存最后一个 section
	OutAST.Sections.Add(MoveTemp(CurrentSection));

	// 向后兼容：将第一个 section 的内容复制到 AST 顶层字段
	if (OutAST.Sections.Num() > 0)
	{
		OutAST.Nodes = OutAST.Sections[0].Nodes;
		OutAST.Sets = OutAST.Sections[0].Sets;
		OutAST.Links = OutAST.Sections[0].Links;
		OutAST.Comments = OutAST.Sections[0].Comments;
		OutAST.Bubbles = OutAST.Sections[0].Bubbles;
	}

	return true;
}

TArray<FString> FWeaveInterpreter::Tokenize(const FString& Code)
{
	TArray<FString> Tokens;
	FString Current;
	bool InString = false;
	bool InComment = false;

	for (int32 i = 0; i < Code.Len(); i++)
	{
		TCHAR Ch = Code[i];


		if (Ch == TEXT('#') && !InString)
		{
			InComment = true;
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
			continue;
		}

		if (InComment)
		{
			if (Ch == TEXT('\n') || Ch == TEXT('\r'))
			{
				InComment = false;
			}
			continue;
		}

		if (InString)
		{
			if (Ch == TEXT('\\') && i + 1 < Code.Len() && Code[i + 1] == TEXT('"'))
			{
				Current.AppendChar(TEXT('"'));
				i++;
			}
			else
			{
				Current.AppendChar(Ch);
				if (Ch == TEXT('"'))
				{
					InString = false;
				}
			}
		}
		else if (Ch == TEXT('"'))
		{
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
			Current.AppendChar(Ch);
			InString = true;
		}
		else if (FChar::IsWhitespace(Ch) || Ch == TEXT('\n') || Ch == TEXT('\r'))
		{
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
		}
		else if (Ch == TEXT(':') || Ch == TEXT('=') || Ch == TEXT('.') || Ch == TEXT('(') || Ch == TEXT(')') || Ch ==
			TEXT(',') || Ch == TEXT('@') || Ch == TEXT('[') || Ch == TEXT(']'))
		{
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
			Tokens.Add(FString::Chr(Ch));
		}
		else if (Ch == TEXT('-') && i + 1 < Code.Len() && Code[i + 1] == TEXT('>'))
		{
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
			Tokens.Add(TEXT("->"));
			i++;
		}
		else
		{
			Current.AppendChar(Ch);
		}
	}

	if (!Current.IsEmpty())
	{
		Tokens.Add(Current);
	}

	return Tokens;
}

bool FWeaveInterpreter::ParseGraph(const TArray<FString>& Tokens, int32& Index, FString& OutGraphName,
	TArray<FWeaveParamDecl>& OutInputParams, TArray<FWeaveParamDecl>& OutOutputParams)
{
	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("graph"))
	{
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}

	OutGraphName = Tokens[Index];
	Index++;

	// 解析输入参数: graph Func(param1: type1, param2: type2)
	if (Index < Tokens.Num() && Tokens[Index] == TEXT("("))
	{
		Index++; // skip (
		while (Index < Tokens.Num() && Tokens[Index] != TEXT(")"))
		{
			if (Tokens[Index] == TEXT(","))
			{
				Index++;
				continue;
			}
			FWeaveParamDecl Param;
			Param.Name = Tokens[Index];
			Index++;
			if (Index < Tokens.Num() && Tokens[Index] == TEXT(":"))
			{
				Index++; // skip :
				if (Index < Tokens.Num())
				{
					Param.Type = Tokens[Index];
					Index++;
				}
			}
			OutInputParams.Add(Param);
		}
		if (Index < Tokens.Num() && Tokens[Index] == TEXT(")"))
		{
			Index++; // skip )
		}
	}

	// 解析输出参数: -> type  或  -> (ret1: type1, ret2: type2)
	if (Index < Tokens.Num() && Tokens[Index] == TEXT("->"))
	{
		Index++; // skip ->
		if (Index < Tokens.Num())
		{
			if (Tokens[Index] == TEXT("("))
			{
				// 多返回值: -> (ret1: type1, ret2: type2)
				Index++; // skip (
				while (Index < Tokens.Num() && Tokens[Index] != TEXT(")"))
				{
					if (Tokens[Index] == TEXT(","))
					{
						Index++;
						continue;
					}
					FWeaveParamDecl Param;
					Param.Name = Tokens[Index];
					Index++;
					if (Index < Tokens.Num() && Tokens[Index] == TEXT(":"))
					{
						Index++; // skip :
						if (Index < Tokens.Num())
						{
							Param.Type = Tokens[Index];
							Index++;
						}
					}
					OutOutputParams.Add(Param);
				}
				if (Index < Tokens.Num() && Tokens[Index] == TEXT(")"))
				{
					Index++; // skip )
				}
			}
			else
			{
				// 单返回值: -> type
				FWeaveParamDecl Param;
				Param.Name = TEXT("ReturnValue");
				Param.Type = Tokens[Index];
				Index++;
				OutOutputParams.Add(Param);
			}
		}
	}

	return true;
}

bool FWeaveInterpreter::ParseNode(const TArray<FString>& Tokens, int32& Index, FWeaveNodeDecl& OutNode)
{
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}

	OutNode.NodeId = Tokens[Index++];

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
	{
		return false;
	}
	Index++;


	FString SchemaId;
	while (Index < Tokens.Num() && Tokens[Index] != TEXT("@") && Tokens[Index] != TEXT("node") && Tokens[Index] !=
		TEXT("set") && Tokens[Index] != TEXT("link") && Tokens[Index] != TEXT("bubble"))
	{
		SchemaId += Tokens[Index++];
	}
	OutNode.SchemaId = SchemaId.TrimStartAndEnd();

	if (Index < Tokens.Num() && Tokens[Index] == TEXT("@"))
	{
		Index++;

		if (Index >= Tokens.Num() || Tokens[Index] != TEXT("("))
		{
			return false;
		}
		Index++;

		if (Index >= Tokens.Num())
		{
			return false;
		}
		float X = FCString::Atof(*Tokens[Index++]);

		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(","))
		{
			return false;
		}
		Index++;

		if (Index >= Tokens.Num())
		{
			return false;
		}
		float Y = FCString::Atof(*Tokens[Index++]);

		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(")"))
		{
			return false;
		}
		Index++;

		OutNode.Position = FVector2D(X, Y);
	}
	else
	{
		OutNode.Position = FVector2D::ZeroVector;
	}

	return true;
}

bool FWeaveInterpreter::ParseSet(const TArray<FString>& Tokens, int32& Index, FWeaveSetStmt& OutSet)
{
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}

	OutSet.NodeId = Tokens[Index++];

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("."))
	{
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}
	OutSet.PinName = Tokens[Index++];
	if (OutSet.PinName.StartsWith(TEXT("\"")) && OutSet.PinName.EndsWith(TEXT("\"")) && OutSet.PinName.Len() >= 2)
	{
		OutSet.PinName = OutSet.PinName.Mid(1, OutSet.PinName.Len() - 2);
	}

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("="))
	{
		return false;
	}
	Index++;

	FString Value;

	if (Index < Tokens.Num() && Tokens[Index].StartsWith(TEXT("\"")))
	{
		// 引号包裹的值，去掉首尾引号
		Value = Tokens[Index++];
		if (Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")) && Value.Len() >= 2)
		{
			Value = Value.Mid(1, Value.Len() - 2);
			// 还原转义的引号
			Value = Value.Replace(TEXT("\\\""), TEXT("\""));
		}
	}
	else
	{
		while (Index < Tokens.Num())
		{
			const FString& Token = Tokens[Index];
			if (Token == TEXT("node") || Token == TEXT("set") || Token == TEXT("link") || Token == TEXT("graph") ||
				Token == TEXT("graphset") || Token == TEXT("var") || Token == TEXT("comment") || Token == TEXT("bubble"))
			{
				break;
			}
			Value += Token;
			Index++;
		}
	}

	OutSet.Value = Value.TrimStartAndEnd();
	return true;
}

bool FWeaveInterpreter::ParseLink(const TArray<FString>& Tokens, int32& Index, FWeaveLinkStmt& OutLink)
{
	const int32 StartIndex = Index;
	Index++;

	// 期望格式: link FromNode . FromPin -> ToNode . ToPin
	if (Index >= Tokens.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: 缺少 FromNode（'link' 后没有更多 token）"), StartIndex);
		return false;
	}

	OutLink.FromNode = Tokens[Index++];

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("."))
	{
		FString Got = Index < Tokens.Num() ? Tokens[Index] : TEXT("<EOF>");
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: FromNode='%s' 后期望 '.' 但得到 '%s'"), Index, *OutLink.FromNode, *Got);
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: FromNode='%s' 后缺少 FromPin"), Index, *OutLink.FromNode);
		return false;
	}
	OutLink.FromPin = Tokens[Index++];
	if (OutLink.FromPin.StartsWith(TEXT("\"")) && OutLink.FromPin.EndsWith(TEXT("\"")) && OutLink.FromPin.Len() >= 2)
	{
		OutLink.FromPin = OutLink.FromPin.Mid(1, OutLink.FromPin.Len() - 2);
	}

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("->"))
	{
		FString Got = Index < Tokens.Num() ? Tokens[Index] : TEXT("<EOF>");
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: '%s.%s' 后期望 '->' 但得到 '%s'"), Index, *OutLink.FromNode, *OutLink.FromPin, *Got);
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: '%s.%s ->' 后缺少 ToNode"), Index, *OutLink.FromNode, *OutLink.FromPin);
		return false;
	}
	OutLink.ToNode = Tokens[Index++];

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("."))
	{
		FString Got = Index < Tokens.Num() ? Tokens[Index] : TEXT("<EOF>");
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: ToNode='%s' 后期望 '.' 但得到 '%s'"), Index, *OutLink.ToNode, *Got);
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: ToNode='%s' 后缺少 ToPin"), Index, *OutLink.ToNode);
		return false;
	}
	OutLink.ToPin = Tokens[Index++];
	if (OutLink.ToPin.StartsWith(TEXT("\"")) && OutLink.ToPin.EndsWith(TEXT("\"")) && OutLink.ToPin.Len() >= 2)
	{
		OutLink.ToPin = OutLink.ToPin.Mid(1, OutLink.ToPin.Len() - 2);
	}

	return true;
}

bool FWeaveInterpreter::ParseVar(const TArray<FString>& Tokens, int32& Index, FWeaveVarDecl& OutVar)
{
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}

	FString VarName;
	while (Index < Tokens.Num() && Tokens[Index] != TEXT(":"))
	{
		if (!VarName.IsEmpty())
		{
			VarName += TEXT(" ");
		}
		VarName += Tokens[Index++];
	}
	OutVar.VarName = VarName.TrimStartAndEnd();

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
	{
		return false;
	}
	Index++; // 跳过 ":"

	if (Index >= Tokens.Num())
	{
		return false;
	}

	FString TypeToken = Tokens[Index++];

	// 检查是否为容器类型前缀：array、set、map
	if (TypeToken == TEXT("array") || TypeToken == TEXT("set"))
	{
		OutVar.ContainerType = (TypeToken == TEXT("array"))
			? EPinContainerType::Array
			: EPinContainerType::Set;

		// 期望 ":" 和元素类型
		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
		{
			return false;
		}
		Index++; // 跳过 ":"

		if (Index >= Tokens.Num())
		{
			return false;
		}
		OutVar.VarType = Tokens[Index++];
	}
	else if (TypeToken == TEXT("map"))
	{
		OutVar.ContainerType = EPinContainerType::Map;

		// 期望 ":" KeyType ":" ValueType
		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
		{
			return false;
		}
		Index++; // 跳过 ":"

		if (Index >= Tokens.Num())
		{
			return false;
		}
		OutVar.VarType = Tokens[Index++]; // Key 类型

		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
		{
			return false;
		}
		Index++; // 跳过 ":"

		if (Index >= Tokens.Num())
		{
			return false;
		}
		OutVar.ValueType = Tokens[Index++]; // Value 类型
	}
	else if (TypeToken == TEXT("class"))
	{
		// class:AActor 或 class:/Game/Path/BP.BP 格式
		OutVar.ContainerType = EPinContainerType::None;
		if (Index < Tokens.Num() && Tokens[Index] == TEXT(":"))
		{
			Index++; // 跳过 ":"
			if (Index < Tokens.Num())
			{
				FString ClassType = Tokens[Index++];
				// 如果类路径以 / 开头，继续收集后续 token
				if (ClassType.StartsWith(TEXT("/")))
				{
					while (Index < Tokens.Num())
					{
						const FString& NextToken = Tokens[Index];
						if (NextToken == TEXT("var") || NextToken == TEXT("graph")
							|| NextToken == TEXT("node") || NextToken == TEXT("set")
							|| NextToken == TEXT("link") || NextToken == TEXT("comment")
							|| NextToken == TEXT("bubble")
							|| NextToken == TEXT("editable") || NextToken == TEXT("readonly")
							|| NextToken == TEXT("spawn") || NextToken == TEXT("category")
							|| NextToken == TEXT("="))
						{
							break;
						}
						if (NextToken.StartsWith(TEXT("\"")))
						{
							break;
						}
						ClassType += NextToken;
						Index++;
					}
				}
				OutVar.VarType = TEXT("class:") + ClassType;
			}
			else
			{
				OutVar.VarType = TEXT("class");
			}
		}
		else
		{
			OutVar.VarType = TEXT("class");
		}
	}
	else
	{
		OutVar.ContainerType = EPinContainerType::None;
		OutVar.VarType = TypeToken;

		// 如果类型以 / 开头（蓝图路径），需要继续收集 . 后面的部分
		// 例如：/Game/Path/To/Asset.BP_Name 会被 tokenizer 拆分为多个 token
		if (TypeToken.StartsWith(TEXT("/")))
		{
			while (Index < Tokens.Num())
			{
				const FString& NextToken = Tokens[Index];
				// 停止收集：遇到关键字、= 号、属性关键字或引号字符串（描述）
				if (NextToken == TEXT("var") || NextToken == TEXT("graph")
					|| NextToken == TEXT("node") || NextToken == TEXT("set")
					|| NextToken == TEXT("link") || NextToken == TEXT("comment")
					|| NextToken == TEXT("bubble")
					|| NextToken == TEXT("editable") || NextToken == TEXT("readonly")
					|| NextToken == TEXT("spawn") || NextToken == TEXT("category")
					|| NextToken == TEXT("="))
				{
					break;
				}
				// 如果遇到引号包裹的字符串，可能是描述，停止类型收集
				if (NextToken.StartsWith(TEXT("\"")))
				{
					break;
				}
				OutVar.VarType += NextToken;
				Index++;
			}
		}
	}

	// 解析可选的默认值：= value 或 = [elem1, elem2, ...]
	if (Index < Tokens.Num() && Tokens[Index] == TEXT("="))
	{
		Index++; // 跳过 "="

		if (Index < Tokens.Num())
		{
			if (Tokens[Index] == TEXT("["))
			{
				// 数组字面量: [elem1, elem2, ...]
				Index++; // 跳过 "["
				while (Index < Tokens.Num() && Tokens[Index] != TEXT("]"))
				{
					// 跳过逗号分隔符
					if (Tokens[Index] == TEXT(","))
					{
						Index++;
						continue;
					}

					// 收集单个元素（支持负号和小数点重组）
					FString Element;

					// 处理负号前缀
					if (Tokens[Index] == TEXT("-") && Index + 1 < Tokens.Num())
					{
						Element = TEXT("-");
						Index++;
					}

					// 引号包裹的元素（字符串数组）
					if (Index < Tokens.Num() && Tokens[Index].StartsWith(TEXT("\"")))
					{
						FString Val = Tokens[Index++];
						if (Val.StartsWith(TEXT("\"")) && Val.EndsWith(TEXT("\"")) && Val.Len() >= 2)
						{
							Val = Val.Mid(1, Val.Len() - 2);
						}
						Element += Val;
					}
					else if (Index < Tokens.Num())
					{
						// 数值元素：收集主体部分
						Element += Tokens[Index++];

						// 重组小数: "1000" + "." + "0" → "1000.0"
						if (Index + 1 < Tokens.Num() && Tokens[Index] == TEXT(".") && Tokens[Index + 1] != TEXT("]") && Tokens[Index + 1] != TEXT(","))
						{
							Element += TEXT(".") + Tokens[Index + 1];
							Index += 2;
						}
					}

					if (!Element.IsEmpty())
					{
						OutVar.ArrayDefaultValues.Add(Element);
					}
				}
				if (Index < Tokens.Num() && Tokens[Index] == TEXT("]"))
				{
					Index++; // 跳过 "]"
				}
			}
			else if (Tokens[Index].StartsWith(TEXT("\"")))
			{
				// 引号包裹的默认值
				FString Val = Tokens[Index++];
				if (Val.StartsWith(TEXT("\"")) && Val.EndsWith(TEXT("\"")) && Val.Len() >= 2)
				{
					Val = Val.Mid(1, Val.Len() - 2);
				}
				Val = Val.Replace(TEXT("\\\""), TEXT("\""));
				Val = Val.Replace(TEXT("\\\\"), TEXT("\\"));
				OutVar.DefaultValue = Val;
			}
			else
			{
				// 非引号值：收集到下一个关键字、引号字符串、属性关键字为止
				FString Val;
				while (Index < Tokens.Num())
				{
					const FString& Token = Tokens[Index];
					if (Token == TEXT("var") || Token == TEXT("graph")
						|| Token == TEXT("node") || Token == TEXT("set")
						|| Token == TEXT("link") || Token == TEXT("comment")
						|| Token == TEXT("bubble")
						|| Token == TEXT("editable") || Token == TEXT("readonly")
						|| Token == TEXT("spawn") || Token == TEXT("category"))
					{
						break;
					}
					if (Token.StartsWith(TEXT("\"")))
					{
						break;
					}
					Val += Token;
					Index++;
				}
				OutVar.DefaultValue = Val.TrimStartAndEnd();
			}
		}
	}

	// 解析可选的变量属性关键字（可以任意顺序组合）
	// 支持: editable, readonly, spawn, category:"分类名"
	while (Index < Tokens.Num())
	{
		const FString& Token = Tokens[Index];

		if (Token == TEXT("editable"))
		{
			OutVar.bInstanceEditable = true;
			Index++;
		}
		else if (Token == TEXT("readonly"))
		{
			OutVar.bBlueprintReadOnly = true;
			Index++;
		}
		else if (Token == TEXT("spawn"))
		{
			OutVar.bExposeOnSpawn = true;
			Index++;
		}
		else if (Token == TEXT("category"))
		{
			Index++; // 跳过 "category"
			// 期望 ":" 后跟分类名
			if (Index < Tokens.Num() && Tokens[Index] == TEXT(":"))
			{
				Index++; // 跳过 ":"
			}
			if (Index < Tokens.Num())
			{
				FString Cat = Tokens[Index++];
				if (Cat.StartsWith(TEXT("\"")) && Cat.EndsWith(TEXT("\"")) && Cat.Len() >= 2)
				{
					Cat = Cat.Mid(1, Cat.Len() - 2);
				}
				OutVar.Category = Cat;
			}
		}
		else
		{
			break; // 不是属性关键字，结束循环
		}
	}

	// 解析可选的变量描述：引号包裹的字符串（在所有属性之后）
	// 语法: var Name : Type "description text"
	// 语法: var Name : Type = value editable category:"战斗" "description text"
	if (Index < Tokens.Num() && Tokens[Index].StartsWith(TEXT("\"")))
	{
		FString Desc = Tokens[Index];
		if (Desc.StartsWith(TEXT("\"")) && Desc.EndsWith(TEXT("\"")) && Desc.Len() >= 2)
		{
			Desc = Desc.Mid(1, Desc.Len() - 2);
		}
		Desc = Desc.Replace(TEXT("\\n"), TEXT("\n"));
		Desc = Desc.Replace(TEXT("\\\""), TEXT("\""));
		Desc = Desc.Replace(TEXT("\\\\"), TEXT("\\"));
		OutVar.Description = Desc;
		Index++;
	}

	UE_LOG(LogTemp, Log, TEXT("[Weaver] ParseVar result: %s type=%s default='%s' arrayDefaults=%d editable=%d readonly=%d spawn=%d category='%s' desc='%s'"),
		*OutVar.VarName, *OutVar.VarType, *OutVar.DefaultValue, OutVar.ArrayDefaultValues.Num(),
		OutVar.bInstanceEditable, OutVar.bBlueprintReadOnly, OutVar.bExposeOnSpawn,
		*OutVar.Category, *OutVar.Description.Left(30));

	return true;
}

bool FWeaveInterpreter::ParseComment(const TArray<FString>& Tokens, int32& Index, FWeaveCommentDecl& OutComment)
{
	// 格式: comment "文本" @ (X, Y) size (W, H) [color (R, G, B, A)] [fontsize N]
	Index++; // 跳过 "comment"

	if (Index >= Tokens.Num())
	{
		return false;
	}

	// 读取注释文本（引号包裹）
	FString Text = Tokens[Index++];
	if (Text.StartsWith(TEXT("\"")) && Text.EndsWith(TEXT("\"")) && Text.Len() >= 2)
	{
		Text = Text.Mid(1, Text.Len() - 2);
	}
	// 还原转义（顺序重要：先处理 \\ 避免与 \n、\" 冲突）
	Text = Text.Replace(TEXT("\\\\"), TEXT("\x01"));  // 临时占位
	Text = Text.Replace(TEXT("\\n"), TEXT("\n"));
	Text = Text.Replace(TEXT("\\\""), TEXT("\""));
	Text = Text.Replace(TEXT("\x01"), TEXT("\\"));
	OutComment.Text = Text;

	// @ (X, Y)
	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("@"))
	{
		return false;
	}
	Index++; // @

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("("))
	{
		return false;
	}
	Index++; // (

	if (Index >= Tokens.Num()) return false;
	OutComment.Position.X = FCString::Atof(*Tokens[Index++]);

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT(",")) return false;
	Index++; // ,

	if (Index >= Tokens.Num()) return false;
	OutComment.Position.Y = FCString::Atof(*Tokens[Index++]);

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT(")")) return false;
	Index++; // )

	// size (W, H)
	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("size"))
	{
		return false;
	}
	Index++; // size

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("(")) return false;
	Index++; // (

	if (Index >= Tokens.Num()) return false;
	OutComment.Size.X = FCString::Atof(*Tokens[Index++]);

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT(",")) return false;
	Index++; // ,

	if (Index >= Tokens.Num()) return false;
	OutComment.Size.Y = FCString::Atof(*Tokens[Index++]);

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT(")")) return false;
	Index++; // )

	// 可选: color (R, G, B, A)
	if (Index < Tokens.Num() && Tokens[Index] == TEXT("color"))
	{
		Index++; // color
		if (Index >= Tokens.Num() || Tokens[Index] != TEXT("(")) return false;
		Index++; // (

		// 颜色值为 0-255 整数格式
		if (Index >= Tokens.Num()) return false;
		OutComment.Color.R = FCString::Atoi(*Tokens[Index++]) / 255.f;
		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(",")) return false;
		Index++;
		if (Index >= Tokens.Num()) return false;
		OutComment.Color.G = FCString::Atoi(*Tokens[Index++]) / 255.f;
		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(",")) return false;
		Index++;
		if (Index >= Tokens.Num()) return false;
		OutComment.Color.B = FCString::Atoi(*Tokens[Index++]) / 255.f;
		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(",")) return false;
		Index++;
		if (Index >= Tokens.Num()) return false;
		OutComment.Color.A = FCString::Atoi(*Tokens[Index++]) / 255.f;

		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(")")) return false;
		Index++; // )
	}

	// 可选: fontsize N
	if (Index < Tokens.Num() && Tokens[Index] == TEXT("fontsize"))
	{
		Index++; // fontsize
		if (Index >= Tokens.Num()) return false;
		OutComment.FontSize = FCString::Atoi(*Tokens[Index++]);
	}

	return true;
}

bool FWeaveInterpreter::ParseBubble(const TArray<FString>& Tokens, int32& Index, FWeaveBubbleStmt& OutBubble)
{
	// 格式: bubble <NodeId> "文本"
	Index++; // 跳过 "bubble"

	if (Index >= Tokens.Num())
	{
		return false;
	}

	OutBubble.NodeId = Tokens[Index++];

	if (Index >= Tokens.Num() || !Tokens[Index].StartsWith(TEXT("\"")))
	{
		return false;
	}

	FString Text = Tokens[Index++];
	if (Text.StartsWith(TEXT("\"")) && Text.EndsWith(TEXT("\"")) && Text.Len() >= 2)
	{
		Text = Text.Mid(1, Text.Len() - 2);
	}
	Text = Text.Replace(TEXT("\\\\"), TEXT("\x01"));
	Text = Text.Replace(TEXT("\\n"), TEXT("\n"));
	Text = Text.Replace(TEXT("\\\""), TEXT("\""));
	Text = Text.Replace(TEXT("\x01"), TEXT("\\"));
	OutBubble.Text = Text;

	return true;
}

bool FWeaveInterpreter::ResolveWeaveType(const FString& TypeStr, FEdGraphPinType& OutPinType)
{
	if (TypeStr == TEXT("bool"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return true;
	}
	if (TypeStr == TEXT("int"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return true;
	}
	if (TypeStr == TEXT("int64"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		return true;
	}
	if (TypeStr == TEXT("float"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		return true;
	}
	if (TypeStr == TEXT("double"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return true;
	}
	if (TypeStr == TEXT("string"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		return true;
	}
	if (TypeStr == TEXT("text"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		return true;
	}
	if (TypeStr == TEXT("name"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		return true;
	}
	if (TypeStr == TEXT("byte"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		return true;
	}
	if (TypeStr == TEXT("Vector"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		return true;
	}
	if (TypeStr == TEXT("Rotator"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		return true;
	}
	if (TypeStr == TEXT("Transform"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		return true;
	}

	// 结构体查找
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (It->GetName() == TypeStr)
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = *It;
			return true;
		}
	}

	// 枚举查找
	for (TObjectIterator<UEnum> It; It; ++It)
	{
		if (It->GetName() == TypeStr)
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			OutPinType.PinSubCategoryObject = *It;
			return true;
		}
	}

	// C++ 类查找 (带前缀)
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetPrefixCPP() + It->GetName() == TypeStr || It->GetName() == TypeStr)
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = *It;
			return true;
		}
	}

	// 蓝图路径查找
	if (TypeStr.StartsWith(TEXT("/")))
	{
		if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *TypeStr))
		{
			if (BP->GeneratedClass)
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				OutPinType.PinSubCategoryObject = BP->GeneratedClass;
				return true;
			}
		}
	}

	return false;
}

// 在编译后将数组字面量默认值写入 CDO
static void ApplyArrayDefaults(UBlueprint* Blueprint, const TArray<FWeaveVarDecl>& Vars)
{
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return;
	}

	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(true);
	if (!CDO)
	{
		return;
	}

	for (const FWeaveVarDecl& VarDecl : Vars)
	{
		if (VarDecl.ArrayDefaultValues.Num() == 0)
		{
			continue;
		}

		FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(FName(*VarDecl.VarName));
		FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
		if (!ArrayProp)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] ApplyArrayDefaults: FArrayProperty not found for '%s'"), *VarDecl.VarName);
			continue;
		}

		// 构造 UE ImportText 格式: (Elem1,Elem2,Elem3)
		FString ImportStr = TEXT("(");
		for (int32 i = 0; i < VarDecl.ArrayDefaultValues.Num(); i++)
		{
			if (i > 0) ImportStr += TEXT(",");
			ImportStr += VarDecl.ArrayDefaultValues[i];
		}
		ImportStr += TEXT(")");

		void* ArrayPtr = ArrayProp->ContainerPtrToValuePtr<void>(CDO);
		const TCHAR* ImportResult = ArrayProp->ImportText_Direct(*ImportStr, ArrayPtr, CDO, 0, nullptr);
		if (ImportResult)
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] ApplyArrayDefaults: %s = %s (%d elements)"),
				*VarDecl.VarName, *ImportStr, VarDecl.ArrayDefaultValues.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] ApplyArrayDefaults: ImportText failed for '%s' with '%s'"),
				*VarDecl.VarName, *ImportStr);
		}
	}
}

int32 FWeaveInterpreter::GenerateBlueprint(const FWeaveAST& AST, UEdGraph* Graph, FString& OutError,
	const TMap<FString, UK2Node*>* ExistingNodes)
{
	if (!Graph)
	{
		OutError = TEXT("Invalid graph");
		return 0;
	}

	FScopedTransaction Transaction(NSLOCTEXT("WeaveLanguage", "GenerateBlueprint", "Weave Generate Blueprint"));
	Graph->Modify();

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Generating blueprint for graph: %s"), *AST.GraphName);
	UE_LOG(LogTemp, Log, TEXT("[Weaver] Vars: %d, Nodes: %d, Sets: %d, Links: %d"), AST.Vars.Num(), AST.Nodes.Num(),
	       AST.Sets.Num(), AST.Links.Num());


	FString FunctionEventNodeId;
	bool bIsFunctionGraph = (AST.GraphName == TEXT("UserConstructionScript") ||
		Graph->GetName().Contains(TEXT("UserConstructionScript")) ||
		!AST.GraphName.Contains(TEXT("EventGraph")));

	if (bIsFunctionGraph)
	{
		for (const FWeaveNodeDecl& NodeDecl : AST.Nodes)
		{
			FString RawSchemaId = NodeDecl.SchemaId;
			if (RawSchemaId.StartsWith(TEXT("\"")) && RawSchemaId.EndsWith(TEXT("\"")) && RawSchemaId.Len() >= 2)
				RawSchemaId = RawSchemaId.Mid(1, RawSchemaId.Len() - 2);
			if (RawSchemaId == TEXT("event.Actor.UserConstructionScript"))
			{
				FunctionEventNodeId = NodeDecl.NodeId;
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] 检测到函数图表中的事件节点 %s，将自动转换为 entry 节点"), *NodeDecl.NodeId);
				break;
			}
		}
	}


	UBlueprint* Blueprint = Cast<UBlueprint>(Graph->GetOuter());
	if (Blueprint && AST.Vars.Num() > 0)
	{
		for (const FWeaveVarDecl& VarDecl : AST.Vars)
		{
			bool bExists = false;
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (Var.VarName.ToString() == VarDecl.VarName)
				{
					bExists = true;
					break;
				}
			}

			// 变量已存在时，仍然更新默认值和描述（如果有）
			if (bExists)
			{
				FName VarName = FName(*VarDecl.VarName);

				if (!VarDecl.DefaultValue.IsEmpty() && VarDecl.ArrayDefaultValues.Num() == 0)
				{
					int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
					if (VarIndex != INDEX_NONE)
					{
						Blueprint->NewVariables[VarIndex].DefaultValue = VarDecl.DefaultValue;
						FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Updated existing variable default: %s = %s"), *VarDecl.VarName, *VarDecl.DefaultValue);
					}
				}

				if (!VarDecl.Description.IsEmpty())
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, TEXT("tooltip"), VarDecl.Description);
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Updated existing variable tooltip: %s = \"%s\""), *VarDecl.VarName, *VarDecl.Description);
				}

				// 更新变量属性
				{
					int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
					if (VarIndex != INDEX_NONE)
					{
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Updating existing var properties: %s (editable=%d readonly=%d spawn=%d category=%s)"),
							*VarDecl.VarName, VarDecl.bInstanceEditable, VarDecl.bBlueprintReadOnly, VarDecl.bExposeOnSpawn, *VarDecl.Category);
						if (VarDecl.bInstanceEditable)
						{
							Blueprint->NewVariables[VarIndex].PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
							Blueprint->NewVariables[VarIndex].PropertyFlags &= ~CPF_DisableEditOnInstance;
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Set existing variable editable: %s (flags=0x%llX)"), *VarDecl.VarName, (uint64)Blueprint->NewVariables[VarIndex].PropertyFlags);
						}
						if (VarDecl.bBlueprintReadOnly)
						{
							Blueprint->NewVariables[VarIndex].PropertyFlags |= CPF_BlueprintReadOnly;
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Set existing variable readonly: %s"), *VarDecl.VarName);
						}
						if (VarDecl.bExposeOnSpawn)
						{
							// spawn 自动隐含 editable（UE 要求）
							Blueprint->NewVariables[VarIndex].PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
							Blueprint->NewVariables[VarIndex].PropertyFlags &= ~CPF_DisableEditOnInstance;
							FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, TEXT("ExposeOnSpawn"), TEXT("true"));
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Set existing variable expose on spawn: %s"), *VarDecl.VarName);
						}
						if (!VarDecl.Category.IsEmpty())
						{
							FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(VarDecl.Category));
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Set existing variable category: %s -> %s"), *VarDecl.VarName, *VarDecl.Category);
						}
					}
				}
			}

			if (!bExists)
			{
				FEdGraphPinType PinType;
				bool bTypeResolved = false;


				if (VarDecl.VarType == TEXT("bool"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("int"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("int64"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("float"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
					PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("double"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
					PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("string"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_String;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("text"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("name"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("byte"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
					bTypeResolved = true;
				}


				if (!bTypeResolved)
				{
					for (TObjectIterator<UScriptStruct> It; It; ++It)
					{
						if (It->GetName() == VarDecl.VarType)
						{
							PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
							PinType.PinSubCategoryObject = *It;
							bTypeResolved = true;
							break;
						}
					}
				}


				if (!bTypeResolved)
				{
					for (TObjectIterator<UEnum> It; It; ++It)
					{
						if (It->GetName() == VarDecl.VarType)
						{
							PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
							PinType.PinSubCategoryObject = *It;
							bTypeResolved = true;
							break;
						}
					}
				}


				if (!bTypeResolved)
				{
					for (TObjectIterator<UClass> It; It; ++It)
					{
						if (It->GetPrefixCPP() + It->GetName() == VarDecl.VarType)
						{
							PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
							PinType.PinSubCategoryObject = *It;
							bTypeResolved = true;
							break;
						}
					}
				}


				if (!bTypeResolved && VarDecl.VarType.StartsWith(TEXT("class:")))
				{
					FString ClassName = VarDecl.VarType.Mid(6);

					if (ClassName.StartsWith(TEXT("/")))
					{
						if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ClassName))
						{
							if (BP->GeneratedClass)
							{
								PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
								PinType.PinSubCategoryObject = BP->GeneratedClass;
								bTypeResolved = true;
							}
						}
					}
					else
					{
						for (TObjectIterator<UClass> It; It; ++It)
						{
							if (It->GetPrefixCPP() + It->GetName() == ClassName)
							{
								PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
								PinType.PinSubCategoryObject = *It;
								bTypeResolved = true;
								break;
							}
						}
					}
				}


				if (!bTypeResolved && VarDecl.VarType.StartsWith(TEXT("/")))
				{
					if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *VarDecl.VarType))
					{
						if (BP->GeneratedClass)
						{
							PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
							PinType.PinSubCategoryObject = BP->GeneratedClass;
							bTypeResolved = true;
						}
					}
				}

				if (!bTypeResolved)
				{
					UE_LOG(LogTemp, Warning,
					       TEXT("[Weaver] Unknown variable type: %s. Use SearchType to find valid names."),
					       *VarDecl.VarType);
					if (!OutError.IsEmpty()) OutError += TEXT("\n");
					OutError += FString::Printf(
						TEXT("var %s : %s 失败：未知类型 '%s'，请先调用 SearchType 查询正确的类型名称。"),
						*VarDecl.VarName, *VarDecl.VarType, *VarDecl.VarType);
					continue;
				}


				// 设置容器类型
				PinType.ContainerType = VarDecl.ContainerType;
				if (VarDecl.ContainerType == EPinContainerType::Map)
				{
					// 解析 Map 的 Value 类型到 PinValueType
					const FString& ValTypeStr = VarDecl.ValueType;
					if (ValTypeStr == TEXT("bool")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Boolean; }
					else if (ValTypeStr == TEXT("int")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int; }
					else if (ValTypeStr == TEXT("int64")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int64; }
					else if (ValTypeStr == TEXT("float")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Real; PinType.PinValueType.TerminalSubCategory = UEdGraphSchema_K2::PC_Float; }
					else if (ValTypeStr == TEXT("double")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Real; PinType.PinValueType.TerminalSubCategory = UEdGraphSchema_K2::PC_Double; }
					else if (ValTypeStr == TEXT("string")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_String; }
					else if (ValTypeStr == TEXT("text")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Text; }
					else if (ValTypeStr == TEXT("name")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Name; }
					else if (ValTypeStr == TEXT("byte")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Byte; }
					else
					{
						// 尝试 struct / enum / class 解析
						bool bValResolved = false;
						for (TObjectIterator<UScriptStruct> It; It && !bValResolved; ++It)
						{
							if (It->GetName() == ValTypeStr)
							{
								PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Struct;
								PinType.PinValueType.TerminalSubCategoryObject = *It;
								bValResolved = true;
							}
						}
						for (TObjectIterator<UEnum> It; It && !bValResolved; ++It)
						{
							if (It->GetName() == ValTypeStr)
							{
								PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Byte;
								PinType.PinValueType.TerminalSubCategoryObject = *It;
								bValResolved = true;
							}
						}
						for (TObjectIterator<UClass> It; It && !bValResolved; ++It)
						{
							if (It->GetPrefixCPP() + It->GetName() == ValTypeStr)
							{
								PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Object;
								PinType.PinValueType.TerminalSubCategoryObject = *It;
								bValResolved = true;
							}
						}
						if (!bValResolved)
						{
							UE_LOG(LogTemp, Warning, TEXT("[Weaver] Unknown map value type: %s"), *ValTypeStr);
						}
					}
				}

				FName VarName = FName(*VarDecl.VarName);
				FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType);

				// 设置变量默认值（标量，数组默认值在编译后通过 CDO 设置）
				if (!VarDecl.DefaultValue.IsEmpty() && VarDecl.ArrayDefaultValues.Num() == 0)
				{
					int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
					if (VarIndex != INDEX_NONE)
					{
						Blueprint->NewVariables[VarIndex].DefaultValue = VarDecl.DefaultValue;
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Set variable default: %s = %s"), *VarDecl.VarName, *VarDecl.DefaultValue);
					}
				}

				// 设置变量描述/tooltip
				if (!VarDecl.Description.IsEmpty())
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, TEXT("tooltip"), VarDecl.Description);
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Set variable tooltip: %s = \"%s\""), *VarDecl.VarName, *VarDecl.Description);
				}

				// 设置变量属性：可编辑性、分类、ExposeOnSpawn
				{
					int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
					if (VarIndex != INDEX_NONE)
					{
						if (VarDecl.bInstanceEditable)
						{
							Blueprint->NewVariables[VarIndex].PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
							Blueprint->NewVariables[VarIndex].PropertyFlags &= ~CPF_DisableEditOnInstance;
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Set variable editable: %s"), *VarDecl.VarName);
						}
						if (VarDecl.bBlueprintReadOnly)
						{
							Blueprint->NewVariables[VarIndex].PropertyFlags |= CPF_BlueprintReadOnly;
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Set variable readonly: %s"), *VarDecl.VarName);
						}
						if (VarDecl.bExposeOnSpawn)
						{
							// spawn 自动隐含 editable（UE 要求）
							Blueprint->NewVariables[VarIndex].PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
							Blueprint->NewVariables[VarIndex].PropertyFlags &= ~CPF_DisableEditOnInstance;
							FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, TEXT("ExposeOnSpawn"), TEXT("true"));
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Set variable expose on spawn: %s"), *VarDecl.VarName);
						}
						if (!VarDecl.Category.IsEmpty())
						{
							FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(VarDecl.Category));
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Set variable category: %s -> %s"), *VarDecl.VarName, *VarDecl.Category);
						}
					}
				}

				UE_LOG(LogTemp, Log, TEXT("[Weaver] Created variable: %s (%s, editable=%d, container=%d)"), *VarDecl.VarName, *VarDecl.VarType, VarDecl.bInstanceEditable, (int32)VarDecl.ContainerType);
			}
		}

		// 编译蓝图骨架，确保新变量的 FProperty 可用于后续节点的 AllocateDefaultPins
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		// 编译后将数组字面量默认值写入 CDO（CDO 只在编译后才存在）
		ApplyArrayDefaults(Blueprint, AST.Vars);
	}


	TMap<FString, UK2Node*> CreatedNodes;
	int32 NodesCreated = 0;

	// 第一趟：优先创建所有 CustomEvent 节点，编译后让它们注册为函数
	// 这样后续 call.BP_XXX.EventName 能找到对应函数，不会误创建同名函数图表
	{
		bool bHasCustomEvents = false;
		for (const FWeaveNodeDecl& NodeDecl : AST.Nodes)
		{
			FString RawSchema = NodeDecl.SchemaId;
			if (RawSchema.StartsWith(TEXT("\"")) && RawSchema.EndsWith(TEXT("\"")) && RawSchema.Len() >= 2)
				RawSchema = RawSchema.Mid(1, RawSchema.Len() - 2);

			if (RawSchema.StartsWith(TEXT("customEvent.")))
			{
				// 增量模式：尝试复用已有 CustomEvent 节点
				if (ExistingNodes)
				{
					UK2Node* const* ExistingPtr = ExistingNodes->Find(NodeDecl.NodeId);
					if (ExistingPtr && *ExistingPtr)
					{
						(*ExistingPtr)->NodePosX = NodeDecl.Position.X;
						(*ExistingPtr)->NodePosY = NodeDecl.Position.Y;
						CreatedNodes.Add(NodeDecl.NodeId, *ExistingPtr);
						NodesCreated++;
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Reused existing CustomEvent: %s"), *NodeDecl.NodeId);
						continue;
					}
				}

				TArray<FString> CEParts;
				RawSchema.ParseIntoArray(CEParts, TEXT("."));
				if (CEParts.Num() >= 2)
				{
					FString EventName = CEParts[1];
					UK2Node_CustomEvent* CENode = SpawnEditorNode<UK2Node_CustomEvent>(Graph);
					if (CENode)
					{
						CENode->CustomFunctionName = FName(*EventName);
						CENode->NodePosX = NodeDecl.Position.X;
						CENode->NodePosY = NodeDecl.Position.Y;
						CENode->AllocateDefaultPins();
						CreatedNodes.Add(NodeDecl.NodeId, CENode);
						NodesCreated++;
						bHasCustomEvents = true;
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Pre-created CustomEvent: %s"), *EventName);
					}
				}
			}
		}

		// CustomEvent 创建后编译蓝图，使其注册为可调用的函数
		if (bHasCustomEvents)
		{
			UBlueprint* BP = Graph->GetTypedOuter<UBlueprint>();
			if (BP)
			{
				FKismetEditorUtilities::CompileBlueprint(BP);
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Compiled blueprint after CustomEvent pre-creation"));
			}
		}
	}

	for (const FWeaveNodeDecl& NodeDecl : AST.Nodes)
	{
		// 跳过已在第一趟预创建的 CustomEvent 节点
		if (CreatedNodes.Contains(NodeDecl.NodeId))
		{
			continue;
		}

		if (!FunctionEventNodeId.IsEmpty() && NodeDecl.NodeId == FunctionEventNodeId)
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] 跳过事件节点 %s，使用 entry 代替"), *NodeDecl.NodeId);
			continue;
		}

		UK2Node* NewNode = nullptr;

		// 增量模式：尝试复用已有节点
		if (ExistingNodes)
		{
			UK2Node* const* ExistingPtr = ExistingNodes->Find(NodeDecl.NodeId);
			if (ExistingPtr && *ExistingPtr)
			{
				NewNode = *ExistingPtr;
				NewNode->NodePosX = NodeDecl.Position.X;
				NewNode->NodePosY = NodeDecl.Position.Y;
				// 不清空引脚默认值——保留 WorldContextObject、枚举默认值等隐式值
				// 后续 set 语句会覆盖需要修改的引脚，link 会重建连接
				CreatedNodes.Add(NodeDecl.NodeId, NewNode);
				NodesCreated++;
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Reused existing node: %s (%s)"),
					*NodeDecl.NodeId, *NodeDecl.SchemaId);
				continue;
			}
		}

		// 去掉 SchemaId 首尾引号（Generator 对含空格的 SchemaId 会用引号包裹）
		FString SchemaId = NodeDecl.SchemaId;
		if (SchemaId.StartsWith(TEXT("\"")) && SchemaId.EndsWith(TEXT("\"")) && SchemaId.Len() >= 2)
		{
			SchemaId = SchemaId.Mid(1, SchemaId.Len() - 2);
		}

		// 处理 entry 节点：直接使用函数图表中现有的 FunctionEntry 节点
		if (SchemaId == TEXT("entry"))
		{
			for (UEdGraphNode* ExistingNode : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(ExistingNode))
				{
					NewNode = EntryNode;
					break;
				}
			}
			if (NewNode)
			{
				CreatedNodes.Add(NodeDecl.NodeId, NewNode);
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Mapped entry node: %s -> existing FunctionEntry"), *NodeDecl.NodeId);
				continue;
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] No FunctionEntry node found in graph for entry node '%s'"), *NodeDecl.NodeId);
				continue;
			}
		}

		// 处理 result 节点：直接使用函数图表中现有的 FunctionResult 节点
		if (SchemaId == TEXT("result"))
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Processing result node '%s', searching %d graph nodes"), *NodeDecl.NodeId, Graph->Nodes.Num());
			for (UEdGraphNode* ExistingNode : Graph->Nodes)
			{
				UE_LOG(LogTemp, Log, TEXT("[Weaver]   Checking node: %s (%s)"), *ExistingNode->GetName(), *ExistingNode->GetClass()->GetName());
				if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(ExistingNode))
				{
					NewNode = ResultNode;
					break;
				}
			}
			if (NewNode)
			{
				CreatedNodes.Add(NodeDecl.NodeId, NewNode);
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Mapped result node: %s -> existing FunctionResult"), *NodeDecl.NodeId);
				continue;
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] No FunctionResult node found in graph for result node '%s'"), *NodeDecl.NodeId);
				continue;
			}
		}

		TArray<FString> Parts;
		SchemaId.ParseIntoArray(Parts, TEXT("."));

		if (Parts.Num() < 2)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Invalid schema ID: %s"), *NodeDecl.SchemaId);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += FString::Printf(
				TEXT(
					"节点 '%s' 的 schema_id '%s' 格式无效（应为 call.类名.函数名 / event.类名.事件名 / macro.宏库名.宏名 / special.类型），该节点未被创建。"),
				*NodeDecl.NodeId, *NodeDecl.SchemaId);
			continue;
		}

		FString NodeKind = Parts[0];

		if (NodeKind == TEXT("special"))
		{
			FString NodeType = Parts[1];
			if (NodeType == TEXT("Branch"))
			{
				NewNode = CreateBranchNode(Graph);
			}
			else if (NodeType == TEXT("Sequence"))
			{
				NewNode = CreateSequenceNode(Graph);
			}
			else if (NodeType == TEXT("MathExpression"))
			{
				NewNode = CreateMathExpressionNode(Graph);
			}
			else if (NodeType == TEXT("Make") && Parts.Num() >= 3)
			{
				FString StructTypeName = Parts[2];
				NewNode = CreateMakeStructNode(Graph, StructTypeName);
			}
			else if (NodeType == TEXT("Break") && Parts.Num() >= 3)
			{
				FString StructTypeName = Parts[2];
				NewNode = CreateBreakStructNode(Graph, StructTypeName);
			}
			else if (NodeType == TEXT("SpawnActorFromClass"))
			{
				NewNode = CreateSpawnActorFromClassNode(Graph);
			}
			else if (NodeType == TEXT("ConstructObjectFromClass"))
			{
				NewNode = CreateConstructObjectFromClassNode(Graph);
			}
			else if (NodeType == TEXT("Cast") && Parts.Num() >= 3)
			{
				// Parts[2:] 可能是完整路径（含 .），需要拼接回来
				FString TargetTypeName = Parts[2];
				for (int32 p = 3; p < Parts.Num(); ++p)
				{
					TargetTypeName += TEXT(".") + Parts[p];
				}
				NewNode = CreateDynamicCastNode(Graph, TargetTypeName);
			}
			else if (NodeType == TEXT("SwitchEnum") && Parts.Num() >= 3)
			{
				FString EnumName = Parts[2];
				NewNode = CreateSwitchEnumNode(Graph, EnumName);
			}
			else if (NodeType == TEXT("GetArrayItem"))
			{
				NewNode = CreateGetArrayItemNode(Graph);
			}
			else if (NodeType == TEXT("Knot"))
			{
				NewNode = CreateKnotNode(Graph);
			}
			else if (NodeType == TEXT("Self"))
			{
				NewNode = CreateSelfNode(Graph);
			}
			else if (NodeType == TEXT("Timeline") && Parts.Num() >= 3)
			{
				FString TimelineName = Parts[2];
				NewNode = CreateTimelineNode(Graph, TimelineName);
			}
		}
		else if (NodeKind == TEXT("asyncAction") && Parts.Num() >= 3)
		{
			// asyncAction.<ClassPath>.<FunctionName>
			// ClassPath 可能含 .（如 /Script/Module.ClassName），最后一段是函数名
			FString FactoryFuncName = Parts.Last();
			FString ClassPath = Parts[1];
			for (int32 p = 2; p < Parts.Num() - 1; ++p)
			{
				ClassPath += TEXT(".") + Parts[p];
			}
			NewNode = CreateAsyncActionNode(Graph, ClassPath, FactoryFuncName);
		}
		else if (NodeKind == TEXT("native") && Parts.Num() >= 2)
		{
			// native.ClassName — 通用第三方插件节点
			NewNode = CreateNativeNode(Graph, Parts[1]);
		}
		else if (NodeKind == TEXT("customEvent") && Parts.Num() >= 2)
		{
			// customEvent.EventName — 自定义事件（UK2Node_CustomEvent）
			FString CustomEventName = Parts[1];
			UK2Node_CustomEvent* CustomEventNode = SpawnEditorNode<UK2Node_CustomEvent>(Graph);
			if (CustomEventNode)
			{
				CustomEventNode->CustomFunctionName = FName(*CustomEventName);
				CustomEventNode->AllocateDefaultPins();
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Created CustomEvent: %s"), *CustomEventName);
			}
			NewNode = CustomEventNode;
		}
		else if (Parts.Num() < 3)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Invalid schema ID: %s"), *NodeDecl.SchemaId);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += FString::Printf(
				TEXT("节点 '%s' 的 schema_id '%s' 格式无效（应为 call.类名.函数名 / event.类名.事件名 / macro.宏库名.宏名 / native.类名），该节点未被创建。"),
				*NodeDecl.NodeId, *NodeDecl.SchemaId);
			continue;
		}
		else
		{
			FString ClassName = Parts[1];
			FString FunctionName = Parts[2];

			// 完整路径支持：call./Script/Module.Class.Func → ClassName="/Script/Module.Class", FunctionName="Func"
			// componentEvent 有自己的多段解析逻辑，不在这里处理
			if (NodeKind != TEXT("componentEvent") && Parts.Num() > 3)
			{
				ClassName = Parts[1];
				for (int32 p = 2; p < Parts.Num() - 1; ++p)
				{
					ClassName += TEXT(".") + Parts[p];
				}
				FunctionName = Parts.Last();
			}

			if (NodeKind == TEXT("componentEvent") && Parts.Num() >= 4)
			{
				// componentEvent.ComponentVarName.DelegateOwnerClass.DelegateName
				// 完整路径时: componentEvent.VarName./Script/Engine.PrimitiveComponent.OnBeginOverlap
				FString ComponentVarName = Parts[1];
				FString DelegateName = Parts.Last();
				FString DelegateClassName = Parts[2];
				for (int32 p = 3; p < Parts.Num() - 1; ++p)
				{
					DelegateClassName += TEXT(".") + Parts[p];
				}
				NewNode = CreateComponentBoundEventNode(Graph, ComponentVarName, DelegateClassName, DelegateName);
			}
			else if (NodeKind == TEXT("event"))
			{
				NewNode = CreateEventNode(Graph, ClassName, FunctionName);
			}
			else if (NodeKind == TEXT("message"))
			{
				NewNode = CreateMessageNode(Graph, ClassName, FunctionName);
			}
			else if (NodeKind == TEXT("call"))
			{
				// 兼容旧脚本：如果脚本引用了 Target 引脚（典型的接口消息节点），优先还原为 UK2Node_Message。
				const bool bWantsTargetPin = AST.Links.ContainsByPredicate([&NodeDecl](const FWeaveLinkStmt& L)
				{
					return (L.ToNode == NodeDecl.NodeId && L.ToPin == TEXT("Target")) || (L.FromNode == NodeDecl.NodeId && L.FromPin == TEXT("Target"));
				}) || AST.Sets.ContainsByPredicate([&NodeDecl](const FWeaveSetStmt& S)
				{
					return (S.NodeId == NodeDecl.NodeId && S.PinName == TEXT("Target"));
				});
				if (bWantsTargetPin)
				{
					NewNode = CreateMessageNode(Graph, ClassName, FunctionName);
				}
				if (!NewNode)
				{
					NewNode = CreateCallNode(Graph, ClassName, FunctionName);
				}
			}
			else if (NodeKind == TEXT("macro"))
			{
				FString MacroPath;
				FString MacroName;

				if (ClassName == TEXT("StandardMacros"))
				{
					MacroPath = FString::Printf(TEXT("/Engine/EditorBlueprintResources/%s.%s"), *ClassName, *ClassName);
					MacroName = FunctionName;
				}
				else
				{
					int32 ColonIndex;
					if (FunctionName.FindChar(TEXT(':'), ColonIndex))
					{
						MacroPath = ClassName + TEXT(".") + FunctionName.Left(ColonIndex);
						MacroName = FunctionName.Mid(ColonIndex + 1);
					}
					else
					{
						MacroPath = ClassName;
						MacroName = FunctionName;
					}
				}

				NewNode = CreateMacroNode(Graph, MacroPath, MacroName);
			}
			else if (NodeKind == TEXT("VariableGet") || NodeKind == TEXT("ValidatedGet") || NodeKind == TEXT("VariableSet"))
			{
				// 对于外部类，路径可能包含多个 .（如 /Script/Module.ClassName.VarName）
				// 最后一段是变量名，前面的拼接回来是类名
				if (Parts.Num() > 3)
				{
					// 重新构建 ClassName：Parts[1] 到 Parts[Num-2] 拼接
					ClassName = Parts[1];
					for (int32 p = 2; p < Parts.Num() - 1; ++p)
					{
						ClassName += TEXT(".") + Parts[p];
					}
					FunctionName = Parts.Last();
				}

				// 判断是自身变量还是外部类成员变量
				bool bIsSelfVar = false;
				bool bIsExternalVar = false;
				UClass* ExternalClass = nullptr;

				UE_LOG(LogTemp, Log, TEXT("[Weaver] Variable node: %s kind=%s class=%s var=%s"), *NodeDecl.NodeId, *NodeKind, *ClassName, *FunctionName);

				// 检查是否为当前蓝图自身变量
				if (Blueprint)
				{
					FString BPClassName;
					if (Blueprint->GeneratedClass)
					{
						BPClassName = Blueprint->GeneratedClass->GetName();
					}

					UE_LOG(LogTemp, Log, TEXT("[Weaver] BPClassName=%s, ClassName match=%d"), *BPClassName, (ClassName == BPClassName || ClassName.IsEmpty()));

					if (ClassName == BPClassName || ClassName.IsEmpty())
					{
						// 属于当前蓝图
						UClass* GenClass = Blueprint->GeneratedClass;
						if (GenClass)
						{
							FProperty* Prop = GenClass->FindPropertyByName(FName(*FunctionName));
							bIsSelfVar = (Prop != nullptr);
						}
						if (!bIsSelfVar)
						{
							for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
							{
								if (ExistingVar.VarName.ToString() == FunctionName)
								{
									bIsSelfVar = true;
									break;
								}
							}
						}
						if (!bIsSelfVar)
						{
							for (const FWeaveVarDecl& VarDecl : AST.Vars)
							{
								if (VarDecl.VarName == FunctionName)
								{
									bIsSelfVar = true;
									break;
								}
							}
						}
					}
					else
					{
						// ClassName 不是当前蓝图类名
						// 优先尝试解析为外部类 — 因为 schema 中明确指定了不同的类名，
						// 说明用户意图访问外部蓝图的变量，即使本蓝图也有同名变量
						ExternalClass = FindClassByShortName(ClassName);
						if (ExternalClass)
						{
							// 验证外部类确实有该属性
							FProperty* ExternalProp = ExternalClass->FindPropertyByName(FName(*FunctionName));
							if (ExternalProp)
							{
								bIsExternalVar = true;
								UE_LOG(LogTemp, Log, TEXT("[Weaver] Variable '%s' resolved as external on class '%s'"), *FunctionName, *ClassName);
							}
							else
							{
								// 外部类存在但没有该属性 — 可能外部BP未编译，尝试编译后再查找
								if (UBlueprint* ExternalBP = Cast<UBlueprint>(ExternalClass->ClassGeneratedBy))
								{
									FKismetEditorUtilities::CompileBlueprint(ExternalBP);
									if (ExternalBP->GeneratedClass)
									{
										ExternalClass = ExternalBP->GeneratedClass;
										ExternalProp = ExternalClass->FindPropertyByName(FName(*FunctionName));
									}
								}
								if (ExternalProp)
								{
									bIsExternalVar = true;
									UE_LOG(LogTemp, Log, TEXT("[Weaver] Variable '%s' resolved as external on class '%s' (after recompile)"), *FunctionName, *ClassName);
								}
								else
								{
									UE_LOG(LogTemp, Warning, TEXT("[Weaver] External class '%s' found but property '%s' missing, falling back to self check"), *ClassName, *FunctionName);
									ExternalClass = nullptr;
								}
							}
						}

						// 外部类解析失败时，回退到 AST.Vars / NewVariables 检查
						// （处理跨蓝图粘贴时源蓝图类名与目标蓝图类名不同的情况）
						if (!bIsExternalVar)
						{
							bool bDeclaredInAST = false;
							for (const FWeaveVarDecl& VarDecl : AST.Vars)
							{
								if (VarDecl.VarName == FunctionName)
								{
									bDeclaredInAST = true;
									break;
								}
							}

							if (bDeclaredInAST)
							{
								bIsSelfVar = true;
								UE_LOG(LogTemp, Log, TEXT("[Weaver] Variable '%s' declared in AST, treating as self var (source class: %s, external class not found/no property)"), *FunctionName, *ClassName);
							}
							else
							{
								bool bFoundInNewVars = false;
								for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
								{
									if (ExistingVar.VarName.ToString() == FunctionName)
									{
										bFoundInNewVars = true;
										break;
									}
								}

								if (bFoundInNewVars)
								{
									bIsSelfVar = true;
									UE_LOG(LogTemp, Log, TEXT("[Weaver] Variable '%s' found in Blueprint->NewVariables, treating as self var"), *FunctionName);
								}
								else
								{
									UE_LOG(LogTemp, Warning, TEXT("[Weaver] External class not found: %s, trying as self var"), *ClassName);
									if (Blueprint->GeneratedClass)
									{
										FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(FName(*FunctionName));
										bIsSelfVar = (Prop != nullptr);
									}
								}
							}
						}
					}
				}

				UE_LOG(LogTemp, Log, TEXT("[Weaver] Variable resolve: %s -> bIsSelfVar=%d bIsExternalVar=%d"), *NodeDecl.NodeId, bIsSelfVar, bIsExternalVar);

				if (!bIsSelfVar && !bIsExternalVar)
				{
					OutError = FString::Printf(
						TEXT("变量 '%s' 不存在：%s.%s.%s 引用了未知变量。")
						TEXT("蓝图中的用户变量和组件变量均未找到同名属性，")
						TEXT("本次 Weave 也未声明 'var %s : <类型>'。")
						TEXT("请先用 SearchContextVar 确认变量名称，或添加 var 声明。"),
						*FunctionName, *NodeKind, *ClassName, *FunctionName, *FunctionName);
					return -1;
				}

				if (NodeKind == TEXT("VariableGet") || NodeKind == TEXT("ValidatedGet"))
				{
					if (bIsExternalVar && ExternalClass)
					{
						NewNode = CreateVariableGetNodeExternal(Graph, ExternalClass, FunctionName);
					}
					else
					{
						NewNode = CreateVariableGetNode(Graph, Blueprint, FunctionName);
					}

					// ValidatedGet: 转换为带执行引脚的验证获取模式
					if (NodeKind == TEXT("ValidatedGet") && NewNode)
					{
						if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(NewNode))
						{
							VarGetNode->SetPurity(false);
						}
					}
				}
				else
				{
					if (bIsExternalVar && ExternalClass)
					{
						NewNode = CreateVariableSetNodeExternal(Graph, ExternalClass, FunctionName);
					}
					else
					{
						NewNode = CreateVariableSetNode(Graph, Blueprint, FunctionName);
					}
				}
			}
		}

		if (NewNode)
		{
			NewNode->NodePosX = NodeDecl.Position.X;
			NewNode->NodePosY = NodeDecl.Position.Y;

			CreatedNodes.Add(NodeDecl.NodeId, NewNode);
			NodesCreated++;

			UE_LOG(LogTemp, Log, TEXT("[Weaver] Created node: %s (%s)"), *NodeDecl.NodeId, *NodeDecl.SchemaId);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Failed to create node: %s (%s)"), *NodeDecl.NodeId, *NodeDecl.SchemaId);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += FString::Printf(
				TEXT("节点 '%s' (schema: %s) 创建失败：函数/类/宏可能不存在于目标蓝图中。如果是自定义函数，请确保目标蓝图中已定义该函数。"),
				*NodeDecl.NodeId, *NodeDecl.SchemaId);
		}
	}

	if (NodesCreated == 0 && AST.Comments.Num() == 0)
	{
		OutError = TEXT("No nodes created");
		return 0;
	}

	// 修补 VariableGet/VariableSet 的引脚类型：
	// AllocateDefaultPins 可能在蓝图未编译时找不到 FProperty，导致引脚类型缺失容器信息。
	// 直接从 Blueprint->NewVariables 读取完整 PinType 覆盖到引脚上。
	// 同时处理跨蓝图（External）变量节点：从外部类的 FProperty 解析正确的引脚类型。
	if (Blueprint)
	{
		for (auto& KV : CreatedNodes)
		{
			UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(KV.Value);
			UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(KV.Value);
			if (!VarGet && !VarSet) continue;

			FName VarName = VarGet ? VarGet->GetVarName() : VarSet->GetVarName();
			bool bPatched = false;

			// 1. 自身变量：从 Blueprint->NewVariables 修补
			for (const FBPVariableDescription& Desc : Blueprint->NewVariables)
			{
				if (Desc.VarName == VarName && Desc.VarType.ContainerType != EPinContainerType::None)
				{
					// 修补输出引脚（Get）或输入引脚（Set）
					UEdGraphPin* ValuePin = KV.Value->FindPin(VarName, VarGet ? EGPD_Output : EGPD_Input);
					if (ValuePin)
					{
						ValuePin->PinType = Desc.VarType;
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Patched pin type for variable '%s' (container=%d)"),
							*VarName.ToString(), (int32)Desc.VarType.ContainerType);
						bPatched = true;
					}
					break;
				}
			}

			// 2. 外部变量：从外部类的 FProperty 解析引脚类型
			if (!bPatched)
			{
				FMemberReference& VarRef = VarGet ? VarGet->VariableReference : VarSet->VariableReference;
				if (!VarRef.IsSelfContext())
				{
					UClass* ExternalClass = VarRef.GetMemberParentClass();
					if (ExternalClass)
					{
						FProperty* Prop = ExternalClass->FindPropertyByName(VarName);
						if (Prop)
						{
							UEdGraphPin* ValuePin = KV.Value->FindPin(VarName, VarGet ? EGPD_Output : EGPD_Input);
							if (ValuePin)
							{
								const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
								if (K2Schema)
								{
									FEdGraphPinType ResolvedType;
									K2Schema->ConvertPropertyToPinType(Prop, ResolvedType);
									ValuePin->PinType = ResolvedType;
									UE_LOG(LogTemp, Log, TEXT("[Weaver] Patched external pin type for '%s.%s' (container=%d)"),
										*ExternalClass->GetName(), *VarName.ToString(), (int32)ResolvedType.ContainerType);
								}
							}
						}
						else
						{
							UE_LOG(LogTemp, Warning, TEXT("[Weaver] External variable '%s' not found on class '%s'"),
								*VarName.ToString(), *ExternalClass->GetName());
						}
					}
				}
			}
		}
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());

	for (const FWeaveSetStmt& Set : AST.Sets)
	{
		UK2Node** NodePtr = CreatedNodes.Find(Set.NodeId);
		if (!NodePtr)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Set failed: node not found (%s)"), *Set.NodeId);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += FString::Printf(
				TEXT("set %s.%s 失败：节点 '%s' 不存在（可能因 schema_id 无效而未被创建）。"), *Set.NodeId, *Set.PinName, *Set.NodeId);
			continue;
		}

		UK2Node* Node = *NodePtr;


		if (Set.PinName == TEXT("Expression"))
		{
			if (UK2Node_MathExpression* MathNode = Cast<UK2Node_MathExpression>(Node))
			{
				FString FinalValue = Set.Value;
				if (FinalValue.StartsWith(TEXT("\"")) && FinalValue.EndsWith(TEXT("\"")) && !FinalValue.Contains(
					TEXT("\\\"")))
				{
					FinalValue = FinalValue.Mid(1, FinalValue.Len() - 2);
				}
				MathNode->Expression = FinalValue;
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Set: %s.Expression = %s"), *Set.NodeId, *FinalValue);
				continue;
			}
		}

		// Timeline 特殊 set ($timeline_*)
		if (Set.PinName.StartsWith(TEXT("$timeline_")))
		{
			UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node);
			if (!TimelineNode)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] %s is not a Timeline node, skipping $timeline_ set"), *Set.NodeId);
				continue;
			}
			UBlueprint* BP = Graph->GetTypedOuter<UBlueprint>();
			UTimelineTemplate* Template = BP ? BP->FindTimelineTemplateByVariableName(TimelineNode->TimelineName) : nullptr;
			if (!Template)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Timeline template not found for %s"), *TimelineNode->TimelineName.ToString());
				continue;
			}

			FString Val = Set.Value;
			if (Val.StartsWith(TEXT("\"")) && Val.EndsWith(TEXT("\"")) && Val.Len() >= 2)
				Val = Val.Mid(1, Val.Len() - 2);

			if (Set.PinName == TEXT("$timeline_float"))
			{
				TArray<FString> TrackNames;
				Val.ParseIntoArray(TrackNames, TEXT(","));
				for (const FString& TName : TrackNames)
				{
					FString Trimmed = TName.TrimStartAndEnd();
					bool bExists = false;
					for (const FTTFloatTrack& Existing : Template->FloatTracks)
					{
						if (Existing.GetTrackName() == FName(*Trimmed)) { bExists = true; break; }
					}
					if (!bExists)
					{
						FTTFloatTrack NewTrack;
						NewTrack.SetTrackName(FName(*Trimmed), Template);
						Template->FloatTracks.Add(NewTrack);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Timeline %s: added float track '%s'"), *Set.NodeId, *Trimmed);
					}
				}
			}
			else if (Set.PinName == TEXT("$timeline_event"))
			{
				TArray<FString> TrackNames;
				Val.ParseIntoArray(TrackNames, TEXT(","));
				for (const FString& TName : TrackNames)
				{
					FString Trimmed = TName.TrimStartAndEnd();
					bool bExists = false;
					for (const FTTEventTrack& Existing : Template->EventTracks)
					{
						if (Existing.GetTrackName() == FName(*Trimmed)) { bExists = true; break; }
					}
					if (!bExists)
					{
						FTTEventTrack NewTrack;
						NewTrack.SetTrackName(FName(*Trimmed), Template);
						Template->EventTracks.Add(NewTrack);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Timeline %s: added event track '%s'"), *Set.NodeId, *Trimmed);
					}
				}
			}
			else if (Set.PinName == TEXT("$timeline_vector"))
			{
				TArray<FString> TrackNames;
				Val.ParseIntoArray(TrackNames, TEXT(","));
				for (const FString& TName : TrackNames)
				{
					FString Trimmed = TName.TrimStartAndEnd();
					bool bExists = false;
					for (const FTTVectorTrack& Existing : Template->VectorTracks)
					{
						if (Existing.GetTrackName() == FName(*Trimmed)) { bExists = true; break; }
					}
					if (!bExists)
					{
						FTTVectorTrack NewTrack;
						NewTrack.SetTrackName(FName(*Trimmed), Template);
						Template->VectorTracks.Add(NewTrack);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Timeline %s: added vector track '%s'"), *Set.NodeId, *Trimmed);
					}
				}
			}
			else if (Set.PinName == TEXT("$timeline_color"))
			{
				TArray<FString> TrackNames;
				Val.ParseIntoArray(TrackNames, TEXT(","));
				for (const FString& TName : TrackNames)
				{
					FString Trimmed = TName.TrimStartAndEnd();
					bool bExists = false;
					for (const FTTLinearColorTrack& Existing : Template->LinearColorTracks)
					{
						if (Existing.GetTrackName() == FName(*Trimmed)) { bExists = true; break; }
					}
					if (!bExists)
					{
						FTTLinearColorTrack NewTrack;
						NewTrack.SetTrackName(FName(*Trimmed), Template);
						Template->LinearColorTracks.Add(NewTrack);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Timeline %s: added color track '%s'"), *Set.NodeId, *Trimmed);
					}
				}
			}
			else if (Set.PinName == TEXT("$timeline_length"))
			{
				Template->TimelineLength = FCString::Atof(*Val);
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Timeline %s: length = %f"), *Set.NodeId, Template->TimelineLength);
			}
			else if (Set.PinName == TEXT("$timeline_autoplay"))
			{
				Template->bAutoPlay = (Val == TEXT("true"));
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Timeline %s: autoplay = %s"), *Set.NodeId, *Val);
			}
			else if (Set.PinName == TEXT("$timeline_loop"))
			{
				Template->bLoop = (Val == TEXT("true"));
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Timeline %s: loop = %s"), *Set.NodeId, *Val);
			}

			continue;
		}

		if (Set.PinName == TEXT("Class"))
		{
			FString ClassPath = Set.Value;
			if (ClassPath.StartsWith(TEXT("\"")) && ClassPath.EndsWith(TEXT("\"")) && !ClassPath.Contains(TEXT("\\\"")))
			{
				ClassPath = ClassPath.Mid(1, ClassPath.Len() - 2);
			}

			if (ClassPath.StartsWith(TEXT("class:")))
			{
				ClassPath.RemoveFromStart(TEXT("class:"));
			}
			if (UK2Node_SpawnActorFromClass* SpawnNode = Cast<UK2Node_SpawnActorFromClass>(Node))
			{
				UClass* ActorClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_None,
				                                     nullptr);
				if (!ActorClass)
				{
					FString AssetPath = ClassPath;
					if (AssetPath.EndsWith(TEXT("_C")))
						AssetPath.RemoveFromEnd(TEXT("_C"));
					if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath))
						ActorClass = BP->GeneratedClass;
				}
				if (ActorClass)
				{
					UEdGraphPin* ClassPin = SpawnNode->GetClassPin();
					if (ClassPin)
					{
						ClassPin->DefaultObject = ActorClass;
						ClassPin->DefaultValue = ActorClass->GetPathName();
						SpawnNode->PinDefaultValueChanged(ClassPin);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] SpawnActorFromClass: Class set to %s"), *ClassPath);
					}
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("[Weaver] SpawnActorFromClass: Class not found: %s"), *ClassPath);
				}
				continue;
			}
			if (UK2Node_ConstructObjectFromClass* ConstructNode = Cast<UK2Node_ConstructObjectFromClass>(Node))
			{
				UClass* ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_None,
				                                   nullptr);
				if (!ObjClass)
				{
					FString AssetPath2 = ClassPath;
					if (AssetPath2.EndsWith(TEXT("_C")))
						AssetPath2.RemoveFromEnd(TEXT("_C"));
					if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath2))
						ObjClass = BP->GeneratedClass;
				}
				if (ObjClass)
				{
					UEdGraphPin* ClassPin = ConstructNode->GetClassPin();
					if (ClassPin)
					{
						ClassPin->DefaultObject = ObjClass;
						ClassPin->DefaultValue = ObjClass->GetPathName();
						ConstructNode->PinDefaultValueChanged(ClassPin);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] ConstructObjectFromClass: Class set to %s"), *ClassPath);
					}
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("[Weaver] ConstructObjectFromClass: Class not found: %s"),
					       *ClassPath);
				}
				continue;
			}
		}


		UEdGraphPin* Pin = Node->FindPin(*Set.PinName, EGPD_Input);

		// 模糊匹配 fallback（处理空格差异）
		if (!Pin)
		{
			FString NormalizedName = Set.PinName.Replace(TEXT(" "), TEXT(""));
			for (UEdGraphPin* P : Node->Pins)
			{
				if (P->Direction == EGPD_Input && !P->bHidden)
				{
					FString ActualName = P->PinName.ToString().Replace(TEXT(" "), TEXT(""));
					if (ActualName.Equals(NormalizedName, ESearchCase::IgnoreCase))
					{
						Pin = P;
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Fuzzy pin match (set): '%s' -> '%s'"), *Set.PinName, *P->PinName.ToString());
						break;
					}
				}
			}
		}

		// 如果找不到引脚，尝试递归展开结构体引脚
		if (!Pin && Set.PinName.Contains(TEXT("_")) && Schema)
		{
			TArray<FString> Ancestors;
			FString PinName = Set.PinName;
			int32 LastUnderscore;
			while (PinName.FindLastChar(TEXT('_'), LastUnderscore))
			{
				PinName = PinName.Left(LastUnderscore);
				Ancestors.Insert(PinName, 0);
			}

			for (const FString& AncestorName : Ancestors)
			{
				UEdGraphPin* AncestorPin = Node->FindPin(*AncestorName, EGPD_Input);
				if (AncestorPin && AncestorPin->SubPins.Num() == 0 &&
					AncestorPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					Schema->SplitPin(AncestorPin, false);
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-split struct pin for set: %s.%s"), *Set.NodeId, *AncestorName);
				}
			}
			Pin = Node->FindPin(*Set.PinName, EGPD_Input);
		}

		if (!Pin)
		{
			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				UFunction* Func = CallNode->GetTargetFunction();
				if (Func)
				{
					for (TFieldIterator<FProperty> It(Func); It; ++It)
					{
						if ((*It)->GetName() == Set.PinName)
						{
							Pin = Node->FindPin(TEXT("self"), EGPD_Input);
							break;
						}
					}
				}
			}
		}

		if (!Pin)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Set failed: pin not found (%s.%s)"), *Set.NodeId, *Set.PinName);
			continue;
		}


		FString FinalValue = Set.Value;
		if (FinalValue.StartsWith(TEXT("\"")) && FinalValue.EndsWith(TEXT("\"")) && !FinalValue.Contains(TEXT("\\\"")))
		{
			FinalValue = FinalValue.Mid(1, FinalValue.Len() - 2);
		}


		{
			TArray<FString> ValParts;
			FinalValue.ParseIntoArray(ValParts, TEXT("."));
			if (ValParts.Num() == 2
				&& !FinalValue.Contains(TEXT(" "))
				&& !FinalValue.StartsWith(TEXT("/"))
				&& CreatedNodes.Contains(ValParts[0]))
			{
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += FString::Printf(
					TEXT("set %s.%s = %s 错误：'%s' 是节点引脚引用，不是一个值。")
					TEXT("应改为 link 语句：link %s -> %s.%s"),
					*Set.NodeId, *Set.PinName, *FinalValue,
					*FinalValue, *FinalValue, *Set.NodeId, *Set.PinName);
				continue;
			}
		}


		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
		{
			FString ClassValue = FinalValue;
			if (ClassValue.StartsWith(TEXT("class:")))
				ClassValue.RemoveFromStart(TEXT("class:"));
			UClass* PinClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassValue, nullptr, LOAD_None,
			                                   nullptr);
			if (!PinClass)
			{
				FString AssetPath = ClassValue;
				if (AssetPath.EndsWith(TEXT("_C")))
					AssetPath.RemoveFromEnd(TEXT("_C"));
				if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath))
					PinClass = BP->GeneratedClass;
			}
			// 使用 FindClassByShortName 作为后备（支持 C++ 类名如 USplineComponent）
			if (!PinClass)
			{
				PinClass = FindClassByShortName(ClassValue);
			}
			if (PinClass)
			{
				Pin->DefaultObject = PinClass;
				Pin->DefaultValue = TEXT("");
				Node->PinDefaultValueChanged(Pin);
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Set class pin: %s.%s = %s"), *Set.NodeId, *Set.PinName,
				       *PinClass->GetName());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Set failed: class not found %s.%s = %s"), *Set.NodeId,
				       *Set.PinName, *FinalValue);
			}
		}
		else
		{
			const bool bIsObjectPin = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
				|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface
				|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject);
			if (bIsObjectPin)
			{
				FString Lower = FinalValue.ToLower();


				if (Lower == TEXT("nullptr") || Lower == TEXT("none") || Lower.IsEmpty()
					|| (Lower == TEXT("self") && !Pin->bHidden))
				{
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Set: %s.%s = (skipped for object pin, value='%s')"),
					       *Set.NodeId, *Set.PinName, *FinalValue);
					continue;
				}
			}


			FString UEValue = FinalValue;
			if (UEValue.StartsWith(TEXT("vec(")) && UEValue.EndsWith(TEXT(")")))
			{
				FString Inner = UEValue.Mid(4, UEValue.Len() - 5);
				TArray<FString> Parts;
				Inner.ParseIntoArray(Parts, TEXT(","), true);
				if (Parts.Num() == 3)
				{
					const float X = FCString::Atof(*Parts[0].TrimStartAndEnd());
					const float Y = FCString::Atof(*Parts[1].TrimStartAndEnd());
					const float Z = FCString::Atof(*Parts[2].TrimStartAndEnd());
					UEValue = FString::Printf(TEXT("%f,%f,%f"), X, Y, Z);
				}
			}

			else if (UEValue.StartsWith(TEXT("rot(")) && UEValue.EndsWith(TEXT(")")))
			{
				FString Inner = UEValue.Mid(4, UEValue.Len() - 5);
				TArray<FString> Parts;
				Inner.ParseIntoArray(Parts, TEXT(","), true);
				if (Parts.Num() == 3)
				{
					const float R = FCString::Atof(*Parts[0].TrimStartAndEnd());
					const float P = FCString::Atof(*Parts[1].TrimStartAndEnd());
					const float Y = FCString::Atof(*Parts[2].TrimStartAndEnd());
					UEValue = FString::Printf(TEXT("(Pitch=%f,Yaw=%f,Roll=%f)"), P, Y, R);
				}
			}
			// 使用 Schema->TrySetDefaultValue 确保枚举等引脚值格式正确并通知节点
			if (Schema)
			{
				FString OldValue = Pin->DefaultValue;
				Schema->TrySetDefaultValue(*Pin, UEValue);
				bool bSetOK = (Pin->DefaultValue != OldValue || Pin->DefaultValue == UEValue);
				if (!bSetOK)
				{
					// 枚举引脚可能需要完整限定名 (如 ESpawnActorCollisionHandlingMethod::AlwaysSpawn)
					// 尝试从引脚类型的 SubCategoryObject 获取枚举前缀
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
					{
						UEnum* PinEnum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
						if (PinEnum)
						{
							FString QualifiedValue = FString::Printf(TEXT("%s::%s"), *PinEnum->GetName(), *UEValue);
							FString OldValue2 = Pin->DefaultValue;
							Schema->TrySetDefaultValue(*Pin, QualifiedValue);
							if (Pin->DefaultValue != OldValue2 || Pin->DefaultValue == QualifiedValue)
							{
								UE_LOG(LogTemp, Log, TEXT("[Weaver] Set (qualified enum): %s.%s = %s"), *Set.NodeId, *Set.PinName, *QualifiedValue);
								bSetOK = true;
							}
						}
					}
					if (!bSetOK)
					{
						// 最终回退: 直接设置 DefaultValue
						Pin->DefaultValue = UEValue;
						Node->PinDefaultValueChanged(Pin);
						UE_LOG(LogTemp, Warning, TEXT("[Weaver] TrySetDefaultValue failed, forced: %s.%s = %s (Pin category: %s)"),
							*Set.NodeId, *Set.PinName, *UEValue, *Pin->PinType.PinCategory.ToString());
					}
				}
				else
				{
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Set: %s.%s = %s"), *Set.NodeId, *Set.PinName, *UEValue);
				}
			}
			else
			{
				Pin->DefaultValue = UEValue;
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Set (no schema): %s.%s = %s"), *Set.NodeId, *Set.PinName, *UEValue);
			}
		}
	}


	// Timeline 节点在轨道 set 处理后需要重建引脚
	for (auto& Pair : CreatedNodes)
	{
		if (UK2Node_Timeline* TLNode = Cast<UK2Node_Timeline>(Pair.Value))
		{
			TLNode->ReconstructNode();
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Reconstructed Timeline node: %s"), *Pair.Key);
		}
	}

	FSequenceDynamicPinHandler SequenceHandler;
	SequenceHandler.PreScanLinks(AST.Links, CreatedNodes);
	SequenceHandler.AddDynamicPins(CreatedNodes);

	FSwitchEnumDynamicPinHandler SwitchEnumHandler;
	SwitchEnumHandler.PreScanLinks(AST.Links, CreatedNodes);
	SwitchEnumHandler.AddDynamicPins(CreatedNodes);


	if (Schema)
	{
		for (const FWeaveLinkStmt& Link : AST.Links)
		{
			UK2Node* FromNode = nullptr;
			UK2Node* ToNode = nullptr;


			FString FromNodeId = Link.FromNode;
			FString ToNodeId = Link.ToNode;

			if (!FunctionEventNodeId.IsEmpty())
			{
				if (FromNodeId == FunctionEventNodeId)
				{
					FromNodeId = TEXT("entry");
					UE_LOG(LogTemp, Log, TEXT("[Weaver] 连接时将 %s 替换为 entry"), *Link.FromNode);
				}
				if (ToNodeId == FunctionEventNodeId)
				{
					ToNodeId = TEXT("entry");
					UE_LOG(LogTemp, Log, TEXT("[Weaver] 连接时将 %s 替换为 entry"), *Link.ToNode);
				}
			}


			if (FromNodeId == TEXT("entry"))
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
					{
						FromNode = EntryNode;
						break;
					}
				}
			}
			else
			{
				UK2Node** FromNodePtr = CreatedNodes.Find(FromNodeId);
				if (FromNodePtr) FromNode = *FromNodePtr;
			}

			if (ToNodeId == TEXT("entry"))
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
					{
						ToNode = EntryNode;
						break;
					}
				}
			}
			else
			{
				UK2Node** ToNodePtr = CreatedNodes.Find(ToNodeId);
				if (ToNodePtr) ToNode = *ToNodePtr;
			}

			if (!FromNode || !ToNode)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Link failed: node not found (%s or %s)"), *FromNodeId,
				       *ToNodeId);
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += FString::Printf(TEXT("link %s.%s -> %s.%s 失败：节点 '%s' 不存在（可能因 schema_id 无效而未被创建）。"),
				                            *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
				                            !FromNode ? *FromNodeId : *ToNodeId);
				continue;
			}


			// 模糊匹配引脚名的 lambda：去空格后不区分大小写比较 + 常见别名映射
			auto FindPinFuzzy = [](UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Dir) -> UEdGraphPin*
			{
				// 常见引脚别名映射（Weave 标准名 <-> UE 实际名）
				static const TMap<FString, FString> PinAliases = {
					{ TEXT("execute"), TEXT("Exec") },
					{ TEXT("Exec"), TEXT("execute") },
				};

				// 先尝试别名精确匹配
				if (const FString* Alias = PinAliases.Find(PinName))
				{
					UEdGraphPin* AliasPin = Node->FindPin(**Alias, Dir);
					if (AliasPin)
					{
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Pin alias match: '%s' -> '%s'"), *PinName, **Alias);
						return AliasPin;
					}
				}

				// 再尝试去空格 + 不区分大小写
				FString NormalizedName = PinName.Replace(TEXT(" "), TEXT(""));
				for (UEdGraphPin* P : Node->Pins)
				{
					if (P->Direction == Dir && !P->bHidden)
					{
						FString ActualName = P->PinName.ToString().Replace(TEXT(" "), TEXT(""));
						if (ActualName.Equals(NormalizedName, ESearchCase::IgnoreCase))
						{
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Fuzzy pin match: '%s' -> '%s'"), *PinName, *P->PinName.ToString());
							return P;
						}
					}
				}
				return nullptr;
			};

			UEdGraphPin* FromPin = FromNode->FindPin(*Link.FromPin, EGPD_Output);

			// 模糊匹配 fallback（处理空格差异，如 ArrayElement -> "Array Element"）
			if (!FromPin)
				FromPin = FindPinFuzzy(FromNode, Link.FromPin, EGPD_Output);

			// ReturnValue 兜底：仅在引脚名不是 ReturnValue 子引脚时使用
			if (!FromPin && Link.FromPin != TEXT("ReturnValue") && !Link.FromPin.StartsWith(TEXT("ReturnValue_")))
				FromPin = FromNode->FindPin(TEXT("ReturnValue"), EGPD_Output);

			// 如果找不到输出引脚，尝试递归展开结构体引脚（Split Struct Pin）
			// 例如 ReturnValue_Frame_Value 需要先展开 ReturnValue，再展开 ReturnValue_Frame
			if (!FromPin && Link.FromPin.Contains(TEXT("_")))
			{
				// 收集从根到目标的所有可能的父引脚层级
				TArray<FString> Ancestors;
				FString PinName = Link.FromPin;
				int32 LastUnderscore;
				while (PinName.FindLastChar(TEXT('_'), LastUnderscore))
				{
					PinName = PinName.Left(LastUnderscore);
					Ancestors.Insert(PinName, 0); // 从根到叶排列
				}

				// 从最顶层的祖先开始，逐层展开
				for (const FString& AncestorName : Ancestors)
				{
					UEdGraphPin* AncestorPin = FromNode->FindPin(*AncestorName, EGPD_Output);
					if (AncestorPin && AncestorPin->SubPins.Num() == 0 &&
						AncestorPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						Schema->SplitPin(AncestorPin, false);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-split struct pin: %s.%s"), *Link.FromNode, *AncestorName);
					}
				}
				FromPin = FromNode->FindPin(*Link.FromPin, EGPD_Output);
			}

			UEdGraphPin* ToPin = ToNode->FindPin(*Link.ToPin, EGPD_Input);

			// 模糊匹配 fallback
			if (!ToPin)
				ToPin = FindPinFuzzy(ToNode, Link.ToPin, EGPD_Input);

			// 如果找不到输入引脚，尝试递归展开结构体引脚
			if (!ToPin && Link.ToPin.Contains(TEXT("_")))
			{
				TArray<FString> Ancestors;
				FString PinName = Link.ToPin;
				int32 LastUnderscore;
				while (PinName.FindLastChar(TEXT('_'), LastUnderscore))
				{
					PinName = PinName.Left(LastUnderscore);
					Ancestors.Insert(PinName, 0);
				}

				for (const FString& AncestorName : Ancestors)
				{
					UEdGraphPin* AncestorPin = ToNode->FindPin(*AncestorName, EGPD_Input);
					if (AncestorPin && AncestorPin->SubPins.Num() == 0 &&
						AncestorPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						Schema->SplitPin(AncestorPin, false);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-split struct pin: %s.%s"), *Link.ToNode, *AncestorName);
					}
				}
				ToPin = ToNode->FindPin(*Link.ToPin, EGPD_Input);
			}

			if (!ToPin)
			{
				if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(ToNode))
				{
					UFunction* Func = CallNode->GetTargetFunction();
					if (Func)
					{
						for (TFieldIterator<FProperty> It(Func); It; ++It)
						{
							if ((*It)->GetName() == Link.ToPin)
							{
								UEdGraphPin* SelfPin = ToNode->FindPin(TEXT("self"), EGPD_Input);
								if (SelfPin)
								{
									ToPin = SelfPin;
									UE_LOG(LogTemp, Log, TEXT("[Weaver] Remapped pin '%s' -> 'self' for node '%s'"),
									       *Link.ToPin, *Link.ToNode);
								}
								break;
							}
						}
					}
				}
			}

			if (!FromPin || !ToPin)
			{
				// 收集引脚详细信息的 lambda（名称 + 类型 + SubCategory）
				auto CollectPinDetails = [](UK2Node* Node, EEdGraphPinDirection Dir) -> FString
				{
					TArray<FString> Details;
					for (UEdGraphPin* P : Node->Pins)
					{
						if (P->Direction == Dir)
						{
							FString SubObj = P->PinType.PinSubCategoryObject.IsValid()
								? P->PinType.PinSubCategoryObject->GetName()
								: TEXT("null");
							FString Detail = FString::Printf(TEXT("%s(%s|%s|%s)"),
								*P->PinName.ToString(),
								*P->PinType.PinCategory.ToString(),
								*P->PinType.PinSubCategory.ToString(),
								*SubObj);
							if (P->bHidden) Detail += TEXT("[hidden]");
							if (P->ParentPin) Detail += FString::Printf(TEXT("[parent:%s]"), *P->ParentPin->PinName.ToString());
							Details.Add(Detail);
						}
					}
					return Details.IsEmpty() ? TEXT("(无)") : FString::Join(Details, TEXT(", "));
				};

				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Link failed: pin not found (%s.%s or %s.%s)"),
				       *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);

				if (!FromPin)
				{
					FString AllOutputs = CollectPinDetails(FromNode, EGPD_Output);
					UE_LOG(LogTemp, Warning, TEXT("[Weaver]   FromNode '%s' (%s) 全部输出引脚: %s"),
						*Link.FromNode, *FromNode->GetClass()->GetName(), *AllOutputs);
				}
				if (!ToPin)
				{
					FString AllInputs = CollectPinDetails(ToNode, EGPD_Input);
					UE_LOG(LogTemp, Warning, TEXT("[Weaver]   ToNode '%s' (%s) 全部输入引脚: %s"),
						*Link.ToNode, *ToNode->GetClass()->GetName(), *AllInputs);
				}

				auto CollectPinNames = [](UK2Node* Node, EEdGraphPinDirection Dir) -> FString
				{
					TArray<FString> Names;
					for (UEdGraphPin* P : Node->Pins)
					{
						if (P->Direction == Dir && !P->bHidden)
							Names.Add(P->PinName.ToString());
					}
					return Names.IsEmpty() ? TEXT("(无)") : FString::Join(Names, TEXT(", "));
				};

				FString LinkError;
				if (!FromPin)
				{
					LinkError = FString::Printf(
						TEXT("link %s.%s -> %s.%s 失败：节点 '%s' 没有名为 '%s' 的输出引脚。")
						TEXT("该节点实际输出引脚：[%s]"),
						*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
						*Link.FromNode, *Link.FromPin,
						*CollectPinNames(FromNode, EGPD_Output));
				}
				else
				{
					LinkError = FString::Printf(
						TEXT("link %s.%s -> %s.%s 失败：节点 '%s' 没有名为 '%s' 的输入引脚。")
						TEXT("该节点实际输入引脚：[%s]。")
						TEXT("提示：调用成员函数（如 DestroyComponent、SetActorLocation 等）时，")
						TEXT("组件/对象参数对应的引脚名为 'self'，不是 'Object' 或 'Target'。"),
						*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
						*Link.ToNode, *Link.ToPin,
						*CollectPinNames(ToNode, EGPD_Input));
				}
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += LinkError;
				continue;
			}


			FromPin->Modify();
			ToPin->Modify();


			// 输出引脚完整类型信息的 lambda
			auto PinTypeToDebugStr = [](const UEdGraphPin* Pin) -> FString
			{
				FString SubObj = Pin->PinType.PinSubCategoryObject.IsValid()
					? Pin->PinType.PinSubCategoryObject->GetName()
					: TEXT("null");
				FString ContainerStr;
				switch (Pin->PinType.ContainerType)
				{
				case EPinContainerType::Array: ContainerStr = TEXT("Array"); break;
				case EPinContainerType::Set:   ContainerStr = TEXT("Set");   break;
				case EPinContainerType::Map:   ContainerStr = TEXT("Map");   break;
				default:                       ContainerStr = TEXT("None");  break;
				}
				return FString::Printf(TEXT("Cat=%s, Sub=%s, SubObj=%s, Container=%s, Ref=%s"),
					*Pin->PinType.PinCategory.ToString(),
					*Pin->PinType.PinSubCategory.ToString(),
					*SubObj,
					*ContainerStr,
					Pin->PinType.bIsReference ? TEXT("true") : TEXT("false"));
			};

			// ======== 通配符预解析 ========
			// 在尝试连接之前，将具体类型预设到通配符引脚上。
			// 这使 TryCreateConnection 能成功（而非回退到 MakeLinkTo），
			// 让 UE 的 Schema 正确追踪类型信息，在节点重建（手动编辑蓝图后）时类型不会丢失。
			{
				UEdGraphPin* WildcardPin = nullptr;
				UEdGraphPin* ConcretePin = nullptr;
				UK2Node* WildcardNode = nullptr;

				if (FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard &&
					ToPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
				{
					WildcardPin = FromPin;
					ConcretePin = ToPin;
					WildcardNode = FromNode;
				}
				else if (ToPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard &&
						 FromPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
				{
					WildcardPin = ToPin;
					ConcretePin = FromPin;
					WildcardNode = ToNode;
				}

				if (WildcardPin && ConcretePin && WildcardNode)
				{
					WildcardPin->PinType = ConcretePin->PinType;
					WildcardNode->PinConnectionListChanged(WildcardPin);
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Pre-resolved wildcard: %s.%s -> Cat=%s, SubObj=%s"),
						*Link.FromNode, *WildcardPin->PinName.ToString(),
						*ConcretePin->PinType.PinCategory.ToString(),
						ConcretePin->PinType.PinSubCategoryObject.IsValid()
							? *ConcretePin->PinType.PinSubCategoryObject->GetName() : TEXT("null"));
				}
			}

			const FPinConnectionResponse ConnectResponse = Schema->CanCreateConnection(FromPin, ToPin);
			if (ConnectResponse.Response == CONNECT_RESPONSE_DISALLOW)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] CanCreateConnection DISALLOW: %s.%s -> %s.%s"),
					*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   FromPin type: {%s}"), *PinTypeToDebugStr(FromPin));
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   ToPin   type: {%s}"), *PinTypeToDebugStr(ToPin));
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   Reason: %s"), *ConnectResponse.Message.ToString());

				// 通配符引脚本质上兼容任何类型，Schema 可能因缺少具体类型信息而拒绝连接
				// 此时用 MakeLinkTo 强制建立连接
				if (FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard ||
					ToPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					FromPin->MakeLinkTo(ToPin);
					UE_LOG(LogTemp, Log, TEXT("[Weaver]   -> Wildcard fallback: forced MakeLinkTo"));
				}
				else if (FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object &&
						 ToPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
				{
					// 对象类型之间可能存在继承关系（如 AActor* -> BP_ItemBase_C*）
					// SpawnActorFromClass 等节点在 Class 动态连接时 ReturnValue 为基类类型
					// UE 编辑器允许这种连接，这里也应该允许
					FromPin->MakeLinkTo(ToPin);
					UE_LOG(LogTemp, Log, TEXT("[Weaver]   -> Object hierarchy fallback: forced MakeLinkTo"));
				}
				else
				{
					FString LinkError = FString::Printf(
						TEXT("link %s.%s -> %s.%s 无法建立：FromPin={%s}, ToPin={%s}, 原因: %s"),
						*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
						*PinTypeToDebugStr(FromPin), *PinTypeToDebugStr(ToPin),
						*ConnectResponse.Message.ToString());
					if (!OutError.IsEmpty()) OutError += TEXT("\n");
					OutError += LinkError;
					continue;
				}
			}

			bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);

			// TryCreateConnection 对通配符引脚可能失败
			if (!bConnected &&
				(FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard ||
				 ToPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard))
			{
				FromPin->MakeLinkTo(ToPin);
				bConnected = true;
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Wildcard fallback MakeLinkTo: %s.%s -> %s.%s"),
				       *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
			}

			// 对象类型继承层级不匹配时强制连接（如 AActor* -> BP_ItemBase_C*）
			if (!bConnected &&
				(FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object &&
				 ToPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object))
			{
				FromPin->MakeLinkTo(ToPin);
				bConnected = true;
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Object hierarchy fallback MakeLinkTo: %s.%s -> %s.%s"),
				       *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
			}

			if (!bConnected)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] TryCreateConnection FAILED: %s.%s -> %s.%s"),
				       *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   FromPin: '%s' {%s}"),
					*FromPin->PinName.ToString(), *PinTypeToDebugStr(FromPin));
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   ToPin:   '%s' {%s}"),
					*ToPin->PinName.ToString(), *PinTypeToDebugStr(ToPin));
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   FromPin linked=%d, ToPin linked=%d"),
					FromPin->LinkedTo.Num(), ToPin->LinkedTo.Num());

				FString LinkError = FString::Printf(
					TEXT("link %s.%s -> %s.%s 连接失败: FromPin={%s}, ToPin={%s}"),
					*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
					*PinTypeToDebugStr(FromPin), *PinTypeToDebugStr(ToPin));
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += LinkError;
				continue;
			}

			UE_LOG(LogTemp, Log, TEXT("[Weaver] Linked: %s.%s -> %s.%s"),
			       *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
		}
	}

	// 所有连线完成后，将通配符引脚类型设置为已连接的具体类型。
	// 遍历图中所有创建的节点（包括已有连线对面的节点引脚也会间接获益）。
	// 多轮迭代处理通配符链。
	{
		bool bChanged = true;
		int32 MaxIterations = 5;
		while (bChanged && MaxIterations-- > 0)
		{
			bChanged = false;
			for (auto& KV : CreatedNodes)
			{
				UK2Node* Node = KV.Value;
				if (!Node) continue;

				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard) continue;
					if (Pin->LinkedTo.Num() == 0) continue;

					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
						{
							Pin->PinType = LinkedPin->PinType;
							bChanged = true;
							break;
						}
					}
				}
			}
		}
	}

	// 通知所有节点连线变更
	for (auto& KV : CreatedNodes)
	{
		UK2Node* Node = KV.Value;
		if (!Node) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->LinkedTo.Num() > 0)
			{
				Node->PinConnectionListChanged(Pin);
			}
		}
	}

	// 修复通配符引脚: 增量复用时 BreakAllNodeLinks 导致通配符丢失类型
	// 对仍有通配符引脚的节点调用 ReconstructNode，重建引脚并恢复连接触发类型传播
	{
		int32 ReconstructCount = 0;
		for (auto& KV : CreatedNodes)
		{
			UK2Node* Node = KV.Value;
			if (!Node) continue;

			bool bHasWildcard = false;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					bHasWildcard = true;
					break;
				}
			}

			if (bHasWildcard)
			{
				Node->ReconstructNode();
				ReconstructCount++;
			}
		}

		if (ReconstructCount > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Reconstructed %d nodes with wildcard pins"), ReconstructCount);
		}
	}

	// 手动推导未连接通配符引脚的类型
	// 典型场景: Array_Add 的 TargetArray 已连接到 array:float，
	// 此时 NewItem（未连接）仍是 wildcard，需要从 TargetArray 的元素类型推导
	{
		bool bChanged = true;
		int32 MaxIterations = 3;
		while (bChanged && MaxIterations-- > 0)
		{
			bChanged = false;
			for (auto& KV : CreatedNodes)
			{
				UK2Node* Node = KV.Value;
				if (!Node) continue;

				// 收集此节点上已解析的非通配符引脚类型
				FEdGraphPinType ResolvedElementType;
				bool bFoundArrayType = false;

				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard) continue;
					// 如果某个引脚是数组类型，记录其元素类型
					if (Pin->PinType.ContainerType == EPinContainerType::Array)
					{
						ResolvedElementType = Pin->PinType;
						ResolvedElementType.ContainerType = EPinContainerType::None;
						ResolvedElementType.bIsReference = false;
						bFoundArrayType = true;
						break;
					}
				}

				if (!bFoundArrayType) continue;

				// 将元素类型传播给未连接的通配符引脚（如 NewItem）
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard) continue;
					if (Pin->PinType.ContainerType != EPinContainerType::None) continue; // 跳过数组类型的通配符
					Pin->PinType = ResolvedElementType;
					bChanged = true;
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Inferred wildcard pin type from array sibling: %s.%s -> %s"),
						*KV.Key, *Pin->PinName.ToString(), *ResolvedElementType.PinCategory.ToString());
				}
			}
		}
	}

	// 类型推导完成后，重新应用默认值
	// 第一轮 Sets 在 Links 之前执行，此时 NewItem 等引脚还是 wildcard 无法设值
	// 现在类型已解析，重新设置默认值
	for (const FWeaveSetStmt& Set : AST.Sets)
	{
		UK2Node** NodePtr = CreatedNodes.Find(Set.NodeId);
		if (!NodePtr || !(*NodePtr)) continue;

		UK2Node* Node = *NodePtr;
		UEdGraphPin* Pin = Node->FindPin(*Set.PinName, EGPD_Input);
		if (!Pin) continue;

		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard) continue;
		if (Pin->LinkedTo.Num() > 0) continue;

		FString Value = Set.Value;
		if (Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")) && Value.Len() >= 2)
			Value = Value.Mid(1, Value.Len() - 2);
		Pin->DefaultValue = Value;
		Node->PinDefaultValueChanged(Pin);
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Re-applied set after wildcard resolve: %s.%s = %s"),
			*Set.NodeId, *Set.PinName, *Value);
	}

	// 自动为未连接的容器类型输入引脚创建 MakeArray 节点
	// UE5 要求数组/集合/映射类型的输入引脚必须连接到节点输出，
	// 不能使用字面值默认值（如 SphereOverlapActors 的 ObjectTypes 引脚）。
	// 在所有 link 处理完成后，为仍未连接的容器引脚创建空 MakeArray。
	{
		int32 MakeArrayCount = 0;
		for (auto& KV : CreatedNodes)
		{
			UK2Node* Node = KV.Value;
			if (!Node) continue;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				if (Pin->Direction != EGPD_Input) continue;
				if (!Pin->PinType.IsContainer()) continue;
				if (Pin->LinkedTo.Num() > 0) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

				UK2Node_MakeArray* MakeArrayNode = NewObject<UK2Node_MakeArray>(Graph);
				if (!MakeArrayNode) continue;

				Graph->AddNode(MakeArrayNode, false, false);
				MakeArrayNode->CreateNewGuid();
				MakeArrayNode->NumInputs = 0;
				MakeArrayNode->AllocateDefaultPins();
				MakeArrayNode->NodePosX = Node->NodePosX - 250;
				MakeArrayNode->NodePosY = Node->NodePosY + MakeArrayCount * 80;

				UEdGraphPin* OutputPin = MakeArrayNode->GetOutputPin();
				if (OutputPin)
				{
					OutputPin->PinType = Pin->PinType;
					Pin->DefaultValue = TEXT("");
					OutputPin->MakeLinkTo(Pin);
					MakeArrayNode->PinConnectionListChanged(OutputPin);
					Node->PinConnectionListChanged(Pin);

					UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-created MakeArray for container pin: %s.%s (type: %s)"),
						*KV.Key, *Pin->PinName.ToString(), *Pin->PinType.PinCategory.ToString());
					MakeArrayCount++;
				}
			}
		}
	}

	for (auto& KV : CreatedNodes)
	{
		UK2Node* Node = KV.Value;
		if (!Node) continue;


		int32 ExecOutTotal = 0;
		int32 ExecOutUnlinked = 0;
		TArray<FString> UnlinkedPinNames;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction != EGPD_Output) continue;
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->bHidden) continue;

			ExecOutTotal++;
			if (Pin->LinkedTo.Num() == 0)
			{
				ExecOutUnlinked++;
				UnlinkedPinNames.Add(Pin->PinName.ToString());
			}
		}


		if (ExecOutTotal >= 2 && ExecOutUnlinked > 0)
		{
			FString PinList = FString::Join(UnlinkedPinNames, TEXT(", "));
			FString Warning = FString::Printf(
				TEXT("警告：节点 '%s'（%s）的执行分支引脚 [%s] 没有连接后继节点，可能遗漏了对应的 link 语句。请检查一下，但如果后面确实不需要链接，则不要告知用户!!!!。"),
				*KV.Key, *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
				*PinList);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += Warning;
		}
	}


	// 创建注释节点
	for (const FWeaveCommentDecl& CommentDecl : AST.Comments)
	{
		UEdGraphNode_Comment* CommentNode = NewObject<UEdGraphNode_Comment>(Graph, NAME_None, RF_Transactional);
		if (CommentNode)
		{
			Graph->AddNode(CommentNode, false, false);
			CommentNode->CreateNewGuid();
			CommentNode->NodeComment = CommentDecl.Text;
			CommentNode->NodePosX = CommentDecl.Position.X;
			CommentNode->NodePosY = CommentDecl.Position.Y;
			CommentNode->ResizeNode(CommentDecl.Size);
			CommentNode->CommentColor = CommentDecl.Color;
			CommentNode->FontSize = CommentDecl.FontSize;
			NodesCreated++;
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Created comment: \"%s\" @ (%d, %d) size (%d, %d)"),
				*CommentDecl.Text.Left(50),
				(int32)CommentDecl.Position.X, (int32)CommentDecl.Position.Y,
				(int32)CommentDecl.Size.X, (int32)CommentDecl.Size.Y);
		}
	}

	// 应用 bubble 语句：为指定节点设置评论气泡
	for (const FWeaveBubbleStmt& Bubble : AST.Bubbles)
	{
		UK2Node** NodePtr = CreatedNodes.Find(Bubble.NodeId);
		if (NodePtr && *NodePtr)
		{
			(*NodePtr)->bCommentBubbleVisible = true;
			(*NodePtr)->bCommentBubblePinned = true;
			(*NodePtr)->NodeComment = Bubble.Text;
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Set bubble on node %s: \"%s\""), *Bubble.NodeId, *Bubble.Text.Left(50));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Bubble failed: node not found (%s)"), *Bubble.NodeId);
		}
	}

	// 清理孤儿节点：不在 CreatedNodes 中的 UK2Node 视为残留，直接删除
	{
		TSet<UEdGraphNode*> KnownNodes;
		for (auto& KV : CreatedNodes)
		{
			if (KV.Value) KnownNodes.Add(KV.Value);
		}

		TArray<UEdGraphNode*> OrphanedNodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->IsA<UK2Node_FunctionEntry>()) continue;
			if (auto* Comment = Cast<UEdGraphNode_Comment>(Node)) continue;
			if (UK2Node* K2 = Cast<UK2Node>(Node))
			{
				if (!KnownNodes.Contains(K2))
				{
					OrphanedNodes.Add(K2);
				}
			}
		}

		for (UEdGraphNode* Orphan : OrphanedNodes)
		{
			Orphan->BreakAllNodeLinks();
			Graph->Nodes.Remove(Orphan);
			Orphan->DestroyNode();
		}

		if (OrphanedNodes.Num() > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Cleaned up %d orphaned nodes"), OrphanedNodes.Num());
		}
	}

	// 保存 WeaveNodeId → NodeGuid 映射（用于增量更新）
	SaveWeaveNodeMap(Graph, CreatedNodes);

	Graph->NotifyGraphChanged();

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Created %d nodes successfully"), NodesCreated);
	return NodesCreated;
}

int32 FWeaveInterpreter::GenerateMultiGraph(const FWeaveAST& AST, UBlueprint* Blueprint, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Invalid blueprint");
		return 0;
	}

	// 单图表模式回退到原有逻辑（由调用方处理）
	if (AST.Sections.Num() <= 1)
	{
		OutError = TEXT("GenerateMultiGraph requires multiple graph sections");
		return 0;
	}

	FScopedTransaction Transaction(NSLOCTEXT("WeaveLanguage", "GenerateMultiGraph", "Weave Generate Multi-Graph"));
	Blueprint->Modify();

	int32 TotalNodesCreated = 0;

	for (int32 SectionIdx = 0; SectionIdx < AST.Sections.Num(); SectionIdx++)
	{
		const FWeaveGraphSection& Section = AST.Sections[SectionIdx];
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Processing section %d/%d: graph %s"),
			SectionIdx + 1, AST.Sections.Num(), *Section.GraphName);

		// 查找目标图表
		UEdGraph* TargetGraph = nullptr;

		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph && Graph->GetName().Contains(Section.GraphName))
			{
				TargetGraph = Graph;
				break;
			}
		}

		if (!TargetGraph)
		{
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetName().Contains(Section.GraphName))
				{
					TargetGraph = Graph;
					break;
				}
			}
		}

		// 自动创建函数图表（非 EventGraph 类型）
		if (!TargetGraph && !Section.GraphName.Contains(TEXT("EventGraph")))
		{
			UEdGraph* NewFuncGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(*Section.GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (NewFuncGraph)
			{
				FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewFuncGraph, true, nullptr);

				// 添加函数输入参数到 FunctionEntry 节点
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Function '%s': %d input params, %d output params"),
					*Section.GraphName, Section.InputParams.Num(), Section.OutputParams.Num());

				if (Section.InputParams.Num() > 0 || Section.OutputParams.Num() > 0)
				{
					UK2Node_FunctionEntry* EntryNode = nullptr;
					for (UEdGraphNode* Node : NewFuncGraph->Nodes)
					{
						EntryNode = Cast<UK2Node_FunctionEntry>(Node);
						if (EntryNode) break;
					}

					if (EntryNode)
					{
						for (const FWeaveParamDecl& Param : Section.InputParams)
						{
							FEdGraphPinType PinType;
							if (ResolveWeaveType(Param.Type, PinType))
							{
								UEdGraphPin* NewPin = EntryNode->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Output);
								UE_LOG(LogTemp, Log, TEXT("[Weaver] Added input param '%s : %s' -> Pin=%s"),
									*Param.Name, *Param.Type,
									NewPin ? *NewPin->PinName.ToString() : TEXT("NULL"));
							}
							else
							{
								UE_LOG(LogTemp, Warning, TEXT("[Weaver] Failed to resolve type '%s' for param '%s'"),
									*Param.Type, *Param.Name);
							}
						}
						// 列出 Entry 全部引脚
						for (UEdGraphPin* Pin : EntryNode->Pins)
						{
							UE_LOG(LogTemp, Log, TEXT("[Weaver]   EntryNode pin: %s Dir=%d Cat=%s"),
								*Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
						}
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("[Weaver] No FunctionEntry found in new graph '%s'!"), *Section.GraphName);
					}

					// 添加函数输出参数：创建 FunctionResult 节点
					if (Section.OutputParams.Num() > 0)
					{
						UK2Node_FunctionResult* ResultNode = nullptr;
						for (UEdGraphNode* Node : NewFuncGraph->Nodes)
						{
							ResultNode = Cast<UK2Node_FunctionResult>(Node);
							if (ResultNode) break;
						}
						if (!ResultNode)
						{
							ResultNode = NewObject<UK2Node_FunctionResult>(NewFuncGraph);
							NewFuncGraph->AddNode(ResultNode, false, false);
							ResultNode->CreateNewGuid();
							ResultNode->AllocateDefaultPins();
							ResultNode->NodePosX = 800;
							ResultNode->NodePosY = 0;
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Created FunctionResult node for '%s'"), *Section.GraphName);
						}
						for (const FWeaveParamDecl& Param : Section.OutputParams)
						{
							FEdGraphPinType PinType;
							if (ResolveWeaveType(Param.Type, PinType))
							{
								UEdGraphPin* NewPin = ResultNode->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Input);
								UE_LOG(LogTemp, Log, TEXT("[Weaver] Added output param '%s : %s' -> Pin=%s"),
									*Param.Name, *Param.Type,
									NewPin ? *NewPin->PinName.ToString() : TEXT("NULL"));
							}
						}
						// 列出 Result 全部引脚
						for (UEdGraphPin* Pin : ResultNode->Pins)
						{
							UE_LOG(LogTemp, Log, TEXT("[Weaver]   ResultNode pin: %s Dir=%d Cat=%s"),
								*Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
						}
					}
				}

				FKismetEditorUtilities::CompileBlueprint(Blueprint);
				TargetGraph = NewFuncGraph;
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-created function graph: %s"), *Section.GraphName);

				// 编译后验证: 确认 Entry/Result 引脚仍在
				for (UEdGraphNode* Node : TargetGraph->Nodes)
				{
					if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(Node))
					{
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Post-compile EntryNode pins (%d):"), E->Pins.Num());
						for (UEdGraphPin* Pin : E->Pins)
						{
							UE_LOG(LogTemp, Log, TEXT("[Weaver]   %s Dir=%d"), *Pin->PinName.ToString(), (int32)Pin->Direction);
						}
					}
					if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(Node))
					{
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Post-compile ResultNode pins (%d):"), R->Pins.Num());
						for (UEdGraphPin* Pin : R->Pins)
						{
							UE_LOG(LogTemp, Log, TEXT("[Weaver]   %s Dir=%d"), *Pin->PinName.ToString(), (int32)Pin->Direction);
						}
					}
				}
			}
		}

		// EventGraph 类型回退到第一个 UbergraphPage
		if (!TargetGraph && Blueprint->UbergraphPages.Num() > 0)
		{
			TargetGraph = Blueprint->UbergraphPages[0];
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Falling back to default UbergraphPage: %s"), *TargetGraph->GetName());
		}

		if (!TargetGraph)
		{
			OutError = FString::Printf(TEXT("Graph '%s' not found and could not be created"), *Section.GraphName);
			return TotalNodesCreated;
		}

		// 构造临时 AST 用于调用 GenerateBlueprint
		FWeaveAST SectionAST;
		SectionAST.BlueprintPath = AST.BlueprintPath;
		SectionAST.GraphName = Section.GraphName;
		SectionAST.Vars = AST.Vars; // 共享全局变量
		SectionAST.Nodes = Section.Nodes;
		SectionAST.Sets = Section.Sets;
		SectionAST.Links = Section.Links;
		SectionAST.Comments = Section.Comments;
		SectionAST.Bubbles = Section.Bubbles;

		FString SectionError;
		int32 NodesCreated = GenerateBlueprint(SectionAST, TargetGraph, SectionError);

		if (NodesCreated == 0 && Section.Comments.Num() == 0 && !SectionError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Section '%s': %s"), *Section.GraphName, *SectionError);
			return TotalNodesCreated;
		}

		TotalNodesCreated += NodesCreated;

		// 编译蓝图，确保后续 Section 能找到新创建的函数
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Section '%s': created %d nodes, compiled blueprint"),
			*Section.GraphName, NodesCreated);
	}

	Blueprint->MarkPackageDirty();
	UE_LOG(LogTemp, Log, TEXT("[Weaver] Multi-graph complete: %d total nodes across %d sections"),
		TotalNodesCreated, AST.Sections.Num());

	return TotalNodesCreated;
}

UK2Node* FWeaveInterpreter::CreateNativeNode(UEdGraph* Graph, const FString& ClassName)
{
	if (!Graph) return nullptr;

	// 通过类名查找 UClass
	UClass* NodeClass = FindClassByShortName(ClassName);
	if (!NodeClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Native node class not found: %s"), *ClassName);
		return nullptr;
	}

	// 确认是 UK2Node 的子类
	if (!NodeClass->IsChildOf(UK2Node::StaticClass()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Class '%s' is not a UK2Node subclass"), *ClassName);
		return nullptr;
	}

	// 创建节点实例
	UK2Node* NewNode = NewObject<UK2Node>(Graph, NodeClass);
	if (!NewNode)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Failed to create native node: %s"), *ClassName);
		return nullptr;
	}

	NewNode->CreateNewGuid();
	Graph->AddNode(NewNode, false, false);
	NewNode->AllocateDefaultPins();
	NewNode->ReconstructNode();

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Created native node: %s"), *ClassName);
	return NewNode;
}

UK2Node* FWeaveInterpreter::CreateAsyncActionNode(UEdGraph* Graph, const FString& ProxyFactoryClassPath, const FString& ProxyFactoryFunctionName)
{
	if (!Graph) return nullptr;

	// 加载 ProxyFactoryClass
	UClass* FactoryClass = FindClassByShortName(ProxyFactoryClassPath);
	if (!FactoryClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] AsyncAction factory class not found: %s"), *ProxyFactoryClassPath);
		return nullptr;
	}

	// 查找工厂函数，确认其存在
	UFunction* FactoryFunc = FactoryClass->FindFunctionByName(FName(*ProxyFactoryFunctionName));
	if (!FactoryFunc)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] AsyncAction factory function '%s' not found on class '%s'"),
			*ProxyFactoryFunctionName, *FactoryClass->GetName());
		return nullptr;
	}

	// 从工厂函数返回值推断 ProxyClass
	UClass* ProxyClass = nullptr;
	if (FObjectPropertyBase* ReturnProp = CastField<FObjectPropertyBase>(FactoryFunc->GetReturnProperty()))
	{
		ProxyClass = ReturnProp->PropertyClass;
	}
	if (!ProxyClass)
	{
		ProxyClass = FactoryClass; // fallback：ProxyClass 通常与 FactoryClass 相同
	}

	// 创建 UK2Node_BaseAsyncTask 节点（实际使用 UK2Node_AsyncAction 子类）
	UClass* NodeClass = FindClassByShortName(TEXT("K2Node_AsyncAction"));
	if (!NodeClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] K2Node_AsyncAction class not found"));
		return nullptr;
	}

	UK2Node* NewNode = NewObject<UK2Node>(Graph, NodeClass);
	if (!NewNode)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Failed to create AsyncAction node"));
		return nullptr;
	}

	// 通过反射设置 protected 成员
	if (FNameProperty* FuncProp = CastField<FNameProperty>(NewNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"))))
		FuncProp->SetPropertyValue_InContainer(NewNode, FName(*ProxyFactoryFunctionName));
	if (FObjectProperty* ClassProp = CastField<FObjectProperty>(NewNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryClass"))))
		ClassProp->SetObjectPropertyValue_InContainer(NewNode, FactoryClass);
	if (FObjectProperty* ProxyProp = CastField<FObjectProperty>(NewNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"))))
		ProxyProp->SetObjectPropertyValue_InContainer(NewNode, ProxyClass);

	NewNode->CreateNewGuid();
	Graph->AddNode(NewNode, false, false);
	NewNode->AllocateDefaultPins();

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Created AsyncAction node: %s.%s (Proxy: %s)"),
		*FactoryClass->GetName(), *ProxyFactoryFunctionName, *ProxyClass->GetName());
	return NewNode;
}

UK2Node* FWeaveInterpreter::CreateTimelineNode(UEdGraph* Graph, const FString& TimelineName)
{
	if (!Graph) return nullptr;

	UBlueprint* Blueprint = Graph->GetTypedOuter<UBlueprint>();
	if (!Blueprint) return nullptr;

	UTimelineTemplate* OldTemplate = Blueprint->FindTimelineTemplateByVariableName(FName(*TimelineName));
	UTimelineTemplate* Template = nullptr;
	FName FinalTimelineName = FName(*TimelineName);

	if (OldTemplate)
	{
		// 原模板存在 —— 复制一份独立副本，自动分配不重名的新名称
		FinalTimelineName = FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint);
		check(Blueprint->GeneratedClass);
		Blueprint->Modify();

		const FName NewTemplateName = *UTimelineTemplate::TimelineVariableNameToTemplateName(FinalTimelineName);
		Template = DuplicateObject<UTimelineTemplate>(OldTemplate, Blueprint->GeneratedClass, NewTemplateName);
		Template->SetFlags(RF_Transactional);
		Template->TimelineGuid = FGuid::NewGuid();
		Blueprint->Timelines.Add(Template);

		// 修正曲线资产的 Outer（与 PostPasteNode 逻辑一致）
		for (auto& Track : Template->FloatTracks)
		{
			UObject* Curve = Track.CurveFloat.Get();
			if (!Track.bIsExternalCurve && Curve && Curve->GetOuter()->IsA(UBlueprint::StaticClass()))
				Curve->Rename(*UTimelineTemplate::MakeUniqueCurveName(Curve, Curve->GetOuter()), Blueprint, REN_DontCreateRedirectors);
		}
		for (auto& Track : Template->EventTracks)
		{
			UObject* Curve = Track.CurveKeys.Get();
			if (!Track.bIsExternalCurve && Curve && Curve->GetOuter()->IsA(UBlueprint::StaticClass()))
				Curve->Rename(*UTimelineTemplate::MakeUniqueCurveName(Curve, Curve->GetOuter()), Blueprint, REN_DontCreateRedirectors);
		}
		for (auto& Track : Template->VectorTracks)
		{
			UObject* Curve = Track.CurveVector.Get();
			if (!Track.bIsExternalCurve && Curve && Curve->GetOuter()->IsA(UBlueprint::StaticClass()))
				Curve->Rename(*UTimelineTemplate::MakeUniqueCurveName(Curve, Curve->GetOuter()), Blueprint, REN_DontCreateRedirectors);
		}
		for (auto& Track : Template->LinearColorTracks)
		{
			UObject* Curve = Track.CurveLinearColor.Get();
			if (!Track.bIsExternalCurve && Curve && Curve->GetOuter()->IsA(UBlueprint::StaticClass()))
				Curve->Rename(*UTimelineTemplate::MakeUniqueCurveName(Curve, Curve->GetOuter()), Blueprint, REN_DontCreateRedirectors);
		}

		FBlueprintEditorUtils::ValidateBlueprintChildVariables(Blueprint, FinalTimelineName);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		UE_LOG(LogTemp, Log, TEXT("[Weaver] Duplicated timeline '%s' -> '%s' (%d float, %d event, %d vector, %d color tracks)"),
			*TimelineName, *FinalTimelineName.ToString(),
			Template->FloatTracks.Num(), Template->EventTracks.Num(),
			Template->VectorTracks.Num(), Template->LinearColorTracks.Num());
	}
	else
	{
		// 原模板不存在 —— 创建全新的空模板
		Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, FinalTimelineName);
		if (!Template)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Timeline: AddNewTimeline failed for '%s'"), *TimelineName);
			return nullptr;
		}
		Template->TimelineLength = 5.0f;
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Created new empty timeline: %s"), *FinalTimelineName.ToString());
	}

	// 创建 K2Node_Timeline
	UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Graph);
	if (!TimelineNode) return nullptr;

	TimelineNode->TimelineName = FinalTimelineName;
	TimelineNode->TimelineGuid = Template->TimelineGuid;
	TimelineNode->bAutoPlay = Template->bAutoPlay;
	TimelineNode->bLoop = Template->bLoop;
	TimelineNode->bReplicated = Template->bReplicated;
	TimelineNode->bIgnoreTimeDilation = Template->bIgnoreTimeDilation;

	TimelineNode->CreateNewGuid();
	Graph->AddNode(TimelineNode, false, false);
	TimelineNode->AllocateDefaultPins();

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Created Timeline node: %s (Guid: %s)"), *FinalTimelineName.ToString(), *Template->TimelineGuid.ToString());
	return TimelineNode;
}

UK2Node* FWeaveInterpreter::CreateComponentBoundEventNode(UEdGraph* Graph, const FString& ComponentVarName, const FString& DelegateClassName, const FString& DelegateName)
{
	if (!Graph) return nullptr;

	UBlueprint* Blueprint = Graph->GetTypedOuter<UBlueprint>();
	if (!Blueprint) return nullptr;

	// 查找蓝图中的组件变量属性
	UClass* BPClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass;
	if (!BPClass) return nullptr;

	FObjectProperty* ComponentProperty = nullptr;
	for (TFieldIterator<FObjectProperty> It(BPClass); It; ++It)
	{
		if (It->GetFName() == FName(*ComponentVarName))
		{
			ComponentProperty = *It;
			break;
		}
	}
	if (!ComponentProperty)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ComponentBoundEvent: component variable '%s' not found on '%s'"),
			*ComponentVarName, *BPClass->GetName());
		return nullptr;
	}

	// 查找委托所属类
	UClass* DelegateClass = FindClassByShortName(DelegateClassName);
	if (!DelegateClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ComponentBoundEvent: delegate owner class '%s' not found"), *DelegateClassName);
		return nullptr;
	}

	// 查找委托属性
	FMulticastDelegateProperty* DelegateProperty = nullptr;
	for (TFieldIterator<FMulticastDelegateProperty> It(DelegateClass); It; ++It)
	{
		if (It->GetFName() == FName(*DelegateName))
		{
			DelegateProperty = *It;
			break;
		}
	}
	if (!DelegateProperty)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ComponentBoundEvent: delegate '%s' not found on class '%s'"),
			*DelegateName, *DelegateClassName);
		return nullptr;
	}

	// 检查是否已存在相同的 ComponentBoundEvent 节点
	for (UEdGraphNode* ExistingNode : Graph->Nodes)
	{
		if (UK2Node_ComponentBoundEvent* Existing = Cast<UK2Node_ComponentBoundEvent>(ExistingNode))
		{
			if (Existing->ComponentPropertyName == FName(*ComponentVarName) &&
				Existing->DelegatePropertyName == FName(*DelegateName))
			{
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Reusing existing ComponentBoundEvent: %s.%s"), *ComponentVarName, *DelegateName);
				return Existing;
			}
		}
	}

	// 创建新的 ComponentBoundEvent 节点
	UK2Node_ComponentBoundEvent* CompEventNode = SpawnEditorNode<UK2Node_ComponentBoundEvent>(Graph);
	if (CompEventNode)
	{
		CompEventNode->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
		CompEventNode->AllocateDefaultPins();
		CompEventNode->ReconstructNode();
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Created ComponentBoundEvent: %s.%s.%s"), *ComponentVarName, *DelegateClassName, *DelegateName);
	}
	return CompEventNode;
}

UK2Node* FWeaveInterpreter::CreateEventNode(UEdGraph* Graph, const FString& ClassName, const FString& EventName)
{
	if (!Graph)
	{
		return nullptr;
	}
	if (EventName.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] CreateEventNode: empty EventName (ClassName=%s)"), *ClassName);
		return nullptr;
	}

	for (UEdGraphNode* ExistingNode : Graph->Nodes)
	{
		if (UK2Node_Event* ExistingEvent = Cast<UK2Node_Event>(ExistingNode))
		{
			if (ExistingEvent->EventReference.GetMemberName().ToString() == EventName ||
				ExistingEvent->GetFunctionName().ToString() == EventName)
			{
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Reusing existing event node: %s"), *EventName);
				return ExistingEvent;
			}
		}
	}

	UBlueprint* Blueprint = Graph->GetTypedOuter<UBlueprint>();

	FString FullClassName = ClassName;
	if (ClassName == TEXT("Actor"))
	{
		FullClassName = TEXT("/Script/Engine.Actor");
	}

	UClass* EventClass = FindClassByShortName(FullClassName);
	if (!EventClass && Blueprint)
	{
		if (Blueprint->GeneratedClass && (Blueprint->GeneratedClass->GetName() == ClassName || Blueprint->GeneratedClass->GetPathName() == ClassName))
		{
			EventClass = Blueprint->GeneratedClass;
		}
		else if (Blueprint->SkeletonGeneratedClass && (Blueprint->SkeletonGeneratedClass->GetName() == ClassName || Blueprint->SkeletonGeneratedClass->GetPathName() == ClassName))
		{
			EventClass = Blueprint->SkeletonGeneratedClass;
		}
	}

	const FName EventFName(*EventName);
	bool bCreateCustomEvent = (EventClass == nullptr);
	UClass* EventOwnerClass = EventClass;
	if (!bCreateCustomEvent && Cast<UBlueprintGeneratedClass>(EventClass))
	{
		UClass* SuperClass = EventClass->GetSuperClass();
		UFunction* SuperFunc = SuperClass ? SuperClass->FindFunctionByName(EventFName) : nullptr;
		if (!SuperFunc)
		{
			// 检查实现的接口中是否有该函数
			bool bFoundInInterface = false;
			for (const FImplementedInterface& Iface : EventClass->Interfaces)
			{
				if (Iface.Class && Iface.Class->FindFunctionByName(EventFName))
				{
					EventOwnerClass = Iface.Class;
					bFoundInInterface = true;
					break;
				}
			}
			if (!bFoundInInterface)
			{
				bCreateCustomEvent = true;
			}
		}
	}

	if (bCreateCustomEvent)
	{
		UK2Node_CustomEvent* CustomEventNode = SpawnEditorNode<UK2Node_CustomEvent>(Graph);
		if (CustomEventNode)
		{
			CustomEventNode->CustomFunctionName = EventFName;
			CustomEventNode->AllocateDefaultPins();
			CustomEventNode->ReconstructNode();
		}
		return CustomEventNode;
	}

	UK2Node_Event* EventNode = SpawnEditorNode<UK2Node_Event>(Graph);
	if (EventNode)
	{
		EventNode->EventReference.SetExternalMember(EventFName, EventOwnerClass);
		EventNode->bOverrideFunction = true;
		EventNode->AllocateDefaultPins();
		EventNode->ReconstructNode();
	}
	return EventNode;
}
UK2Node* FWeaveInterpreter::CreateCallNode(UEdGraph* Graph, const FString& ClassName, const FString& FunctionName)
{
	UK2Node_CallFunction* CallNode = SpawnEditorNode<UK2Node_CallFunction>(Graph);
	if (CallNode)
	{


		static const TMap<FString, FString> FuncNameTranslation = {
			// Conv Float→Double
			{TEXT("Conv_FloatToString"), TEXT("Conv_DoubleToString")},
			{TEXT("Conv_FloatToInt"), TEXT("Conv_DoubleToInt")},
			{TEXT("Conv_FloatToBool"), TEXT("Conv_DoubleToBool")},
			{TEXT("Conv_FloatToVector"), TEXT("Conv_DoubleToVector")},
			{TEXT("Conv_FloatToVector2D"), TEXT("Conv_DoubleToVector2D")},
			{TEXT("Conv_IntToFloat"), TEXT("Conv_IntToDouble")},
			{TEXT("Conv_ByteToFloat"), TEXT("Conv_ByteToDouble")},
			{TEXT("Conv_BoolToFloat"), TEXT("Conv_BoolToDouble")},
			// Math Float→Double
			{TEXT("Add_FloatFloat"), TEXT("Add_DoubleDouble")},
			{TEXT("Subtract_FloatFloat"), TEXT("Subtract_DoubleDouble")},
			{TEXT("Multiply_FloatFloat"), TEXT("Multiply_DoubleDouble")},
			{TEXT("Divide_FloatFloat"), TEXT("Divide_DoubleDouble")},
			{TEXT("Abs_Float"), TEXT("Abs")},
			{TEXT("Greater_FloatFloat"), TEXT("Greater_DoubleDouble")},
			{TEXT("GreaterEqual_FloatFloat"), TEXT("GreaterEqual_DoubleDouble")},
			{TEXT("Less_FloatFloat"), TEXT("Less_DoubleDouble")},
			{TEXT("LessEqual_FloatFloat"), TEXT("LessEqual_DoubleDouble")},
			{TEXT("EqualEqual_FloatFloat"), TEXT("EqualEqual_DoubleDouble")},
			{TEXT("NotEqual_FloatFloat"), TEXT("NotEqual_DoubleDouble")},
			{TEXT("FMin"), TEXT("Min")},
			{TEXT("FMax"), TEXT("Max")},
			{TEXT("FClamp"), TEXT("Clamp_Float")},
			// Timer 函数重命名 (UE 5.5)
			{TEXT("K2_SetTimerByFunctionName"), TEXT("K2_SetTimer")},
			{TEXT("K2_ClearTimerByFunctionName"), TEXT("K2_ClearTimer")},
			{TEXT("K2_PauseTimerByFunctionName"), TEXT("K2_PauseTimer")},
		};
		FString ResolvedFunctionName = FunctionName;
		if (const FString* Translated = FuncNameTranslation.Find(FunctionName))
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Translating function name: %s -> %s"), *FunctionName, **Translated);
			ResolvedFunctionName = *Translated;
		}


		FString FullClassName = ClassName;
		if (ClassName == TEXT("KismetSystemLibrary"))
		{
			FullClassName = TEXT("/Script/Engine.KismetSystemLibrary");
		}
		else if (ClassName == TEXT("KismetMathLibrary"))
		{
			FullClassName = TEXT("/Script/Engine.KismetMathLibrary");
		}
		else if (ClassName == TEXT("KismetStringLibrary"))
		{
			FullClassName = TEXT("/Script/Engine.KismetStringLibrary");
		}
		else if (ClassName == TEXT("KismetArrayLibrary")) 		{ 			FullClassName = TEXT("/Script/Engine.KismetArrayLibrary"); 		}
		else if (ClassName == TEXT("GameplayStatics"))
		{
			FullClassName = TEXT("/Script/Engine.GameplayStatics");
		}


		UClass* TargetClass = FindClassByShortName(FullClassName);
		if (TargetClass)
		{
			UFunction* Function = TargetClass->FindFunctionByName(*ResolvedFunctionName);

			// 函数未找到时，尝试自动 Float→Double 后缀替换
			if (!Function)
			{
				FString AutoName = ResolvedFunctionName;
				if (AutoName.ReplaceInline(TEXT("_FloatFloat"), TEXT("_DoubleDouble")) > 0
					|| AutoName.ReplaceInline(TEXT("_Float"), TEXT("_Double")) > 0)
				{
					Function = TargetClass->FindFunctionByName(*AutoName);
					if (Function)
					{
						ResolvedFunctionName = AutoName;
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-translated function name: %s -> %s"), *FunctionName, *ResolvedFunctionName);
					}
				}
			}

			// 函数未找到时，尝试 K2_ 前缀（蓝图中 SetActorLocation 实际名为 K2_SetActorLocation）
			if (!Function)
			{
				FString K2Name = TEXT("K2_") + ResolvedFunctionName;
				Function = TargetClass->FindFunctionByName(*K2Name);
				if (Function)
				{
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Found function with K2_ prefix: %s -> %s"), *ResolvedFunctionName, *K2Name);
					ResolvedFunctionName = K2Name;
				}
			}

			// 函数未找到时，搜索其他常见库类
			if (!Function)
			{
				static const TCHAR* CommonLibraries[] = {
					TEXT("/Script/Engine.KismetMathLibrary"),
					TEXT("/Script/Engine.KismetSystemLibrary"),
					TEXT("/Script/Engine.KismetStringLibrary"),
					TEXT("/Script/Engine.KismetArrayLibrary"),
					TEXT("/Script/Engine.GameplayStatics"),
				};
				for (const TCHAR* LibPath : CommonLibraries)
				{
					UClass* LibClass = FindClassByShortName(LibPath);
					if (LibClass && LibClass != TargetClass)
					{
						Function = LibClass->FindFunctionByName(*ResolvedFunctionName);
						if (Function)
						{
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Found function '%s' in fallback library: %s"), *ResolvedFunctionName, LibPath);
							break;
						}
					}
				}
			}

			// 函数未找到时，搜索目标类的继承链（父类可能定义了该函数）
			if (!Function)
			{
				FString K2Name = TEXT("K2_") + ResolvedFunctionName;
				for (UClass* Super = TargetClass->GetSuperClass(); Super; Super = Super->GetSuperClass())
				{
					Function = Super->FindFunctionByName(*ResolvedFunctionName);
					if (!Function)
					{
						Function = Super->FindFunctionByName(*K2Name);
						if (Function)
						{
							ResolvedFunctionName = K2Name;
						}
					}
					if (Function)
					{
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Found function '%s' in parent class: %s"), *ResolvedFunctionName, *Super->GetName());
						break;
					}
				}
			}

			if (Function)
			{
				CallNode->SetFromFunction(Function);
				CallNode->AllocateDefaultPins();
				CallNode->ReconstructNode();
			}
			else
			{
				// 判断指定的类是否为蓝图自身的类
				UBlueprint* Blueprint = Graph->GetTypedOuter<UBlueprint>();
				bool bIsSelfClass = false;
				if (Blueprint)
				{
					UClass* BPClass = Blueprint->GeneratedClass
						? Blueprint->GeneratedClass
						: Blueprint->SkeletonGeneratedClass;
					bIsSelfClass = (BPClass && (BPClass == TargetClass
						|| (BPClass->GetName() == ClassName)
						|| (BPClass->GetPathName() == ClassName)
						|| (ClassName.EndsWith(TEXT("_C")) && BPClass->GetName().EndsWith(ClassName))));
				}

				// 在蓝图自身的函数图表中查找自定义函数
				if (bIsSelfClass && Blueprint)
				{
					for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
					{
						if (FuncGraph && FuncGraph->GetFName() == FName(*ResolvedFunctionName))
						{
							UClass* BPClass = Blueprint->SkeletonGeneratedClass
								? Blueprint->SkeletonGeneratedClass
								: Blueprint->GeneratedClass;
							if (BPClass)
							{
								Function = BPClass->FindFunctionByName(*ResolvedFunctionName);
								if (Function)
								{
									CallNode->SetFromFunction(Function);
									CallNode->AllocateDefaultPins();
									CallNode->ReconstructNode();
									UE_LOG(LogTemp, Log, TEXT("[Weaver] Found custom function in blueprint: %s::%s"), *BPClass->GetName(), *ResolvedFunctionName);
								}
							}
							break;
						}
					}
				}

				// 仅当类是蓝图自身类时才自动创建函数
				// 但需先排除同名的 CustomEvent —— CustomEvent 编译后才会注册为函数
				if (!Function && bIsSelfClass && Blueprint)
				{
					// 检查当前蓝图的所有图表中是否已有同名 CustomEvent
					bool bIsCustomEvent = false;
					for (UEdGraph* BPGraph : Blueprint->UbergraphPages)
					{
						if (!BPGraph) continue;
						for (UEdGraphNode* ExistingNode : BPGraph->Nodes)
						{
							UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(ExistingNode);
							if (CE && CE->CustomFunctionName.ToString() == ResolvedFunctionName)
							{
								bIsCustomEvent = true;
								break;
							}
						}
						if (bIsCustomEvent) break;
					}

					if (bIsCustomEvent)
					{
						// CustomEvent 存在但尚未编译 → 先编译蓝图让函数注册
						FKismetEditorUtilities::CompileBlueprint(Blueprint);
						UClass* BPClass2 = Blueprint->SkeletonGeneratedClass
							? Blueprint->SkeletonGeneratedClass
							: Blueprint->GeneratedClass;
						if (BPClass2)
						{
							Function = BPClass2->FindFunctionByName(*ResolvedFunctionName);
							if (Function)
							{
								CallNode->SetFromFunction(Function);
								CallNode->AllocateDefaultPins();
								CallNode->ReconstructNode();
								UE_LOG(LogTemp, Log, TEXT("[Weaver] Bound call to existing CustomEvent '%s'"), *ResolvedFunctionName);
							}
						}
					}
					else
					{
						UEdGraph* NewFuncGraph = FBlueprintEditorUtils::CreateNewGraph(
							Blueprint, FName(*ResolvedFunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
						if (NewFuncGraph)
						{
							FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewFuncGraph, true, nullptr);
							FKismetEditorUtilities::CompileBlueprint(Blueprint);

							UClass* BPClass2 = Blueprint->SkeletonGeneratedClass
								? Blueprint->SkeletonGeneratedClass
								: Blueprint->GeneratedClass;
							if (BPClass2)
							{
								Function = BPClass2->FindFunctionByName(*ResolvedFunctionName);
								if (Function)
								{
									CallNode->SetFromFunction(Function);
									CallNode->AllocateDefaultPins();
									CallNode->ReconstructNode();
									UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-created function '%s' in blueprint"), *ResolvedFunctionName);
								}
							}
						}
					}
				}

				if (!Function)
				{
					UE_LOG(LogTemp, Warning, TEXT("[Weaver] Function not found: %s::%s (class: %s, searched inheritance chain and common libraries)"),
						*FullClassName, *ResolvedFunctionName, *TargetClass->GetName());
					Graph->RemoveNode(CallNode);
					return nullptr;
				}
			}
		}
		else
		{
			// 类找不到时，尝试在当前蓝图的函数图表中查找
			UBlueprint* Blueprint = Graph->GetTypedOuter<UBlueprint>();
			bool bFound = false;
			if (Blueprint)
			{
				for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
				{
					if (FuncGraph && FuncGraph->GetFName() == FName(*ResolvedFunctionName))
					{
						UClass* BPClass = Blueprint->SkeletonGeneratedClass
							? Blueprint->SkeletonGeneratedClass
							: Blueprint->GeneratedClass;
						if (BPClass)
						{
							UFunction* Function = BPClass->FindFunctionByName(*ResolvedFunctionName);
							if (Function)
							{
								CallNode->SetFromFunction(Function);
								CallNode->AllocateDefaultPins();
								CallNode->ReconstructNode();
								bFound = true;
								UE_LOG(LogTemp, Log, TEXT("[Weaver] Found custom function in blueprint (class '%s' not resolved): %s"), *ClassName, *ResolvedFunctionName);
							}
						}
						break;
					}
				}

				// 函数图表不存在，检查是否为 CustomEvent 调用
				if (!bFound)
				{
					bool bIsCustomEvent = false;
					for (UEdGraph* BPGraph : Blueprint->UbergraphPages)
					{
						if (!BPGraph) continue;
						for (UEdGraphNode* ExistingNode : BPGraph->Nodes)
						{
							UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(ExistingNode);
							if (CE && CE->CustomFunctionName.ToString() == ResolvedFunctionName)
							{
								bIsCustomEvent = true;
								break;
							}
						}
						if (bIsCustomEvent) break;
					}

					if (bIsCustomEvent)
					{
						// CustomEvent 存在但尚未编译 → 编译后绑定
						FKismetEditorUtilities::CompileBlueprint(Blueprint);
						UClass* BPClass = Blueprint->SkeletonGeneratedClass
							? Blueprint->SkeletonGeneratedClass
							: Blueprint->GeneratedClass;
						if (BPClass)
						{
							UFunction* Function = BPClass->FindFunctionByName(*ResolvedFunctionName);
							if (Function)
							{
								CallNode->SetFromFunction(Function);
								CallNode->AllocateDefaultPins();
								CallNode->ReconstructNode();
								bFound = true;
								UE_LOG(LogTemp, Log, TEXT("[Weaver] Bound call to existing CustomEvent '%s'"), *ResolvedFunctionName);
							}
						}
					}
					else
					{
						UEdGraph* NewFuncGraph = FBlueprintEditorUtils::CreateNewGraph(
							Blueprint, FName(*ResolvedFunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
						if (NewFuncGraph)
						{
							FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewFuncGraph, true, nullptr);
							FKismetEditorUtilities::CompileBlueprint(Blueprint);

							UClass* BPClass = Blueprint->SkeletonGeneratedClass
								? Blueprint->SkeletonGeneratedClass
								: Blueprint->GeneratedClass;
							if (BPClass)
							{
								UFunction* Function = BPClass->FindFunctionByName(*ResolvedFunctionName);
								if (Function)
								{
									CallNode->SetFromFunction(Function);
									CallNode->AllocateDefaultPins();
									CallNode->ReconstructNode();
									bFound = true;
									UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-created function '%s' in blueprint"), *ResolvedFunctionName);
								}
							}
						}
					}
				}
			}

			if (!bFound)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Class not found: %s, function: %s"), *FullClassName, *ResolvedFunctionName);
				Graph->RemoveNode(CallNode);
				return nullptr;
			}
		}
	}
	return CallNode;
}


UK2Node* FWeaveInterpreter::CreateMessageNode(UEdGraph* Graph, const FString& ClassName, const FString& FunctionName)
{
	UK2Node_Message* MessageNode = SpawnEditorNode<UK2Node_Message>(Graph);
	if (!MessageNode)
	{
		return nullptr;
	}

	static const TMap<FString, FString> FuncNameTranslation = {
		{TEXT("Conv_FloatToString"), TEXT("Conv_DoubleToString")},
		{TEXT("Conv_FloatToInt"), TEXT("Conv_DoubleToInt")},
		{TEXT("Conv_FloatToBool"), TEXT("Conv_DoubleToBool")},
		{TEXT("Conv_FloatToVector"), TEXT("Conv_DoubleToVector")},
		{TEXT("Conv_FloatToVector2D"), TEXT("Conv_DoubleToVector2D")},
		{TEXT("Conv_IntToFloat"), TEXT("Conv_IntToDouble")},
		{TEXT("Conv_ByteToFloat"), TEXT("Conv_ByteToDouble")},
		{TEXT("Conv_BoolToFloat"), TEXT("Conv_BoolToDouble")},
		{TEXT("FMin"), TEXT("Min")},
		{TEXT("FMax"), TEXT("Max")},
		{TEXT("FClamp"), TEXT("Clamp_Float")},
	};

	FString ResolvedFunctionName = FunctionName;
	if (const FString* Translated = FuncNameTranslation.Find(FunctionName))
	{
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Translating function name: %s -> %s"), *FunctionName, **Translated);
		ResolvedFunctionName = *Translated;
	}

	FString FullClassName = ClassName;
	if (ClassName == TEXT("KismetSystemLibrary"))
	{
		FullClassName = TEXT("/Script/Engine.KismetSystemLibrary");
	}
	else if (ClassName == TEXT("KismetMathLibrary"))
	{
		FullClassName = TEXT("/Script/Engine.KismetMathLibrary");
	}
	else if (ClassName == TEXT("KismetStringLibrary"))
	{
		FullClassName = TEXT("/Script/Engine.KismetStringLibrary");
	}
	else if (ClassName == TEXT("GameplayStatics"))
	{
		FullClassName = TEXT("/Script/Engine.GameplayStatics");
	}

	UClass* TargetClass = FindClassByShortName(FullClassName);
	if (!TargetClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Class not found: %s"), *FullClassName);
		Graph->RemoveNode(MessageNode);
		return nullptr;
	}

	if (!TargetClass->HasAnyClassFlags(CLASS_Interface))
	{
		// 不是接口类就不创建 Message 节点（避免错误还原）。
		// 必须销毁已创建的节点，否则会残留在图表中。
		Graph->RemoveNode(MessageNode);
		return nullptr;
	}

	UFunction* Function = TargetClass->FindFunctionByName(*ResolvedFunctionName);
	if (!Function)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Function not found: %s::%s"), *FullClassName, *ResolvedFunctionName);
		Graph->RemoveNode(MessageNode);
		return nullptr;
	}

	MessageNode->SetFromFunction(Function);
	MessageNode->AllocateDefaultPins();
	MessageNode->ReconstructNode();
	return MessageNode;
}

UK2Node* FWeaveInterpreter::CreateMacroNode(UEdGraph* Graph, const FString& MacroPath, const FString& MacroName)
{
	UK2Node_MacroInstance* MacroNode = SpawnEditorNode<UK2Node_MacroInstance>(Graph);
	if (MacroNode)
	{

		UE_LOG(LogTemp, Log, TEXT("[Weaver] Loading macro: Path=%s, Name=%s"), *MacroPath, *MacroName);


		UBlueprint* MacroBlueprint = LoadObject<UBlueprint>(nullptr, *MacroPath);
		if (MacroBlueprint)
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Macro blueprint loaded, searching for macro graph..."));


			UEdGraph* MacroGraph = nullptr;
			for (UEdGraph* TempGraph : MacroBlueprint->MacroGraphs)
			{
				if (TempGraph)
				{
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Found macro graph: %s"), *TempGraph->GetName());
					if (TempGraph->GetName() == MacroName)
					{
						MacroGraph = TempGraph;
						break;
					}
				}
			}

			if (MacroGraph)
			{
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Macro graph found, setting up node..."));

				MacroNode->SetMacroGraph(MacroGraph);
				MacroNode->AllocateDefaultPins();
				MacroNode->ReconstructNode();
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Macro graph '%s' not found in blueprint"), *MacroName);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Failed to load macro blueprint: %s"), *MacroPath);
		}
	}
	return MacroNode;
}

UK2Node* FWeaveInterpreter::CreateBranchNode(UEdGraph* Graph)
{
	UK2Node_IfThenElse* BranchNode = SpawnEditorNode<UK2Node_IfThenElse>(Graph);
	if (BranchNode)
	{
		BranchNode->AllocateDefaultPins();
		BranchNode->ReconstructNode();
	}
	return BranchNode;
}

UK2Node* FWeaveInterpreter::CreateSequenceNode(UEdGraph* Graph)
{
	UK2Node_ExecutionSequence* SequenceNode = SpawnEditorNode<UK2Node_ExecutionSequence>(Graph);
	if (SequenceNode)
	{
		SequenceNode->AllocateDefaultPins();
		SequenceNode->ReconstructNode();
	}
	return SequenceNode;
}

UK2Node* FWeaveInterpreter::CreateMathExpressionNode(UEdGraph* Graph)
{
	UK2Node_MathExpression* MathNode = SpawnEditorNode<UK2Node_MathExpression>(Graph);
	if (MathNode)
	{
		MathNode->AllocateDefaultPins();
	}
	return MathNode;
}

UK2Node* FWeaveInterpreter::CreateMakeStructNode(UEdGraph* Graph, const FString& StructTypeName)
{
	UScriptStruct* StructType = nullptr;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (It->GetStructCPPName() == StructTypeName)
		{
			StructType = *It;
			break;
		}
	}

	if (StructType)
	{
		UK2Node_MakeStruct* MakeNode = SpawnEditorNode<UK2Node_MakeStruct>(Graph);
		if (MakeNode)
		{
			MakeNode->StructType = StructType;
			MakeNode->AllocateDefaultPins();


			bool bHasInputPins = false;
			for (UEdGraphPin* Pin : MakeNode->Pins)
			{
				if (Pin->Direction == EGPD_Input)
				{
					bHasInputPins = true;
					break;
				}
			}
			if (bHasInputPins)
				return MakeNode;

			MakeNode->DestroyNode();
		}
	}


	FString TypeShortName = StructTypeName;
	if (TypeShortName.StartsWith(TEXT("F")))
		TypeShortName = TypeShortName.RightChop(1);
	UE_LOG(LogTemp, Log, TEXT("[Weaver] MakeStruct fallback to KismetMathLibrary.Make%s"), *TypeShortName);
	return CreateCallNode(Graph, TEXT("KismetMathLibrary"), FString::Printf(TEXT("Make%s"), *TypeShortName));
}

UK2Node* FWeaveInterpreter::CreateBreakStructNode(UEdGraph* Graph, const FString& StructTypeName)
{
	UScriptStruct* StructType = nullptr;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (It->GetStructCPPName() == StructTypeName)
		{
			StructType = *It;
			break;
		}
	}

	if (StructType)
	{
		UK2Node_BreakStruct* BreakNode = SpawnEditorNode<UK2Node_BreakStruct>(Graph);
		if (BreakNode)
		{
			BreakNode->StructType = StructType;
			BreakNode->AllocateDefaultPins();


			bool bHasOutputPins = false;
			for (UEdGraphPin* Pin : BreakNode->Pins)
			{
				if (Pin->Direction == EGPD_Output)
				{
					bHasOutputPins = true;
					break;
				}
			}
			if (bHasOutputPins)
				return BreakNode;

			BreakNode->DestroyNode();
		}
	}


	FString TypeShortName = StructTypeName;
	if (TypeShortName.StartsWith(TEXT("F")))
		TypeShortName = TypeShortName.RightChop(1);
	UE_LOG(LogTemp, Log, TEXT("[Weaver] BreakStruct fallback to KismetMathLibrary.Break%s"), *TypeShortName);
	return CreateCallNode(Graph, TEXT("KismetMathLibrary"), FString::Printf(TEXT("Break%s"), *TypeShortName));
}

UK2Node* FWeaveInterpreter::CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VarName)
{
	UK2Node_VariableGet* VarGetNode = SpawnEditorNode<UK2Node_VariableGet>(Graph);
	if (VarGetNode)
	{
		FName VarFName = FName(*VarName);
		VarGetNode->VariableReference.SetSelfMember(VarFName);
		VarGetNode->AllocateDefaultPins();
	}
	return VarGetNode;
}

UK2Node* FWeaveInterpreter::CreateVariableGetNodeExternal(UEdGraph* Graph, UClass* OwnerClass, const FString& VarName)
{
	UK2Node_VariableGet* VarGetNode = SpawnEditorNode<UK2Node_VariableGet>(Graph);
	if (VarGetNode)
	{
		FName VarFName = FName(*VarName);

		// 确保外部类的属性可用：如果属性不存在，尝试编译外部蓝图
		FProperty* Prop = OwnerClass->FindPropertyByName(VarFName);
		if (!Prop)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] External property '%s' not found on '%s', attempting to compile external Blueprint..."),
				*VarName, *OwnerClass->GetName());
			if (UBlueprint* ExternalBP = Cast<UBlueprint>(OwnerClass->ClassGeneratedBy))
			{
				FKismetEditorUtilities::CompileBlueprint(ExternalBP);
				if (ExternalBP->GeneratedClass)
				{
					OwnerClass = ExternalBP->GeneratedClass;
					Prop = OwnerClass->FindPropertyByName(VarFName);
					UE_LOG(LogTemp, Log, TEXT("[Weaver] After compile: property '%s' %s on '%s'"),
						*VarName, Prop ? TEXT("FOUND") : TEXT("STILL MISSING"), *OwnerClass->GetName());
				}
			}
		}

		VarGetNode->VariableReference.SetExternalMember(VarFName, OwnerClass);
		VarGetNode->AllocateDefaultPins();

		// 验证引脚是否正确创建
		UEdGraphPin* OutputPin = VarGetNode->FindPin(VarFName, EGPD_Output);
		UEdGraphPin* SelfPin = VarGetNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);

		if (!OutputPin || !SelfPin)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] External VariableGet '%s.%s': pins incomplete after AllocateDefaultPins (Output=%s, Self=%s), trying ReconstructNode..."),
				*OwnerClass->GetName(), *VarName,
				OutputPin ? TEXT("OK") : TEXT("MISSING"),
				SelfPin ? TEXT("OK") : TEXT("MISSING"));
			VarGetNode->ReconstructNode();
			OutputPin = VarGetNode->FindPin(VarFName, EGPD_Output);
			SelfPin = VarGetNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
		}

		UE_LOG(LogTemp, Log, TEXT("[Weaver] Created external VariableGet: %s.%s (Output=%s, Self=%s, PinCount=%d)"),
			*OwnerClass->GetName(), *VarName,
			OutputPin ? TEXT("OK") : TEXT("MISSING"),
			SelfPin ? TEXT("OK") : TEXT("MISSING"),
			VarGetNode->Pins.Num());
	}
	return VarGetNode;
}

UK2Node* FWeaveInterpreter::CreateVariableSetNodeExternal(UEdGraph* Graph, UClass* OwnerClass, const FString& VarName)
{
	UK2Node_VariableSet* VarSetNode = SpawnEditorNode<UK2Node_VariableSet>(Graph);
	if (VarSetNode)
	{
		FName VarFName = FName(*VarName);

		// 确保外部类的属性可用：如果属性不存在，尝试编译外部蓝图
		FProperty* Prop = OwnerClass->FindPropertyByName(VarFName);
		if (!Prop)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] External property '%s' not found on '%s', attempting to compile external Blueprint..."),
				*VarName, *OwnerClass->GetName());
			if (UBlueprint* ExternalBP = Cast<UBlueprint>(OwnerClass->ClassGeneratedBy))
			{
				FKismetEditorUtilities::CompileBlueprint(ExternalBP);
				if (ExternalBP->GeneratedClass)
				{
					OwnerClass = ExternalBP->GeneratedClass;
				}
			}
		}

		VarSetNode->VariableReference.SetExternalMember(VarFName, OwnerClass);
		VarSetNode->AllocateDefaultPins();

		// 验证引脚是否正确创建
		UEdGraphPin* InputPin = VarSetNode->FindPin(VarFName, EGPD_Input);
		UEdGraphPin* SelfPin = VarSetNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);

		if (!InputPin || !SelfPin)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] External VariableSet '%s.%s': pins incomplete (Input=%s, Self=%s), trying ReconstructNode..."),
				*OwnerClass->GetName(), *VarName,
				InputPin ? TEXT("OK") : TEXT("MISSING"),
				SelfPin ? TEXT("OK") : TEXT("MISSING"));
			VarSetNode->ReconstructNode();
		}

		UE_LOG(LogTemp, Log, TEXT("[Weaver] Created external VariableSet: %s.%s (PinCount=%d)"),
			*OwnerClass->GetName(), *VarName, VarSetNode->Pins.Num());
	}
	return VarSetNode;
}

UK2Node* FWeaveInterpreter::CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VarName)
{
	UK2Node_VariableSet* VarSetNode = SpawnEditorNode<UK2Node_VariableSet>(Graph);
	if (VarSetNode)
	{
		FName VarFName = FName(*VarName);
		VarSetNode->VariableReference.SetSelfMember(VarFName);
		VarSetNode->AllocateDefaultPins();
	}
	return VarSetNode;
}

UK2Node* FWeaveInterpreter::CreateSpawnActorFromClassNode(UEdGraph* Graph)
{
	UK2Node_SpawnActorFromClass* SpawnNode = SpawnEditorNode<UK2Node_SpawnActorFromClass>(Graph);
	if (SpawnNode)
	{
		SpawnNode->AllocateDefaultPins();
	}
	return SpawnNode;
}

UK2Node* FWeaveInterpreter::CreateConstructObjectFromClassNode(UEdGraph* Graph)
{
	UK2Node_ConstructObjectFromClass* ConstructNode = SpawnEditorNode<UK2Node_ConstructObjectFromClass>(Graph);
	if (ConstructNode)
	{
		ConstructNode->AllocateDefaultPins();
	}
	return ConstructNode;
}

UK2Node* FWeaveInterpreter::CreateDynamicCastNode(UEdGraph* Graph, const FString& TargetTypeName)
{
	UK2Node_DynamicCast* CastNode = SpawnEditorNode<UK2Node_DynamicCast>(Graph);
	if (CastNode)
	{


		UClass* TargetClass = FindClassByShortName(TargetTypeName);

		if (TargetClass)
		{
			CastNode->TargetType = TargetClass;
			UE_LOG(LogTemp, Log, TEXT("[Weaver] CreateDynamicCastNode: Found type %s (Class=%s)"), *TargetTypeName, *TargetClass->GetName());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] CreateDynamicCastNode: Type not found: %s"), *TargetTypeName);
		}

		CastNode->AllocateDefaultPins();

		// 诊断：列出所有引脚
		for (UEdGraphPin* Pin : CastNode->Pins)
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Cast pin: Name=%s Dir=%d Category=%s SubObj=%s"),
				*Pin->PinName.ToString(),
				(int32)Pin->Direction,
				*Pin->PinType.PinCategory.ToString(),
				Pin->PinType.PinSubCategoryObject.IsValid() ? *Pin->PinType.PinSubCategoryObject->GetName() : TEXT("null"));
		}

		// AllocateDefaultPins 在某些情况下会创建 wildcard 引脚而非 PC_Object，
		// 手动修正 Object 引脚和 As 输出引脚类型
		for (UEdGraphPin* Pin : CastNode->Pins)
		{
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			{
				// 修正 Object 输入引脚
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
					Pin->PinType.PinSubCategoryObject = UObject::StaticClass();
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Fixed Cast input pin: %s -> PC_Object"), *Pin->PinName.ToString());
				}
				// 修正 As 输出引脚
				else if (Pin->Direction == EGPD_Output && TargetClass)
				{
					if (TargetClass->IsChildOf(UInterface::StaticClass()))
					{
						Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
					}
					else
					{
						Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
					}
					Pin->PinType.PinSubCategoryObject = TargetClass;
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Fixed Cast output pin: %s -> PC_Object/%s"), *Pin->PinName.ToString(), *TargetClass->GetName());
				}
			}
		}
	}
	return CastNode;
}

UK2Node* FWeaveInterpreter::CreateSwitchEnumNode(UEdGraph* Graph, const FString& EnumName)
{
	UEnum* TargetEnum = nullptr;
	for (TObjectIterator<UEnum> It; It; ++It)
	{
		UEnum* Candidate = *It;
		if (!Candidate->HasAnyFlags(RF_Public))
			continue;


		FString FullName = Candidate->GetName();
		FString ShortName = FullName;
		int32 ColonIdx;
		if (FullName.FindLastChar(TEXT(':'), ColonIdx))
			ShortName = FullName.RightChop(ColonIdx + 1);

		if (ShortName == EnumName || FullName == EnumName)
		{
			TargetEnum = Candidate;
			break;
		}
	}

	if (!TargetEnum)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] CreateSwitchEnumNode: Enum not found: %s"), *EnumName);
		return nullptr;
	}

	UK2Node_SwitchEnum* SwitchNode = SpawnEditorNode<UK2Node_SwitchEnum>(Graph);
	if (SwitchNode)
	{

		SwitchNode->Enum = TargetEnum;
		SwitchNode->EnumEntries.Empty();
		SwitchNode->EnumFriendlyNames.Empty();

		if (TargetEnum)
		{
			if (IsInGameThread() || TargetEnum->IsPostLoadThreadSafe())
			{
				TargetEnum->ConditionalPostLoad();
			}

			for (int32 EnumIndex = 0; EnumIndex < TargetEnum->NumEnums() - 1; ++EnumIndex)
			{
				const bool bShouldBeHidden = TargetEnum->HasMetaData(TEXT("Hidden"), EnumIndex) || TargetEnum->HasMetaData(TEXT("Spacer"), EnumIndex);
				if (bShouldBeHidden)
				{
					continue;
				}

				const FString EnumValueName = TargetEnum->GetNameStringByIndex(EnumIndex);
				SwitchNode->EnumEntries.Add(FName(*EnumValueName));
				SwitchNode->EnumFriendlyNames.Add(TargetEnum->GetDisplayNameTextByIndex(EnumIndex));
			}
		}

		SwitchNode->ReconstructNode();
	}
	return SwitchNode;
}

UK2Node* FWeaveInterpreter::CreateGetArrayItemNode(UEdGraph* Graph)
{
	UK2Node_GetArrayItem* ArrayGetNode = SpawnEditorNode<UK2Node_GetArrayItem>(Graph);
	if (ArrayGetNode)
	{
		ArrayGetNode->AllocateDefaultPins();
	}
	return ArrayGetNode;
}

UK2Node* FWeaveInterpreter::CreateKnotNode(UEdGraph* Graph)
{
	UK2Node_Knot* KnotNode = SpawnEditorNode<UK2Node_Knot>(Graph);
	if (KnotNode)
	{
		KnotNode->AllocateDefaultPins();
	}
	return KnotNode;
}

UK2Node* FWeaveInterpreter::CreateSelfNode(UEdGraph* Graph)
{
	UK2Node_Self* SelfNode = SpawnEditorNode<UK2Node_Self>(Graph);
	if (SelfNode)
	{
		SelfNode->AllocateDefaultPins();
	}
	return SelfNode;
}


// ============================================================
// 增量更新辅助：保存/加载 WeaveNodeId → NodeGuid 映射
// ============================================================

void FWeaveInterpreter::SaveWeaveNodeMap(UEdGraph* Graph, const TMap<FString, UK2Node*>& CreatedNodes)
{
	if (!Graph) return;

	// 查找已有映射注释节点
	UEdGraphNode_Comment* MapComment = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (auto* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			if (Comment->NodeComment.StartsWith(TEXT("__WEAVE_NODE_MAP__")))
			{
				MapComment = Comment;
				break;
			}
		}
	}

	// 不存在则创建
	if (!MapComment)
	{
		MapComment = NewObject<UEdGraphNode_Comment>(Graph, NAME_None, RF_Transactional);
		Graph->AddNode(MapComment, false, false);
		MapComment->CreateNewGuid();
		MapComment->NodePosX = -99999;
		MapComment->NodePosY = -99999;
		MapComment->ResizeNode(FVector2D(1, 1));
		MapComment->CommentColor = FLinearColor(0, 0, 0, 0);
	}

	// 序列化映射表
	FString MapStr = TEXT("__WEAVE_NODE_MAP__\n");
	for (const auto& KV : CreatedNodes)
	{
		if (KV.Value)
		{
			MapStr += FString::Printf(TEXT("%s|%s\n"), *KV.Key, *KV.Value->NodeGuid.ToString());
		}
	}
	MapComment->NodeComment = MapStr;

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Saved WeaveNodeMap: %d entries"), CreatedNodes.Num());
}

void FWeaveInterpreter::LoadWeaveNodeMap(UEdGraph* Graph, TMap<FString, UK2Node*>& OutMap)
{
	if (!Graph) return;

	// 查找映射注释节点
	UEdGraphNode_Comment* MapComment = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (auto* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			if (Comment->NodeComment.StartsWith(TEXT("__WEAVE_NODE_MAP__")))
			{
				MapComment = Comment;
				break;
			}
		}
	}
	if (!MapComment) return;

	// 建立 GUID → UK2Node* 索引
	TMap<FGuid, UK2Node*> GuidToNode;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node* K2 = Cast<UK2Node>(Node))
		{
			GuidToNode.Add(Node->NodeGuid, K2);
		}
	}

	// 解析映射
	TArray<FString> Lines;
	MapComment->NodeComment.ParseIntoArrayLines(Lines);
	for (int32 i = 1; i < Lines.Num(); i++)
	{
		FString Line = Lines[i].TrimStartAndEnd();
		if (Line.IsEmpty()) continue;

		int32 PipePos;
		if (Line.FindChar(TEXT('|'), PipePos))
		{
			FString NodeId = Line.Left(PipePos);
			FString GuidStr = Line.Mid(PipePos + 1);
			FGuid Guid;
			if (FGuid::Parse(GuidStr, Guid))
			{
				if (UK2Node** NodePtr = GuidToNode.Find(Guid))
				{
					OutMap.Add(NodeId, *NodePtr);
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Loaded WeaveNodeMap: %d entries matched"), OutMap.Num());
}


