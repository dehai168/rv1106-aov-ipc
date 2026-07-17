<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'
import { ElMessage } from 'element-plus'
import {
  alarmSnapshotUrl,
  getAlarmEvents,
  getAlarmMotion,
  setAlarmMotion,
  type AlarmEvent,
  type AlarmMotion,
} from '../api'

const loading = ref(true)
const saving = ref(false)
let refreshTimer: number | undefined

const DAY_LABELS = ['一', '二', '三', '四', '五', '六', '日'] as const

const motion = ref<AlarmMotion | null>(null)
const events = ref<AlarmEvent[]>([])
const eventsLimit = ref(50)
const startHm = ref('00:00')
const endHm = ref('24:00')

function ensureDefaults(m: AlarmMotion): AlarmMotion {
  if (!m.region) m.region = { enabled: false, x: 0, y: 0, w: 0, h: 0 }
  if (!m.schedule) m.schedule = { enabled: false, start_min: 0, end_min: 1440, days: 0x7f }
  return m
}

function minToHm(min: number): string {
  const m = Math.max(0, Math.min(1440, Math.floor(min)))
  if (m >= 1440) return '24:00'
  const h = Math.floor(m / 60)
  const mm = m % 60
  return `${String(h).padStart(2, '0')}:${String(mm).padStart(2, '0')}`
}

function hmToMin(hm: string): number {
  const parts = hm.split(':')
  if (parts.length < 2) return 0
  const h = Number(parts[0])
  const m = Number(parts[1])
  if (!Number.isFinite(h) || !Number.isFinite(m)) return 0
  if (h === 24 && m === 0) return 1440
  return Math.max(0, Math.min(1439, h * 60 + m))
}

function dayOn(bit: number): boolean {
  return !!motion.value && (motion.value.schedule.days & (1 << bit)) !== 0
}

function toggleDay(bit: number, on: boolean | string | number) {
  if (!motion.value) return
  const enabled = on === true || on === 'true' || on === 1
  if (enabled) motion.value.schedule.days |= 1 << bit
  else motion.value.schedule.days &= ~(1 << bit)
}

async function refresh() {
  loading.value = true
  try {
    const [m, evs] = await Promise.all([getAlarmMotion(), getAlarmEvents(eventsLimit.value)])
    if (m.code !== 0) throw new Error(m.msg || '读取告警配置失败')
    if (evs.code !== 0) throw new Error(evs.msg || '读取告警事件失败')
    motion.value = ensureDefaults(m.data)
    startHm.value = minToHm(motion.value.schedule.start_min)
    endHm.value = minToHm(motion.value.schedule.end_min)
    events.value = evs.data.events
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function onSave() {
  if (!motion.value) return
  saving.value = true
  try {
    motion.value.schedule.start_min = hmToMin(startHm.value)
    motion.value.schedule.end_min = hmToMin(endHm.value)
    const r = await setAlarmMotion({
      enabled: motion.value.enabled,
      sensitivity: motion.value.sensitivity,
      square_pct: motion.value.square_pct,
      hit_frames: motion.value.hit_frames,
      region: motion.value.region,
      schedule: motion.value.schedule,
      apply: true,
    })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存失败')
      return
    }
    ElMessage.success('已保存（请保持实时预览打开以便立即生效）')
    await refresh()
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    saving.value = false
  }
}

onMounted(() => {
  refresh()
  refreshTimer = window.setInterval(() => {
    getAlarmEvents(eventsLimit.value)
      .then((evs) => {
        if (evs.code === 0) events.value = evs.data.events
      })
      .catch(() => {})
    getAlarmMotion()
      .then((m) => {
        if (m.code === 0 && motion.value) {
          motion.value.running = m.data.running
          motion.value.motion_count = m.data.motion_count
          motion.value.last_event = m.data.last_event
        }
      })
      .catch(() => {})
  }, 3000)
})

onUnmounted(() => {
  if (refreshTimer) window.clearInterval(refreshTimer)
})
</script>

<template>
  <section class="page">
    <div>
      <h2 class="page-title">移动侦测</h2>
      <p class="page-desc">保持「实时预览」打开约 3 秒后自动启动侦测；本页每 3 秒刷新事件。</p>
    </div>

    <el-row v-loading="loading" :gutter="16">
      <el-col :xs="24" :lg="12">
        <el-card v-if="motion" shadow="never" class="panel-card mb">
          <template #header>检测配置</template>
          <el-form label-width="120px">
            <el-form-item label="启用侦测">
              <el-switch v-model="motion.enabled" />
            </el-form-item>
            <el-form-item label="灵敏度 0-4">
              <el-slider v-model="motion.sensitivity" :min="0" :max="4" show-stops />
            </el-form-item>
            <el-form-item label="square_pct">
              <el-input-number v-model="motion.square_pct" :min="1" />
            </el-form-item>
            <el-form-item label="hit_frames">
              <el-input-number v-model="motion.hit_frames" :min="1" />
            </el-form-item>

            <el-divider content-position="left">布防区域（约 640×360）</el-divider>
            <el-form-item label="启用区域">
              <el-switch v-model="motion.region.enabled" active-text="过滤区域外运动" />
            </el-form-item>
            <el-form-item label="矩形">
              <el-space wrap>
                <el-input-number v-model="motion.region.x" :min="0" controls-position="right" />
                <el-input-number v-model="motion.region.y" :min="0" controls-position="right" />
                <el-input-number v-model="motion.region.w" :min="0" controls-position="right" />
                <el-input-number v-model="motion.region.h" :min="0" controls-position="right" />
              </el-space>
              <div class="hint">顺序：x / y / w / h</div>
            </el-form-item>

            <el-divider content-position="left">布防时间</el-divider>
            <el-form-item label="启用时间表">
              <el-switch v-model="motion.schedule.enabled" active-text="关闭=全天" />
            </el-form-item>
            <el-form-item label="起止">
              <el-space>
                <el-input v-model="startHm" style="width: 100px" placeholder="00:00" />
                <span>—</span>
                <el-input v-model="endHm" style="width: 100px" placeholder="24:00" />
              </el-space>
            </el-form-item>
            <el-form-item label="星期">
              <el-checkbox
                v-for="(lab, i) in DAY_LABELS"
                :key="lab"
                :model-value="dayOn(i)"
                @change="(v: string | number | boolean) => toggleDay(i, v)"
              >
                周{{ lab }}
              </el-checkbox>
            </el-form-item>

            <el-form-item>
              <el-button type="primary" :loading="saving" @click="onSave">保存并应用</el-button>
              <el-button @click="refresh">刷新</el-button>
            </el-form-item>
          </el-form>

          <el-descriptions :column="1" border size="small">
            <el-descriptions-item label="运行中">{{ motion.running ? '是' : '否' }}</el-descriptions-item>
            <el-descriptions-item label="MotionCount">{{ motion.motion_count }}</el-descriptions-item>
            <el-descriptions-item v-if="motion.last_event" label="最近事件">
              ts={{ motion.last_event.ts }} · square={{ motion.last_event.square }}
              <span v-if="motion.last_event.snapshot"> · {{ motion.last_event.snapshot }}</span>
            </el-descriptions-item>
          </el-descriptions>
        </el-card>
      </el-col>

      <el-col :xs="24" :lg="12">
        <el-card shadow="never" class="panel-card">
          <template #header>
            <div class="hdr">
              <span>事件列表</span>
              <el-input-number v-model="eventsLimit" :min="1" :max="200" size="small" />
            </div>
          </template>
          <el-table :data="events" height="560" empty-text="暂无事件" stripe>
            <el-table-column label="时间" min-width="150">
              <template #default="{ row }">
                {{ new Date(row.ts * 1000).toLocaleString() }}
              </template>
            </el-table-column>
            <el-table-column label="square" prop="square" width="80" />
            <el-table-column label="抓图" min-width="120">
              <template #default="{ row }">
                <a v-if="row.snapshot" :href="alarmSnapshotUrl(row.snapshot)" target="_blank" rel="noopener">
                  <img class="thumb" :src="alarmSnapshotUrl(row.snapshot)" :alt="row.snapshot" />
                </a>
                <span v-else class="hint">—</span>
              </template>
            </el-table-column>
          </el-table>
        </el-card>
      </el-col>
    </el-row>
  </section>
</template>

<style scoped>
.mb { margin-bottom: 1rem; }
.hint { color: var(--ipc-muted); font-size: 0.8rem; margin-top: 0.25rem; }
.hdr { display: flex; align-items: center; justify-content: space-between; gap: 0.75rem; }
.thumb {
  max-width: 96px;
  max-height: 64px;
  object-fit: contain;
  border: 1px solid var(--ipc-border);
  background: #111;
  vertical-align: middle;
}
</style>
