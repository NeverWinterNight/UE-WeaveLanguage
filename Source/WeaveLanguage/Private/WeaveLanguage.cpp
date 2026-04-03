#include "WeaveLanguage.h"
#include "Slate/SWeaverDebugger.h"
#include "Framework/Application/SlateApplication.h"
#include "ToolMenus.h"
#include "ToolMenuEntry.h"
#include "Core/WeaveOperator.h"
#include "Kismet2/BlueprintEditorUtils.h"

#define LOCTEXT_NAMESPACE "FWeaveLanguageModule"


void FWeaveLanguageModule::StartupModule()
{
	FModuleManager::Get().LoadModuleChecked("ToolMenus");

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FWeaveLanguageModule::RegisterMenus));

	FCoreDelegates::OnPostEngineInit.AddLambda([]()
	{
		if (GEditor)
		{
			FTimerHandle TimerHandle;
			GEditor->GetTimerManager()->SetTimer(TimerHandle, []()
			{
				UWeaveOperator::GenerateWeaveLanguage();
			}, 2.0f, false);
		}
	});
}

void FWeaveLanguageModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FWeaveLanguageModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// 1. LevelEditor 主菜单
	if (UToolMenu* LevelMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu"))
	{
		FToolMenuSection& Section = LevelMenu->AddSection("Weaver", LOCTEXT("Weaver", "Weaver"));
		Section.AddSubMenu(
			"WeaverMenu",
			LOCTEXT("WeaverMenu", "Weaver"),
			LOCTEXT("WeaverMenuTooltip", "Weaver Plugin Menu"),
			FNewToolMenuDelegate::CreateRaw(this, &FWeaveLanguageModule::FillWeaverMenu)
		);
	}

	// 2. LevelEditor 工具栏
	if (UToolMenu* LevelToolbar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolbar"))
	{
		FToolMenuSection& Section = LevelToolbar->AddSection("Weaver", LOCTEXT("Weaver_Toolbar", "Weaver"));
		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"WeaveDebugger_Toolbar",
			FUIAction(FExecuteAction::CreateRaw(this, &FWeaveLanguageModule::OnOpenDebugger)),
			LOCTEXT("WeaveDebugger_Toolbar", "Weave 调试器"),
			LOCTEXT("WeaverDebuggerTooltip_Toolbar", "打开 Weave 调试器"),
			FSlateIcon()
		));
	}

	// 3. Blueprint Editor 工具栏
	if (UToolMenu* BPToolbar = UToolMenus::Get()->ExtendMenu("AssetEditor.BlueprintEditor.ToolBar"))
	{
		FToolMenuSection& Section = BPToolbar->AddSection("Weaver", LOCTEXT("Weaver_BP_Toolbar", "Weaver"));
		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"WeaveDebugger_BP_Toolbar",
			FUIAction(FExecuteAction::CreateRaw(this, &FWeaveLanguageModule::OnOpenDebugger)),
			LOCTEXT("WeaveDebugger_BP_Toolbar", "Weave 调试器"),
			LOCTEXT("WeaveDebuggerTooltip_BP_Toolbar", "从选中节点生成 Weave 代码"),
			FSlateIcon()
		));
	}

	// 4. Blueprint Editor 主菜单 - Tools 段
	if (UToolMenu* BPMainMenu = UToolMenus::Get()->ExtendMenu("AssetEditor.BlueprintEditor.MainMenu"))
	{
		if (FToolMenuSection* ToolsSection = BPMainMenu->FindSection("Tools"))
		{
			ToolsSection->AddMenuEntry(
				"WeaveDebugger_BP",
				LOCTEXT("WeaveDebugger_BP", "Weave 代码生成解释"),
				LOCTEXT("WeaveDebuggerTooltip_BP", "从选中节点生成 Weave 代码或解释 Weave 代码"),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateRaw(this, &FWeaveLanguageModule::OnOpenDebugger))
			);
		}
	}
}

void FWeaveLanguageModule::FillWeaverMenu(UToolMenu* Menu)
{
	FToolMenuSection& Section = Menu->AddSection("WeaverActions", LOCTEXT("WeaverActions", "Actions"));

	Section.AddMenuEntry(
		"WeaveDebugger",
		LOCTEXT("WeaveDebugger", "Weave 生成解释调试器"),
		LOCTEXT("WeaverDebuggerTooltip", "从选中节点生成 Weave 代码或解释 Weave 代码"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FWeaveLanguageModule::OnOpenDebugger))
	);
}


void FWeaveLanguageModule::OnGenerateWeave()
{
	UWeaveOperator::GenerateWeaveLanguage();
}


void FWeaveLanguageModule::OnOpenDebugger()
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("Weave 代码生成解释调试器")))
		.ClientSize(FVector2D(800, 600))
		.SupportsMaximize(true)
		.SupportsMinimize(true)
		.IsTopmostWindow(true);

	Window->SetContent(SNew(SWeaverDebugger));

	FSlateApplication::Get().AddWindow(Window);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FWeaveLanguageModule, WeaveLanguage)
