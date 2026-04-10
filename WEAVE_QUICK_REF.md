# Weave 语法速查表

> 完整文档见 `WEAVE_SYNTAX.md`，本文件覆盖 90% 常用语法。

## 三条铁律
1. **纯节点不能进执行链** — VariableGet/数学/比较/转换只连数据线，不连 execute/then
2. **执行链只串非纯节点** — event→Set/Branch/Sequence/Print/Delay→...
3. **函数必须指定正确类名** — 数学→KismetMathLibrary，字符串→KismetStringLibrary，系统→KismetSystemLibrary

## 基本结构
```
graphset <名称> <资产路径>
var <名> : <类型> [= <默认值>] [editable] [readonly] [spawn] [category:"分类"] "描述"
graph <名>[(参数)] [-> (返回值)]
node <id> : <schema> @ (x, y)
set <id>.<pin> = <值>
link <from>.<pin> -> <to>.<pin>
comment "文本" @ (x,y) size (w,h) [color (R,G,B,A)] [fontsize N]
bubble <id> "文本"
```

## var 变量
```
var HP : float = 100.0 editable category:"战斗" "血量"
var Level : int = 1 editable readonly category:"配置"
var Team : int = 0 spawn category:"生成"          # spawn 自动隐含 editable
var Timer : float category:"内部"                  # 不暴露到实例
var Soldiers : array:AActor                        # 容器类型
var Road : /Game/BP/BP_Road.BP_Road                # 对象引用（完整路径）
var EnemyClass : class:AActor                      # 类引用
var Velocity : Vector                              # 结构体
```
类型: bool, int, int64, float, double, string, text, name, byte, Vector, Rotator, LinearColor, Transform
容器: `array:<T>`, `set:<T>`, `map:<K>:<V>`

## node Schema ID
| 类型 | 格式 | 纯/非纯 |
|------|------|---------|
| 事件 | `event.Actor.ReceiveBeginPlay` | 非纯(只有then) |
| 自定义事件 | `customEvent.OnHit` | 非纯 |
| 组件事件 | `componentEvent.<Comp>.<Class>.<Delegate>` | 非纯 |
| 函数调用 | `call.KismetSystemLibrary.PrintString` | 看函数 |
| 接口消息 | `message.<InterfaceClass>.<Func>` | 非纯 |
| 变量读 | `VariableGet.<Class>.<Var>` | **纯** |
| 变量写 | `VariableSet.<Class>.<Var>` | 非纯 |
| 验证读 | `ValidatedGet.<Class>.<Var>` | 非纯 |
| 分支 | `special.Branch` | 非纯 |
| 序列 | `special.Sequence` | 非纯 |
| Cast | `special.Cast.<TypePath>` | 非纯 |
| 生成Actor | `special.SpawnActorFromClass` | 非纯 |
| 数组取值 | `special.GetArrayItem` | 纯 |
| Self引用 | `special.Self` | 纯 |
| 宏 | `macro.StandardMacros.ForEachLoop` | 非纯 |
| Timeline | `special.Timeline.<Name>` | 非纯 |
| 第三方 | `native.<K2NodeClassName>` | 看节点 |

类名支持短名称 `Actor` 或完整路径 `/Script/Engine.Actor`。

## 关键引脚名
| 节点 | 输入 | 输出 |
|------|------|------|
| 通用执行 | `execute` | `then` |
| Branch | `Condition` | `then`, `else` |
| Sequence | `execute` | `then_0`, `then_1`, ... |
| VariableGet | 无数据输入(纯读取)；跨蓝图时有`self` | **变量名本身**(不是ReturnValue!) |
| VariableSet | `execute`, **变量名** | `then`, `Output_Get` |
| Cast | `Object` | `then`, `CastFailed`, `"As ClassName"` |
| ForEachLoop | `Exec`, `Array` | `"Loop Body"`, `"Array Element"`, `"Array Index"`, `Completed` |
| GetArrayItem | `Array`, `"Dimension 1"` | `Output` |
| SpawnActor | `Class`, `SpawnTransform` | `then`, `ReturnValue` |
| Self | 无 | `self`(小写!) |
| 函数 | 参数名 | `ReturnValue` |

> 引脚名含空格时用引号: `link cast."As Pawn" -> target.self`
> 模糊匹配: `ArrayElement` 可匹配 `"Array Element"`

## set 特殊值
```
set s.Class = class:BP_Enemy                     # 类引脚
set s.Class = class:/Game/BP/BP_Enemy.BP_Enemy   # 完整路径
set a.Location = vec(100,200,300)                # 向量简写
set a.Rotation = rot(0,90,0)                     # 旋转简写
set a.CoordinateSpace = World                    # 枚举裸值
set tl.$timeline_float = "Alpha"                 # Timeline轨道
set tl.$timeline_length = "5.0"                  # Timeline长度
```

## 常见纯函数（不能进执行链）
KismetMathLibrary: Add/Subtract/Multiply/Divide_FloatFloat, Greater/Less/Equal_FloatFloat, FClamp, Abs, VSize, Normal, MakeVector, BreakVector, Conv_VectorToRotator, BooleanAND/OR, Not_PreBool
KismetStringLibrary: Conv_FloatToString, Conv_IntToString, Concat_StrStr
Actor: GetActorLocation, GetActorRotation, GetDistanceTo
SplineComponent: GetLocationAtDistanceAlongSpline, GetSplineLength

## 常见非纯函数（必须在执行链中）
KismetSystemLibrary: PrintString, Delay, SphereOverlapActors
Actor: SetActorLocation, SetActorRotation, DestroyActor, GetComponentByClass
KismetArrayLibrary: Array_Add, Array_Remove

## 常见误区

1. **仅支持上表的 Schema 类型** — `literal`、`call.Array.Get`、`cast.XXX` 等自创格式不存在，会被跳过
2. **数组取值 = `special.GetArrayItem`** — 不是 `call.Array.Get`
3. **Cast 必须完整路径** — `special.Cast./Game/BP/BP_Enemy.BP_Enemy_C`，不能 `cast.BP_Enemy_C`
4. **Cast 引脚名含空格加引号** — `cast."AsBP Test Enemy"`，不能 `cast.AsBP_Test_Enemy`
5. **事件没有 self 引脚** — 用 `special.Self` 节点获取自身引用
6. **VariableGet 不接受数据输入** — 只有输出(=变量名)，不能 `link X -> varGet.HP`，只能 `link varGet.HP -> X`
7. **类引脚用 set** — `set node.ActorClass = class:/Game/BP/BP_Enemy.BP_Enemy`，不需要 literal 节点
8. **旋转/向量参数用 set + 简写** — `set node.Rotation = rot(0,90,0)`、`set node.Location = vec(0,0,0)`，不能用字符串 `"0,90,0"`
9. **Self 输出引脚是小写** — `selfNode.self`，不是 `selfNode.Self`
10. **组件不是 var 变量** — SplineComponent 等通过 AddComponent 添加，不能 `var Spline : USplineComponent`，用 `call.Actor.GetComponentByClass` 获取
11. **link 只能从输出到输入** — VariableGet 的变量名引脚是输出，不能作为 link 的目标端

## Float→Double 自动翻译
`Add_FloatFloat`→`Add_DoubleDouble`, `Conv_FloatToString`→`Conv_DoubleToString` 等 UE5 自动处理。
