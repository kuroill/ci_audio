# CI130X SDK 简介

## 概述

CI130X SDK 适用于CI130X系列芯片

## SDK目录结构

| 目录       | 描述             |
| ---------- | ---------------- |
| .vscode    | 编译器配置文件   |
| components | 组件             |
| driver     | 驱动             |
| libs       | 库文件           |
| projects   | 示例工程         |
| startup    | 启动文件         |
| system     | 系统文件         |
| tool       | 固件构建升级工具 |
| utils      | 调试帮助工具     |

## 资料查找
编译环境安装说明、芯片手册、开发板资料等，详见启英泰伦语音AI平台网址：https://aiplatform.chipintelli.com

*版权归chipintelli公司所有，未经允许不得使用或修改*

$env:PATH="C:\Users\game\Documents\ci_audio\tools\build-tools\bin;C:\Users\game\Documents\nuclei-gcc-9.2.0\gcc\bin;$env:PATH"

cd C:\Users\game\Documents\ci_audio\projects\offline_asr_llm_aiot_iis_sample\project_file

make -j8