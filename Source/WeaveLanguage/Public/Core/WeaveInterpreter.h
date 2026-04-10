#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Math/Color.h"


struct FWeaveNodeDecl
{
	FString NodeId;
	FString SchemaId;
	FVector2D Position;
	TMap<FString, FString> InlineProps;
};

struct FWeaveSetStmt
{
	FString NodeId;
	FString PinName;
	FString Value;
};

struct FWeaveLinkStmt
{
	FString FromNode;
	FString FromPin;
	FString ToNode;
	FString ToPin;
};

struct FWeaveVarDecl
{
	FString VarName;
	FString VarType;
	EPinContainerType ContainerType = EPinContainerType::None;
	FString ValueType; // Map 的 Value 类型（Key 类型存在 VarType 中）
	FString DefaultValue; // 变量默认值（可选）
	TArray<FString> ArrayDefaultValues; // 数组字面量默认值，如 [1000.0, 3000.0]
	FString Description; // 变量描述/tooltip（可选）
	FString Category; // 变量分类（可选）
	bool bInstanceEditable = false; // 实例可编辑（Instance Editable）
	bool bBlueprintReadOnly = false; // 蓝图只读（Blueprint Read Only）
	bool bExposeOnSpawn = false; // 生成时暴露（Expose on Spawn）
};

struct FWeaveBubbleStmt
{
	FString NodeId;
	FString Text;
};

struct FWeaveCommentDecl
{
	FString Text;
	FVector2D Position;
	FVector2D Size;
	FLinearColor Color = FLinearColor(1.f, 1.f, 1.f, 1.f);
	int32 FontSize = 18;
};

struct FWeaveParamDecl
{
	FString Name;
	FString Type;
};

struct FWeaveGraphSection
{
	FString GraphName;
	TArray<FWeaveParamDecl> InputParams;
	TArray<FWeaveParamDecl> OutputParams;
	TArray<FWeaveNodeDecl> Nodes;
	TArray<FWeaveSetStmt> Sets;
	TArray<FWeaveLinkStmt> Links;
	TArray<FWeaveCommentDecl> Comments;
	TArray<FWeaveBubbleStmt> Bubbles;
};

struct FWeaveAST
{
	FString BlueprintPath;
	FString GraphName;         // 第一个图表名（向后兼容）
	TArray<FWeaveVarDecl> Vars; // 变量声明全局共享
	TArray<FWeaveNodeDecl> Nodes;    // 向后兼容（等于 Sections[0]）
	TArray<FWeaveSetStmt> Sets;
	TArray<FWeaveLinkStmt> Links;
	TArray<FWeaveCommentDecl> Comments;
	TArray<FWeaveBubbleStmt> Bubbles;
	TArray<FWeaveGraphSection> Sections; // 多图表段落
};

class WEAVELANGUAGE_API FWeaveInterpreter
{
public:
	// 自动纠错：修复常见的 Weave 语法错误
	static FString AutoFixWeaveCode(const FString& WeaveCode, TArray<FString>& OutFixes);
	// 自动纠错：修复缺失的连接和错误的 set 值
	static FString AutoFixLinksAndSets(const FString& WeaveCode, TArray<FString>& OutFixes);

	static bool Parse(const FString& WeaveCode, FWeaveAST& OutAST, FString& OutError);


	static int32 GenerateBlueprint(const FWeaveAST& AST, class UEdGraph* Graph, FString& OutError);
	static int32 GenerateMultiGraph(const FWeaveAST& AST, class UBlueprint* Blueprint, FString& OutError);

private:
	static TArray<FString> Tokenize(const FString& Code);


	static bool ParseGraph(const TArray<FString>& Tokens, int32& Index, FString& OutGraphName,
		TArray<FWeaveParamDecl>& OutInputParams, TArray<FWeaveParamDecl>& OutOutputParams);


	static bool ParseNode(const TArray<FString>& Tokens, int32& Index, FWeaveNodeDecl& OutNode);


	static bool ParseSet(const TArray<FString>& Tokens, int32& Index, FWeaveSetStmt& OutSet);


	static bool ParseLink(const TArray<FString>& Tokens, int32& Index, FWeaveLinkStmt& OutLink);


	static bool ParseVar(const TArray<FString>& Tokens, int32& Index, FWeaveVarDecl& OutVar);
	static bool ParseComment(const TArray<FString>& Tokens, int32& Index, FWeaveCommentDecl& OutComment);
	static bool ParseBubble(const TArray<FString>& Tokens, int32& Index, FWeaveBubbleStmt& OutBubble);


	static bool ResolveWeaveType(const FString& TypeStr, FEdGraphPinType& OutPinType);
	static UK2Node* CreateNativeNode(UEdGraph* Graph, const FString& ClassName);
	static UK2Node* CreateAsyncActionNode(UEdGraph* Graph, const FString& ProxyFactoryClassPath, const FString& ProxyFactoryFunctionName);
	static UK2Node* CreateTimelineNode(UEdGraph* Graph, const FString& TimelineName);
	static UK2Node* CreateComponentBoundEventNode(UEdGraph* Graph, const FString& ComponentVarName, const FString& DelegateClassName, const FString& DelegateName);
	static UK2Node* CreateEventNode(UEdGraph* Graph, const FString& ClassName, const FString& EventName);
	static UK2Node* CreateCallNode(UEdGraph* Graph, const FString& ClassName, const FString& FunctionName);
    static UK2Node* CreateMessageNode(UEdGraph* Graph, const FString& ClassName, const FString& FunctionName);
	static UK2Node* CreateMacroNode(UEdGraph* Graph, const FString& MacroPath, const FString& MacroName);
	static UK2Node* CreateBranchNode(UEdGraph* Graph);
	static UK2Node* CreateSequenceNode(UEdGraph* Graph);
	static UK2Node* CreateMathExpressionNode(UEdGraph* Graph);
	static UK2Node* CreateMakeStructNode(UEdGraph* Graph, const FString& StructTypeName);
	static UK2Node* CreateBreakStructNode(UEdGraph* Graph, const FString& StructTypeName);
	static UK2Node* CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VarName);
	static UK2Node* CreateVariableGetNodeExternal(UEdGraph* Graph, UClass* OwnerClass, const FString& VarName);
	static UK2Node* CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VarName);
	static UK2Node* CreateVariableSetNodeExternal(UEdGraph* Graph, UClass* OwnerClass, const FString& VarName);
	static UK2Node* CreateSpawnActorFromClassNode(UEdGraph* Graph);
	static UK2Node* CreateConstructObjectFromClassNode(UEdGraph* Graph);
	static UK2Node* CreateDynamicCastNode(UEdGraph* Graph, const FString& TargetTypeName);
	static UK2Node* CreateSwitchEnumNode(UEdGraph* Graph, const FString& EnumName);
	static UK2Node* CreateGetArrayItemNode(UEdGraph* Graph);
	static UK2Node* CreateKnotNode(UEdGraph* Graph);
	static UK2Node* CreateSelfNode(UEdGraph* Graph);
};

