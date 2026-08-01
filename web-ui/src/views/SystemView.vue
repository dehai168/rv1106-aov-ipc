<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
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
  type RebootSchedule,
  type SystemTimeConfig,
} from '../api'
import PageShell from '../components/PageShell.vue'
import { useDeviceName } from '../composables/useDeviceName'

const DAY_LABELS = ['日', '一', '二', '三', '四', '五', '六']
const TIMEZONES = [
  { value: 'Asia/Shanghai', offset: '+8' },
  { value: 'Asia/Hong_Kong', offset: '+8' },
  { value: 'Asia/Taipei', offset: '+8' },
  { value: 'Asia/Tokyo', offset: '+9' },
  { value: 'Asia/Seoul', offset: '+9' },
  { value: 'Asia/Singapore', offset: '+8' },
  { value: 'Asia/Bangkok', offset: '+7' },
  { value: 'Asia/Jakarta', offset: '+7' },
  { value: 'Asia/Dubai', offset: '+4' },
  { value: 'Asia/Kolkata', offset: '+5:30' },
  { value: 'Europe/London', offset: '+0/+1' },
  { value: 'Europe/Berlin', offset: '+1/+2' },
  { value: 'Europe/Moscow', offset: '+3' },
  { value: 'America/New_York', offset: '-5/-4' },
  { value: 'America/Chicago', offset: '-6/-5' },
  { value: 'America/Denver', offset: '-7/-6' },
  { value: 'America/Los_Angeles', offset: '-8/-7' },
  { value: 'America/Sao_Paulo', offset: '-3' },
  { value: 'Australia/Sydney', offset: '+10/+11' },
  { value: 'Pacific/Auckland', offset: '+12/+13' },
  { value: 'UTC', offset: '+0' },
]

const router = useRouter()
const { setDeviceNameLocal } = useDeviceName()
const loading = ref(true)
const saving = ref(false)
const savingName = ref(false)
const savingReboot = ref(false)
const activeTab = ref('device')
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

const timeText = computed({
  get() {
    if (!time.value.unix_time) return ''
    const d = new Date(time.value.unix_time * 1000)
    const pad = (n: number) => String(n).padStart(2, '0')
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
  },
  set(v: string) {
    if (!v) return
    const t = Date.parse(v.replace(' ', 'T'))
    if (!Number.isNaN(t)) time.value.unix_time = Math.floor(t / 1000)
  },
})

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
    deviceName.value = si.data.device_name
    setDeviceNameLocal(si.data.device_name)
    time.value = st.data
    reboot.value = rs.data
    checksFromDays(rs.data.days)
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function onSaveName() {
  savingName.value = true
  try {
    const r = await setSystemInfo({ device_name: deviceName.value.trim() })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存设备名失败')
      return
    }
    deviceName.value = r.data.device_name
    setDeviceNameLocal(r.data.device_name)
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
  <PageShell title="基本设置" subtitle="设备名称、时间、定时重启与系统维护。">
    <div v-loading="loading">
      <el-tabs v-model="activeTab">
        <el-tab-pane label="设备信息" name="device">
          <el-form label-width="100px" style="max-width: 480px">
            <el-form-item label="设备名">
              <el-input v-model="deviceName" maxlength="32" show-word-limit />
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="savingName" @click="onSaveName">保存名称</el-button>
            </el-form-item>
          </el-form>
        </el-tab-pane>

        <el-tab-pane label="时间设置" name="time">
          <el-form label-width="100px" style="max-width: 520px">
            <el-form-item label="设备时间">
              <el-date-picker
                v-model="timeText"
                type="datetime"
                value-format="YYYY-MM-DD HH:mm:ss"
                format="YYYY-MM-DD HH:mm:ss"
                placeholder="yyyy-MM-dd HH:mm:ss"
                style="width: 100%"
              />
            </el-form-item>
            <el-form-item>
              <el-button @click="useBrowserTime">使用浏览器当前时间</el-button>
            </el-form-item>
            <el-form-item label="时区">
              <el-select v-model="time.timezone" filterable allow-create default-first-option style="width: 100%">
                <el-option
                  v-for="tz in TIMEZONES"
                  :key="tz.value"
                  :label="`${tz.value}  ${tz.offset}`"
                  :value="tz.value"
                />
              </el-select>
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
        </el-tab-pane>

        <el-tab-pane label="定时重启" name="reboot">
          <el-form label-width="100px" style="max-width: 560px">
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
        </el-tab-pane>

        <el-tab-pane label="维护" name="maintain">
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
          </el-space>
        </el-tab-pane>
      </el-tabs>
    </div>
  </PageShell>
</template>

<style scoped>
.mb { margin-bottom: 1rem; }
.sep { margin: 0 0.4rem; }
</style>
