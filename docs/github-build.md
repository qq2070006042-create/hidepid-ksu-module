# GitHub 编译教程

这个仓库已经带了 GitHub Actions 配置：

```text
.github/workflows/build-ko.yml
```

它会在 GitHub 的 Ubuntu 机器上编译这些 ko：

```text
android12-5.10
android13-5.10
android13-5.15
android14-5.15
android14-6.1
android15-6.6
android16-6.12
```

最后会打包出：

```text
hidepid-ksu-module-stealth.zip
```

## 第一次使用

1. 打开 https://github.com
2. 登录账号。
3. 点右上角的 `+`。
4. 点 `New repository`。
5. 仓库名随便填，例如 `hidepid-ksu-module`。
6. 选择 `Public` 或 `Private` 都可以。
7. 点 `Create repository`。

## 上传代码

最简单的方法：

1. 打开刚创建的 GitHub 仓库页面。
2. 点 `uploading an existing file`。
3. 把这个文件夹里的所有文件拖进去。
4. 注意 `.github` 文件夹也要一起上传，否则不会自动编译。
5. 页面底部点 `Commit changes`。

如果网页不让你上传文件夹，建议安装 GitHub Desktop，然后把整个 `F:\hidepid-ksu-module` 文件夹发布到 GitHub。

## 开始编译

1. 打开 GitHub 仓库页面。
2. 点上面的 `Actions`。
3. 左边选择 `Build KO`。
4. 点右边的 `Run workflow`。
5. 分支选择 `main` 或 `master`。
6. 再点绿色的 `Run workflow`。

## 下载编译结果

1. 等 `Build KO` 变成绿色对勾。
2. 点进去这次运行记录。
3. 页面最下面找到 `Artifacts`。
4. 下载 `hidepid-ksu-module-stealth`。
5. 下载后解压，会得到 `hidepid-ksu-module-stealth.zip`。
6. 把这个 zip 放到手机里，用 KernelSU 管理器从本地安装。

## 如果失败

点失败的红色任务进去，看是哪一个 KMI 失败。

常见情况：

- `ddk pull` 失败：GitHub 网络或 ddk 源临时问题，重新运行一次。
- 某个 `androidXX-X.X` 编译失败：说明源码不兼容这个 KMI，需要单独修代码。
- 最后打包失败：通常是某个 ko 没编出来。
