#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "WeaveSettings.generated.h"

UENUM()
enum class EWeaveLogLevel : uint8
{
	Silent    UMETA(DisplayName="静默"),
	Warning   UMETA(DisplayName="警告"),
	Verbose   UMETA(DisplayName="详细"),
};

UCLASS(config=Weaver, defaultconfig, meta=(DisplayName="Weaver"))
class WEAVELANGUAGE_API UWeaverSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UWeaverSettings();

	static UWeaverSettings* Get();

	// --- UDeveloperSettings ---
	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Weaver"); }

	// --- 纠错规则开关 ---

	UPROPERTY(config, EditAnywhere, Category="Validation Rules", meta=(DisplayName="InternalSchemaFix (K2Node类名转换)"))
	bool bEnableInternalSchemaFix = true;

	UPROPERTY(config, EditAnywhere, Category="Validation Rules", meta=(DisplayName="SelfLoopRemoval (自连链接移除)"))
	bool bEnableSelfLoopRemoval = true;

	UPROPERTY(config, EditAnywhere, Category="Validation Rules", meta=(DisplayName="PureExecRemoval (纯节点执行链移除)"))
	bool bEnablePureExecRemoval = true;

	UPROPERTY(config, EditAnywhere, Category="Validation Rules", meta=(DisplayName="VarSetPinFix (VariableSet引脚修正)"))
	bool bEnableVarSetPinFix = true;

	UPROPERTY(config, EditAnywhere, Category="Validation Rules", meta=(DisplayName="InterfaceEventSuggestion (接口事件建议)"))
	bool bEnableInterfaceEventSuggestion = true;

	UPROPERTY(config, EditAnywhere, Category="Validation Rules", meta=(DisplayName="ExecMultiTargetSequence (多目标自动Sequence)"))
	bool bEnableExecMultiTargetSequence = true;

	// --- 通用设置 ---

	UPROPERTY(config, EditAnywhere, Category="General", meta=(DisplayName="纠错后回写代码"))
	bool bAutoRewriteCode = true;

	UPROPERTY(config, EditAnywhere, Category="General", meta=(DisplayName="日志级别"))
	EWeaveLogLevel LogLevel = EWeaveLogLevel::Warning;
};
