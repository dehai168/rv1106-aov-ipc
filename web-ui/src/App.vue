<script setup lang="ts">
import zhCn from 'element-plus/es/locale/lang/zh-cn'
import { ElConfigProvider } from 'element-plus'
</script>

<template>
  <el-config-provider :locale="zhCn">
    <router-view />
  </el-config-provider>
</template>
