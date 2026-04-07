🌐[英語](README.en.md)\|[日本語](README.ja.md)\|[韓国語](README.ko.md)\|**中文**

## 織り言語

AI に最適化されたグラフ レイアウト言語。現在 Blueprint AI で使用されています。

ブループリントには多くの機能がありますが、エスケープは`K2Node`テキスト、そうですね`LLM`かなりの負担ですよ。  
`Weave语言`是人和`LLM`全員がブループリントの読み書きができます。

このプラグインは、LLM とブループリントの間の相互作用の障壁を取り除き、AI とブループリントの間の相互作用のためのミドルウェアを作成することを目的としています。今後、マテリアルチャート、アニメーションチャート、コントロールリグチャートなどにも順次対応していく予定ですので、開発者の皆様と協力して開発していただければ幸いです。

### 特性

-   **クロスブループリント**: ブループリントを開かずに編集できます。
-   **可読性**：人間もAIも理解できる
-   **双方向変換**：BlueprintとWeave間のロスレス変換（アップデート中）
-   **パッチの仕組み**: AI はファイル全体を書き換えるのではなく、「どこを変更する」と言うだけで済みます。
-   **スキーマ制約**：AIが存在しないノードや不正な接続を生成することは不可能
-   **トークンの効率的**: ブループリント データを直接読み取る場合と比べて時間を節約できます。`70%以上`トークン

### Weave Language 语法参考

1つ`.weave`ファイルには複数の宣言ブロックが含まれています。

    graphset <名称> <蓝图路径>
    graph <函数名>

    node ...
    link ...
    set ...

* * *

| キーワード      | 用途                                     |
| ---------- | -------------------------------------- |
| `graphset` | ブループリント リソースを宣言します。このブループリントはどこにありますか? |
| `graph`    | イベント グラフ/関数を宣言します。どのグラフがどのようなグラフであるか   |
| `node`     | ノードを宣言する                               |
| `link`     | ノードエンドポイントを接続します                       |
| `set`      | ノード属性値を設定する                            |

* * *

#### 例

##### ゲーム開始後、Sequenceを使用して4つのPrint Stringを順番に実行します。

-   このとき、/Game/MyActor2 ブループリントのイベントグラフが操作されます。

```weave
graphset MyActor2 /Game/MyActor2.MyActor2
graph EventGraph

node beginplay : event.Actor.ReceiveBeginPlay @ (-144, 16)
node sequence : special.Sequence @ (96, 16)
node print01 : call.KismetSystemLibrary.PrintString @ (384, 16)
node d : call.KismetSystemLibrary.PrintString @ (384, 160)
node e : call.KismetSystemLibrary.PrintString @ (384, 304)
node f : call.KismetSystemLibrary.PrintString @ (384, 448)

link beginplay.then -> sequence.execute
link sequence.then_0 -> print01.execute
link sequence.then_1 -> d.execute
link sequence.then_2 -> e.execute
link sequence.then_3 -> f.execute

set print01.WorldContextObject = self
set print01.InString = Print 1
set print01.bPrintToScreen = true
set print01.bPrintToLog = true
set print01.TextColor = (R=0.000000,G=0.660000,B=1.000000,A=1.000000)
set print01.Duration = 2.000000
set d.WorldContextObject = self
set d.InString = Print 2
set d.bPrintToScreen = true
set e.WorldContextObject = self
set e.InString = Print 3
set f.WorldContextObject = self
set f.InString = Print 4
```

### 使い方は？

拉取插件源代码，放到虚幻`cpp`プロジェクトのプラグインフォルダーにプロジェクトファイルを再生成し、コンパイル後に起動します。
\[Release] でリリース バージョンをダウンロードし、プロジェクトのプラグイン フォルダー (存在する場合) に直接置くこともできます。

#### クイックテスト

プラグインが正常にインストールされると、`Weaver`という単語が含まれるドロップダウン メニュー、メニューを開いた後にクリックします`Weave 生成解释调试器`開ける。ブループリント ノードを選択し、 をクリックします。`Generate from Selection`選択したブループリント チェーン全体を Weave 言語に翻訳できます。もちろんクリックしてください`Apply to Blueprint`対応するチャートを適用できます。

# Weave Language API 参考

すべての操作は UWeaveOperator を通じて呼び出されます。
まず電話する必要があります`GenerateWeaveLanguage`これにより、プラグインはすべてのイベント、関数、マクロ、タイプなどを再計算することになるため、Weave は信頼性の高い解析のためにこの関数によって生成されたスキーマを使用する必要があります。この関数は、エディターの起動後にデフォルトで 1 回呼び出されます。

### ブループリント Weave コードを取得する

FString GetBlueprintWeave(BlueprintPath, GraphName, EntryNode = TEXT(""))

ブループリントを Weave 言語に変換します。

パラメータ:

-   BlueprintPath: ブループリント パス (/Game/MyActor.MyActor など)
-   GraphName: 関数名またはイベント名 (TestFunction など)
-   EntryNode: オプション、エントリ ノードを指定します

戻り値: 織りコード文字列。

* * *

### 差分を適用

FString applyWeaveDiff(OriginalWeave, DiffCode, OutError)

Weave コードに Diff を適用し、変更された完全な Weave を返します。

パラメータ:

-   OriginalWeave: オリジナルの Weave コード
-   DiffCode: AI が生成した差分
-   OutError: エラー メッセージ、成功した場合は空

戻り値: 変更された完全な Weave コード。

* * *

### 差分の検証

FString DiffCheck(BlueprintPath、GraphName、DiffCode、OutError)

ブループリントを変更せずに、実際に適用する前に Diff エフェクトをプレビューします。

パラメータ:

-   BlueprintPath: ブループリント パス
-   GraphName: 関数名
-   DiffCode: AI が生成した差分
-   OutError: エラーメッセージ

戻り値: プレビューされた Weave コード。

* * *

### ブループリントに適用する

bool applyWeaveToBlueprintWithUndo(WeaveCode, BlueprintPath, GraphName, OutError)

元に戻すことをサポートするために、Weave コードをブループリントに書き込みます。

パラメータ:

-   WeaveCode: 完全な Weave コード
-   BlueprintPath: ブループリント パス
-   GraphName: 関数名
-   OutError: エラーメッセージ

戻り値: true は成功を示します。

* * *

### 生成 Weave

void GenerateWeaveLanguage()

現在のブループリントから Weave 言語を生成します。

* * *

### 検索ノード

TArray SearchNode(クエリ)

現在のブループリント内のノードを検索します。

パラメータ:

-   クエリ: 検索キーワード

戻り値: 一致するノードのリスト。

* * *

### 検索タイプ

TArray 検索タイプ(クエリ)

利用可能なノード タイプを検索します。

パラメータ:

-   クエリ: キーワードを入力します

戻り値: 一致する型のリスト。

* * *

### ブループリント変数を検索する

TArray SearchBlueprintVariables(BlueprintPath, Query)

ブループリント内の変数を検索します。

パラメータ:

-   BlueprintPath: ブループリント パス
-   クエリ: 変数名キーワード

戻り値: 一致する変数のリスト。

* * *

### コンテキスト変数の検索

TArray SearchContextVar(BlueprintPath, Query)

現在のコンテキストで使用可能な変数を検索します。

パラメータ:

-   BlueprintPath: ブループリント パス
-   クエリ: 変数名キーワード

戻り値: 一致する変数のリスト。

* * *

### 検索コンテキスト関数

TArray SearchContextFunctions(BlueprintPath、クエリ)

現在のコンテキストで利用可能な関数を検索します。

パラメータ:

-   BlueprintPath: ブループリント パス
-   クエリ: 関数名キーワード

戻り値: 一致する関数のリスト。

* * *

### リソースを検索する

TArray SearchAsset(クエリ、MaxResults = 20)

プロジェクト内のリソースを検索します。

パラメータ:

-   クエリ: リソース名のキーワード
-   MaxResults: 戻り値の最大数、デフォルトは 20

戻り値: 一致するリソース パスのリスト。

* * *

### リソース参照の取得

TArray GetAssetReferences(AssetPath, MaxResults = 20)

リソースの参照関係を取得します。

パラメータ:

-   AssetPath: リソースパス
-   MaxResults: 戻り値の最大数、デフォルトは 20

戻り値: このリソースを参照する他のリソースのリスト。

* * *

## 変数の操作

### 変数を変更する

bool ModifyVar(BlueprintPath, VarName, NewValue, OutError)

ブループリント変数の値を変更します。

パラメータ:

-   BlueprintPath: ブループリント パス
-   VarName: 変数名
-   NewValue: 新しい値
-   OutError: エラーメッセージ

戻り値: true は成功を示します。

* * *

### 変数の削除

bool DeleteVar(BlueprintPath, VarName, OutError)

ブループリント変数を削除します。

パラメータ:

-   BlueprintPath: ブループリント パス
-   VarName: 変数名
-   OutError: エラーメッセージ

戻り値: true は成功を示します。

* * *

## ノードの操作

### ノードの取得

FString GetNodeById(Id)

IDでノード情報を取得します。

パラメータ:

-   ID: ノードID

戻り値: ノードの JSON 文字列。

* * *

### カテゴリごとにノードを取得する

TArray GetNodesByCategory(カテゴリ)

カテゴリごとにノードのリストを取得します。

パラメータ:

-   カテゴリ: カテゴリ名

戻り値: ノードリスト。

* * *

### ノードの追加

void AddNodeFromJson(JsonString)

JSON 経由でノードを追加します。

パラメータ:

-   JsonString: ノードの JSON 説明

* * *

### ノードの削除

bool 削除ノード(ID)

指定したノードを削除します。

パラメータ:

-   ID: ノードID

戻り値: true は成功を示します。

* * *

### すべてのノードを取得する

TArray GetAllNodesAsJson()

現在のすべてのノードの JSON リストを取得します。

戻り値: すべてのノードの JSON 文字列リスト。

* * *

### ノードをクリアする

void ClearNodes()

現在のノードをすべてクリアします。

* * *

### ノード数を取得する

int32 GetNodeCount()

戻り値: 現在のノードの総数。

* * *

### ブループリントに差分を適用

bool applyDiff(OutError)

ステージングされた Diff をブループリントに適用します。

パラメータ:

-   OutError: エラーメッセージ

戻り値: true は成功を示します。

* * *

## 非同期インターフェース

void SearchNodeAsync(Query, OnComplete)

ブロックを避けるためにノードを非同期的に検索します。

パラメータ:

-   クエリ: 検索キーワード
-   OnComplete: コールバック関数、パラメータは検索結果です
