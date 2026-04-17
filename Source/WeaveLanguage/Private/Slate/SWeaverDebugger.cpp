#include "Slate/SWeaverDebugger.h"
#include "Core/WeaveInterpreter.h"
#include "Core/WeaveGenerator.h"
#include "Core/WeaveValidator.h"
#include "Core/WeaveSerializer.h"
#include "Core/WeaveSettings.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "EditorStyleSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_FunctionEntry.h"
#include "Editor.h"
#include "Selection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "BlueprintEditor.h"

void SWeaverDebugger::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(0)
		[
			SNew(SBox)
			.WidthOverride(800)
			.HeightOverride(600)
			[
				SNew(SVerticalBox)


				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
					.Padding(FMargin(10, 8))
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Weave Debugger")))
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						.ColorAndOpacity(FLinearColor::White)
					]
				]


				+ SVerticalBox::Slot()
				.FillHeight(0.6f)
				.Padding(8)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.Padding(8)
					[
						SAssignNew(CodeInputBox, SMultiLineEditableTextBox)
						.HintText(FText::FromString(TEXT(
							"Enter Weave code...\n\nExample:\ngraph MyActor.EventGraph\n\nnode e : event.Actor.ReceiveBeginPlay\nnode print : call.KismetSystemLibrary.PrintString")))
						.Font(FAppStyle::GetFontStyle("NormalFont"))
						.AutoWrapText(false)
					]
				]


				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(8, 4)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Apply to Blueprint")))
						.HAlign(HAlign_Center)
						.OnClicked(this, &SWeaverDebugger::OnApply)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4, 0, 0, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Generate from Selection")))
						.OnClicked(this, &SWeaverDebugger::OnGenerateFromSelection)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4, 0, 0, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Clear")))
						.OnClicked(this, &SWeaverDebugger::OnClear)
					]
				]


				// 设置面板（可折叠）
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(8, 2)
				[
					SNew(SExpandableArea)
					.AreaTitle(FText::FromString(TEXT("设置")))
					.InitiallyCollapsed(true)
					.BodyContent()
					[
						SNew(SVerticalBox)
						// 第一行：规则开关
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(4, 2)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([]() { return UWeaverSettings::Get()->bEnableInternalSchemaFix ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UWeaverSettings::Get()->bEnableInternalSchemaFix = (State == ECheckBoxState::Checked); UWeaverSettings::Get()->SaveConfig(); })
								[
									SNew(STextBlock).Text(FText::FromString(TEXT("Schema修正")))
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([]() { return UWeaverSettings::Get()->bEnableSelfLoopRemoval ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UWeaverSettings::Get()->bEnableSelfLoopRemoval = (State == ECheckBoxState::Checked); UWeaverSettings::Get()->SaveConfig(); })
								[
									SNew(STextBlock).Text(FText::FromString(TEXT("自连移除")))
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([]() { return UWeaverSettings::Get()->bEnablePureExecRemoval ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UWeaverSettings::Get()->bEnablePureExecRemoval = (State == ECheckBoxState::Checked); UWeaverSettings::Get()->SaveConfig(); })
								[
									SNew(STextBlock).Text(FText::FromString(TEXT("纯节点执行链")))
								]
							]
						]
						// 第二行：更多规则开关
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(4, 2)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([]() { return UWeaverSettings::Get()->bEnableVarSetPinFix ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UWeaverSettings::Get()->bEnableVarSetPinFix = (State == ECheckBoxState::Checked); UWeaverSettings::Get()->SaveConfig(); })
								[
									SNew(STextBlock).Text(FText::FromString(TEXT("VarSet引脚")))
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([]() { return UWeaverSettings::Get()->bEnableInterfaceEventSuggestion ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UWeaverSettings::Get()->bEnableInterfaceEventSuggestion = (State == ECheckBoxState::Checked); UWeaverSettings::Get()->SaveConfig(); })
								[
									SNew(STextBlock).Text(FText::FromString(TEXT("接口事件建议")))
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([]() { return UWeaverSettings::Get()->bEnableExecMultiTargetSequence ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UWeaverSettings::Get()->bEnableExecMultiTargetSequence = (State == ECheckBoxState::Checked); UWeaverSettings::Get()->SaveConfig(); })
								[
									SNew(STextBlock).Text(FText::FromString(TEXT("多目标Sequence")))
								]
							]
						]
						// 第三行：通用设置
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(4, 2)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([]() { return UWeaverSettings::Get()->bAutoRewriteCode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UWeaverSettings::Get()->bAutoRewriteCode = (State == ECheckBoxState::Checked); UWeaverSettings::Get()->SaveConfig(); })
								[
									SNew(STextBlock).Text(FText::FromString(TEXT("纠错后回写代码")))
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("日志级别:")))
							]
							+ SHorizontalBox::Slot().AutoWidth()
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
								[
									SNew(SCheckBox)
									.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
									.IsChecked_Lambda([]() { return UWeaverSettings::Get()->LogLevel == EWeaveLogLevel::Silent ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
									.OnCheckStateChanged_Lambda([](ECheckBoxState) { UWeaverSettings::Get()->LogLevel = EWeaveLogLevel::Silent; UWeaverSettings::Get()->SaveConfig(); })
									[
										SNew(STextBlock).Text(FText::FromString(TEXT("静默")))
									]
								]
								+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
								[
									SNew(SCheckBox)
									.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
									.IsChecked_Lambda([]() { return UWeaverSettings::Get()->LogLevel == EWeaveLogLevel::Warning ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
									.OnCheckStateChanged_Lambda([](ECheckBoxState) { UWeaverSettings::Get()->LogLevel = EWeaveLogLevel::Warning; UWeaverSettings::Get()->SaveConfig(); })
									[
										SNew(STextBlock).Text(FText::FromString(TEXT("警告")))
									]
								]
								+ SHorizontalBox::Slot().AutoWidth()
								[
									SNew(SCheckBox)
									.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
									.IsChecked_Lambda([]() { return UWeaverSettings::Get()->LogLevel == EWeaveLogLevel::Verbose ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
									.OnCheckStateChanged_Lambda([](ECheckBoxState) { UWeaverSettings::Get()->LogLevel = EWeaveLogLevel::Verbose; UWeaverSettings::Get()->SaveConfig(); })
									[
										SNew(STextBlock).Text(FText::FromString(TEXT("详细")))
									]
								]
							]
						]
					]
				]

				+ SVerticalBox::Slot()
				.FillHeight(0.4f)
				.Padding(8, 0, 8, 8)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(8)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SAssignNew(ResultText, STextBlock)
							.Text(FText::FromString(TEXT("Waiting for input...")))
							.Font(FAppStyle::GetFontStyle("NormalFont"))
							.AutoWrapText(true)
						]
					]
				]
			]
		]
	];
}

FReply SWeaverDebugger::OnApply()
{
	if (!CodeInputBox.IsValid())
	{
		return FReply::Handled();
	}

	FString WeaveCode = CodeInputBox->GetText().ToString();
	if (WeaveCode.IsEmpty())
	{
		ResultText->SetText(FText::FromString(TEXT("Error: Code is empty")));
		return FReply::Handled();
	}


	FWeaveAST AST;
	FString Error;

	// 自动纠错预处理
	TArray<FString> AutoFixes;
	FString ProcessedCode = FWeaveInterpreter::AutoFixWeaveCode(WeaveCode, AutoFixes);

	if (!FWeaveInterpreter::Parse(ProcessedCode, AST, Error))
	{
		ResultText->SetText(FText::FromString(FString::Printf(TEXT("Parse failed: %s"), *Error)));
		return FReply::Handled();
	}

	// AST 层纠错（根据设置开关规则）
	UWeaverSettings* Settings = UWeaverSettings::Get();
	FWeaveValidator Validator;
	Validator.SetRuleEnabled(TEXT("InternalSchemaFix"), Settings->bEnableInternalSchemaFix);
	Validator.SetRuleEnabled(TEXT("SelfLoopRemoval"), Settings->bEnableSelfLoopRemoval);
	Validator.SetRuleEnabled(TEXT("PureExecRemoval"), Settings->bEnablePureExecRemoval);
	Validator.SetRuleEnabled(TEXT("VarSetPinFix"), Settings->bEnableVarSetPinFix);
	Validator.SetRuleEnabled(TEXT("InterfaceEventSuggestion"), Settings->bEnableInterfaceEventSuggestion);
	Validator.SetRuleEnabled(TEXT("ExecMultiTargetSequence"), Settings->bEnableExecMultiTargetSequence);

	TArray<FWeaveCorrection> Corrections;
	bool bASTCorrected = Validator.Validate(AST, Corrections);

	if (bASTCorrected && Settings->bAutoRewriteCode)
	{
		// 将纠正后的 AST 序列化回 Weave 代码并回写到编辑框
		FString CorrectedCode = FWeaveSerializer::Serialize(AST);
		CodeInputBox->SetText(FText::FromString(CorrectedCode));
	}

	// 显示纠错信息
	FString Result;
	if (AutoFixes.Num() > 0)
	{
		Result += FString::Printf(TEXT("文本层自动修正了 %d 处问题:\n"), AutoFixes.Num());
		for (const FString& Fix : AutoFixes)
		{
			Result += TEXT("  ") + Fix + TEXT("\n");
		}
		Result += TEXT("\n");
	}

	int32 CorrectionCount = 0;
	int32 SuggestionCount = 0;
	for (const FWeaveCorrection& C : Corrections)
	{
		if (C.bIsSuggestion) SuggestionCount++;
		else CorrectionCount++;
	}

	if (Corrections.Num() > 0)
	{
		if (CorrectionCount > 0)
		{
			Result += FString::Printf(TEXT("AST 纠正了 %d 处问题:\n"), CorrectionCount);
		}
		for (const FWeaveCorrection& C : Corrections)
		{
			if (!C.bIsSuggestion)
			{
				Result += FString::Printf(TEXT("  [%s] %s\n"), *C.RuleName, *C.Description);
			}
		}
		if (SuggestionCount > 0)
		{
			Result += FString::Printf(TEXT("\n建议 (%d 条):\n"), SuggestionCount);
			for (const FWeaveCorrection& C : Corrections)
			{
				if (C.bIsSuggestion)
				{
					Result += FString::Printf(TEXT("  [建议] %s\n"), *C.Description);
				}
			}
		}
		Result += TEXT("\n");
	}

	Result += FString::Printf(
		TEXT("Parse successful!\n\nBlueprint: %s\nGraph: %s\nSections: %d\nNodes: %d\nSets: %d\nLinks: %d\n\n"),
		*AST.BlueprintPath, *AST.GraphName, AST.Sections.Num(), AST.Nodes.Num(), AST.Sets.Num(), AST.Links.Num()
	);

	if (AST.Sections.Num() > 1)
	{
		for (int32 i = 0; i < AST.Sections.Num(); i++)
		{
			const FWeaveGraphSection& Sec = AST.Sections[i];
			Result += FString::Printf(TEXT("[Section %d] graph %s: %d nodes, %d links\n"),
				i, *Sec.GraphName, Sec.Nodes.Num(), Sec.Links.Num());
		}
		Result += TEXT("\n");
	}

	for (const FWeaveNodeDecl& Node : AST.Nodes)
	{
		Result += FString::Printf(TEXT("Node %s: %s @ (%.0f, %.0f)\n"),
		                          *Node.NodeId, *Node.SchemaId, Node.Position.X, Node.Position.Y);
	}


	if (!AST.BlueprintPath.IsEmpty())
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AST.BlueprintPath);
		if (BP)
		{
			// 多图表模式
			if (AST.Sections.Num() > 1)
			{
				Result += FString::Printf(TEXT("\nMulti-graph mode: %d sections\n"), AST.Sections.Num());
				for (const FWeaveGraphSection& Sec : AST.Sections)
				{
					Result += FString::Printf(TEXT("  - %s (%d nodes, %d links)\n"),
						*Sec.GraphName, Sec.Nodes.Num(), Sec.Links.Num());
				}

				FString GenError;
				int32 NodesCreated = FWeaveInterpreter::GenerateMultiGraph(AST, BP, GenError);

				if (NodesCreated > 0)
				{
					Result += FString::Printf(TEXT("\nGenerated %d nodes across %d graphs successfully!"),
						NodesCreated, AST.Sections.Num());
				}
				else
				{
					Result += FString::Printf(TEXT("\nGeneration failed: %s"), *GenError);
				}
			}
			else
			{
				// 单图表模式（原有逻辑）
				UEdGraph* TargetGraph = nullptr;

				for (UEdGraph* Graph : BP->UbergraphPages)
				{
					if (Graph && Graph->GetName().Contains(AST.GraphName))
					{
						TargetGraph = Graph;
						break;
					}
				}

				if (!TargetGraph)
				{
					for (UEdGraph* Graph : BP->FunctionGraphs)
					{
						if (Graph && Graph->GetName().Contains(AST.GraphName))
						{
							TargetGraph = Graph;
							break;
						}
					}
				}

				// 检查 Weave 代码中是否包含事件节点（事件必须在 EventGraph 中）
				bool bHasEventNodes = false;
				for (const FWeaveNodeDecl& NodeDecl : AST.Nodes)
				{
					if (NodeDecl.SchemaId.StartsWith(TEXT("event.")) ||
						NodeDecl.SchemaId.StartsWith(TEXT("customEvent.")) ||
						NodeDecl.SchemaId.StartsWith(TEXT("componentEvent.")))
					{
						bHasEventNodes = true;
						break;
					}
				}

				if (!TargetGraph && !AST.GraphName.Contains(TEXT("EventGraph")) && !bHasEventNodes)
				{
					UEdGraph* NewFuncGraph = FBlueprintEditorUtils::CreateNewGraph(
						BP, FName(*AST.GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
					if (NewFuncGraph)
					{
						FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewFuncGraph, true, nullptr);
						FKismetEditorUtilities::CompileBlueprint(BP);
						TargetGraph = NewFuncGraph;
						UE_LOG(LogTemp, Log, TEXT("[Weaver Debugger] Auto-created function graph: %s"), *AST.GraphName);
					}
				}

				if (!TargetGraph && BP->UbergraphPages.Num() > 0)
				{
					TargetGraph = BP->UbergraphPages[0];
					UE_LOG(LogTemp, Log, TEXT("[Weaver Debugger] Falling back to default UbergraphPage: %s"), *TargetGraph->GetName());
				}

				if (TargetGraph)
				{
					Result += FString::Printf(TEXT("\nTarget Graph: %s\n"), *TargetGraph->GetName());

					FString GenError;
					int32 NodesCreated = 0;

					// 尝试加载增量映射
					TMap<FString, UK2Node*> ExistingWeaveNodes;
					FWeaveInterpreter::LoadWeaveNodeMap(TargetGraph, ExistingWeaveNodes);

					if (ExistingWeaveNodes.Num() > 0)
					{
						// === 增量模式 ===
						TSet<FString> NewNodeIds;
						for (const FWeaveNodeDecl& Decl : AST.Nodes)
							NewNodeIds.Add(Decl.NodeId);

						// 删除不再需要的旧节点
						TArray<FString> RemovedIds;
						for (auto& KV : ExistingWeaveNodes)
						{
							if (!NewNodeIds.Contains(KV.Key) && KV.Value)
							{
								KV.Value->BreakAllNodeLinks();
								TargetGraph->Nodes.Remove(KV.Value);
								KV.Value->DestroyNode();
								RemovedIds.Add(KV.Key);
							}
						}
						for (const FString& Id : RemovedIds)
							ExistingWeaveNodes.Remove(Id);

						// 删除旧注释节点（非映射节点，将全部重建）
						TArray<UEdGraphNode*> OldComments;
						for (UEdGraphNode* Node : TargetGraph->Nodes)
						{
							if (auto* Comment = Cast<UEdGraphNode_Comment>(Node))
							{
								if (!Comment->NodeComment.StartsWith(TEXT("__WEAVE_NODE_MAP__")))
									OldComments.Add(Comment);
							}
						}
						for (UEdGraphNode* C : OldComments)
						{
							TargetGraph->Nodes.Remove(C);
							C->DestroyNode();
						}

						// 断开保留节点的可见引脚链接（保留隐藏引脚如静态函数的内部self连接）
						for (auto& KV : ExistingWeaveNodes)
						{
							if (KV.Value)
							{
								for (UEdGraphPin* Pin : KV.Value->Pins)
								{
									if (!Pin->bHidden)
									{
										Pin->BreakAllPinLinks();
									}
								}
							}
						}

						UE_LOG(LogTemp, Log, TEXT("[Weaver] Debugger incremental mode: %d existing, %d in new AST, %d removed"),
							ExistingWeaveNodes.Num(), NewNodeIds.Num(), RemovedIds.Num());

						NodesCreated = FWeaveInterpreter::GenerateBlueprint(AST, TargetGraph, GenError, &ExistingWeaveNodes);
					}
					else
					{
						// === 首次全量模式 ===
						TArray<UEdGraphNode*> NodesToRemove;
						for (UEdGraphNode* Node : TargetGraph->Nodes)
						{
							if (Node && !Node->IsA<UK2Node_FunctionEntry>())
							{
								Node->BreakAllNodeLinks();
								NodesToRemove.Add(Node);
							}
						}
						for (UEdGraphNode* Node : NodesToRemove)
						{
							TargetGraph->Nodes.Remove(Node);
						}

						NodesCreated = FWeaveInterpreter::GenerateBlueprint(AST, TargetGraph, GenError);
					}

					if (NodesCreated > 0)
					{
						Result += FString::Printf(TEXT("Generated %d nodes successfully!"), NodesCreated);
						BP->MarkPackageDirty();
					}
					else
					{
						Result += FString::Printf(TEXT("Generation failed: %s"), *GenError);
					}
				}
				else
				{
					Result += TEXT("\nError: Graph not found in blueprint");
				}
			}
		}
		else
		{
			Result += TEXT("\nError: Failed to load blueprint");
		}
	}
	else
	{
		Result += TEXT("\n[Note] No blueprint path specified. Use 'graphset' command.");
	}

	ResultText->SetText(FText::FromString(Result));

	UE_LOG(LogTemp, Log, TEXT("[Weaver Debugger] Parsed successfully: %s"), *AST.GraphName);

	return FReply::Handled();
}

FReply SWeaverDebugger::OnClear()
{
	if (CodeInputBox.IsValid())
	{
		CodeInputBox->SetText(FText::GetEmpty());
	}

	if (ResultText.IsValid())
	{
		ResultText->SetText(FText::FromString(TEXT("Cleared")));
	}

	return FReply::Handled();
}

FReply SWeaverDebugger::OnGenerateFromSelection()
{
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		ResultText->SetText(FText::FromString(TEXT("Error: Cannot access asset editor subsystem")));
		return FReply::Handled();
	}
	
	TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
	const UBlueprint* CurrentBP = nullptr;
	const FBlueprintEditor* BlueprintEditor = nullptr;

	for (UObject* Asset : EditedAssets)
	{
		if (UBlueprint* BP = Cast<UBlueprint>(Asset))
		{
			if (IAssetEditorInstance* Editor = AssetEditorSubsystem->FindEditorForAsset(BP, false))
			{
				// Safe: outer loop guarantees Asset is UBlueprint, so Editor is FBlueprintEditor
				FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(Editor);
				if (BPEditor && BPEditor->GetSelectedNodes().Num() > 0)
				{
					CurrentBP = BP;
					BlueprintEditor = BPEditor;
					break;
				}
				if (!CurrentBP)
				{
					CurrentBP = BP;
					BlueprintEditor = BPEditor;
				}
			}
		}
	}

	if (!CurrentBP)
	{
		ResultText->SetText(FText::FromString(TEXT("Error: No blueprint is currently open")));
		return FReply::Handled();
	}

	if (!BlueprintEditor)
	{
		ResultText->SetText(FText::FromString(TEXT("Error: Cannot access blueprint editor")));
		return FReply::Handled();
	}


	TSet<UObject*> SelectedObjects = BlueprintEditor->GetSelectedNodes();
	TArray<UEdGraphNode*> SelectedNodes;
	UEdGraph* CurrentGraph = nullptr;

	for (UObject* Obj : SelectedObjects)
	{
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(Obj))
		{
			SelectedNodes.Add(Node);
			if (!CurrentGraph)
			{
				CurrentGraph = Node->GetGraph();
			}
		}
	}

	if (!CurrentGraph || SelectedNodes.Num() == 0)
	{
		ResultText->SetText(FText::FromString(TEXT("Error: No nodes selected in blueprint graph")));
		return FReply::Handled();
	}


	FString GeneratedCode;
	if (FWeaveGenerator::Generate(SelectedNodes, CurrentGraph, GeneratedCode))
	{
		CodeInputBox->SetText(FText::FromString(GeneratedCode));
		ResultText->SetText(FText::FromString(FString::Printf(
			TEXT("Generated Weave code from %d selected node(s)"), SelectedNodes.Num())));
	}
	else
	{
		ResultText->SetText(FText::FromString(TEXT("Error: Failed to generate Weave code")));
	}

	return FReply::Handled();
}

void SWeaverDebugger::TriggerGenerateFromSelection()
{
	OnGenerateFromSelection();
}
