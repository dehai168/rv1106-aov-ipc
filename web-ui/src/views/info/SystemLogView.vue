<script setup lang="ts">
import { systemLogUrl } from '../../api'
import { Document, Download } from '@element-plus/icons-vue'

const logUrl = systemLogUrl()
</script>

<template>
  <section class="page">
    <div>
      <h2 class="page-title">系统日志</h2>
      <p class="page-desc">下载设备运行日志，便于排查预览、侦测与录像问题。</p>
    </div>

    <el-card shadow="never" class="panel-card">
      <el-result title="运行日志" sub-title="当前支持下载完整 ipc_app.log（后续将支持检索与尾部预览）。">
        <template #icon>
          <el-icon :size="48" color="var(--ipc-accent)"><Document /></el-icon>
        </template>
        <template #extra>
          <a :href="logUrl" target="_blank" rel="noopener">
            <el-button type="primary" :icon="Download">下载 ipc_app.log</el-button>
          </a>
        </template>
      </el-result>
    </el-card>
  </section>
</template>
