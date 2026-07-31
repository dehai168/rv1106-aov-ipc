<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'
import { ElMessage } from 'element-plus'
import {
  alarmSnapshotUrl,
  getAlarmEvents,
  getAlarmMotion,
  previewSnapshot,
  setAlarmMotion,
  type AlarmEvent,
  type AlarmMotion,
  type AlarmRegion,
} from '../api'
import RegionDrawCanvas from '../components/RegionDrawCanvas.vue'
import ScheduleGrid from '../components/ScheduleGrid.vue'

const loading = ref(true)
const saving = ref(false)
const bgLoading = ref(false)
const bgUrl = ref('')
let refreshTimer: number | undefined
let bgObjectUrl: string | undefined

const motion = ref<AlarmMotion | null>(null)
const events = ref<AlarmEvent[]>([])
const eventsLimit = ref(50)
const previewOpen = ref(false)
const previewSrc = ref('')

function emptyRegions(): AlarmRegion[] {
  return Array.from({ length: 4 }, () => ({ enabled: false, x: 0, y: 0, w: 0, h: 0 }))
}

function emptyGrid() {
  return '1'.repeat(168)
}

function ensureDefaults(m: AlarmMotion): AlarmMotion {
  if (!m.regions || !Array.isArray(m.regions) || m.regions.length === 0) {
    m.regions = emptyRegions()
    if (m.region) m.regions[0] = { ...m.region }
  }
  while (m.regions.length < 4) {
    m.regions.push({ enabled: false, x: 0, y: 0, w: 0, h: 0 })
  }
  m.regions = m.regions.slice(0, 4).map((r) => ({
    enabled: !!r.enabled,
    x: r.x || 0,
    y: r.y || 0,
    w: r.w || 0,
    h: r.h || 0,
  }))
  m.region = m.regions[0]
  if (!m.schedule) {
    m.schedule = { enabled: false, start_min: 0, end_min: 1440, days: 0x7f, grid: emptyGrid() }
  }
  if (!m.schedule.grid || m.schedule.grid.length !== 168) {
    m.schedule.grid = emptyGrid()
  }
  if (!m.link) m.link = { record: true, snapshot: true }
  return m
}

async function refreshBg() {
  bgLoading.value = true
  try {
    const r = await previewSnapshot()
    if (r.code !== 0) throw new Error(r.msg || '抓图失败（请先打开实时预览）')
    const file = r.data.file
    const url = alarmSnapshotUrl(file)
    const res = await fetch(url, { credentials: 'same-origin' })
    if (!res.ok) throw new Error('下载底图失败')
    const blob = await res.blob()
    if (bgObjectUrl) URL.revokeObjectURL(bgObjectUrl)
    bgObjectUrl = URL.createObjectURL(blob)
    bgUrl.value = bgObjectUrl
    ElMessage.success('底图已更新')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    bgLoading.value = false
  }
}

async function refresh() {
  loading.value = true
  try {
    const [m, evs] = await Promise.all([getAlarmMotion(), getAlarmEvents(eventsLimit.value)])
    if (m.code !== 0) throw new Error(m.msg || '读取告警配置失败')
    if (evs.code !== 0) throw new Error(evs.msg || '读取告警事件失败')
    motion.value = ensureDefaults(m.data)
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
    motion.value.region = motion.value.regions[0]
    const r = await setAlarmMotion({
      enabled: motion.value.enabled,
      sensitivity: motion.value.sensitivity,
      square_pct: motion.value.square_pct,
      hit_frames: motion.value.hit_frames,
      regions: motion.value.regions,
      schedule: motion.value.schedule,
      link: motion.value.link,
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

function openPreview(file: string) {
  previewSrc.value = alarmSnapshotUrl(file)
  previewOpen.value = true
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
  if (bgObjectUrl) URL.revokeObjectURL(bgObjectUrl)
})
</script>

<template>
  <section class="page">
    <div>
      <h2 class="page-title">移动侦测</h2>
      <p class="page-desc">保持「实时预览」打开约 3 秒后自动启动侦测；本页每 3 秒刷新事件。</p>
    </div>

    <el-row v-loading="loading" :gutter="16">
      <el-col :xs="24" :lg="14">
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

            <el-divider content-position="left">布防区域（侦测坐标 640×360）</el-divider>
            <el-form-item label-width="0">
              <div class="full">
                <el-button size="small" :loading="bgLoading" @click="refreshBg">刷新底图</el-button>
                <RegionDrawCanvas v-model="motion.regions" :image-url="bgUrl" :logic-w="640" :logic-h="360" />
                <div class="hint">未画任何区域 = 全画面侦测；区域外运动将被软过滤。</div>
              </div>
            </el-form-item>

            <el-divider content-position="left">布防时间（7×24）</el-divider>
            <el-form-item label="启用时间表">
              <el-switch v-model="motion.schedule.enabled" active-text="关闭=全天" />
            </el-form-item>
            <el-form-item label-width="0">
              <ScheduleGrid
                v-model="motion.schedule.grid!"
                class="full"
                :disabled="!motion.schedule.enabled"
              />
            </el-form-item>

            <el-divider content-position="left">联动</el-divider>
            <el-form-item label="联动录像">
              <el-switch v-model="motion.link.record" />
            </el-form-item>
            <el-form-item label="联动抓图">
              <el-switch v-model="motion.link.snapshot" />
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

      <el-col :xs="24" :lg="10">
        <el-card shadow="never" class="panel-card">
          <template #header>
            <div class="hdr">
              <span>事件列表</span>
              <el-input-number v-model="eventsLimit" :min="1" :max="200" size="small" />
            </div>
          </template>
          <el-table :data="events" height="640" empty-text="暂无事件" stripe>
            <el-table-column label="时间" min-width="140">
              <template #default="{ row }">
                {{ new Date(row.ts * 1000).toLocaleString() }}
              </template>
            </el-table-column>
            <el-table-column label="square" prop="square" width="72" />
            <el-table-column label="抓图" min-width="120">
              <template #default="{ row }">
                <a
                  v-if="row.snapshot"
                  href="#"
                  @click.prevent="openPreview(row.snapshot)"
                >
                  <img class="thumb" :src="alarmSnapshotUrl(row.snapshot)" :alt="row.snapshot" />
                </a>
                <span v-else class="hint">—</span>
              </template>
            </el-table-column>
          </el-table>
        </el-card>
      </el-col>
    </el-row>

    <el-dialog v-model="previewOpen" title="告警抓图" width="640px" destroy-on-close>
      <img v-if="previewSrc" class="preview-img" :src="previewSrc" alt="snapshot" />
    </el-dialog>
  </section>
</template>

<style scoped>
.mb { margin-bottom: 1rem; }
.full { width: 100%; }
.hint { color: var(--ipc-muted); font-size: 0.8rem; margin-top: 0.25rem; }
.hdr { display: flex; align-items: center; justify-content: space-between; gap: 0.75rem; }
.thumb {
  max-width: 96px;
  max-height: 64px;
  object-fit: contain;
  border: 1px solid var(--ipc-border);
  background: #111;
  vertical-align: middle;
  cursor: pointer;
}
.preview-img {
  width: 100%;
  max-height: 70vh;
  object-fit: contain;
  background: #111;
}
</style>
