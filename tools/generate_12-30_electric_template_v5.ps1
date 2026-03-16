$ErrorActionPreference = "Stop"

function Get-ParaRangeByContainsText {
  param(
    [Parameter(Mandatory = $true)] $Doc,
    [Parameter(Mandatory = $true)] [string] $Needle
  )
  $r = $Doc.Content.Duplicate
  $r.Find.ClearFormatting() | Out-Null
  $r.Find.Text = $Needle
  $r.Find.Forward = $true
  $r.Find.Wrap = 1 # wdFindContinue
  $found = $r.Find.Execute()
  if (-not $found) {
    throw "Cannot find paragraph containing: $Needle"
  }
  return $r.Paragraphs.Item(1).Range
}

function Replace-RangeTextBetweenParas {
  param(
    [Parameter(Mandatory = $true)] $Doc,
    [Parameter(Mandatory = $true)] $StartParaRange,
    [Parameter(Mandatory = $true)] $EndParaRange,
    [Parameter(Mandatory = $true)] [string] $NewText
  )
  $range = $Doc.Range($StartParaRange.End, $EndParaRange.Start)
  $range.Text = $NewText
  return $range
}

function Insert-ParagraphText {
  param(
    [Parameter(Mandatory = $true)] $Doc,
    [Parameter(Mandatory = $true)] $AtRange,
    [Parameter(Mandatory = $true)] [string] $Text
  )
  $r = $Doc.Range($AtRange.Start, $AtRange.Start)
  $r.InsertAfter($Text) | Out-Null
  $r.Collapse(0) | Out-Null # wdCollapseEnd
  return $r
}

function Paste-InlinePictureFromDoc {
  param(
    [Parameter(Mandatory = $true)] $SourceDoc,
    [Parameter(Mandatory = $true)] [int] $PictureOrdinal, # 1-based among Type==1 InlineShapes
    [Parameter(Mandatory = $true)] $DestDoc,
    [Parameter(Mandatory = $true)] $AtRange
  )
  $count = 0
  $picked = $null
  for ($i = 1; $i -le $SourceDoc.InlineShapes.Count; $i++) {
    $ils = $SourceDoc.InlineShapes.Item($i)
    if ($ils.Type -ne 1) { continue }
    $count++
    if ($count -eq $PictureOrdinal) { $picked = $ils; break }
  }
  if ($null -eq $picked) {
    throw "Cannot locate picture ordinal $PictureOrdinal in source doc; found $count picture(s)."
  }

  $picked.Range.Copy() | Out-Null
  $r = $DestDoc.Range($AtRange.Start, $AtRange.Start)
  $r.Paste() | Out-Null
  $r.Collapse(0) | Out-Null
  return $r
}

function Remove-SubstringInRange {
  param(
    [Parameter(Mandatory = $true)] $Range,
    [Parameter(Mandatory = $true)] [string] $Needle
  )
  $Range.Find.ClearFormatting() | Out-Null
  $Range.Find.Replacement.ClearFormatting() | Out-Null
  $Range.Find.Text = $Needle
  $Range.Find.Replacement.Text = ""
  $Range.Find.Wrap = 1 # wdFindContinue
  # 2 = wdReplaceAll
  $Range.Find.Execute($Needle, $false, $false, $false, $false, $false, $true, 1, $false, "", 2) | Out-Null
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$templatePath = Join-Path $repoRoot "12-30电学专利交底书范本.doc"
$draftPath    = Join-Path $repoRoot "12-30专利草稿-劫持库专利交底书.doc"
$outPath      = Join-Path $repoRoot "12-30电学专利交底书_劫持库_终稿_v5.doc"

if (-not (Test-Path $templatePath)) { throw "Missing template: $templatePath" }
if (-not (Test-Path $draftPath)) { throw "Missing draft doc: $draftPath" }

$title = "一种基于CUDA Runtime接口拦截的显存配额隔离与kernel发射许可控制方法"

$techContact = "（待补充：技术联系人姓名/电话/email）"
$handler = "（待补充：经办人姓名/电话/email）"
$remarks = "是否仅用于申请政府高新资质或政府项目：否（可改）；是否将来申请国外专利：否（可改）。"

$text_I_1 = @"
本专利涉及多进程/多容器共享同一物理GPU的资源隔离与访问控制。现有技术中，与本专利目的最接近的方案通常为：在GPU软件栈的调用链上设置控制点，并结合外部管理模块完成配额分配与计量统计。

一种常见实现方式是在GPU驱动侧（例如驱动接口层或其上层封装层）对显存分配、释放以及内核发射相关接口进行拦截，进而实现显存限制或发射节奏控制；另一种实现方式仅在应用进程侧拦截显存分配接口（例如仅对cudaMalloc/等价接口做阈值判断），超过阈值时返回分配失败，以达到“粗粒度限制”的效果。上述方案一般还需要在集群或节点侧设置管理端，用于维护各租户的配额上限与已用量，并向拦截点提供查询与更新能力。

为便于理解，现有技术与本发明的控制点位置及控制链路可参见图1（示意性对比），其中现有技术通常将拦截点布置在更靠近驱动侧的位置，或仅覆盖分配接口而缺少查询视图一致化与释放精确回收的闭环。
"@

$text_I_2 = @"
基于上述现有技术方案，在实际落地到不同GPU软件栈实现时，通常存在如下客观缺点：

1）控制点不稳定或不可覆盖：当GPU运行时内部实现与公开的驱动接口调用链存在差异时，仅在驱动侧设置拦截点可能出现拦截不触发或触发不稳定的情况，从而导致配额强制无法可靠生效。

2）显存“可见视图”与“可分配额度”不一致：若仅对分配接口做限制，而未同步改写cudaMemGetInfo、设备属性等查询接口的返回值，则应用框架可能仍观测到较大的free/total显存并据此建立缓存或做图初始化，随后在真实分配时频繁失败，引发初始化异常、重复回退或性能抖动。

3）释放阶段难以精确回收：释放接口通常仅携带设备指针，不携带分配字节数；若缺少“指针—大小”的配套记录机制，则难以在释放时准确回收配额，导致计量漂移、配额被“虚占”、长期运行后可用配额不断缩小。

4）仅显存限制难以抑制算力争用：在多租户共享场景中，显存配额并不能直接约束kernel发射节奏；缺少可实施的发射许可控制点时，仍可能出现某一租户通过高频发射长期占用算力、影响其他租户时延与吞吐的问题。
"@

$text_II_1 = @"
（1）具体方案

1. 总体结构与组成
如图2所示，本发明提供一种在CUDA兼容运行时环境中实施GPU资源隔离的方法及系统，其至少包括：
— 应用进程：在容器或宿主机上运行的CUDA生态应用或框架进程，其通过CUDA Runtime接口发起显存查询、显存分配/释放以及kernel发射请求；
— 运行时拦截库：以预加载方式注入到应用进程的用户态动态库，用于在应用进程内部拦截CUDA Runtime接口调用，并在满足配额条件时转发至真实运行时库；
— 真实运行时库：GPU平台提供的CUDA Runtime实现；
— 管理端：用于维护租户/进程维度的显存配额与已用量，并向运行时拦截库提供查询、入账与出账服务；管理端可为本机进程或远端服务；
— （可选）调度/策略模块：用于生成或更新发射许可（token/quota）策略，管理端可与调度/策略模块交互，但本发明不限定具体策略算法；
— 物理GPU/设备：由真实运行时库与底层驱动协同访问的物理GPU资源。

2. 方法流程（结合附图的步骤次序）
如图3所示，本发明的方法流程可包括如下步骤（以单个应用进程为例；多进程/多容器情况下并行执行，管理端按标识隔离计量）：

步骤（1）（预加载注入）：在启动应用进程之前，在其运行环境中配置预加载参数，使运行时拦截库优先被动态链接器加载；同时配置用于身份绑定与资源域划分的运行参数，例如租户/容器/进程标识、设备标识、管理端地址及策略开关。

步骤（2）（真实符号解析与初始化）：当应用进程首次调用被拦截的CUDA Runtime接口时，运行时拦截库通过动态符号解析机制获得真实运行时库中对应函数的入口地址并缓存；运行时拦截库同时完成与管理端的通信初始化，并初始化并发控制原语与本地计量数据结构。

步骤（3）（显存查询视图一致化）：当应用进程调用显存查询接口时，运行时拦截库向管理端查询该租户在指定设备上的显存配额信息（包括配额上限、已用量/可用量），并将查询接口的输出参数修正为受配额约束的显存视图后返回给应用进程。具体地：
— 对于cudaMemGetInfo类接口，运行时拦截库返回受配额约束的可用显存与配额上限；
— 对于设备属性查询接口，运行时拦截库将设备总显存字段（例如totalGlobalMem或等价字段）修正为配额上限。
通过上述方式，应用进程观测到的显存视图与后续分配强制保持一致。

步骤（4）（分配前校验与标准语义返回）：当应用进程请求显存分配时，运行时拦截库在调用真实运行时库之前，先向管理端获取当前可用配额；若请求字节数大于可用配额，则运行时拦截库直接返回运行时标准的分配失败错误码（OOM语义），不进入真实分配；若请求字节数不大于可用配额，则运行时拦截库调用真实运行时库执行分配。

步骤（5）（分配入账与映射维护）：当真实分配成功返回设备指针后，运行时拦截库将设备指针与本次分配字节数写入本地映射表（用于释放时获取大小），并向管理端上报“入账事件”以更新已用配额。可选地，当并发竞态导致管理端拒绝入账时，运行时拦截库执行回滚：调用真实释放并清理映射表条目，再向应用进程返回分配失败错误码。

步骤（6）（释放出账与精确回收）：当应用进程请求释放设备指针时，运行时拦截库从映射表中查询该指针对应的分配字节数并删除该条目，并向管理端上报“出账事件”以使已用配额减小；随后调用真实运行时库完成释放并返回真实错误码。通过“设备指针—分配字节数”映射实现释放阶段的精确回收，避免配额漂移。

步骤（7）（kernel发射许可控制）：当应用进程调用kernel发射接口时（例如cudaLaunchKernel/cudaLaunch或等价入口），运行时拦截库在发射前按策略开关与当前配额状态判断是否需要申请新的发射许可token或时间配额；若需要，则运行时拦截库向管理端同步请求许可；仅当许可成功授予后，运行时拦截库才调用真实发射函数完成发射。其交互时序示例可参见图4。可选地，当管理端不可用时，运行时拦截库可按预设策略降级为仅显存控制或拒绝发射，但不构成对本发明的必要限定。

3. 与现有技术的差别及其带来的改进机理
相较于现有技术将控制点布置于驱动侧或仅覆盖分配接口的实现方式，本发明至少具有如下差别特征：
（i）将可强制的控制点前移至应用进程内部的CUDA Runtime接口入口，通过预加载拦截实现“运行时入口可控”，降低对底层调用链一致性的依赖；
（ii）同时拦截并改写显存查询类接口，构造受配额约束的显存视图，使应用的观测结果与分配强制一致，从而减少框架异常；
（iii）通过alloc_map建立“设备指针—分配字节数”的映射，使释放阶段能够精确回收配额，实现计量闭环；
（iv）在kernel发射入口提供同步许可机制，使算力控制具备可实施的强制点，并且策略算法可由管理端/策略模块替换实现而不影响拦截点生效。
"@

$text_II_2 = @"
（2）技术效果

本发明通过在CUDA Runtime接口入口实施“显存视图一致化 + 分配强制 + 释放精确回收 + 发射许可控制”的组合闭环，可获得如下技术效果：

1）可靠生效：相较于仅在更低层（例如驱动接口层）设置拦截点的方式，本发明将强制点布置在应用进程内部的运行时入口，能够在不同运行时实现差异下保持更高的拦截覆盖与可控性，从而提高配额强制的可靠性。

2）减少框架异常：通过步骤（3）对查询接口返回值做受配额约束的修正，使应用观测到的free/total显存与后续分配强制一致，能够降低深度学习框架在初始化、显存缓存、图构建阶段因“视图不一致”导致的反复失败与回退。

3）计量可回收且长期稳定：通过步骤（5）–步骤（6）的“设备指针—分配字节数”映射与入账/出账机制，释放时能够准确回收配额，避免长期运行后因计量漂移造成的配额虚占与资源浪费。

4）算力争用可控：通过步骤（7）在kernel发射前增加同步许可控制点，可在不限定具体调度算法的前提下对发射节奏施加约束，使多租户共享场景下的延迟与吞吐更易于管理端进行策略化调优。
"@

$text_III = @"
本发明存在如下可选替代方案/变形方式（用于扩大保护范围），其均不脱离本发明的基本构思：

1）通信机制替代：运行时拦截库与管理端之间的通信可采用本机IPC（例如Unix Domain Socket、共享内存、管道等）或远端网络通信（例如TCP/HTTP等）；消息字段可按实现需要增减。

2）接口集合替代：除cudaMalloc/cudaFree/cudaMemGetInfo/cudaGetDeviceProperties/cudaLaunchKernel/cudaLaunch外，也可拦截与其等价或语义相近的运行时接口集合，以覆盖更多框架调用路径。

3）计量结构替代：alloc_map可采用哈希表、平衡树或其他键值映射结构实现；也可按线程/流维度维护分段映射并在释放时汇聚，但需保证“指针—大小”可追溯以实现精确回收。

4）发射许可策略替代：token/quota的授予策略可按固定时间片、按负载、按历史burst估计、按优先级等方式实现；运行时拦截库仅提供发射前的强制点，不限定管理端的策略算法。

5）降级策略替代：当管理端不可用或通信失败时，可选择默认放行、默认拒绝或仅显存控制等降级策略；也可设置超时与重试机制以适配不同业务容忍度。
"@

$text_IV = @"
本发明的关键技术点在于（建议作为权利要求布局的核心）：

1）在应用进程内部，通过预加载的运行时拦截库拦截CUDA Runtime接口并转发至真实运行时库的实现方式（运行时入口可控）。

2）同时对显存查询接口与显存分配接口进行协同控制：查询返回受配额约束的显存视图，分配前校验并以标准OOM语义阻断超额请求（视图一致化 + 强制闭环）。

3）维护“设备指针—分配字节数”的映射表，并在释放阶段依据该映射精确出账与回收配额（释放精确回收）。

4）在kernel发射接口入口实施发射前同步许可控制（token/quota gating），使算力控制具备可实施的强制点且不限定具体调度算法。
"@

$text_V = @"
为便于理解与复现，本案相关的补充资料包括（均为可选提供，不构成对本发明的必要限定）：

1）实现代码目录：本地工程目录（包含运行时拦截库与管理端实现）。
2）方法步骤与代码对应关系：本地“代码对照表”文档（用于标注关键拦截点与管理端消息交互在代码中的对应位置）。
"@

try {
  $word = New-Object -ComObject Word.Application
  $word.Visible = $false
  $word.DisplayAlerts = 0

  Write-Output "Opening template..."
  # Copy at filesystem level first to avoid Word SaveAs/SaveCopyAs hangs on .doc
  if (Test-Path $outPath) { Remove-Item -Force $outPath }
  Copy-Item -Force $templatePath $outPath
  $doc = $word.Documents.Open($outPath, $false, $false)
  Write-Output "Opening draft (for figures)..."
  $draft = $word.Documents.Open($draftPath, $false, $true) # read-only; used only for copying pictures
  Write-Output "Filling header table..."

  # Fill header table (and remove "必须填写" hints in label cells)
  if ($doc.Tables.Count -ge 1) {
    $tbl = $doc.Tables.Item(1)
    $tbl.Cell(1,1).Range.Text = "交底书名称"
    $tbl.Cell(2,1).Range.Text = "技术联系人姓名及其电话、email"
    $tbl.Cell(3,1).Range.Text = "经办人姓名及其电话、email（如不填写，则默认技术联系人为负责人）"
    $tbl.Cell(4,1).Range.Text = "备注事项"

    $tbl.Cell(1,2).Range.Text = $title
    $tbl.Cell(2,2).Range.Text = $techContact
    $tbl.Cell(3,2).Range.Text = $handler
    $tbl.Cell(4,2).Range.Text = $remarks
  }

  # Replace body sections according to template headings
  $p_I_1 = Get-ParaRangeByContainsText -Doc $doc -Needle "1、现有技术的方案简述"
  $p_I_2 = Get-ParaRangeByContainsText -Doc $doc -Needle "2、现有技术的客观缺点"
  $p_II  = Get-ParaRangeByContainsText -Doc $doc -Needle "二、本专利的技术"
  $p_II_1 = Get-ParaRangeByContainsText -Doc $doc -Needle "（1）具体方案"
  $p_II_2 = Get-ParaRangeByContainsText -Doc $doc -Needle "（2）技术效果"
  $p_III = Get-ParaRangeByContainsText -Doc $doc -Needle "三、上述技术方案是否有替代方案"
  $p_IV  = Get-ParaRangeByContainsText -Doc $doc -Needle "四、本发明"
  $p_V   = Get-ParaRangeByContainsText -Doc $doc -Needle "五、其他有助于理解"

  # I.1 content (between I.1 heading and I.2 heading)
  Write-Output "Writing section I.1 and inserting Figure 1..."
  Replace-RangeTextBetweenParas -Doc $doc -StartParaRange $p_I_1 -EndParaRange $p_I_2 -NewText ("`r" + $text_I_1 + "`r`r") | Out-Null
  $insertI1 = $doc.Range($p_I_2.Start, $p_I_2.Start) # insert before I.2 heading
  $insertI1 = Paste-InlinePictureFromDoc -SourceDoc $draft -PictureOrdinal 1 -DestDoc $doc -AtRange $insertI1
  $insertI1 = Insert-ParagraphText -Doc $doc -AtRange $insertI1 -Text ("`r图1 现有技术与本发明控制点差异对比示意图`r`r")

  # I.2 content (between I.2 heading and II heading)
  Write-Output "Writing section I.2..."
  Replace-RangeTextBetweenParas -Doc $doc -StartParaRange $p_I_2 -EndParaRange $p_II -NewText ("`r" + $text_I_2 + "`r`r") | Out-Null

  # II.1 content (between II.1 heading and II.2 heading)
  Write-Output "Writing section II.(1) and inserting Figures 2-4..."
  $rII1 = Replace-RangeTextBetweenParas -Doc $doc -StartParaRange $p_II_1 -EndParaRange $p_II_2 -NewText ("`r" + $text_II_1 + "`r`r")

  # Insert figures 2-4 inside II.1, after the "总体结构与组成" / "流程" / "时序" references.
  # Strategy: append them at the end of II.1 section, in order, with captions that match the in-text references.
  $insertAt = $doc.Range($p_II_1.End, $p_II_2.Start)
  $insertAt.Collapse(0) | Out-Null
  $insertAt = Paste-InlinePictureFromDoc -SourceDoc $draft -PictureOrdinal 2 -DestDoc $doc -AtRange $insertAt
  $insertAt = Insert-ParagraphText -Doc $doc -AtRange $insertAt -Text ("`r图2 本发明系统组成与连接关系示意图`r`r")
  $insertAt = Paste-InlinePictureFromDoc -SourceDoc $draft -PictureOrdinal 3 -DestDoc $doc -AtRange $insertAt
  $insertAt = Insert-ParagraphText -Doc $doc -AtRange $insertAt -Text ("`r图3 本发明方法流程示意图`r`r")
  $insertAt = Paste-InlinePictureFromDoc -SourceDoc $draft -PictureOrdinal 4 -DestDoc $doc -AtRange $insertAt
  $insertAt = Insert-ParagraphText -Doc $doc -AtRange $insertAt -Text ("`r图4 kernel发射许可请求与执行时序示意图`r`r")

  # II.2 content (between II.2 heading and III heading)
  Write-Output "Writing section II.(2)..."
  Replace-RangeTextBetweenParas -Doc $doc -StartParaRange $p_II_2 -EndParaRange $p_III -NewText ("`r" + $text_II_2 + "`r`r") | Out-Null

  # III content (between III heading and IV heading)
  Write-Output "Writing section III..."
  Replace-RangeTextBetweenParas -Doc $doc -StartParaRange $p_III -EndParaRange $p_IV -NewText ("`r" + $text_III + "`r`r") | Out-Null

  # IV content (between IV heading and V heading)
  Write-Output "Writing section IV..."
  Replace-RangeTextBetweenParas -Doc $doc -StartParaRange $p_IV -EndParaRange $p_V -NewText ("`r" + $text_IV + "`r`r") | Out-Null

  # V content (between V heading and end)
  Write-Output "Writing section V..."
  $rangeV = $doc.Range($p_V.End, $doc.Content.End)
  $rangeV.Text = ("`r" + $text_V + "`r")

  Write-Output "Saving..."
  $doc.Save()
  Write-Output "Closing..."
  $draft.Close($false) | Out-Null
  $doc.Close($false)
  $word.Quit()

  Write-Output ("Wrote: {0}" -f $outPath)
} finally {
  try { if ($null -ne $draft) { [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($draft) } } catch {}
  try { [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($doc) } catch {}
  try { [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($word) } catch {}
}
