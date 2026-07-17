<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { getSystemInfo, type SystemInfo } from '../../api'

const loading = ref(true)
const error = ref('')
const info = ref<SystemInfo | null>(null)

function formatUptime(sec: number) {
  const d = Math.floor(sec / 86400)
  const h = Math.floor((sec % 86400) / 3600)
  const m = Math.floor((sec % 3600) / 60)
  if (d > 0) return `${d} 天 ${h} 小时 ${m} 分`
  return `${h} 小时 ${m} 分`
}

async function refresh() {
  loading.value = true
  error.value = ''
  try {
    const r = await getSystemInfo()
    if (r.code !== 0) throw new Error(r.msg || '读取失败')
    info.value = r.data
  } catch (e) {
    error.value = String(e)
  } finally {
    loading.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <section class="page">
    <div>
      <h2 class="page-title">设备信息</h2>
      <p class="page-desc">查看设备型号、版本与运行状态。</p>
    </div>

    <el-card v-loading="loading" shadow="never" class="panel-card">
      <el-alert v-if="error" :title="error" type="error" show-icon :closable="false" class="mb" />
      <el-descriptions v-if="info" :column="2" border>
        <el-descriptions-item label="设备名称">{{ info.device_name }}</el-descriptions-item>
        <el-descriptions-item label="主机名">{{ info.hostname }}</el-descriptions-item>
        <el-descriptions-item label="型号">{{ info.model }}</el-descriptions-item>
        <el-descriptions-item label="固件/应用版本">{{ info.version }}</el-descriptions-item>
        <el-descriptions-item label="运行时长">{{ formatUptime(info.uptime_sec) }}</el-descriptions-item>
        <el-descriptions-item label="内存">
          {{ Math.round(info.mem_free_kb / 1024) }} / {{ Math.round(info.mem_total_kb / 1024) }} MB
          （可用/总计）
        </el-descriptions-item>
      </el-descriptions>
      <div class="actions">
        <el-button @click="refresh">刷新</el-button>
      </div>
    </el-card>
  </section>
</template>

<style scoped>
.mb { margin-bottom: 1rem; }
.actions { margin-top: 1rem; }
</style>
