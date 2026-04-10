# Weave Language Syntax Specification v1.0

> Weave 是一种用于描述 Unreal Engine 蓝图图表的领域特定语言（DSL）。
> 它可以在文本与蓝图节点之间双向转换：生成（Blueprint → Weave）和还原（Weave → Blueprint）。

---

## ⚠ 编写 Weave 代码前必读 — 最重要的三条规则

### 规则一：执行链只能连接非纯节点

蓝图中有两类节点：
- **非纯节点**：有白色的 `execute` 输入引脚和 `then` 输出引脚。只有这些节点可以出现在执行链中。
- **纯节点**：没有 `execute`/`then` 引脚。它们只通过彩色数据线连接，蓝图引擎会在需要时自动计算它们。

**纯节点绝对不能出现在 `link X.then -> Y.execute` 的执行链中！** 这样做会直接报错。

判断方法：
| 类型 | 纯/非纯 | 能否用 execute/then |
|------|---------|-------------------|
| `VariableGet` | **纯** | ❌ 不能 |
| `VariableSet` | 非纯 | ✅ 可以 |
| `ValidatedGet` | 非纯 | ✅ 可以 |
| `special.Branch` / `special.Sequence` | 非纯 | ✅ 可以 |
| `special.Self` / `special.Knot` | **纯** | ❌ 不能 |
| `event.*` | 非纯（只有 then） | ✅ 可以 |
| `call.KismetMathLibrary.*`（所有数学/比较函数） | **纯** | ❌ 不能 |
| `call.KismetStringLibrary.*`（所有转换/字符串函数） | **纯** | ❌ 不能 |
| `call.KismetSystemLibrary.PrintString` | 非纯 | ✅ 可以 |
| `call.KismetSystemLibrary.Delay` | 非纯 | ✅ 可以 |
| `call.Actor.SetActorLocation` | 非纯 | ✅ 可以 |
| `call.Actor.GetActorLocation` | **纯** | ❌ 不能 |

**简单判断法**：
- **Get/读取/计算/比较 = 纯** → 只连数据线
- **Set/写入/打印/生成/销毁 = 非纯** → 放在执行链中

### 规则二：执行链的正确构建方式

执行链只串联非纯节点，纯节点通过数据线"挂"在执行链旁边：

```
# 正确的模式 ✅：
#
#   [纯节点A] ──数据──┐
#                      ▼
#   [event] ──then──> [VariableSet] ──then──> [Branch] ──then──> [PrintString]
#                      ▲                        ▲
#   [纯节点B] ──数据──┘   [纯比较节点] ──数据──┘(Condition)
#
# 错误的模式 ❌：
#   [event] ──then──> [VariableGet] ──then──> [Add_IntInt] ──then──> [Branch]
#                      ↑纯节点没有execute       ↑纯节点没有execute

# 具体代码示例——正确 ✅：
node tick : event.Actor.ReceiveTick @ (0, 0)
node getVal : VariableGet.BP_Test_C.Speed @ (200, 100)      # 纯节点，不在执行链中
node addCalc : call.KismetMathLibrary.Add_FloatFloat @ (400, 100)  # 纯节点
node setDist : VariableSet.BP_Test_C.Distance @ (400, 0)    # 非纯节点
node cmp : call.KismetMathLibrary.GreaterEqual_FloatFloat @ (600, 100)  # 纯节点
node branch : special.Branch @ (600, 0)                      # 非纯节点
node doStop : VariableSet.BP_Test_C.IsMoving @ (800, 0)     # 非纯节点

# 执行链：只连非纯节点
link tick.then -> setDist.execute
link setDist.then -> branch.execute
link branch.then -> doStop.execute

# 数据链：纯节点通过数据线连接
link getVal.Speed -> addCalc.A
link tick.DeltaSeconds -> addCalc.B
link addCalc.ReturnValue -> setDist.Distance
link setDist.Output_Get -> cmp.A
link cmp.ReturnValue -> branch.Condition
```

### 规则三：函数必须指定正确的所属类

`call.类名.函数名` 中的类名必须是函数实际定义的类，不能随意填写：
- 数学运算 → `KismetMathLibrary`
- 字符串处理 → `KismetStringLibrary`
- 系统功能（打印/延迟/射线检测） → `KismetSystemLibrary`
- Actor 方法（移动/旋转/销毁） → `Actor`
- 组件方法（样条线/碰撞体） → 对应组件类（如 `SplineComponent`）

**⚠️ 重要：绝对禁止使用 UE 内部类名！**

以下不是有效的 Weave schema ID，不要使用：
- ❌ `K2Node_Message` → 正确：`message.<InterfaceClass>.<FunctionName>`
- ❌ `K2Node_CallFunction` → 正确：`call.<ClassName>.<FunctionName>`
- ❌ `K2Node_VariableGet` → 正确：`VariableGet.<ClassName>.<VarName>`
- ❌ `K2Node_IfThenElse` → 正确：`special.Branch`

如果不确定正确的 schema ID，使用 Generator 从现有蓝图生成 Weave 代码，然后参考输出的格式。

---

## 目录

1. [基本结构](#1-基本结构)
2. [关键字一览](#2-关键字一览)
3. [graphset — 蓝图声明](#3-graphset--蓝图声明)
4. [graph — 图表段落](#4-graph--图表段落)
5. [var — 变量声明](#5-var--变量声明)
6. [node — 节点声明](#6-node--节点声明)
7. [link — 连接声明](#7-link--连接声明)
8. [set — 设置默认值](#8-set--设置默认值)
9. [comment — 注释框](#9-comment--注释框)
10. [bubble — 节点评论气泡](#95-bubble--节点评论气泡)
11. [Schema ID 完整参考](#10-schema-id-完整参考)
11. [引脚命名规则](#11-引脚命名规则)
12. [变量类型参考](#12-变量类型参考)
13. [Tokenizer 规则](#13-tokenizer-规则)
14. [多图表模式](#14-多图表模式)
15. [常见错误与注意事项](#15-常见错误与注意事项)
16. [蓝图逻辑常见模式](#16-蓝图逻辑常见模式)
17. [完整示例](#17-完整示例)

---

## 1. 基本结构

一个 Weave 脚本的基本结构如下：

```
graphset <BlueprintName> <BlueprintAssetPath>

var <VarName> : <Type> [= <DefaultValue>] [editable] [readonly] [spawn] [category:"分类"] "描述"

graph <GraphName>

node <NodeId> : <SchemaId> @ (<X>, <Y>)
set <NodeId>.<PinName> = <Value>
link <FromNode>.<FromPin> -> <ToNode>.<ToPin>
comment "<Text>" @ (<X>, <Y>) size (<W>, <H>)
bubble <NodeId> "<Text>"
```

**行注释**使用 `#`：

```
# 这是一行注释
node a : call.KismetSystemLibrary.PrintString  # 行尾注释
```

---

## 2. 关键字一览

| 关键字 | 用途 | 必需 |
|--------|------|------|
| `graphset` | 声明目标蓝图名称和资产路径 | 可选 |
| `graph` | 开始一个图表段落 | **必需**（至少一个） |
| `var` | 声明蓝图变量（全局共享） | 可选 |
| `node` | 声明蓝图节点 | 可选 |
| `link` | 连接两个引脚 | 可选 |
| `set` | 设置引脚默认值或节点属性 | 可选 |
| `comment` | 创建注释框 | 可选 |
| `bubble` | 为节点添加评论气泡 | 可选 |

---

## 3. graphset — 蓝图声明

```
graphset <DisplayName> <AssetPath>
```

- `DisplayName`：蓝图的显示名称（一个 token）
- `AssetPath`：蓝图的完整资产路径

**示例：**
```
graphset BP_Player /Game/Blueprints/BP_Player.BP_Player
```

> **注意**：`graphset` 是可选的。如果省略，需要由调用方（Debugger 面板或 API）提供蓝图上下文。

---

## 4. graph — 图表段落

```
graph <GraphName>
graph <GraphName>(<Param1>: <Type1>, <Param2>: <Type2>) -> <ReturnType>
graph <GraphName>(<Param1>: <Type1>) -> (<Out1>: <Type1>, <Out2>: <Type2>)
```

- `GraphName`：目标图表的名称（一个 token）
- `(<Params>)`：可选，函数输入参数列表，每个参数格式为 `名称: 类型`
- `-> <Type>`：可选，单返回值类型（引脚名自动为 `ReturnValue`）
- `-> (<Outputs>)`：可选，多返回值列表

**常见图表名：**
- `EventGraph` — 事件图表（最常用）
- `UserConstructionScript` — 构造脚本
- 任意自定义函数名，如 `MyFunction`、`Initialize`

**示例：**
```
graph EventGraph

# 带参数的函数
graph GetColumnsAtDistance(Distance: float) -> int

# 多参数多返回值
graph CalculateOffset(ColumnIndex: int, TotalColumns: int) -> (Offset: float, bValid: bool)
```

> 如果目标蓝图中不存在该图表，且名称不包含 "EventGraph"，系统会自动创建同名函数图表。
> 参数类型支持所有 [变量类型](#12-变量类型参考)。

### result — 函数返回节点

当函数有返回值时，使用 `result` 节点连接返回值引脚：

```
node result : result
link someCalc.ReturnValue -> result.ReturnValue
link lastExec.then -> result.execute
```

`result` 节点映射到函数图表中的 `UK2Node_FunctionResult`，与 `entry` 类似不会新建节点。

---

## 5. var — 变量声明

```
var <VarName> : <Type>
var <VarName> : <Type> = <DefaultValue>
var <VarName> : <Type> = <DefaultValue> [属性...] "描述文本"
var <VarName> : <Type> [属性...] "描述文本"
```

变量声明是**全局的**，可以出现在任何 `graph` 之前或之间。变量会在目标蓝图中创建（如果尚不存在）。

- `= <DefaultValue>`（可选）：设置变量的默认值。值含空格或特殊字符时需要用引号包裹。
- `[属性...]`（可选）：任意顺序组合的属性关键字（见下方属性列表）。
- `"描述文本"`（可选）：设置变量的 tooltip 描述（蓝图编辑器详情面板中的 Description 字段）。

### 变量属性

| 关键字 | 作用 | 蓝图等价 |
|--------|------|----------|
| `editable` | 实例可编辑，在关卡中选中Actor可修改 | Instance Editable ✓ |
| `readonly` | 蓝图只读，只能Get不能Set | Blueprint Read Only ✓ |
| `spawn` | 生成时暴露，SpawnActor节点上显示引脚（自动隐含 `editable`） | Expose on Spawn ✓ |
| `category:"分类名"` | 变量分组，在 My Blueprint 面板中归类 | Category |

> **注意**：`spawn` 自动隐含 `editable`，因为 UE 要求 Expose on Spawn 的变量必须是 Instance Editable。写 `spawn` 时可以省略 `editable`。

```
# 需要在实例中调节的参数
var Health : float = 100.0 editable category:"战斗" "角色血量"
var MoveSpeed : float = 600.0 editable category:"移动"
var TeamColor : LinearColor editable category:"外观"

# 内部逻辑变量，不需要暴露
var InternalTimer : float category:"内部"

# 只读变量
var MaxHealth : float = 100.0 editable readonly category:"战斗"

# SpawnActor 时暴露的参数（spawn 自动包含 editable）
var SpawnLevel : int = 1 spawn category:"生成"
```

### 基础类型

```
var MyBool : bool
var MyInt : int
var MyInt64 : int64
var MyFloat : float
var MyDouble : double
var MyString : string
var MyText : text
var MyName : name
var MyByte : byte
```

### 带默认值

```
var Health : float = 100.0
var PlayerName : string = "Hero"
var IsAlive : bool = true
var SpawnCount : int = 5
var StartPos : Vector = "(X=0.0,Y=0.0,Z=100.0)"
var Speed : float = 600.0 "角色移动速度"
```

### 容器类型

使用 `array:`、`set:`、`map:` 前缀：

```
var MyArray : array:int
var MySet : set:string
var MyMap : map:string:int
```

> Map 类型格式：`map:<KeyType>:<ValueType>`

### 对象/结构体/枚举类型

```
var MyVector : Vector
var MyRotator : Rotator
var MyActor : AActor
var MyEnum : ECollisionChannel
var MyBPRef : /Game/Blueprints/BP_Enemy.BP_Enemy
var MyClassRef : class:AActor
```

> 详见 [变量类型参考](#12-变量类型参考)。

---

## 6. node — 节点声明

```
node <NodeId> : <SchemaId> @ (<X>, <Y>)
```

- `NodeId`：节点的唯一标识符（在 `link` 和 `set` 中引用）
- `SchemaId`：节点类型标识（详见 [Schema ID 参考](#10-schema-id-完整参考)）
- `@ (<X>, <Y>)`：节点在图表中的位置坐标

**特殊节点 ID：**
- `entry` — 函数图表的入口节点（自动映射到已有的 `FunctionEntry` 节点，不会新建）

**示例：**
```
node a : event.Actor.ReceiveBeginPlay @ (0, 0)
node b : call.KismetSystemLibrary.PrintString @ (300, 0)
node c : special.Branch @ (600, 0)
node entry : entry
```

> 如果 SchemaId 包含空格，需要用引号包裹：`node a : "call.My Class.My Function" @ (0, 0)`

---

## 7. link — 连接声明

```
link <FromNode>.<FromPin> -> <ToNode>.<ToPin>
```

- 从 `FromNode` 的**输出引脚** `FromPin` 连接到 `ToNode` 的**输入引脚** `ToPin`
- `->` 是连接方向符号

**引脚名包含空格时需加引号：**
```
link cast."As Pawn" -> target.self
link loop."Array Element" -> move.self
```

> **模糊匹配**：解释器支持引脚名模糊匹配——去除空格后不区分大小写比较。因此以下写法也能正确连接：
> ```
> link loop.ArrayElement -> move.self      # 自动匹配到 "Array Element"
> link loop.ArrayIndex -> mul.A            # 自动匹配到 "Array Index"
> ```
> 推荐使用引号写法（更准确），但不加引号的驼峰写法也可以工作。

### 执行链 vs 数据链

蓝图中有两种连接：

**执行链（白色线）**— 控制执行顺序：
```
link a.then -> b.execute       # a 执行完后执行 b
```

**数据链（彩色线）**— 传递数据：
```
link getHP.Health -> print.InString    # 将 Health 值传给 PrintString
```

### 重要规则

1. **执行输出引脚只能连接一个目标**。如需分支，使用 `special.Branch` 或 `special.Sequence`
2. **纯函数（如数学运算）没有执行引脚**，只需连接数据引脚即可自动计算
3. **禁止自连**：`FromNode` 和 `ToNode` 不能是同一个节点

---

## 8. set — 设置默认值

```
set <NodeId>.<PinName> = <Value>
```

设置节点引脚的默认值。

**引号规则：**
- 值包含空格、`.`、`=`、`(`、`)` 或与关键字同名时，**必须**用引号包裹
- 引号内的 `"` 用 `\"` 转义

```
set a.InString = "Hello World"
set a.Duration = "2.0"
set a.Count = 5
set a.bEnabled = true
```

### 特殊 set 语句

| 目标 | 用途 | 示例 |
|------|------|------|
| `nodeId.Expression` | MathExpression 表达式 | `set m.Expression = "A + B * 2"` |
| `nodeId.Class` | SpawnActor/ConstructObject 的类 | `set s.Class = class:BP_Enemy` |
| `nodeId.ComponentClass` | GetComponentByClass 的组件类 | `set g.ComponentClass = class:USplineComponent` |
| `nodeId.$timeline_float` | Timeline Float 轨道 | `set tl.$timeline_float = "Alpha,Beta"` |
| `nodeId.$timeline_event` | Timeline Event 轨道 | `set tl.$timeline_event = "OnFinished"` |
| `nodeId.$timeline_vector` | Timeline Vector 轨道 | `set tl.$timeline_vector = "Movement"` |
| `nodeId.$timeline_color` | Timeline Color 轨道 | `set tl.$timeline_color = "FadeColor"` |
| `nodeId.$timeline_length` | Timeline 总长度（秒） | `set tl.$timeline_length = "5.0"` |
| `nodeId.$timeline_autoplay` | Timeline 自动播放 | `set tl.$timeline_autoplay = true` |
| `nodeId.$timeline_loop` | Timeline 循环 | `set tl.$timeline_loop = true` |

### class: 值格式

`class:` 前缀用于设置类引脚的值，支持以下格式：

```
set s.Class = class:BP_Enemy                          # 蓝图类（短名称）
set s.Class = class:/Game/BP/BP_Enemy.BP_Enemy        # 蓝图类（完整路径）
set g.ComponentClass = class:USplineComponent         # C++ 类（带 U/A 前缀）
set g.ComponentClass = class:SplineComponent          # C++ 类（不带前缀，自动查找）
```

> **注意**：C++ 类名支持带或不带 `U`/`A` 前缀（如 `USplineComponent` 和 `SplineComponent` 均可）。

### 向量和旋转的简写

```
set a.Location = vec(100,200,300)
set a.Rotation = rot(0,90,0)
```

---

## 9. comment — 注释框

```
comment "<Text>" @ (<X>, <Y>) size (<W>, <H>) [color (<R>, <G>, <B>, <A>)] [fontsize <N>]
```

- `"Text"` — 注释文本（支持 `\n` 换行、`\\` 反斜杠、`\"` 引号转义）
- `@ (<X>, <Y>)` — 位置坐标（整数）
- `size (<W>, <H>)` — 尺寸（整数）
- `color (<R>, <G>, <B>, <A>)` — 颜色，**0-255 整数**（可选，默认白色）
- `fontsize <N>` — 字号（可选，默认 18）

**示例：**
```
comment "Init Variables" @ (0, -100) size (400, 200) color (50, 150, 255, 255) fontsize 14
comment "Line1\nLine2" @ (500, 0) size (300, 150)
```

---

## 9.5. bubble — 节点评论气泡

```
bubble <NodeId> "<Text>"
```

在指定节点上显示固定的评论气泡（Comment Bubble），用于在图表中直接标注关键节点的用途。

- `<NodeId>` — 目标节点的 ID（需要在 `node` 声明中先定义）
- `"<Text>"` — 气泡文本（支持 `\n` 换行转义）

**示例：**
```weave
node getHP : VariableGet.BP_Player_C.HP @ (0, 0)
node setHP : VariableSet.BP_Player_C.HP @ (300, 0)
bubble getHP "读取当前血量"
bubble setHP "扣血逻辑入口"
```

> 注意：bubble 是**节点级**功能，每个节点可以单独设置。与 `comment`（注释框）不同，bubble 附着在节点上方，随节点移动。

---

## 10. Schema ID 完整参考

Schema ID 是节点类型的唯一标识符，格式为点分隔的层级结构。

### 类名格式

Schema ID 中的类名支持两种格式：

- **短名称**（向后兼容）：`Actor`、`KismetSystemLibrary`、`BP_Test_C`
- **完整路径**（推荐，消除歧义）：`/Script/Engine.Actor`、`/Script/Engine.KismetSystemLibrary`、`/Game/BP/BP_Test.BP_Test_C`

Generator 默认输出完整路径格式，Interpreter 两种格式都支持。完整路径可以避免类名冲突和查找警告。

### 事件节点

```
event.<ClassName>.<EventName>
```

| 示例 | 说明 |
|------|------|
| `event./Script/Engine.Actor.ReceiveBeginPlay` | BeginPlay 事件（完整路径） |
| `event.Actor.ReceiveBeginPlay` | BeginPlay 事件（短名称，向后兼容） |
| `event./Script/Engine.Actor.ReceiveTick` | Tick 事件 |
| `event./Script/Engine.Actor.ReceiveActorBeginOverlap` | 碰撞开始 |
| `event./Script/Engine.Actor.UserConstructionScript` | 构造脚本 |

#### 接口事件实现

当蓝图实现了 Blueprint Interface 时，接口函数会以 `event` 节点的形式出现在事件图表中。Generator 输出格式为：

```
event.<BlueprintClassName>.<InterfaceFunctionName>
```

| 示例 | 说明 |
|------|------|
| `event.BP_Character_Test_C.Trigger` | BP_Character_Test 实现的 BPI_ObjectInterface.Trigger |
| `event./Game/BP/BP_Test.BP_Test_C.OnInteract` | 接口事件实现（完整路径） |

Interpreter 会自动检测该函数是否定义在蓝图实现的接口中。如果是，创建 `UK2Node_Event` 并将 `EventReference` 指向接口类（而非蓝图类本身），确保 UE 正确识别为接口事件实现。

**与 customEvent 的区别**：接口事件实现会继承接口函数签名中的参数引脚（如 `BackActor`），而 `customEvent` 不会自动拥有这些参数。

> **注意**：不要将接口事件实现写成 `customEvent.*`，否则会与接口函数同名冲突，导致编译错误"找到了多个同命名函数"。

### 自定义事件

```
customEvent.<EventName>
```

用于创建蓝图自定义事件（UK2Node_CustomEvent）。自定义事件在蓝图中注册为可调用函数，其他节点可通过 `call.BP_XXX_C.EventName` 调用。

| 示例 | 说明 |
|------|------|
| `customEvent.OnHit` | 名为 OnHit 的自定义事件 |
| `customEvent.DoAttack` | 名为 DoAttack 的自定义事件 |

**与 event 的区别：**
- `event.*` — 引擎/父类定义的事件（如 BeginPlay、Tick），每个蓝图中只能有一个
- `customEvent.*` — 用户自定义事件，可以有多个，编译后成为蓝图的可调用函数

**自动处理：**
- Interpreter 会先创建所有 customEvent 节点并编译蓝图，然后再创建 call 节点
- 这确保 `call.BP_XXX_C.EventName` 能找到对应的函数，不会误创建同名函数图表

### 组件绑定事件

当蓝图中的组件变量绑定了委托事件（如碰撞、重叠、自定义委托）时，使用 `componentEvent`：

```
componentEvent.<ComponentVarName>.<DelegateOwnerClass>.<DelegateName>
```

- `ComponentVarName`：蓝图中组件变量的名称
- `DelegateOwnerClass`：委托所定义的类名（支持完整路径）
- `DelegateName`：委托属性名

| 示例 | 说明 |
|------|------|
| `componentEvent.Box./Script/Engine.PrimitiveComponent.OnComponentBeginOverlap` | Box 组件的碰撞开始事件（完整路径） |
| `componentEvent.Box.PrimitiveComponent.OnComponentBeginOverlap` | Box 组件的碰撞开始事件（短名称） |
| `componentEvent.InterfaceComp.BPAC_InterfaceComponent_C.地贴UI重叠事件__DelegateSignature` | 自定义组件的委托事件 |

> **注意**：不要把组件绑定事件写成 `event.BP_XXX_C.DelegateName`，这会丢失组件信息并变成蓝图自身的事件。

### 函数调用

```
call.<ClassName>.<FunctionName>
```

ClassName 支持短名称和完整路径两种格式：
```
call.KismetSystemLibrary.PrintString                          # 短名称（向后兼容）
call./Script/Engine.KismetSystemLibrary.PrintString           # 完整路径（推荐）
call./Game/BP/BP_Test.BP_Test_C.MyFunction                    # 蓝图类完整路径
```

**重要：ClassName 必须是函数实际所属的类，不是随意填写的！如果不确定函数属于哪个类，请参考下方的常用函数速查表。**

> **纯函数**（如数学运算、转换函数）没有 execute/then 引脚，不能放在执行链中。
> **非纯函数**（如 PrintString、Delay、SetActorLocation）有 execute/then 引脚。

#### 纯节点 vs 非纯节点速查

以下节点类型是**纯节点（无 execute/then 引脚）**，只能通过数据引脚连接：
- `VariableGet` — 变量读取
- `special.Self` — Self 引用
- `special.Knot` — 重路由节点
- `KismetMathLibrary` 的所有函数 — 数学运算、比较运算
- `KismetStringLibrary` 的大部分函数 — 字符串操作、类型转换
- `call.*.Get*` — 大部分 Getter 函数（如 GetActorLocation）
- 任何返回值但不改变状态的函数

以下节点类型是**非纯节点（有 execute/then 引脚）**，必须放在执行链中：
- `VariableSet` — 变量写入
- `ValidatedGet` — 验证后变量读取
- `event.*` — 事件节点（只有 then，无 execute）
- `special.Branch` — 条件分支
- `special.Sequence` — 执行序列
- `special.Cast.*` — 类型转换
- `call.KismetSystemLibrary.PrintString` — 打印（非纯）
- `call.KismetSystemLibrary.Delay` — 延迟（非纯）
- `call.Actor.SetActorLocation` — 设置位置（非纯）
- `call.Actor.SetActorRotation` — 设置旋转（非纯）
- 任何以 `Set*` 开头或会改变游戏状态的函数

#### 常用函数速查表（按所属类分组）

**KismetMathLibrary（纯函数，无 execute/then）**：
| 函数名 | 说明 |
|--------|------|
| `Add_IntInt` / `Add_FloatFloat` | 加法（Float 自动转 Double） |
| `Subtract_IntInt` / `Subtract_FloatFloat` | 减法 |
| `Multiply_IntInt` / `Multiply_FloatFloat` | 乘法 |
| `Divide_IntInt` / `Divide_FloatFloat` | 除法 |
| `Greater_FloatFloat` / `GreaterEqual_FloatFloat` | 大于/大于等于 |
| `Less_FloatFloat` / `LessEqual_FloatFloat` | 小于/小于等于 |
| `EqualEqual_FloatFloat` / `NotEqual_FloatFloat` | 等于/不等于 |
| `And_BoolBool` / `Or_BoolBool` / `Not_PreBool` | 布尔运算 |
| `Abs` / `FClamp` / `FMin` / `FMax` | 绝对值/限制/最小/最大 |
| `Sin` / `Cos` / `Tan` / `Sqrt` | 三角函数/平方根 |
| `RandomIntegerInRange` / `RandomFloatInRange` | 随机数 |
| `VSize` / `Normal` / `Dot_VectorVector` | 向量运算 |
| `MakeVector` / `BreakVector` | 向量构造/拆解 |

**KismetSystemLibrary（部分纯、部分非纯）**：
| 函数名 | 纯/非纯 | 说明 |
|--------|---------|------|
| `PrintString` | 非纯 | 打印字符串到屏幕/日志 |
| `Delay` | 非纯（Latent） | 延迟执行 |
| `IsValid` | 纯 | 对象有效性检查 |
| `GetDisplayName` | 纯 | 获取显示名称 |
| `LineTraceSingle` | 非纯 | 射线检测 |
| `BoxOverlapActors` | 非纯 | 盒体重叠检测 |
| `SphereOverlapActors` | 非纯 | 球体重叠检测 |
| `SetTimer` / `ClearTimer` | 非纯 | 定时器 |
| `GetClassName` | 纯 | 获取类名 |

**KismetStringLibrary（纯函数）**：
| 函数名 | 说明 |
|--------|------|
| `Conv_IntToString` / `Conv_FloatToString` | 数字转字符串 |
| `Conv_StringToInt` / `Conv_StringToFloat` | 字符串转数字 |
| `Conv_BoolToString` | 布尔转字符串 |
| `Concat_StrStr` | 字符串拼接 |
| `Len` | 字符串长度 |
| `Contains` | 字符串包含检查 |

**GameplayStatics（混合）**：
| 函数名 | 纯/非纯 | 说明 |
|--------|---------|------|
| `GetPlayerController` | 纯 | 获取玩家控制器 |
| `GetPlayerCharacter` | 纯 | 获取玩家角色 |
| `GetGameInstance` | 纯 | 获取游戏实例 |
| `GetAllActorsOfClass` | 非纯 | 获取所有指定类的Actor |
| `SpawnSoundAtLocation` | 非纯 | 播放声音 |
| `OpenLevel` | 非纯 | 打开关卡 |
| `SetGamePaused` | 非纯 | 暂停游戏 |
| `GetWorldDeltaSeconds` | 纯 | 获取帧时间 |

**Actor（对象方法，大部分非纯）**：
| 函数名 | 纯/非纯 | 说明 |
|--------|---------|------|
| `GetActorLocation` | 纯 | 获取位置 |
| `GetActorRotation` | 纯 | 获取旋转 |
| `SetActorLocation` | 非纯 | 设置位置 |
| `SetActorRotation` | 非纯 | 设置旋转 |
| `SetActorTransform` | 非纯 | 设置变换 |
| `GetDistanceTo` | 纯 | 获取到另一个Actor的距离 |
| `GetComponentByClass` | 非纯 | 获取指定类的组件（需 `self` 引脚，**不是** `Target`） |
| `Destroy` | 非纯 | 销毁 Actor |
| `SetLifeSpan` | 非纯 | 设置生命周期 |

**SceneComponent / PrimitiveComponent**：
| 函数名 | 所属类 | 纯/非纯 | 说明 |
|--------|--------|---------|------|
| `GetWorldLocation` | SceneComponent | 纯 | 获取世界位置 |
| `SetWorldLocation` | SceneComponent | 非纯 | 设置世界位置 |
| `GetRelativeLocation` | SceneComponent | 纯 | 获取相对位置 |
| `SetRelativeLocation` | SceneComponent | 非纯 | 设置相对位置 |
| `GetComponentVelocity` | SceneComponent | 纯 | 获取速度 |

**SplineComponent（样条线相关）**：
| 函数名 | 纯/非纯 | 说明 |
|--------|---------|------|
| `GetSplineLength` | 纯 | 获取样条线总长度 |
| `GetLocationAtDistanceAlongSpline` | 纯 | 根据距离获取位置 |
| `GetRotationAtDistanceAlongSpline` | 纯 | 根据距离获取旋转 |
| `GetLocationAtSplinePoint` | 纯 | 获取样条点位置 |
| `GetNumberOfSplinePoints` | 纯 | 获取样条点数量 |

**CharacterMovementComponent**：
| 函数名 | 纯/非纯 | 说明 |
|--------|---------|------|
| `SetMovementMode` | 非纯 | 设置移动模式 |
| `GetMaxSpeed` | 纯 | 获取最大速度 |

**重要提示：如果你不确定一个函数属于哪个类，请遵循以下规则：**
1. **数学运算**（加减乘除、比较、三角函数）→ `KismetMathLibrary`
2. **字符串操作**（转换、拼接、查找）→ `KismetStringLibrary`
3. **系统功能**（打印、延迟、射线检测）→ `KismetSystemLibrary`
4. **游戏全局**（获取玩家、打开关卡、暂停）→ `GameplayStatics`
5. **操作特定对象**（Actor移动/旋转/销毁）→ 该对象的类（如 `Actor`）
6. **操作特定组件**（样条线、碰撞体、Mesh）→ 该组件的类（如 `SplineComponent`）
7. **自定义蓝图函数** → `BP_类名_C`

### 消息节点（接口调用）

```
message.<InterfaceClassName>.<FunctionName>
```

用于调用 Blueprint Interface 定义的函数。与 `call.*` 不同，消息节点通过接口进行多态调用，目标对象不需要是特定类。

| 示例 | 说明 |
|------|------|
| `message.BPI_ObjectInterface_C.Trigger` | 调用接口的 Trigger 函数（短名称） |
| `message./Game/BP/Interface/BPI_ObjectInterface.SKEL_BPI_ObjectInterface_C.Trigger` | 完整路径 |

**引脚：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 执行输出 | Output | `then` |
| 目标对象 | Input | `self`（要调用接口函数的目标对象） |
| 函数参数 | Input | 接口函数定义的参数名 |

**接口事件 vs 接口消息的区别：**
- `event.<BPClass>.<FuncName>` — **实现**接口函数（在自身蓝图中响应接口调用）
- `message.<InterfaceClass>.<FuncName>` — **调用**接口函数（向其他对象发送接口消息）

**配合 DoesImplementInterface 使用：**

通常在调用接口消息前，先检查目标对象是否实现了该接口：

```
node check : call./Script/Engine.KismetSystemLibrary.DoesImplementInterface @ (0, 100)
node branch : special.Branch @ (200, 0)
node msg : message.BPI_ObjectInterface_C.Trigger @ (400, 0)

set check.Interface = "/Game/BP/Interface/BPI_ObjectInterface.BPI_ObjectInterface_C"

link someActor.ReturnValue -> check.TestObject
link check.ReturnValue -> branch.Condition
link branch.then -> msg.execute
link someActor.ReturnValue -> msg.self
```

### 宏节点

```
macro.StandardMacros.<MacroName>        # 引擎内置宏
macro.<BlueprintPath>:<MacroName>       # 自定义宏
```

| 示例 | 说明 |
|------|------|
| `macro.StandardMacros.ForEachLoop` | ForEach 循环 |
| `macro.StandardMacros.IsValid` | 有效性检查 |

### 变量读取/写入

```
VariableGet.<ClassName>.<VarName>       # 读取变量（纯节点，无执行引脚）
ValidatedGet.<ClassName>.<VarName>      # 验证后读取变量（带执行引脚，自动检查有效性）
VariableSet.<ClassName>.<VarName>       # 写入变量
```

| 示例 | 说明 |
|------|------|
| `VariableGet.BP_Player_C.Health` | 读取 Health 变量（纯节点） |
| `ValidatedGet.BP_Player_C.MyActor` | 验证后读取 MyActor 变量（带 execute/then 引脚） |
| `VariableSet.BP_Player_C.Health` | 写入 Health 变量 |

> Self 变量使用短类名（如 `BP_Player_C`），外部类变量使用完整路径名。
> `ValidatedGet` 等同于在蓝图中右键 VariableGet → "Convert to Validated Get"，节点会带有执行引脚和 `IsValid` 输出。

**外部类变量访问：**

读取**其他蓝图**的变量时，ClassName 使用完整资产路径 + `_C` 后缀，且必须通过 `self` 引脚指定从哪个对象实例读取：

```
# 读取 BP_MoveRoad 上的 Spline 变量
node getRoad : VariableGet.BP_Test_C.MoveRoad @ (0, 200)
node getSpline : VariableGet./Game/BP/BP_MoveRoad.BP_MoveRoad_C.Spline @ (300, 200)****

link getRoad.MoveRoad -> getSpline.self    # 必须连接 self：指定从哪个 MoveRoad 实例读取
link getSpline.Spline -> someFunc.self     # 读到的 SplineComponent 可直接使用
```

> **注意**：读取自身变量（`VariableGet.BP_Test_C.XXX`）不需要 self 连接，读取外部对象变量时**必须**提供 self。

### 特殊节点

| Schema ID | 说明 |
|-----------|------|
| `entry` | 函数入口节点（映射到已有节点） |
| `special.Branch` | 条件分支（if/else） |
| `special.Sequence` | 执行序列 |
| `special.MathExpression` | 数学表达式 |
| `special.Make.<StructName>` | 构造结构体（如 `special.Make.FVector`） |
| `special.Break.<StructName>` | 拆解结构体（如 `special.Break.FRotator`） |
| `special.SpawnActorFromClass` | 生成 Actor |
| `special.ConstructObjectFromClass` | 构造对象 |
| `special.Cast.<TypePath>` | 类型转换（如 `special.Cast./Script/Engine.Pawn`） |
| `special.SwitchEnum.<EnumName>` | 枚举 Switch |
| `special.GetArrayItem` | 获取数组元素 |
| `special.Knot` | 重路由节点（Reroute） |
| `special.Self` | Self 引用节点（获取自身引用） |
| `special.Timeline.<TimelineName>` | 时间轴节点 |

### 时间轴节点

```
special.Timeline.<TimelineName>
```

时间轴节点用于创建蓝图时间轴。Generator 会自动导出时间轴的轨道信息和属性作为特殊的 `$timeline_*` set 语句。

**时间轴 set 语句（`$timeline_*`）：**

| set 语句 | 说明 | 示例 |
|----------|------|------|
| `$timeline_float` | Float 轨道名称（逗号分隔多个） | `set tl.$timeline_float = "Alpha,Beta"` |
| `$timeline_event` | Event 轨道名称 | `set tl.$timeline_event = "OnFinished"` |
| `$timeline_vector` | Vector 轨道名称 | `set tl.$timeline_vector = "Movement"` |
| `$timeline_color` | LinearColor 轨道名称 | `set tl.$timeline_color = "FadeColor"` |
| `$timeline_length` | 时间轴总长度（秒） | `set tl.$timeline_length = "5.0"` |
| `$timeline_autoplay` | 是否自动播放 | `set tl.$timeline_autoplay = true` |
| `$timeline_loop` | 是否循环 | `set tl.$timeline_loop = true` |

**完整示例：**
```
node tl : special.Timeline.MyTimeline @ (0, 0)
set tl.$timeline_float = "Alpha"
set tl.$timeline_event = "Finished"
set tl.$timeline_length = "5.0"
set tl.$timeline_autoplay = true
set tl.$timeline_loop = false
```

**时间轴引脚：**
- 执行输入：`execute`（Play）
- 执行输出：`then`（已过时，一般不用）
- 方向控制：`Play`、`PlayFromStart`、`Stop`、`Reverse`、`ReverseFromEnd`（执行输入）
- Float 轨道输出：`<TrackName>`（Float 值）
- Event 轨道输出：`<TrackName>`（执行输出）
- `Direction`：输出当前播放方向
- `Finished`：播放完成时触发（执行输出）

**注意事项：**
- 如果蓝图中已存在同名时间轴，Interpreter 会复制现有模板（包括曲线数据）并重命名
- 如果不存在，Interpreter 会使用 `FBlueprintEditorUtils::AddNewTimeline` 创建新的时间轴
- 时间轴模板的 Outer 必须是 `UClass`（GeneratedClass），不能是 UBlueprint

### 异步动作节点

```
asyncAction.<FactoryClassPath>.<FunctionName>
```

用于异步动作节点（UK2Node_BaseAsyncTask 的子类），如第三方插件的异步节点。

- `FactoryClassPath`：工厂类的完整路径（如 `/Script/SequencerScripting.MovieSceneTrackExtensions`）
- `FunctionName`：工厂函数名称

| 示例 | 说明 |
|------|------|
| `asyncAction./Script/Engine.GameplayTask.RunAction` | 异步 Gameplay Task |

**引脚特点：**
- 有 `execute` 输入引脚
- 多个执行输出引脚（代表不同的异步回调，如 `OnSuccess`、`OnFail`）
- 数据输入/输出引脚

### 第三方插件节点（通用）

```
native.<ClassName>
```

用于第三方插件创建的自定义节点。`ClassName` 是 UE 内部的 C++ 类名（通常以 `K2Node_` 开头）。

| 示例 | 说明 |
|------|------|
| `native.K2Node_ForLoopWithDelay` | XTools 的延迟循环节点 |
| `native.K2Node_BlueprintAssistAction` | BlueprintAssist 自定义节点 |

**工作原理**：
- **Generator**（蓝图→Weave）：遇到未识别的节点类型时自动生成 `native.*` schema
- **Interpreter**（Weave→蓝图）：通过类名查找并创建节点，自动分配引脚

> **注意**：`native.*` 是通用兜底方案。某些需要特殊初始化的复杂自定义节点可能无法完美还原，但引脚连接和默认值会正常处理。如果常用的节点类型是 `call`/`event`/`special` 的标准变体，应优先使用对应的标准 schema。

---

## 11. 引脚命名规则

### 通用执行引脚

| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 执行输出 | Output | `then` |

### 各节点类型的引脚

**entry（函数入口）：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输出 | Output | `then` |

**event 节点：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输出 | Output | `then` |
| 事件参数 | Output | 参数名（如 `DeltaSeconds`） |

**call 节点（非纯函数）：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 执行输出 | Output | `then` |
| 函数参数 | Input | 参数名（如 `InString`、`Duration`） |
| 返回值 | Output | `ReturnValue` |
| self 引用 | Input | `self` |

**call 节点（纯函数，如数学运算）：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 函数参数 | Input | 参数名（如 `A`、`B`） |
| 返回值 | Output | `ReturnValue` |

> **纯函数没有 execute/then 引脚！** 不要对纯函数使用执行链连接。

**special.Branch：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 条件 | Input | `Condition`（bool） |
| True 输出 | Output | `then` |
| False 输出 | Output | `else` |

**special.Sequence：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 序列输出 | Output | `then_0`、`then_1`、`then_2`... |

> 序列引脚会根据 link 引用自动创建。

**VariableGet：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 对象引用 | Input | `self`（仅读取**外部类**变量时出现，指定从哪个对象实例读取） |
| 数据输出 | Output | **变量名本身**（如变量名为 `Health`，引脚就是 `Health`） |

> **⚠️ VariableGet 的输出引脚名是变量名本身，不是 `ReturnValue`！** 例如 `VariableGet.BP_Test_C.TestHP` 的输出引脚是 `TestHP`，连接时应写 `link getHP.TestHP -> setHP.TestHP`。
> 读取自身蓝图变量时没有 `self` 引脚；读取其他蓝图的变量时**必须**连接 `self`。

**VariableSet：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 执行输出 | Output | `then` |
| 数据输入 | Input | **变量名本身**（如 `Health`） |
| 数据输出 | Output | `Output_Get` |

> **重要**：VariableSet 的输出引脚是 `Output_Get`，不是变量名！

**special.Cast：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 对象输入 | Input | `Object` |
| 成功输出 | Output | `then` |
| 失败输出 | Output | `CastFailed` |
| 转换结果 | Output | `"As <ClassName>"`（含空格，需引号） |

**macro.StandardMacros.ForEachLoop：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `Exec`（⚠️ 宏节点用 `Exec` 而非 `execute`） |
| 数组输入 | Input | `Array` |
| 循环体执行 | Output | `LoopBody` |
| 数组元素 | Output | `"Array Element"`（含空格） |
| 数组索引 | Output | `"Array Index"`（含空格） |
| 循环完成 | Output | `Completed` |

> 注意：`Array Element` 和 `Array Index` 的引脚名含空格，正式写法需要引号。也可以用不带空格的 `ArrayElement`、`ArrayIndex`，解释器会模糊匹配。

**macro.StandardMacros.ForEachLoopWithBreak：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `Exec` |
| 数组输入 | Input | `Array` |
| 循环体执行 | Output | `LoopBody` |
| 数组元素 | Output | `"Array Element"` |
| 数组索引 | Output | `"Array Index"` |
| 中断输入 | Input | `"Break"` |
| 循环完成 | Output | `Completed` |

**macro.StandardMacros.IsValid（宏版本）：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `Exec` |
| 对象输入 | Input | `"Input Object"` |
| 有效输出 | Output | `"Is Valid"` |
| 无效输出 | Output | `"Is Not Valid"` |

> **⚠️ 宏节点的执行引脚名与普通节点不同！**
> - 普通节点：执行输入 = `execute`，执行输出 = `then`
> - **宏节点**：执行输入 = `Exec`，其他引脚名由宏图表的 Tunnel 节点定义
>
> 解释器已内置 `execute` ↔ `Exec` 的别名映射，因此写 `execute` 也能自动匹配到 `Exec`。但为准确起见，宏节点建议使用 `Exec`。
>
> **通用规则**：宏节点的引脚名常含空格。如不确定，使用 Generator 从现有蓝图生成代码查看实际引脚名。

---

## 12. 变量类型参考

### 基础类型

| Weave 类型 | UE 类型 |
|-----------|---------|
| `bool` | Boolean |
| `int` | Integer |
| `int64` | Integer64 |
| `float` | Float |
| `double` | Double |
| `string` | String |
| `text` | Text |
| `name` | Name |
| `byte` | Byte |

### 结构体类型（直接使用结构体名称）

| Weave 类型 | UE 类型 |
|-----------|---------|
| `Vector` | FVector |
| `Rotator` | FRotator |
| `Transform` | FTransform |
| `LinearColor` | FLinearColor |
| `Vector2D` | FVector2D |

### 对象类型（使用前缀 + 类名）

| Weave 类型 | UE 类型 |
|-----------|---------|
| `AActor` | Actor |
| `UStaticMeshComponent` | StaticMeshComponent |
| `/Game/BP/BP_Enemy.BP_Enemy` | 蓝图对象引用 |

### 类引用类型

```
class:AActor
class:/Game/BP/BP_Enemy.BP_Enemy
```

### 容器类型

```
array:<ElementType>     # 数组
set:<ElementType>       # 集合
map:<KeyType>:<ValueType>  # 映射
```

---

## 13. Tokenizer 规则

了解 Tokenizer 规则有助于理解为什么某些值需要引号包裹。

### 分隔符字符

以下字符会被拆分为独立 token：

| 字符 | 说明 |
|------|------|
| `:` | 冒号（类型分隔符） |
| `=` | 等号（set 赋值） |
| `.` | 点号（引脚访问） |
| `(` `)` | 括号（坐标） |
| `,` | 逗号（坐标分隔） |
| `@` | at 符号（位置标记） |

### 特殊规则

- `->` 被识别为单个 token（连接方向符号）
- `-` 后面不跟 `>` 时正常累积（如 `-100` 是单个 token）
- `#` 开始行注释（直到行尾）
- `"..."` 引号字符串作为单个 token（内部 `\"` 转义）
- `$` 在普通 token 中合法（用于 `$timeline_*` 等特殊引脚名）
- `/` 在普通 token 中合法（用于完整类路径如 `/Script/Engine.Actor`）
- 空白字符分隔 token

### 为什么浮点数需要引号

因为 `.` 是分隔符，`3.14` 会被拆分为 `3`、`.`、`14` 三个 token。因此在 `set` 语句中，浮点数值必须用引号：

```
set a.Duration = "2.0"     # 正确
set a.Duration = 2.0       # 错误！会被拆分
```

### 为什么含空格的值需要引号

空白字符也是分隔符，不加引号的值会被按空白拆分，可能被误认为后续关键字：

```
set a.InString = "Hello World"    # 正确
set a.InString = Hello World      # 错误！"World" 可能被当作下一个语句
```

---

## 14. 多图表模式

一个 Weave 脚本可以包含多个 `graph` 段落，用于在同一脚本中定义自定义函数及其调用。

### 语法

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

var SharedVar : int                   # 变量是全局的

graph MyHelper                        # 第一个图表：自定义函数
node entry : entry
node a : call.KismetSystemLibrary.PrintString @ (300, 0)
link entry.then -> a.execute

graph EventGraph                      # 第二个图表：事件图表
node e : event.Actor.ReceiveBeginPlay
node f : call.BP_Test_C.MyHelper @ (300, 0)
link e.then -> f.execute
```

### 执行顺序

1. 按书写顺序依次处理每个 `graph` 段落
2. 每个段落处理后会编译蓝图（确保后续段落能引用新创建的函数）
3. **先定义被调用的函数，再定义调用方**

### 规则

- `var` 声明全局共享，可放在任何位置
- `node`、`link`、`set`、`comment` 属于当前 `graph` 段落
- 单 `graph` 脚本完全向后兼容，行为不变
- 不存在的函数图表会自动创建

---

## 15. 常见错误与注意事项

### 错误 1：对纯函数使用执行链

```
# 错误 ❌
node add : call.KismetMathLibrary.Add_IntInt @ (200, 0)
link a.then -> add.execute      # Add_IntInt 是纯函数，没有 execute 引脚！

# 正确 ✅
link a.then -> nextExecNode.execute    # 跳过纯函数，连接下一个有执行引脚的节点
link someOutput -> add.A               # 纯函数只需连数据引脚
link add.ReturnValue -> target.Input   # 自动在需要时计算
```

**纯函数列表（常见）**：所有 `KismetMathLibrary` 数学运算、比较运算、类型转换函数（`Conv_*`）、`KismetStringLibrary` 字符串操作等。

### 错误 1b：VariableGet 放在执行链中

```
# 错误 ❌ — VariableGet 是纯节点，没有 execute/then 引脚
node getHP : VariableGet.BP_Test_C.HP @ (200, 0)
link a.then -> getHP.execute     # 失败！VariableGet 没有 execute

# 正确 ✅ — VariableGet 只通过数据引脚连接，不参与执行链
node getHP : VariableGet.BP_Test_C.HP @ (200, 96)
link getHP.HP -> print.InString   # 纯数据连接，无需执行链

# 如果确实需要带执行引脚的变量获取（如对象引用需要有效性检查），使用 ValidatedGet：
node getActor : ValidatedGet.BP_Test_C.MyActor @ (200, 0)
link a.then -> getActor.execute   # ValidatedGet 有 execute/then
link getActor.then -> b.execute
link getActor.MyActor -> b.Target
```

### 错误 1c：函数归属类错误

**函数必须指定其实际所属类，不能随意填写。这是最常见的错误！**

```
# 错误 ❌ — SetActorLocation 不属于 KismetMathLibrary
node move : call.KismetMathLibrary.SetActorLocation @ (200, 0)

# 正确 ✅ — SetActorLocation 属于 Actor
node move : call.Actor.SetActorLocation @ (200, 0)

# 错误 ❌ — GetSplineLength 不属于 KismetMathLibrary
node len : call.KismetMathLibrary.GetSplineLength @ (200, 0)

# 正确 ✅ — GetSplineLength 属于 SplineComponent
node len : call.SplineComponent.GetSplineLength @ (200, 0)
```

> **判断规则**：如果函数操作的是某个特定对象（Actor、Component 等），它大概率属于那个对象的类，而非 KismetMathLibrary 或其他工具库。

### 错误 1d：GetComponentByClass 使用 Target 而非 self

**`GetComponentByClass` 是 Actor 的成员函数，对象输入引脚是 `self`，不是 `Target`！**`Target` 是接口消息节点专用的引脚名，普通成员函数一律使用 `self`。

```
# 错误 ❌ — 使用 Target 引脚（会触发错误的消息节点创建）
node getRoad : VariableGet.BP_Test_C.MoveRoad @ (500, 600)
node getComp : call.Actor.GetComponentByClass @ (700, 580)
link getRoad.MoveRoad -> getComp.Target      # Target 不存在！应该是 self

# 正确 ✅ — 使用 self 引脚
node getRoad : VariableGet.BP_Test_C.MoveRoad @ (500, 600)
node getComp : call.Actor.GetComponentByClass @ (700, 580)
set getComp.ComponentClass = class:USplineComponent
link getRoad.MoveRoad -> getComp.self         # 正确：成员函数用 self
link getComp.ReturnValue -> splineFunc.self   # 返回值连接到组件函数
```

> **规则**：只有 `message.<Interface>.<Function>` 节点使用 `Target` 引脚。所有 `call.<Class>.<Function>` 成员函数一律使用 `self`。

### 错误 1e：使用 UE 内部节点类名作为 Schema ID

**绝对不要使用 UE 内部的 C++ 节点类名作为 Schema ID！** 这些会被自动移除。

```
# 错误 ❌ — 使用内部类名
node v : K2Node_Message @ (0, 0)
node w : K2Node_CallFunction @ (0, 0)
node x : K2Node_IfThenElse @ (0, 0)

# 正确 ✅ — 使用 Weave schema ID
node v : message.MyInterface.MyFunction @ (0, 0)
node w : call.Actor.SetActorLocation @ (0, 0)
node x : special.Branch @ (0, 0)
```

> 完整的无效类名列表：`K2Node_Message`、`K2Node_CallFunction`、`K2Node_VariableGet`、`K2Node_VariableSet`、`K2Node_IfThenElse`、`K2Node_ExecutionSequence`、`K2Node_MacroInstance`、`K2Node_Event`、`K2Node_CustomEvent`、`K2Node_ComponentBoundEvent`、`K2Node_FunctionEntry`、`K2Node_FunctionResult`、`K2Node_DynamicCast`、`K2Node_MakeStruct`、`K2Node_BreakStruct`、`K2Node_SpawnActorFromClass`、`K2Node_SwitchEnum`、`K2Node_SwitchName`、`K2Node_Timeline`、`K2Node_MathExpression`。

### 错误 2：执行输出连接多个目标

```
# 错误 ❌ — 执行输出只能连一个目标
link a.then -> b.execute
link a.then -> c.execute

# 正确 ✅ — 使用 Sequence 节点
node seq : special.Sequence @ (200, 0)
link a.then -> seq.execute
link seq.then_0 -> b.execute
link seq.then_1 -> c.execute
```

### 错误 3：VariableSet 输出引脚名

```
# 错误 ❌
link setHP.Health -> print.InString      # VariableSet 的输出不是变量名

# 正确 ✅
link setHP.Output_Get -> print.InString  # 输出引脚名是 Output_Get
```

### 错误 4：浮点数和含空格字符串未加引号

```
# 错误 ❌
set a.Duration = 2.0
set a.InString = Hello World

# 正确 ✅
set a.Duration = "2.0"
set a.InString = "Hello World"
```

### 错误 5：Link 方向反了

```
# 错误 ❌ — 从输入引脚链接到输出引脚
link subtract.A -> getVar.MyVar

# 正确 ✅ — 从输出引脚链接到输入引脚
link getVar.MyVar -> subtract.A
```

### 错误 6：代码中混入自然语言或非法字符

Weave 代码是严格的 DSL，不是自然语言。**只有 `#` 开头的部分才是注释**，不能在代码中直接写思考过程。

```
# 错误 ❌ — 在代码中写了自然语言和非法字符
link getFactors.OffsetFactors -> forEach.Array? 不能直接连，需要按索引获取。
# "?" 不是合法的 token，后面的中文也不是注释，会导致解析失败！

# 错误 ❌ — 把思考过程写进代码
node factor : call.KismetArrayLibrary.Get @ (600, 800)   # 获取当前角色的偏移系数
# 实际上 ForEach 循环提供了 ArrayIndex，我们可以用 Get 节点从 OffsetFactors 数组中取元素。
node getFactorByIndex : call.KismetArrayLibrary.Get @ (600, 860)
# 上面声明了两个做同样事情的节点！factor 被废弃但没删除

# 正确 ✅ — 只保留最终方案，思考过程写在 # 注释中
# 用 ForEachLoop 的 ArrayIndex 从 OffsetFactors 数组中按索引获取偏移系数
node getFactorByIndex : call.KismetArrayLibrary.Get @ (600, 860)
link forEach.ArrayIndex -> getFactorByIndex.Index
```

### 错误 7：link/set 引用了未声明的节点

**每个出现在 `link` 或 `set` 中的节点 ID 必须先用 `node` 声明！**

```
# 错误 ❌ — getHalf 从未声明
node setHalf : VariableSet.BP_Test_C.HalfWidth @ (500, 0)
link getHalf.HalfWidth -> offsetLen.B     # getHalf 不存在！

# 正确 ✅ — 同时声明 VariableSet 和 VariableGet
node setHalf : VariableSet.BP_Test_C.HalfWidth @ (500, 0)
node getHalf : VariableGet.BP_Test_C.HalfWidth @ (500, 100)  # 声明对应的 Get 节点
link getHalf.HalfWidth -> offsetLen.B
```

> **常见陷阱**：VariableSet（设置变量）和 VariableGet（读取变量）是两种完全不同的节点。如果初始化时用了 `setSpeed`（VariableSet），后续读取时还需要单独声明 `getSpeed`（VariableGet）。

### 错误 8：重复声明 set 语句

```
# 错误 ❌ — 同一个引脚设置了两次
set rightVec.Z = "0.0"
# ... 其他代码 ...
set rightVec.Z = "0.0"    # 重复！后者覆盖前者

# 正确 ✅ — 每个引脚只 set 一次
set rightVec.Z = "0.0"
```

### 错误 9：自创不存在的 Schema 类型

**Weave 仅支持文档中列出的 Schema 类型。** 不存在的类型会被静默跳过，导致节点丢失。

```
# 错误 ❌ — 不存在的 Schema 类型
node a : literal.ActorClass /Game/BP/MyActor @ (0, 0)      # literal 不存在
node b : call.Array.Get @ (200, 0)                          # 不存在，数组取值用 special.GetArrayItem
node c : cast.BP_Enemy_C @ (400, 0)                         # cast 不是有效前缀

# 正确 ✅ — 使用标准 Schema
set getAllActors.ActorClass = class:/Game/BP/MyActor         # 类引脚用 set 设置
node b : special.GetArrayItem @ (200, 0)                     # 数组取值专用节点
node c : special.Cast./Game/BP/BP_Enemy.BP_Enemy_C @ (400, 0)  # Cast 完整路径
```

### 错误 10：事件节点当作 Self 引用使用

**事件节点只有 `then` 和事件参数输出引脚，没有 `self` 引脚。** 获取自身引用必须使用 `special.Self`。

```
# 错误 ❌ — 事件没有 self 引脚
node begin : event.Actor.ReceiveBeginPlay @ (0, 0)
link begin.self -> getLoc.self          # 失败！begin 没有 self 输出

# 正确 ✅ — 使用 special.Self 节点
node begin : event.Actor.ReceiveBeginPlay @ (0, 0)
node self : special.Self @ (100, 100)
link self.self -> getLoc.self           # Self 节点提供自身引用
```

### 错误 11：向 VariableGet 传入数据

**VariableGet 是纯节点，只有数据输出引脚（=变量名），不接受数据输入。** 唯一的可选输入是跨蓝图时的 `self` 引脚。

```
# 错误 ❌ — 试图向 VariableGet 写入数据
node setHP : VariableSet.BP_Test_C.HP @ (300, 0)
node getHP : VariableGet.BP_Test_C.HP @ (500, 0)
link setHP.Output_Get -> getHP.HP       # 失败！HP 是 getHP 的输出引脚

# 正确 ✅ — 直接使用 VariableSet 的 Output_Get 或声明新的 VariableGet
link setHP.Output_Get -> print.InFloat  # 用 Output_Get 直接获取刚设置的值
# 或
node getHP : VariableGet.BP_Test_C.HP @ (500, 100)
link getHP.HP -> print.InFloat          # 独立读取，无需从 Set 传入
```

### 错误 12：`special.Self` 输出引脚大小写错误

**`special.Self` 的输出引脚名是小写 `self`，不是大写 `Self`。**

```
# 错误 ❌ — 大写 Self
node selfNode : special.Self @ (100, 100)
link selfNode.Self -> getLoc.self          # Self 大写，连线失败！

# 正确 ✅ — 小写 self
node selfNode : special.Self @ (100, 100)
link selfNode.self -> getLoc.self          # 小写 self
```

### 错误 13：向量/旋转值使用字符串而非简写语法

**向量和旋转值必须使用 `vec()` / `rot()` 简写语法，不能用逗号分隔的字符串。**

```
# 错误 ❌ — 字符串格式不会被解析为结构体
set node.Location = "100, 200, 300"
set node.Rotation = "0, 90, 0"

# 正确 ✅ — 使用 Weave 简写语法
set node.Location = vec(100, 200, 300)
set node.Rotation = rot(0, 90, 0)
```

### 错误 14：用 `var` 声明组件引用

**UE 中组件（SplineComponent、StaticMeshComponent 等）是通过蓝图编辑器 AddComponent 添加的，不是通过 `var` 声明的变量。** Weave 的 `var` 用于声明蓝图变量，不用于组件。

```
# 错误 ❌ — 组件不是变量
var SplineComp : USplineComponent

# 正确 ✅ — 通过 GetComponentByClass 获取组件引用
node getSpline : call.Actor.GetComponentByClass @ (500, 100)
set getSpline.ComponentClass = class:USplineComponent
link selfNode.self -> getSpline.self
link getSpline.ReturnValue -> splineFunc.self
```

### 错误 15：多图表时函数定义顺序（原错误 9）

```
# 错误 ❌ — 先调用后定义，编译时找不到函数
graph EventGraph
node f : call.BP_Test_C.MyFunc @ (300, 0)

graph MyFunc
node entry : entry

# 正确 ✅ — 先定义后调用
graph MyFunc
node entry : entry

graph EventGraph
node f : call.BP_Test_C.MyFunc @ (300, 0)
```

---

## 16. 蓝图逻辑常见模式

本章展示常见的蓝图逻辑应如何用 Weave 正确表达。**编写 Weave 代码前请仔细阅读这些模式。**

### 模式 1：一个输入引脚只能接收一条连接

蓝图中，每个输入引脚只能有一条连线。如果需要多个值参与计算，必须用中间节点串联：

```
# 错误 ❌ — 两条线连到同一个输入引脚
link a.Value -> add.B
link b.Value -> add.B        # 覆盖了上一条！

# 正确 ✅ — 用中间节点串联
node mul : call.KismetMathLibrary.Multiply_FloatFloat @ (200, 100)
node add : call.KismetMathLibrary.Add_FloatFloat @ (400, 100)
link a.Value -> mul.A
link b.Value -> mul.B
link mul.ReturnValue -> add.B
```

> **注意**：输出引脚可以连接多个目标（一对多），但输入引脚只能接收一条（多对一不行）。

### 模式 2：条件执行 — 先比较再 Branch

当你需要根据条件决定是否执行某个操作时，必须用 `special.Branch`：

```
# 错误 ❌ — 比较结果直接写入变量，且 stopMoving 无条件执行
link setLocation.then -> stopMoving.execute          # 每帧都停止！
link compareDistance.ReturnValue -> stopMoving.Value  # 逻辑混乱

# 正确 ✅ — 用 Branch 控制条件执行
node cmp : call.KismetMathLibrary.GreaterEqual_FloatFloat @ (600, 100)  # 纯节点
node branchStop : special.Branch @ (800, 0)      # 非纯节点
node doStop : VariableSet.BP_Test_C.bActive @ (1000, 0)

set doStop.bActive = false

# 执行链
link prevNode.then -> branchStop.execute
link branchStop.then -> doStop.execute     # 条件为 true 时才执行

# 数据链
link cmp.ReturnValue -> branchStop.Condition
```

### 模式 3：成员函数需要 self/target 连接

调用对象的成员函数时（非静态库函数），需要通过 `self` 引脚告诉蓝图"在哪个对象上调用"：

```
# 错误 ❌ — SplineComponent 的函数没有指定对象
node getLen : call.SplineComponent.GetSplineLength @ (200, 100)
# getLen 不知道对哪个样条线求长度！

# 正确 ✅ — 连接对象引用到 self 引脚
node getSpline : VariableGet.BP_Test_C.MySpline @ (0, 200)
node getLen : call.SplineComponent.GetSplineLength @ (200, 100)
link getSpline.MySpline -> getLen.self

# 同样适用于 Actor 函数：
node selfRef : special.Self @ (0, 200)
node setLoc : call.Actor.SetActorLocation @ (200, 0)
link selfRef.self -> setLoc.self
```

**通过 GetComponentByClass 获取组件再调用组件函数：**

```
# 从另一个 Actor（MoveRoad）上获取 SplineComponent，再调用样条线函数
node getRoad : VariableGet.BP_Test_C.MoveRoad @ (500, 600)
node getComp : call.Actor.GetComponentByClass @ (700, 580)
node splinePos : call.SplineComponent.GetLocationAtDistanceAlongSpline @ (900, 480)
node splineLen : call.SplineComponent.GetSplineLength @ (1100, 480)

set getComp.ComponentClass = class:USplineComponent   # 指定要获取的组件类

link getRoad.MoveRoad -> getComp.self                  # Actor 引用 → self（不是 Target！）
link getComp.ReturnValue -> splinePos.self             # 组件引用 → 样条线函数的 self
link getComp.ReturnValue -> splineLen.self             # 同一个组件可以连多个函数
```

**判断是否需要 self 连接：**
- `KismetMathLibrary`、`KismetStringLibrary`、`GameplayStatics` → **不需要** self（静态库函数）
- `Actor`、`SplineComponent`、`CharacterMovementComponent` 等 → **需要** self（成员函数）
- 如果函数操作的是"某个具体对象"，就需要 self

**`self` vs `Target` 引脚的区别：**
- `self`：用于 `call.<Class>.<Function>` 成员函数调用，指定"在哪个对象上调用"
- `Target`：仅用于 `message.<Interface>.<Function>` 接口消息节点，指定"发送消息给谁"
- **绝对不要**对 `call.*` 节点使用 `Target`，也不要对 `message.*` 节点使用 `self`

### 模式 3b：获取外部对象组件的两种方式

当需要访问**另一个蓝图对象**上的组件（如样条线）时，有两种方式：

**方式 A：直接读取对方暴露的组件变量（推荐）**

如果目标蓝图（如 `BP_MoveRoad`）已经将组件声明为变量（如 `Spline`），可以直接用外部类 VariableGet 读取：

```
# 先获取目标对象引用，再读取其组件变量
node getRoad : VariableGet.BP_Test_C.MoveRoad @ (0, 200)
node getSpline : VariableGet./Game/BP/BP_MoveRoad.BP_MoveRoad_C.Spline @ (300, 200)

link getRoad.MoveRoad -> getSpline.self        # 指定对象实例
link getSpline.Spline -> splinePos.self        # 直接拿到 SplineComponent
link getSpline.Spline -> splineLen.self        # 同一个组件可连多个函数
```

**方式 B：用 GetComponentByClass 按类查找**

如果目标蓝图没有暴露组件变量，可以用 `GetComponentByClass` 按类型查找：

```
node getRoad : VariableGet.BP_Test_C.MoveRoad @ (0, 200)
node getComp : call.Actor.GetComponentByClass @ (300, 200)

set getComp.ComponentClass = class:USplineComponent

link getRoad.MoveRoad -> getComp.self           # Actor 引用 → self（不是 Target！）
link getComp.ReturnValue -> splinePos.self      # 返回的组件引用
link getComp.ReturnValue -> splineLen.self
```

**两种方式的区别：**
| | 方式 A（直接读变量） | 方式 B（GetComponentByClass） |
|---|---|---|
| 前提 | 目标蓝图已暴露组件变量 | 无前提，按类型查找 |
| 类型安全 | 类型精确（直接是 SplineComponent） | 返回通用类型，可能需要转换 |
| 适用场景 | 已知目标蓝图结构 | 不确定目标蓝图是否暴露了变量 |

> **推荐使用方式 A**：类型精确、不需要额外设置 ComponentClass、不会出现类型不匹配的问题。

### 模式 4：速度 × 时间 = 位移增量

Tick 中按速度移动是最常见的模式。DeltaSeconds 是每帧时间，必须乘以速度：

```
# 错误 ❌ — 直接加速度，帧率越高移动越快
link getSpeed.Speed -> addDist.B

# 正确 ✅ — 速度 × DeltaSeconds = 每帧位移
node mulDelta : call.KismetMathLibrary.Multiply_FloatFloat @ (300, 200)
node addDist : call.KismetMathLibrary.Add_FloatFloat @ (500, 200)

link getSpeed.Speed -> mulDelta.A
link tick.DeltaSeconds -> mulDelta.B
link getCurrentDist.Distance -> addDist.A
link mulDelta.ReturnValue -> addDist.B
```

### 模式 5：沿样条线移动的完整模式

这是一个完整的"沿样条线移动物体"实现：

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

var MoveSpeed : float
var CurrentDist : float
var bIsMoving : bool
var MySpline : USplineComponent

graph EventGraph

# === 事件节点 ===
node beginPlay : event.Actor.ReceiveBeginPlay @ (0, 0)
node tick : event.Actor.ReceiveTick @ (0, 300)

# === BeginPlay：初始化变量 ===
node setSpeed : VariableSet.BP_Test_C.MoveSpeed @ (250, 0)
node setDist : VariableSet.BP_Test_C.CurrentDist @ (500, 0)
node setMoving : VariableSet.BP_Test_C.bIsMoving @ (750, 0)

set setSpeed.MoveSpeed = "200.0"
set setDist.CurrentDist = "0.0"
set setMoving.bIsMoving = true

# === Tick：移动逻辑（非纯节点）===
node branchIsMoving : special.Branch @ (250, 300)
node updateDist : VariableSet.BP_Test_C.CurrentDist @ (550, 300)
node setLocation : call.Actor.SetActorLocation @ (850, 300)
node branchEnd : special.Branch @ (1150, 300)
node stopMoving : VariableSet.BP_Test_C.bIsMoving @ (1400, 300)
node printDone : call.KismetSystemLibrary.PrintString @ (1650, 300)

set stopMoving.bIsMoving = false
set printDone.InString = "Arrived!"
set printDone.bPrintToScreen = true

# === 纯节点（不参与执行链）===
node getMoving : VariableGet.BP_Test_C.bIsMoving @ (50, 420)
node getDist : VariableGet.BP_Test_C.CurrentDist @ (200, 480)
node getSpeed : VariableGet.BP_Test_C.MoveSpeed @ (200, 540)
node mulDelta : call.KismetMathLibrary.Multiply_FloatFloat @ (350, 480)
node addDist : call.KismetMathLibrary.Add_FloatFloat @ (550, 480)
node getSpline : VariableGet.BP_Test_C.MySpline @ (500, 600)
node splinePos : call.SplineComponent.GetLocationAtDistanceAlongSpline @ (700, 480)
node splineLen : call.SplineComponent.GetSplineLength @ (900, 480)
node cmpDist : call.KismetMathLibrary.GreaterEqual_FloatFloat @ (1100, 480)
node selfRef : special.Self @ (700, 380)

# === BeginPlay 执行链 ===
link beginPlay.then -> setSpeed.execute
link setSpeed.then -> setDist.execute
link setDist.then -> setMoving.execute

# === Tick 执行链（只连非纯节点）===
link tick.then -> branchIsMoving.execute
link branchIsMoving.then -> updateDist.execute
link updateDist.then -> setLocation.execute
link setLocation.then -> branchEnd.execute
link branchEnd.then -> stopMoving.execute
link stopMoving.then -> printDone.execute

# === 数据链 ===
# 是否移动判断
link getMoving.bIsMoving -> branchIsMoving.Condition

# 距离计算：CurrentDist + MoveSpeed * DeltaSeconds
link getSpeed.MoveSpeed -> mulDelta.A
link tick.DeltaSeconds -> mulDelta.B
link getDist.CurrentDist -> addDist.A
link mulDelta.ReturnValue -> addDist.B
link addDist.ReturnValue -> updateDist.CurrentDist

# 样条线位置 → Actor 位置
link getSpline.MySpline -> splinePos.self
link updateDist.Output_Get -> splinePos.Distance
link splinePos.ReturnValue -> setLocation.NewLocation
link selfRef.self -> setLocation.self

# 到达终点判断
link getSpline.MySpline -> splineLen.self
link updateDist.Output_Get -> cmpDist.A
link splineLen.ReturnValue -> cmpDist.B
link cmpDist.ReturnValue -> branchEnd.Condition
```

**要点总结：**
1. 执行链只串联 `event → VariableSet → Branch → SetActorLocation → Branch → VariableSet → PrintString`
2. 纯节点（VariableGet、数学运算、SplineComponent.Get*）只通过数据线连接
3. 成员函数（SplineComponent、Actor）需要 self 连接
4. `MoveSpeed × DeltaSeconds` 保证帧率无关的移动速度
5. 用 Branch 控制条件执行，不要把比较结果直接写入变量

> 上面的示例使用了自身蓝图的 `MySpline` 变量（方式 A 本地版）。如果样条线在另一个蓝图上（如 `BP_MoveRoad`），参考下方的变体：

### 模式 5b：从外部蓝图获取样条线（方式 A）

当样条线组件在另一个蓝图对象上时，通过外部类 VariableGet 读取：

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

var MoveSpeed : float
var CurrentDist : float
var bIsMoving : bool
var MoveRoad : /Game/BP/BP_MoveRoad.BP_MoveRoad

graph EventGraph

# === 事件节点 ===
node beginPlay : event.Actor.ReceiveBeginPlay @ (0, 0)
node tick : event.Actor.ReceiveTick @ (0, 300)

# === BeginPlay：初始化变量 ===
node setSpeed : VariableSet.BP_Test_C.MoveSpeed @ (250, 0)
node setDist : VariableSet.BP_Test_C.CurrentDist @ (500, 0)
node setMoving : VariableSet.BP_Test_C.bIsMoving @ (750, 0)

set setSpeed.MoveSpeed = "200.0"
set setDist.CurrentDist = "0.0"
set setMoving.bIsMoving = true

# === 从外部蓝图读取样条线组件（纯数据连接，不在执行链中）===
node getRoad : VariableGet.BP_Test_C.MoveRoad @ (784, 512)
node getSpline : VariableGet./Game/BP/BP_MoveRoad.BP_MoveRoad_C.Spline @ (992, 512)

# === Tick：移动逻辑（非纯节点）===
node branchIsMoving : special.Branch @ (250, 300)
node updateDist : VariableSet.BP_Test_C.CurrentDist @ (550, 300)
node setLocation : call.Actor.SetActorLocation @ (850, 300)
node branchEnd : special.Branch @ (1150, 300)
node stopMoving : VariableSet.BP_Test_C.bIsMoving @ (1400, 300)
node printDone : call.KismetSystemLibrary.PrintString @ (1650, 300)

set stopMoving.bIsMoving = false
set printDone.InString = "Arrived!"
set printDone.bPrintToScreen = true

# === 纯节点 ===
node getMoving : VariableGet.BP_Test_C.bIsMoving @ (50, 420)
node getDist : VariableGet.BP_Test_C.CurrentDist @ (200, 480)
node getSpeed : VariableGet.BP_Test_C.MoveSpeed @ (200, 540)
node mulDelta : call.KismetMathLibrary.Multiply_FloatFloat @ (350, 480)
node addDist : call.KismetMathLibrary.Add_FloatFloat @ (550, 480)
node splinePos : call.SplineComponent.GetLocationAtDistanceAlongSpline @ (700, 480)
node splineLen : call.SplineComponent.GetSplineLength @ (900, 480)
node cmpDist : call.KismetMathLibrary.GreaterEqual_FloatFloat @ (1100, 480)
node selfRef : special.Self @ (700, 380)

# === BeginPlay 执行链 ===
link beginPlay.then -> setSpeed.execute
link setSpeed.then -> setDist.execute
link setDist.then -> setMoving.execute

# === Tick 执行链 ===
link tick.then -> branchIsMoving.execute
link branchIsMoving.then -> updateDist.execute
link updateDist.then -> setLocation.execute
link setLocation.then -> branchEnd.execute
link branchEnd.then -> stopMoving.execute
link stopMoving.then -> printDone.execute

# === 数据链 ===
link getMoving.bIsMoving -> branchIsMoving.Condition

link getSpeed.MoveSpeed -> mulDelta.A
link tick.DeltaSeconds -> mulDelta.B
link getDist.CurrentDist -> addDist.A
link mulDelta.ReturnValue -> addDist.B
link addDist.ReturnValue -> updateDist.CurrentDist

# 关键：从外部蓝图读取样条线
link getRoad.MoveRoad -> getSpline.self          # 指定从哪个 MoveRoad 实例读取
link getSpline.Spline -> splinePos.self          # 读到的 SplineComponent → 样条线函数
link getSpline.Spline -> splineLen.self

link updateDist.Output_Get -> splinePos.Distance
link splinePos.ReturnValue -> setLocation.NewLocation
link selfRef.self -> setLocation.self

link updateDist.Output_Get -> cmpDist.A
link splineLen.ReturnValue -> cmpDist.B
link cmpDist.ReturnValue -> branchEnd.Condition
```

**与模式 5 的区别：**
- 样条线不在自身蓝图上，而是在外部的 `BP_MoveRoad` 上
- 使用外部类 VariableGet + `self` 引脚代替本地 VariableGet
- `getRoad` 和 `getSpline` 是纯节点，**不参与执行链**

### 模式 6：ForEachLoop 遍历数组

使用宏节点遍历数组，注意宏节点的引脚名与普通节点不同：

```
var QueueActors : array:AActor

node tick : event.Actor.ReceiveTick @ (0, 400)
node updateDist : VariableSet.BP_Test_C.LeaderDistance @ (300, 400)
node forEach : macro.StandardMacros.ForEachLoop @ (600, 400)
node setLoc : call.Actor.SetActorLocation @ (1000, 400)
node getQueue : VariableGet.BP_Test_C.QueueActors @ (400, 560)

# 执行链：Tick → 更新距离 → ForEachLoop → 循环体中移动每个 Actor
link tick.then -> updateDist.execute
link updateDist.then -> forEach.Exec       # ⚠️ 宏节点执行输入是 Exec，不是 execute
link forEach.LoopBody -> setLoc.execute     # 每次循环执行
link forEach."Array Element" -> setLoc.self # 当前遍历的 Actor（引脚名含空格）

# 数据链
link getQueue.QueueActors -> forEach.Array  # 提供要遍历的数组
```

> **宏节点引脚注意事项：**
> - 执行输入是 `Exec`（不是 `execute`），解释器有别名映射，写 `execute` 也能工作
> - `"Array Element"` 和 `"Array Index"` 含空格，正式写法需要引号；也可以写 `ArrayElement` / `ArrayIndex`（模糊匹配）
> - `Completed` 在循环结束后触发，可连接后续逻辑

### 模式 7：用 ForEachLoop 的 ArrayIndex 按索引访问另一个数组

当有两个等长数组需要配对使用时（如 `QueueActors` 和 `OffsetFactors`），ForEachLoop 遍历一个数组，用 `ArrayIndex` 从另一个数组取值：

```
var QueueActors : array:AActor
var OffsetFactors : array:float

node forEach : macro.StandardMacros.ForEachLoop @ (600, 400)
node getQueue : VariableGet.BP_Test_C.QueueActors @ (400, 560)
node getFactors : VariableGet.BP_Test_C.OffsetFactors @ (400, 620)
node getByIndex : special.GetArrayItem @ (800, 560)

# ForEachLoop 遍历 QueueActors
link getQueue.QueueActors -> forEach.Array

# 用循环索引从 OffsetFactors 取对应元素
link getFactors.OffsetFactors -> getByIndex.Array
link forEach."Array Index" -> getByIndex."Dimension 1"

# forEach."Array Element" 是当前的 Actor
# getByIndex.Output 是对应的 OffsetFactor
link getByIndex.Output -> someNode.InputPin
```

> **注意**：数组按索引取元素的节点是 `special.GetArrayItem`（UE 内部的 `K2Node_GetArrayItem`），不是 `call.KismetArrayLibrary.Get`。
>
> **GetArrayItem 引脚名（⚠️ 与常规节点不同！）**：
> | 引脚 | 方向 | 名称 |
> |------|------|------|
> | 数组输入 | Input | `Array` |
> | 索引输入 | Input | `"Dimension 1"`（⚠️ 含空格，不是 Index） |
> | 元素输出 | Output | `Output`（⚠️ 不是 ReturnValue） |
>
> 也可以用模糊匹配写 `Dimension1`（不加引号不加空格）。

### 模式 8：Knot 节点（线路整理/信号分发）

`special.Knot` 是蓝图中的"重定向节点"（Reroute Node），纯粹用于整理线路，不改变数据。当一个值需要连接到多个远处的节点时，用 Knot 中继可以让蓝图更整洁：

```
node getLeader : VariableGet.BP_Test_C.LeaderDistance @ (100, 500)
node knot1 : special.Knot @ (400, 500)
node knot2 : special.Knot @ (600, 500)

# 从 VariableGet → Knot → 多个目标
link getLeader.LeaderDistance -> knot1.InputPin
link knot1.OutputPin -> knot2.InputPin
link knot2.OutputPin -> distCalc.A
link knot2.OutputPin -> tangentCalc.Distance
```

> **Knot 引脚名**：输入 = `InputPin`，输出 = `OutputPin`。Knot 是纯节点，不能放在执行链中。

### 模式 9：向量运算 — BreakVector / MakeVector / Normal

在 3D 计算中经常需要拆分和组合向量：

```
# 获取样条线切线方向
node tangent : call.SplineComponent.GetTangentAtDistanceAlongSpline @ (800, 740)

# 拆分为 X, Y, Z 分量
node breakTan : call.KismetMathLibrary.BreakVector @ (1000, 688)
link tangent.ReturnValue -> breakTan.InVec

# 交换 X/Y 得到"右向量"（XY 平面旋转 90°）
node rightVec : call.KismetMathLibrary.MakeVector @ (1200, 680)
link breakTan.Y -> rightVec.X      # Y 分量 → 新 X
link breakTan.X -> rightVec.Y      # X 分量 → 新 Y
set rightVec.Z = "0.0"

# 归一化
node normRight : call.KismetMathLibrary.Normal @ (1400, 680)
link rightVec.ReturnValue -> normRight.A
```

> **引脚名参考（⚠️ 必须使用 UE 实际引脚名，不要猜测！）**：
>
> | 函数 | 输入引脚 | 输出引脚 |
> |------|---------|---------|
> | `BreakVector` | `InVec`（⚠️ 不是 InVector） | `X`, `Y`, `Z` |
> | `MakeVector` | `X`, `Y`, `Z` | `ReturnValue` |
> | `Normal`（归一化） | `A`（⚠️ 不是 InVector）, `Tolerance` | `ReturnValue` |
> | `Normalize` | `A`, `Tolerance` | `ReturnValue` |
> | `Add_VectorVector` | `A`, `B` | `ReturnValue` |
> | `Subtract_VectorVector` | `A`, `B` | `ReturnValue` |
> | `Multiply_VectorFloat` | `A`（Vector）, `B`（Float） | `ReturnValue` |
> | `CrossProduct` | `A`, `B` | `ReturnValue` |
> | `DotProduct` | `A`, `B` | `ReturnValue` |
> | `GetLocationAtDistanceAlongSpline` | `Distance`, `CoordinateSpace`, `self` | `ReturnValue` |
> | `GetTangentAtDistanceAlongSpline` | `Distance`, `CoordinateSpace`, `self` | `ReturnValue` |
> | `GetSplineLength` | `self` | `ReturnValue` |
> | `Conv_IntToDouble` | `InInt` | `ReturnValue` |
> | `Conv_IntToFloat` | `InInt` | `ReturnValue` |
> | `Array_Length`（KismetArrayLibrary） | `TargetArray`（⚠️ 不是 Array） | `ReturnValue`（int） |
> | `Array_Add`（KismetArrayLibrary） | `TargetArray`, `NewItem`（⚠️ 不是 Item） | `ReturnValue`（int，新索引） |
> | `Array_Get`（KismetArrayLibrary） | `TargetArray`, `Index` | `Item` |
> | `Array_Set`（KismetArrayLibrary） | `TargetArray`, `Index`, `Item` | — |
> | `Array_Remove`（KismetArrayLibrary） | `TargetArray`, `Index` | — |
> | `Array_Contains`（KismetArrayLibrary） | `TargetArray`, `ItemToFind` | `ReturnValue`（bool） |
> | `Array_Clear`（KismetArrayLibrary） | `TargetArray` | — |
> | `Dot_VectorVector` | `A`, `B` | `ReturnValue`（float） |
> | `FindLocationClosestToWorldLocation`（SplineComponent） | `WorldLocation`, `self` | `ReturnValue`（Vector） |
> | `FindInputKeyClosestToWorldLocation`（SplineComponent） | `WorldLocation`, `self` | `ReturnValue`（float，InputKey） |
> | `GetDistanceAlongSplineAtSplineInputKey`（SplineComponent） | `InKey`, `self` | `ReturnValue`（float，Distance） |
> | `GetTangentAtSplineInputKey`（SplineComponent） | `InKey`, `CoordinateSpace`, `self` | `ReturnValue`（Vector） |
>
> 使用本地坐标：`set node.CoordinateSpace = Local`
>
> **⚠️ KismetArrayLibrary 函数名规则**：所有数组函数以 `Array_` 开头（如 `Array_Add`、`Array_Length`），数组参数统一叫 `TargetArray`。不要写 `Add`、`Length`、`Array` 等简写。
>
> **⚠️ SplineComponent InputKey vs Distance**：
> - `FindInputKeyClosestToWorldLocation` 返回的是 **InputKey**（0~N-1 的浮点键值），不是 Distance
> - 如果需要 Distance，用 `GetDistanceAlongSplineAtSplineInputKey` 把 InputKey 转换为 Distance
> - 如果要用 InputKey 获取切线，用 `GetTangentAtSplineInputKey`（不是 `GetTangentAtDistanceAlongSpline`）
>
> **⚠️ 常见错误**：不要猜测引脚名！`BreakVector` 的输入是 `InVec` 而非 `InVector`，`Normal` 的输入是 `A` 而非 `InVector`。如果不确定，使用 Generator 从蓝图生成代码查看实际引脚名。

---

## 17. 完整示例

### 示例 1：简单的 BeginPlay 打印

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

graph EventGraph

node a : event.Actor.ReceiveBeginPlay @ (0, 0)
node b : call.KismetSystemLibrary.PrintString @ (300, 0)

set b.InString = "Hello Weave!"
set b.bPrintToScreen = true
set b.Duration = "2.0"

link a.then -> b.execute
```

### 示例 2：条件分支

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

graph EventGraph

var bReady : bool

node begin : event.Actor.ReceiveBeginPlay @ (0, 0)
node getReady : VariableGet.BP_Test_C.bReady @ (200, 96)
node branch : special.Branch @ (400, 0)
node printYes : call.KismetSystemLibrary.PrintString @ (700, -48)
node printNo : call.KismetSystemLibrary.PrintString @ (700, 96)

set printYes.InString = "Ready!"
set printNo.InString = "Not Ready"

link begin.then -> branch.execute
link getReady.bReady -> branch.Condition
link branch.then -> printYes.execute
link branch.else -> printNo.execute
```

### 示例 3：带自定义函数的多图表

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

var Counter : int

# 先定义函数
graph IncrementAndPrint
node entry : entry
node get : VariableGet.BP_Test_C.Counter @ (200, 0)
node add : call.KismetMathLibrary.Add_IntInt @ (400, 0)
node setVar : VariableSet.BP_Test_C.Counter @ (600, 0)
node conv : call.KismetSystemLibrary.Conv_IntToString @ (600, -80)
node print : call.KismetSystemLibrary.PrintString @ (800, 0)

set add.B = 1
set print.bPrintToScreen = true

link entry.then -> setVar.execute
link setVar.then -> print.execute
link get.Counter -> add.A
link add.ReturnValue -> setVar.Counter
link setVar.Output_Get -> conv.InInt
link conv.ReturnValue -> print.InString

# 再从 EventGraph 调用
graph EventGraph
node e : event.Actor.ReceiveBeginPlay @ (0, 0)
node call1 : call.BP_Test_C.IncrementAndPrint @ (300, 0)

link e.then -> call1.execute
```

### 示例 4：Tick 计时器 + Sequence

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

graph EventGraph

var bActive : bool
var Timer : float

node tick : event.Actor.ReceiveTick @ (0, 0)
node getActive : VariableGet.BP_Test_C.bActive @ (0, 96)
node branch : special.Branch @ (200, 0)
node addTime : call.KismetMathLibrary.Add_FloatFloat @ (400, 96)
node getTimer : VariableGet.BP_Test_C.Timer @ (300, 192)
node setTimer : VariableSet.BP_Test_C.Timer @ (600, 0)
node cmp : call.KismetMathLibrary.GreaterEqual_FloatFloat @ (600, 128)
node branch2 : special.Branch @ (800, 0)
node resetTimer : VariableSet.BP_Test_C.Timer @ (1000, 0)
node seq : special.Sequence @ (1200, 0)
node printA : call.KismetSystemLibrary.PrintString @ (1400, 0)
node printB : call.KismetSystemLibrary.PrintString @ (1400, 128)

set cmp.B = "1.0"
set resetTimer.Timer = "0.0"
set printA.InString = "One second passed"
set printB.InString = "Also doing this"

# 执行链
link tick.then -> branch.execute
link getActive.bActive -> branch.Condition
link branch.then -> setTimer.execute
link setTimer.then -> branch2.execute
link branch2.then -> resetTimer.execute
link resetTimer.then -> seq.execute
link seq.then_0 -> printA.execute
link seq.then_1 -> printB.execute

# 数据链
link tick.DeltaSeconds -> addTime.A
link getTimer.Timer -> addTime.B
link addTime.ReturnValue -> setTimer.Timer
link setTimer.Output_Get -> cmp.A
link cmp.ReturnValue -> branch2.Condition
```

### 示例 5：自定义事件 + 调用

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

graph EventGraph

# 定义自定义事件
node myEvent : customEvent.OnDamage @ (0, 0)
node print : call./Script/Engine.KismetSystemLibrary.PrintString @ (300, 0)
set print.InString = "Damage received!"

link myEvent.then -> print.execute

# BeginPlay 中调用该事件
node begin : event./Script/Engine.Actor.ReceiveBeginPlay @ (0, 300)
node callEvent : call./Game/BP/BP_Test.BP_Test_C.OnDamage @ (300, 300)

link begin.then -> callEvent.execute
```

### 示例 6：时间轴

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

graph EventGraph

node begin : event./Script/Engine.Actor.ReceiveBeginPlay @ (0, 0)
node tl : special.Timeline.FadeTimeline @ (300, 0)
node setOpacity : call./Script/Engine.Actor.SetActorHiddenInGame @ (600, 0)

# 时间轴配置
set tl.$timeline_float = "Alpha"
set tl.$timeline_length = "2.0"
set tl.$timeline_autoplay = false
set tl.$timeline_loop = false

# 执行链
link begin.then -> tl.Play

# 数据链：Alpha 轨道输出连接到目标
link tl.Alpha -> setOpacity.bNewHidden
```

### 示例 7：注释框

```
comment "Init Section\nSetup all variables" @ (0, -200) size (600, 300) color (50, 150, 255, 255) fontsize 14
comment "Main Loop" @ (0, 200) size (1200, 400) color (255, 200, 50, 255) fontsize 16
```
