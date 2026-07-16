<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
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
const error = ref('')
const ok = ref('')
const info = ref<SystemInfo | null>(null)
const time = ref<SystemTimeConfig>({
  unix_time: 0,
  timezone: 'Asia/Shanghai',
  ntp_enabled: false,
  ntp_server: 'pool.ntp.org',
})

async function refresh() {
  loading.value = true
  error.value = ''
  try {
    const [si, st] = await Promise.all([getSystemInfo(), getSystemTime()])
    if (si.code !== 0) throw new Error(si.msg || '读取设备信息失败')
    if (st.code !== 0) throw new Error(st.msg || '读取时间失败')
    info.value = si.data
    time.value = st.data
  } catch (e) {
    error.value = String(e)
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
  error.value = ''
  ok.value = ''
  try {
    const r = await setSystemTime({
      unix_time: time.value.unix_time,
      timezone: time.value.timezone,
      ntp_enabled: time.value.ntp_enabled,
      ntp_server: time.value.ntp_server,
      apply_ntp: time.value.ntp_enabled,
    })
    if (r.code !== 0) {
      error.value = r.msg || '保存时间失败'
      return
    }
    time.value = r.data
    ok.value = '时间配置已保存'
  } catch (e) {
    error.value = String(e)
  } finally {
    saving.value = false
  }
}

function useBrowserTime() {
  time.value.unix_time = Math.floor(Date.now() / 1000)
}

async function onReboot() {
  if (!window.confirm('确认立即重启设备？')) return
  error.value = ''
  try {
    const r = await rebootSystem()
    if (r.code !== 0) {
      error.value = r.msg || '重启失败'
      return
    }
    ok.value = '正在重启…'
  } catch (e) {
    error.value = String(e)
  }
}

async function onReset() {
  if (
    !window.confirm(
      '恢复出厂将清除用户配置（不含 TF 卡录像）。账号将恢复为 admin/admin。确认继续？',
    )
  ) {
    return
  }
  error.value = ''
  try {
    const r = await resetSystem()
    if (r.code !== 0) {
      error.value = r.msg || '恢复出厂失败'
      return
    }
    ok.value = '已恢复出厂，请重新登录'
    await router.replace('/login')
  } catch (e) {
    error.value = String(e)
  }
}

onMounted(refresh)
</script>

<template>
  <section class="system">
    <h2>系统管理</h2>
    <p v-if="loading" class="muted">加载中…</p>
    <div v-else class="grid">
      <div class="panel">
        <h3>设备信息</h3>
        <p v-if="info" class="muted">
          名称 <b>{{ info.device_name }}</b> · 主机名 <b>{{ info.hostname }}</b>
        </p>
        <p v-if="info" class="muted">
          型号 {{ info.model }} · 版本 {{ info.version }}
        </p>
        <p v-if="info" class="muted">
          运行 {{ formatUptime(info.uptime_sec) }} · 内存
          {{ Math.round(info.mem_free_kb / 1024) }}/{{ Math.round(info.mem_total_kb / 1024) }} MB
        </p>
        <a class="link" :href="systemLogUrl()" target="_blank" rel="noopener">下载运行日志</a>
      </div>

      <div class="panel">
        <h3>时间设置</h3>
        <label>
          Unix 时间戳
          <input v-model.number="time.unix_time" type="number" />
        </label>
        <button type="button" class="ghost" @click="useBrowserTime">使用浏览器当前时间</button>
        <label>
          时区
          <input v-model="time.timezone" placeholder="Asia/Shanghai" />
        </label>
        <label class="row-check">
          <input v-model="time.ntp_enabled" type="checkbox" />
          启用 NTP（保存时尝试同步）
        </label>
        <label>
          NTP 服务器
          <input v-model="time.ntp_server" :disabled="!time.ntp_enabled" />
        </label>
        <button type="button" :disabled="saving" @click="onSaveTime">
          {{ saving ? '…' : '保存时间' }}
        </button>
      </div>

      <div class="panel danger">
        <h3>维护</h3>
        <p class="muted">重启与恢复出厂为高危操作，请谨慎执行。</p>
        <div class="row">
          <button type="button" @click="onReboot">重启设备</button>
          <button type="button" class="danger-btn" @click="onReset">恢复出厂</button>
          <button type="button" class="ghost" @click="refresh">刷新</button>
        </div>
      </div>
    </div>
    <p v-if="error" class="error">{{ error }}</p>
    <p v-if="ok" class="muted">{{ ok }}</p>
  </section>
</template>

<style scoped>
.system { max-width: 720px; display: grid; gap: 1rem; }
.grid { display: grid; gap: 1rem; }
.panel {
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 0.85rem;
  display: grid;
  gap: 0.65rem;
}
.panel.danger { border-color: rgba(220, 80, 80, 0.45); }
h3 { margin: 0; font-size: 1rem; }
label { display: grid; gap: 0.35rem; color: var(--muted); font-size: 0.9rem; }
input { padding: 0.35rem 0.5rem; }
.row { display: flex; gap: 0.75rem; flex-wrap: wrap; align-items: center; }
.row-check { display: flex; flex-direction: row; align-items: center; gap: 0.5rem; }
.ghost {
  background: transparent;
  border: 1px solid var(--border);
  color: inherit;
  width: fit-content;
}
.danger-btn {
  background: rgba(220, 80, 80, 0.15);
  border: 1px solid rgba(220, 80, 80, 0.5);
  color: inherit;
}
.link { color: var(--accent, #3d8bfd); font-size: 0.9rem; }
</style>
