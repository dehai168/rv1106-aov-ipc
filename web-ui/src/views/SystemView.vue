<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import {
  getRebootSchedule,
  getSystemInfo,
  getSystemTime,
  rebootSystem,
  resetSystem,
  setRebootSchedule,
  setSystemInfo,
  setSystemTime,
  systemLogUrl,
  type RebootSchedule,
  type SystemInfo,
  type SystemTimeConfig,
} from '../api'

const DAY_LABELS = ['日', '一', '二', '三', '四', '五', '六']

const router = useRouter()
const loading = ref(true)
const saving = ref(false)
const savingName = ref(false)
const savingReboot = ref(false)
const info = ref<SystemInfo | null>(null)
const deviceName = ref('')
const time = ref<SystemTimeConfig>({
  unix_time: 0,
  timezone: 'Asia/Shanghai',
  ntp_enabled: false,
  ntp_server: 'pool.ntp.org',
})
const reboot = ref<RebootSchedule>({
  enabled: false,
  hour: 3,
  minute: 30,
  days: '1111111',
})
const dayChecks = ref<boolean[]>([true, true, true, true, true, true, true])

function daysFromChecks(): string {
  return dayChecks.value.map((x) => (x ? '1' : '0')).join('')
}

function checksFromDays(days: string) {
  const d = (days || '1111111').padEnd(7, '0').slice(0, 7)
  dayChecks.value = d.split('').map((c) => c === '1')
}

async function refresh() {
  loading.value = true
  try {
    const [si, st, rs] = await Promise.all([
      getSystemInfo(),
      getSystemTime(),
      getRebootSchedule(),
    ])
    if (si.code !== 0) throw new Error(si.msg || '读取设备信息失败')
    if (st.code !== 0) throw new Error(st.msg || '读取时间失败')
    if (rs.code !== 0) throw new Error(rs.msg || '读取定时重启失败')
    info.value = si.data
    deviceName.value = si.data.device_name
    time.value = st.data
    reboot.value = rs.data
    checksFromDays(rs.data.days)
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

async function onSaveName() {
  savingName.value = true
  try {
    const r = await setSystemInfo({ device_name: deviceName.value.trim() })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存设备名失败')
      return
    }
    info.value = r.data
    deviceName.value = r.data.device_name
    ElMessage.success('设备名已保存')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    savingName.value = false
  }
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

async function onSaveReboot() {
  savingReboot.value = true
  try {
    const r = await setRebootSchedule({
      enabled: reboot.value.enabled,
      hour: reboot.value.hour,
      minute: reboot.value.minute,
      days: daysFromChecks(),
    })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存定时重启失败')
      return
    }
    reboot.value = r.data
    checksFromDays(r.data.days)
    ElMessage.success('定时重启已保存')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    savingReboot.value = false
  }
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
          <el-form label-width="80px">
            <el-form-item label="设备名">
              <el-input v-model="deviceName" maxlength="32" show-word-limit />
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="savingName" @click="onSaveName">保存名称</el-button>
            </el-form-item>
          </el-form>
          <template v-if="info">
            <p class="muted">主机名 <b>{{ info.hostname }}</b></p>
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
        <el-card shadow="never" class="panel-card mb">
          <template #header>定时重启</template>
          <el-form label-width="100px">
            <el-form-item label="启用">
              <el-switch v-model="reboot.enabled" />
            </el-form-item>
            <el-form-item label="时间">
              <el-input-number v-model="reboot.hour" :min="0" :max="23" />
              <span class="sep">:</span>
              <el-input-number v-model="reboot.minute" :min="0" :max="59" />
            </el-form-item>
            <el-form-item label="星期">
              <el-checkbox
                v-for="(lab, i) in DAY_LABELS"
                :key="lab"
                v-model="dayChecks[i]"
                :disabled="!reboot.enabled"
              >
                {{ lab }}
              </el-checkbox>
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="savingReboot" @click="onSaveReboot">保存定时重启</el-button>
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
.sep { margin: 0 0.4rem; }
</style>
