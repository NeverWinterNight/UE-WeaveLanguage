#pragma once

#include "CoreMinimal.h"

struct FWeaveAST;
struct FWeaveGraphSection;
struct FWeaveNodeDecl;

// 一条纠错记录
struct FWeaveCorrection
{
	FString RuleName;       // 规则标识
	FString Description;    // 中文描述
	bool bIsSuggestion;     // true=建议 false=已自动修正

	FWeaveCorrection() : bIsSuggestion(false) {}
	FWeaveCorrection(const FString& InRule, const FString& InDesc, bool bSuggestion = false)
		: RuleName(InRule), Description(InDesc), bIsSuggestion(bSuggestion) {}
};

// 纠错规则接口
class IWeaveValidationRule
{
public:
	virtual ~IWeaveValidationRule() = default;
	virtual FString GetRuleName() const = 0;
	virtual bool Apply(FWeaveAST& AST, TArray<FWeaveCorrection>& OutCorrections) = 0;
};

// 主验证器：注册并执行所有规则
class WEAVELANGUAGE_API FWeaveValidator
{
public:
	FWeaveValidator();

	void SetRuleEnabled(const FString& RuleName, bool bEnabled);
	bool Validate(FWeaveAST& AST, TArray<FWeaveCorrection>& OutCorrections);

private:
	struct FRuleEntry
	{
		TSharedPtr<IWeaveValidationRule> Rule;
		bool bEnabled = true;
	};
	TArray<FRuleEntry> Rules;

	// 将 Sections[0] 同步到 AST 顶层字段
	static void SyncTopLevelFromSections(FWeaveAST& AST);
};
