<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { getSystemInfo, type SystemInfo } from '../../api'
import PageShell from '../../components/PageShell.vue'
import { useDeviceName } from '../../composables/useDeviceName'

const loading = ref(true)
const error = ref('')
const info = ref<SystemInfo | null>(null)
const { setDeviceNameLocal } = useDeviceName()

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
    setDeviceNameLocal(r.data.device_name)
  } catch (e) {
    error.value = String(e)
  } finally {
    loading.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <PageShell title="设备信息" subtitle="查看设备型号、版本与运行状态。">
    <div v-loading="loading">
      <el-alert v-if="error" :title="error" type="error" show-icon :closable="false" class="mb" />
      <el-descriptions v-if="info" class="ipc-desc" :column="2" border>
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
    </div>
  </PageShell>
</template>

<style scoped>
.mb { margin-bottom: 1rem; }
</style>
