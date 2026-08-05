公司 Logo 替换说明

1. 将公司 Logo 导出为透明背景 PNG，建议宽高比约 3:1，推荐 600×200 px。
2. 文件必须命名为 company_logo.png，并放到本目录。
3. 重新执行 cmake --build build 后，CMake 会把 assets 复制到 exe 所在目录。
4. 也可以不重新编译，直接替换 exe 同级 assets/company_logo.png，然后重启程序。

程序按比例缩放图片，不会拉伸。未找到 PNG 时显示 COMPANY LOGO 占位文字。
