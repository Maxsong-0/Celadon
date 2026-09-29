# 安装向导的界面语言

- `ChineseSimplified.isl`：Inno Setup 的简体中文向导文案。不随 Inno Setup 安装包发行，取自其官方源码仓库
  [jrsoftware/issrc](https://github.com/jrsoftware/issrc) 标签 `is-6_7_3` 的 `Files/Languages/Unofficial/ChineseSimplified.isl`
  （社区维护，文件头有维护者信息），与上游逐字节相同，SHA-256 `7d544b9bb1d142cfa11f2e5d3cc8abe2e55f8e066c5124e3772675aa236e1278`。
- 编码：不带 BOM 的 UTF-8。Inno Setup 6 起认这种文件，官方也建议不加 BOM（安装目录 whatsnew.htm）；`Celadon.iss` 同样不带 BOM。
- 升级 Inno Setup 时按同一路径换成对应标签的版本。
