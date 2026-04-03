#include "Core/WeaveSerializer.h"
#include "Core/WeaveInterpreter.h"

namespace
{
	bool Ser_NeedsQuote(const FString& Str)
	{
		if (Str.IsEmpty()) return true;
		for (const TCHAR Ch : Str)
		{
			if (Ch == TEXT(' ') || Ch == TEXT('\t') ||
				Ch == TEXT('(') || Ch == TEXT(')') ||
				Ch == TEXT('.') || Ch == TEXT('=') ||
				Ch == TEXT(':') || Ch == TEXT(',') ||
				Ch == TEXT('@') || Ch == TEXT('#'))
			{
				return true;
			}
		}
		return false;
	}

	FString Ser_QuoteIfNeeded(const FString& Str)
	{
		if (Ser_NeedsQuote(Str))
		{
			return FString::Printf(TEXT("\"%s\""), *Str.Replace(TEXT("\""), TEXT("\\\"")));
		}
		return Str;
	}

	FString Ser_EscapeCommentText(const FString& Text)
	{
		FString Result = Text;
		Result.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Result.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Result.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		return Result;
	}

	FString SerializeVar(const FWeaveVarDecl& Var)
	{
		FString TypeStr;
		switch (Var.ContainerType)
		{
		case EPinContainerType::Array:
			TypeStr = FString::Printf(TEXT("array:%s"), *Var.VarType);
			break;
		case EPinContainerType::Set:
			TypeStr = FString::Printf(TEXT("set:%s"), *Var.VarType);
			break;
		case EPinContainerType::Map:
			TypeStr = FString::Printf(TEXT("map:%s:%s"), *Var.VarType, *Var.ValueType);
			break;
		default:
			TypeStr = Var.VarType;
			break;
		}
		FString Result = FString::Printf(TEXT("var %s : %s"), *Ser_QuoteIfNeeded(Var.VarName), *TypeStr);

		if (!Var.DefaultValue.IsEmpty())
		{
			Result += FString::Printf(TEXT(" = %s"), *Ser_QuoteIfNeeded(Var.DefaultValue));
		}

		if (Var.bInstanceEditable)
		{
			Result += TEXT(" editable");
		}
		if (Var.bBlueprintReadOnly)
		{
			Result += TEXT(" readonly");
		}
		if (Var.bExposeOnSpawn)
		{
			Result += TEXT(" spawn");
		}
		if (!Var.Category.IsEmpty())
		{
			Result += FString::Printf(TEXT(" category:%s"), *Ser_QuoteIfNeeded(Var.Category));
		}

		return Result;
	}

	FString SerializeNode(const FWeaveNodeDecl& Node)
	{
		FString SchemaStr = Ser_NeedsQuote(Node.SchemaId) ? Ser_QuoteIfNeeded(Node.SchemaId) : Node.SchemaId;
		FString NodeIdStr = Ser_NeedsQuote(Node.NodeId) ? Ser_QuoteIfNeeded(Node.NodeId) : Node.NodeId;
		return FString::Printf(TEXT("node %s : %s @ (%.0f, %.0f)"),
			*NodeIdStr, *SchemaStr, Node.Position.X, Node.Position.Y);
	}

	FString SerializeLink(const FWeaveLinkStmt& Link)
	{
		return FString::Printf(TEXT("link %s.%s -> %s.%s"),
			*Link.FromNode, *Ser_QuoteIfNeeded(Link.FromPin),
			*Link.ToNode, *Ser_QuoteIfNeeded(Link.ToPin));
	}

	FString SerializeSet(const FWeaveSetStmt& Set)
	{
		return FString::Printf(TEXT("set %s.%s = %s"),
			*Set.NodeId, *Ser_QuoteIfNeeded(Set.PinName), *Ser_QuoteIfNeeded(Set.Value));
	}

	FString SerializeComment(const FWeaveCommentDecl& Comment)
	{
		FString Result = FString::Printf(TEXT("comment \"%s\" @ (%.0f, %.0f) size (%.0f, %.0f)"),
			*Ser_EscapeCommentText(Comment.Text),
			Comment.Position.X, Comment.Position.Y,
			Comment.Size.X, Comment.Size.Y);

		// 非默认颜色时输出 color
		const FLinearColor DefaultColor(1.f, 1.f, 1.f, 1.f);
		if (!Comment.Color.Equals(DefaultColor, 0.01f))
		{
			Result += FString::Printf(TEXT(" color (%d, %d, %d, %d)"),
				FMath::RoundToInt(Comment.Color.R * 255.f),
				FMath::RoundToInt(Comment.Color.G * 255.f),
				FMath::RoundToInt(Comment.Color.B * 255.f),
				FMath::RoundToInt(Comment.Color.A * 255.f));
		}

		// 非默认字号时输出 fontsize
		if (Comment.FontSize != 18)
		{
			Result += FString::Printf(TEXT(" fontsize %d"), Comment.FontSize);
		}

		return Result;
	}

	void SerializeSection(const FWeaveGraphSection& Section, FString& Out)
	{
		Out += FString::Printf(TEXT("graph %s\n"), *Section.GraphName);
		Out += TEXT("\n");

		// nodes
		for (const FWeaveNodeDecl& Node : Section.Nodes)
		{
			Out += SerializeNode(Node) + TEXT("\n");
		}
		if (Section.Nodes.Num() > 0) Out += TEXT("\n");

		// links
		for (const FWeaveLinkStmt& Link : Section.Links)
		{
			Out += SerializeLink(Link) + TEXT("\n");
		}
		if (Section.Links.Num() > 0) Out += TEXT("\n");

		// sets
		for (const FWeaveSetStmt& Set : Section.Sets)
		{
			Out += SerializeSet(Set) + TEXT("\n");
		}
		if (Section.Sets.Num() > 0) Out += TEXT("\n");

		// comments
		for (const FWeaveCommentDecl& Comment : Section.Comments)
		{
			Out += SerializeComment(Comment) + TEXT("\n");
		}

		// bubbles
		for (const FWeaveBubbleStmt& Bubble : Section.Bubbles)
		{
			FString BubbleText = Bubble.Text;
			BubbleText = BubbleText.Replace(TEXT("\\"), TEXT("\\\\"));
			BubbleText = BubbleText.Replace(TEXT("\""), TEXT("\\\""));
			BubbleText = BubbleText.Replace(TEXT("\n"), TEXT("\\n"));
			Out += FString::Printf(TEXT("bubble %s \"%s\"\n"), *Bubble.NodeId, *BubbleText);
		}
	}
}

FString FWeaveSerializer::Serialize(const FWeaveAST& AST)
{
	FString Result;

	// graphset
	if (!AST.BlueprintPath.IsEmpty())
	{
		// 从路径提取短名称："/Game/BP/BP_Test.BP_Test" → "BP_Test"
		FString ShortName;
		int32 DotIdx;
		if (AST.BlueprintPath.FindLastChar(TEXT('.'), DotIdx))
		{
			ShortName = AST.BlueprintPath.Mid(DotIdx + 1);
		}
		else
		{
			int32 SlashIdx;
			if (AST.BlueprintPath.FindLastChar(TEXT('/'), SlashIdx))
			{
				ShortName = AST.BlueprintPath.Mid(SlashIdx + 1);
			}
			else
			{
				ShortName = AST.BlueprintPath;
			}
		}
		Result += FString::Printf(TEXT("graphset %s %s\n\n"), *ShortName, *AST.BlueprintPath);
	}

	// vars（全局）
	for (const FWeaveVarDecl& Var : AST.Vars)
	{
		Result += SerializeVar(Var) + TEXT("\n");
	}
	if (AST.Vars.Num() > 0) Result += TEXT("\n");

	// sections
	if (AST.Sections.Num() > 0)
	{
		for (int32 i = 0; i < AST.Sections.Num(); i++)
		{
			if (i > 0) Result += TEXT("\n");
			SerializeSection(AST.Sections[i], Result);
		}
	}
	else if (!AST.GraphName.IsEmpty())
	{
		// 向后兼容：使用顶层字段
		FWeaveGraphSection FakeSection;
		FakeSection.GraphName = AST.GraphName;
		FakeSection.Nodes = AST.Nodes;
		FakeSection.Links = AST.Links;
		FakeSection.Sets = AST.Sets;
		FakeSection.Comments = AST.Comments;
		SerializeSection(FakeSection, Result);
	}

	return Result;
}
