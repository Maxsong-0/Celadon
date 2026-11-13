# T-108 任务书 · 删掉曲风与交付（连声音处理一起）

> 2026-11-13 09:1x 写，会话「重构包 A 效果图」（T-107）。vault 同内容：`04-协调/2026-11-13-T-108-删除曲风与交付-任务书.md`。
> 档位：重档 · Opus 5.5 · high（改 DSP 代码）。任务号 T-108 已在 vault TASKS ① 登记（状态「阻塞：等 T-106 合 main」）；ADR 号认领时自己占。
> 分支 `claude/t108-remove-genre-delivery`：从 origin/main `a768b8a` 起，只含这份任务书。T-106 合 main 后先 `git fetch` + `git rebase origin/main` 再动代码。

## 来由

- Max 看 T-107 第一版效果图时回「直接删掉，不用再搞曲风这些的了」；T-107 第二版（分支 `claude/t107-panel-package-a`，`daa2faf`）只删了面板上的两个选择器，参数和声音处理没动。
- 随后问「要不要连参数和声音处理一起删」，Max 答 **「连声音处理也删」**。
- 要删的是 T-038 整套：曲风（通用 / Deep House / Tech House / Melodic-Progressive，含 House 学习方式、限幅跟拍、House 反相提示）与交付（标准 / 流媒体 / 俱乐部，含各档目标响度与真峰值上限、流媒体封顶）。

## 现状范围（09:1x grep `Genre|Delivery|glazeGenre|glazeDelivery|houseActive|isHouse`，次数仅供估量）

- 声音处理：`Source/dsp/GlazeController.cpp`（24）、`MusicAnalyzer.cpp`（19）、`GlazeConstants.{h,cpp}`（9 / 18）、`GlazePlanner.{h,cpp}`（9 / 12）、`CeladonChain.cpp`（12）、`GlazeTypes.h`（12；`enum class Genre`、`Delivery`、`isHouse`）、`GlazeProfile.cpp`（8）、`PsrGuard.cpp`（5）、`GlazeController.h`（4）。
- 参数与处理器：`Source/ParameterIds.h`、`ParameterDefaults.h`、`Parameters.cpp`、`PluginProcessor.cpp`。
- 界面：`Source/ui/HelpText.cpp`、`GlazeStatusText.cpp`（House 前缀、反相提示）、`Localisation`（曲风 / 交付的键）、CLAY 卡片的 House 两行提示；T-107 分支上面板选择器已删。
- 测试：`tests/GlazePlannerTests.cpp`（146）、`MusicAnalyzerTests.cpp`（40）、`GlazeControllerTests.cpp`（37）、`GuardFamilyTests.cpp`（18）、`AdvancedMasteringTests.cpp`（14）、`LegacyFixtureTests.cpp`（11）、`GlazeStatusTextTests.cpp`、`HelpTextTests.cpp`、`Phase2Contract.h`、`LocalisationTests.cpp`。
- 工具：`tools/render/RenderMain.cpp`（17；`--genre` / `--delivery` 一类开关、House 门禁脚本）、截图工具的 House 状态。
- 官网：`site/src/content/docs/{zh,en}/` 下约 20 页提到曲风 / 交付（interface、glaze、limit、clay、listening、quickstart、faq、install、meters、advanced、controls）。
- vault 规格：`01-框架/设计规格-曲风与交付.md`、`设计规格-实时GLAZE-曲风.md`、`设计规格-曲风-House学习方式.md`；ADR-026（曲风与交付拆成两个开关）要标「已被取代」。

## 先问 Max 的（需求本身，按复杂任务 Grill，一次问完）

1. **交付删了以后，目标响度和真峰值上限按哪一档的口径固定？** 推荐「标准」（原默认）。流媒体的 −1 dBTP 封顶 / 编码余量、俱乐部的响目标都随之没有。
2. **旧工程存的是 House / 流媒体 / 俱乐部，打开后声音会变**（回到通用 + 标准）。接受吗？要不要在界面提示一次？（参照 ADR-035 旧工程变声政策、T-048 Q1 的「旧工程提示一次」做法。）
3. **两个参数 ID 怎么处理**：从参数表里删掉（宿主里的自动化通道消失，AU / VST3 参数列表变）还是留着但不再生效、隐藏？推荐：留 ID 不生效、不进自动化列表（不破坏宿主工程），读档时忽略。
4. 连带作废：T-098 第 8 条（播放中切曲风，Max 11-12 选 2）、T-038 的 House 门禁（22 首 / House 合成曲门槛里 House 那几行）、0.1.8.11 发版门槛里和 House 有关的项。确认一起撤。

## 约束

- **排在 T-106 之后**：T-106 正在改 `Source/dsp/GlazeController.{h,cpp}`、`OutputLevelTracker`、相关测试（分支 `claude/t106-gtrim-error-basis`），没合 main 之前只做调查与 Grill，不改 `Source/dsp/`。动手前 `git fetch` 后从含 T-106 的 origin/main 起分支（本地 `main` 引用是旧的，一律以 `origin/main` 为准）。
- T-107（重构包 A）还在分支上等 Max，没进 main；界面上删选择器那部分以 T-107 为准，T-108 不重复做，合并顺序到时再定。
- 这是会改声音的 DSP 任务：按全局 SOP 走复杂任务全套（读 INDEX + TASKS、查重、认领、计划先落盘、开工基线、DGX 编测、最后统一全量测试与 House / 22 首门禁、独立复查）。编译测试经 `~/.cache/celadon-spark/bin/spark-test.sh`（全机锁），不在 Mac 上跑。
- 合 main 前要 Max 认可声音变化（旧工程变声）。

## 完成标准

- 曲风 / 交付相关的 DSP 路径、参数、界面、测试、渲染工具、官网中英、vault 规格全部清干净（按第 3 条的决定处理参数 ID），grep 不留死代码。
- DGX 全量测试对同机基线不新增失败；GLAZE 0 逐位不变；House 门禁按 Max 的决定撤或改。
- 截图工具 `--only check` 全过；改前改后截图对照。
- ADR 一份（取代 ADR-026），规格加「已被取代」指针，不删原档。
