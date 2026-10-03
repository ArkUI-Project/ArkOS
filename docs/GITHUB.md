# GitHub 源码上传

仓库入口为 `README.md`，开发说明为 `CONTRIBUTING.md`；`.gitignore`
管理构建输出、虚拟机数据和本地编辑器配置。`.gitattributes` 统一
源码换行，标记二进制资源、生成数据与固定第三方组件。

上传前查看待提交内容：

```sh
git status --short
git diff --cached --stat
```

初次提交及连接仓库：

```sh
git commit -m "Initial ArkOS source"
git remote add origin <仓库的SSH或HTTPS地址>
git push -u origin main
```

GitHub 仓库可按需要选择可见性。源码目录、必要字库/词典数据、测试
样例和第三方许可证随代码提交；每个版本的启动镜像和完整验证证据
通过独立发布附件提供。实测摘要与日志按验证批次保存在对应发布附件内。

工作流使用只读仓库权限，checkout 固定于
[v7.0.1 对应提交](https://github.com/actions/checkout/commit/3d3c42e5aac5ba805825da76410c181273ba90b1)，
按 [GitHub 的固定 Action SHA 建议](https://docs.github.com/en/actions/reference/security/secure-use)
配置。首次推送后查看 Actions 的实际执行结果；本地开发验证记录
与 GitHub 托管运行分别记录。
