<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import {
  getSystemInfo,
  getSystemTime,
  rebootSystem,
  resetSystem,
  setSystemTime,
  systemLogUrl,
  type SystemInfo,
  type SystemTimeConfig,
} from '../api'

const router = useRouter()
const loading = ref(true)
const saving = ref(false)
const info = ref<SystemInfo | null>(null)
const time = ref<SystemTimeConfig>({
  unix_time: 0,
  timezone: 'Asia/Shanghai',
  ntp_enabled: false,
  ntp_server: 'pool.ntp.org',
})

async function refresh() {
  loading.value = true
  try {
    const [si, st] = await Promise.all([getSystemInfo(), getSystemTime()])
    if (si.code !== 0) throw new Error(si.msg || '读取设备信息失败')
    if (st.code !== 0) throw new Error(st.msg || '读取时间失败')
    info.value = si.data
    time.value = st.data
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

function formatUptime(sec: number) {
  const h = Math.floor(sec / 3600)
  const m = Math.floor((sec % 3600) / 60)
  return `${h}h ${m}m`
}

async function onSaveTime() {
  saving.value = true
  try {
    const r = await setSystemTime({
      unix_time: time.value.unix_time,
      timezone: time.value.timezone,
      ntp_enabled: time.value.ntp_enabled,
      ntp_server: time.value.ntp_server,
      apply_ntp: time.value.ntp_enabled,
    })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存时间失败')
      return
    }
    time.value = r.data
    ElMessage.success('时间配置已保存')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    saving.value = false
  }
}

function useBrowserTime() {
  time.value.unix_time = Math.floor(Date.now() / 1000)
}

async function onReboot() {
  try {
    await ElMessageBox.confirm('确认立即重启设备？', '重启', {
      type: 'warning',
      confirmButtonText: '重启',
      cancelButtonText: '取消',
    })
  } catch {
    return
  }
  try {
    const r = await rebootSystem()
    if (r.code !== 0) {
      ElMessage.error(r.msg || '重启失败')
      return
    }
    ElMessage.success('正在重启…')
  } catch (e) {
    ElMessage.error(String(e))
  }
}

async function onReset() {
  try {
    await ElMessageBox.confirm(
      '恢复出厂将清除用户配置（不含 TF 卡录像）。账号将恢复为 admin/admin。确认继续？',
      '恢复出厂',
      { type: 'error', confirmButtonText: '恢复出厂', cancelButtonText: '取消' },
    )
  } catch {
    return
  }
  try {
    const r = await resetSystem()
    if (r.code !== 0) {
      ElMessage.error(r.msg || '恢复出厂失败')
      return
    }
    ElMessage.success('已恢复出厂，请重新登录')
    await router.replace('/login')
  } catch (e) {
    ElMessage.error(String(e))
  }
}

onMounted(refresh)
</script>

<template>
  <section class="page">
    <div>
      <h2 class="page-title">基本设置</h2>
      <p class="page-desc">设备信息、时间与系统维护。</p>
    </div>

    <el-row v-loading="loading" :gutter="16">
      <el-col :xs="24" :md="12">
        <el-card shadow="never" class="panel-card mb">
          <template #header>设备摘要</template>
          <template v-if="info">
            <p>名称 <b>{{ info.device_name }}</b> · 主机名 <b>{{ info.hostname }}</b></p>
            <p class="muted">型号 {{ info.model }} · 版本 {{ info.version }}</p>
            <p class="muted">
              运行 {{ formatUptime(info.uptime_sec) }} · 内存
              {{ Math.round(info.mem_free_kb / 1024) }}/{{ Math.round(info.mem_total_kb / 1024) }} MB
            </p>
          </template>
          <a :href="systemLogUrl()" target="_blank" rel="noopener">
            <el-button text type="primary">下载运行日志</el-button>
          </a>
        </el-card>
      </el-col>

      <el-col :xs="24" :md="12">
        <el-card shadow="never" class="panel-card mb">
          <template #header>时间设置</template>
          <el-form label-width="100px">
            <el-form-item label="Unix 时间">
              <el-input-number v-model="time.unix_time" :controls="false" class="w-full" />
            </el-form-item>
            <el-form-item>
              <el-button @click="useBrowserTime">使用浏览器当前时间</el-button>
            </el-form-item>
            <el-form-item label="时区">
              <el-input v-model="time.timezone" placeholder="Asia/Shanghai" />
            </el-form-item>
            <el-form-item label="NTP">
              <el-switch v-model="time.ntp_enabled" active-text="启用（保存时同步）" />
            </el-form-item>
            <el-form-item label="NTP 服务器">
              <el-input v-model="time.ntp_server" :disabled="!time.ntp_enabled" />
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="saving" @click="onSaveTime">保存时间</el-button>
            </el-form-item>
          </el-form>
        </el-card>
      </el-col>

      <el-col :span="24">
        <el-card shadow="never" class="panel-card">
          <template #header>维护</template>
          <el-alert
            title="重启与恢复出厂为高危操作，请谨慎执行。"
            type="warning"
            show-icon
            :closable="false"
            class="mb"
          />
          <el-space wrap>
            <el-button type="warning" @click="onReboot">重启设备</el-button>
            <el-button type="danger" @click="onReset">恢复出厂</el-button>
            <el-button @click="refresh">刷新</el-button>
          </el-space>
        </el-card>
      </el-col>
    </el-row>
  </section>
</template>

<style scoped>
.mb { margin-bottom: 1rem; }
.muted { color: var(--ipc-muted); }
.w-full { width: 100%; }
</style>
