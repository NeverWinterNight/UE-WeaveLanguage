🌐[영어](README.en.md)\|[일본어](README.ja.md)\|[한국어](README.ko.md)\|[**중국인**](README.md)

## 직조 언어

현재 Blueprint AI에서 사용되고 있는 AI에 최적화된 그래프 레이아웃 언어입니다.

블루프린트에는 많은 기능이 있지만 이스케이프는`K2Node`텍스트, 오른쪽`LLM`꽤 부담스럽네요.  
`Weave语言`그것은 사람과`LLM`모두가 청사진을 읽고 쓸 수 있습니다.

이 플러그인의 목표는 LLM과 Blueprint 간의 상호 작용 장벽을 열고 AI와 Blueprint 간의 상호 작용을 위한 미들웨어를 만드는 것입니다. 앞으로는 재료 차트, 애니메이션 차트, Control Rig 차트 등을 점차적으로 지원할 예정입니다. 개발자가 함께 협력하여 개발할 수 있기를 바랍니다.

### 특성

-   **교차 청사진**: 도면을 열지 않고도 편집이 가능합니다.
-   **가독성**: 인간과 AI 모두 이해할 수 있다
-   **양방향 변환**: Blueprint와 Weave 간 무손실 변환(업데이트 중)
-   **패치 메커니즘**: AI는 전체 파일을 다시 작성하는 대신 "위치 변경"만 말하면 됩니다.
-   **스키마 제약**: AI가 존재하지 않는 노드나 불법적인 연결을 생성하는 것은 불가능합니다.
-   **토큰 효율성**: 설계도 데이터를 직접 읽는 것보다 시간이 절약됩니다.`70%以上`토큰

### Weave 언어 구문 참조

하나`.weave`파일에는 여러 선언 블록이 포함되어 있습니다.

    graphset <名称> <蓝图路径>
    graph <函数名>

    node ...
    link ...
    set ...

* * *

| 키워드        | 사용                              |
| ---------- | ------------------------------- |
| `graphset` | 청사진 리소스를 선언합니다. 이 청사진은 어디에 있나요? |
| `graph`    | 声明事件图/函数，具体图表是哪个                |
| `node`     | 노드 선언                           |
| `link`     | 노드 엔드포인트 연결                     |
| `set`      | 노드 속성 값 설정                      |

* * *

#### 예

##### 게임을 시작한 후 Sequence를 사용하여 4개의 Print String을 순차적으로 실행합니다.

-   이때 /Game/MyActor2 블루프린트의 이벤트 그래프가 동작합니다.

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

### 사용하는 방법?

플러그인 소스 코드를 가져와 Unreal에 넣습니다.`cpp`프로젝트의 플러그인 폴더에서 프로젝트 파일을 재생성하고 컴파일 후 시작합니다.
릴리스에서 릴리스 버전을 다운로드하여 프로젝트 플러그인 폴더(있는 경우)에 직접 넣을 수도 있습니다.

#### 빠른 테스트

플러그인이 성공적으로 설치되면`Weaver`단어가 있는 드롭다운 메뉴, 메뉴를 연 후 클릭`Weave 生成解释调试器`열다. 블루프린트 노드를 선택하고 클릭합니다.`Generate from Selection`선택한 블루프린트 체인 전체를 Weave 언어로 번역할 수 있습니다. 물론 클릭하세요.`Apply to Blueprint`해당 차트를 적용할 수 있습니다.

# Weave Language API 참조

모든 작업은 UWeaveOperator를 통해 호출됩니다.
먼저 전화하셔야 해요`GenerateWeaveLanguage`이로 인해 플러그인이 모든 이벤트, 함수, 매크로, 유형 등을 다시 계산하게 되므로 Weave는 안정적인 구문 분석을 위해 이 함수로 생성된 스키마를 사용해야 합니다. 이 함수는 기본적으로 편집기가 시작된 후 한 번 호출됩니다.

### 블루프린트 위브 코드 받기

FString GetBlueprintWeave(BlueprintPath, GraphName, EntryNode = TEXT(""))

청사진을 Weave 언어로 변환합니다.

매개변수:

-   BlueprintPath: /Game/MyActor.MyActor와 같은 블루프린트 경로
-   GraphName: TestFunction과 같은 함수 이름 또는 이벤트 이름
-   EntryNode: 선택 사항, 항목 노드를 지정합니다.

반환: Weave 코드 문자열.

* * *

### 차이점 적용

FString ApplyWeaveDiff(OriginalWeave, DiffCode, OutError)

Weave 코드에 Diff를 적용하여 수정된 Weave 전체를 반환합니다.

매개변수:

-   OriginalWeave: 원본 Weave 코드
-   DiffCode: AI가 생성한 Diff
-   OutError: 오류 메시지, 성공하면 비어 있음

반환: 수정된 전체 Weave 코드.

* * *

### 차이점 확인

FString DiffCheck(BlueprintPath, GraphName, DiffCode, OutError)

블루프린트를 수정하지 않고도 Diff 효과를 실제로 적용하기 전에 미리 볼 수 있습니다.

매개변수:

-   BlueprintPath: 청사진 경로
-   GraphName: 함수 이름
-   DiffCode: AI가 생성한 Diff
-   OutError: 오류 메시지

반환: 미리 본 Weave 코드.

* * *

### 청사진에 적용

bool ApplyWeaveToBlueprintWithUndo(WeaveCode, BlueprintPath, GraphName, OutError)

실행 취소를 지원하려면 Weave 코드를 블루프린트에 작성하세요.

매개변수:

-   WeaveCode: Weave 코드 완성
-   BlueprintPath: 청사진 경로
-   GraphName: 함수 이름
-   OutError: 오류 메시지

반환: true는 성공을 나타냅니다.

* * *

### 직조 생성

무효 생성WeaveLanguage()

현재 블루프린트에서 Weave 언어를 생성합니다.

* * *

### 검색 노드

TArray 검색 노드(쿼리)

현재 블루프린트에서 노드를 검색합니다.

매개변수:

-   쿼리: 검색 키워드

반환: 일치하는 노드 목록.

* * *

### 검색 유형

TArray 검색 유형(쿼리)

사용 가능한 노드 유형을 검색합니다.

매개변수:

-   쿼리: 유형 키워드

반환: 일치하는 유형의 목록.

* * *

### 청사진 변수 검색

TArray 검색BlueprintVariables(BlueprintPath, 쿼리)

Blueprint에서 변수를 검색합니다.

매개변수:

-   BlueprintPath: 청사진 경로
-   쿼리: 변수 이름 키워드

반환값: 일치하는 변수 목록.

* * *

### 컨텍스트 변수 검색

TArray SearchContextVar(BlueprintPath, 쿼리)

현재 컨텍스트에서 사용 가능한 변수를 검색합니다.

매개변수:

-   BlueprintPath: 청사진 경로
-   쿼리: 변수 이름 키워드

반환값: 일치하는 변수 목록.

* * *

### 검색 컨텍스트 기능

TArray SearchContextFunctions(BlueprintPath, 쿼리)

현재 컨텍스트에서 사용 가능한 기능을 검색합니다.

매개변수:

-   BlueprintPath: 청사진 경로
-   쿼리: 함수 이름 키워드

반환: 일치하는 함수 목록.

* * *

### 리소스 검색

TArray SearchAsset(쿼리, MaxResults = 20)

프로젝트에서 리소스를 검색합니다.

매개변수:

-   쿼리: 리소스 이름 키워드
-   MaxResults: 최대 반환 수, 기본값 20

반환: 일치하는 리소스 경로 목록.

* * *

### 리소스 참조 받기

TArray GetAssetReferences(AssetPath, MaxResults = 20)

리소스의 참조 관계를 가져옵니다.

매개변수:

-   AssetPath: 리소스 경로
-   MaxResults: 최대 반환 수, 기본값 20

반환: 이 리소스를 참조하는 다른 리소스의 목록입니다.

* * *

## 변수 조작

### 修改变量

bool ModifyVar(BlueprintPath, VarName, NewValue, OutError)

청사진 변수의 값을 수정합니다.

매개변수:

-   BlueprintPath: 청사진 경로
-   VarName: 변수 이름
-   NewValue: 새 값
-   OutError: 오류 메시지

반환: true는 성공을 나타냅니다.

* * *

### 변수 삭제

bool DeleteVar(BlueprintPath, VarName, OutError)

청사진 변수를 삭제합니다.

매개변수:

-   BlueprintPath: 청사진 경로
-   VarName: 변수 이름
-   OutError: 오류 메시지

반환: true는 성공을 나타냅니다.

* * *

## 노드 운영

### 노드 가져오기

FString GetNodeById(Id)

ID별로 노드 정보를 가져옵니다.

매개변수:

-   ID: 노드 ID

반환: 노드 JSON 문자열.

* * *

### 카테고리별로 노드 가져오기

TArray GetNodesByCategory(범주)

카테고리별로 노드 목록을 가져옵니다.

매개변수:

-   카테고리: 카테고리 이름

반환: 노드 목록.

* * *

### 노드 추가

무효 AddNodeFromJson(JsonString)

JSON을 통해 노드를 추가합니다.

매개변수:

-   JsonString: 노드에 대한 JSON 설명

* * *

### 노드 삭제

bool RemoveNode(Id)

지정된 노드를 삭제합니다.

매개변수:

-   ID: 노드 ID

반환: true는 성공을 나타냅니다.

* * *

### 모든 노드 가져오기

TArray GetAllNodesAsJson()

모든 현재 노드의 JSON 목록을 가져옵니다.

반환: 모든 노드의 JSON 문자열 목록.

* * *

### 노드 지우기

무효 ClearNodes()

현재 노드를 모두 지웁니다.

* * *

### 노드 수를 가져옵니다.

int32 GetNodeCount()

반환값: 현재 총 노드 수.

* * *

### 블루프린트에 Diff 적용

bool ApplyDiff(OutError)

블루프린트에 단계적 차이를 적용합니다.

매개변수:

-   OutError: 오류 메시지

반환: true는 성공을 나타냅니다.

* * *

## 비동기 인터페이스

무효 SearchNodeAsync(쿼리, OnComplete)

차단을 피하기 위해 노드를 비동기식으로 검색합니다.

매개변수:

-   쿼리: 검색 키워드
-   OnComplete: 콜백 함수, 매개변수는 검색 결과입니다.
