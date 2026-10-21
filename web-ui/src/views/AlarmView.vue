<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
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
import PageShell from '../components/PageShell.vue'
import RegionDrawCanvas from '../components/RegionDrawCanvas.vue'
import ScheduleGrid from '../components/ScheduleGrid.vue'

const loading = ref(true)
const saving = ref(false)
const bgLoading = ref(false)
const bgUrl = ref('')
const activeTab = ref('detect')
const regionIdx = ref(0)
let refreshTimer: number | undefined
let bgObjectUrl: string | undefined
let autoSnapDone = false

const motion = ref<AlarmMotion | null>(null)
const events = ref<AlarmEvent[]>([])
const eventsLimit = ref(100)
const page = ref(1)
const pageSize = ref(20)
const previewOpen = ref(false)
const previewSrc = ref('')

const pagedEvents = computed(() => {
  const start = (page.value - 1) * pageSize.value
  return events.value.slice(start, start + pageSize.value)
})

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

async function refreshBg(silent = false) {
  bgLoading.value = true
  try {
    const r = await previewSnapshot()
    if (r.code !== 0) throw new Error(r.msg || '抓图失败（请先打开实时预览或等待媒体起流）')
    const file = r.data.file
    const url = alarmSnapshotUrl(file)
    const res = await fetch(url, { credentials: 'same-origin' })
    if (!res.ok) throw new Error('下载底图失败')
    const blob = await res.blob()
    if (bgObjectUrl) URL.revokeObjectURL(bgObjectUrl)
    bgObjectUrl = URL.createObjectURL(blob)
    bgUrl.value = bgObjectUrl
    if (!silent) ElMessage.success('底图已更新')
  } catch (e) {
    if (!silent) ElMessage.error(String(e))
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
    events.value = (evs.data.events || []).filter((e) => e && typeof e.square === 'number')
    page.value = 1
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
    ElMessage.success('已保存并应用')
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

watch(activeTab, (tab) => {
  if (tab === 'region' && !autoSnapDone) {
    autoSnapDone = true
    refreshBg(true)
  }
})

onMounted(() => {
  refresh()
  refreshTimer = window.setInterval(() => {
    getAlarmEvents(eventsLimit.value)
      .then((evs) => {
        if (evs.code === 0) {
          events.value = (evs.data.events || []).filter((e) => e && typeof e.square === 'number')
        }
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
  <PageShell title="移动侦测" subtitle="左侧配置侦测参数，右侧查看实时事件。">
    <div v-loading="loading" class="split-page alarm-split">
      <div class="split-left">
        <template v-if="motion">
          <div class="runtime">
            <el-tag :type="motion.running ? 'success' : 'info'" size="small">
              {{ motion.running ? '运行中' : '未运行' }}
            </el-tag>
            <span class="muted">事件计数 {{ motion.motion_count }}</span>
          </div>
          <el-tabs v-model="activeTab">
            <el-tab-pane label="检测配置" name="detect">
              <el-form label-width="110px">
                <el-form-item label="启用侦测">
                  <el-switch v-model="motion.enabled" />
                </el-form-item>
                <el-form-item label="灵敏度">
                  <div class="slider-row">
                    <el-slider v-model="motion.sensitivity" :min="0" :max="4" show-stops style="width: 250px; max-width: 250px" />
                    <span class="val">{{ motion.sensitivity }}</span>
                  </div>
                </el-form-item>
                <el-form-item label="面积阈值">
                  <el-input-number v-model="motion.square_pct" :min="1" />
                  <span class="unit">运动区域占比阈值</span>
                </el-form-item>
                <el-form-item label="连续帧数">
                  <el-input-number v-model="motion.hit_frames" :min="1" />
                  <span class="unit">连续命中后才触发</span>
                </el-form-item>
              </el-form>
            </el-tab-pane>

            <el-tab-pane label="布防区域" name="region">
              <div class="region-btns">
                <el-button
                  v-for="i in 4"
                  :key="i"
                  size="small"
                  :type="regionIdx === i - 1 ? 'primary' : 'default'"
                  @click="regionIdx = i - 1"
                >
                  区域{{ i }}
                </el-button>
                <el-button size="small" :loading="bgLoading" @click="refreshBg(false)">刷新底图</el-button>
              </div>
              <p class="hint">未画区域 = 全画面侦测；坐标 640×360。</p>
              <RegionDrawCanvas
                v-model="motion.regions"
                :image-url="bgUrl"
                :logic-w="640"
                :logic-h="360"
                :active-index="regionIdx"
              />
            </el-tab-pane>

            <el-tab-pane label="布防时间" name="schedule">
              <el-form label-width="100px">
                <el-form-item label="启用时间表">
                  <el-switch v-model="motion.schedule.enabled" active-text="关闭=全天" />
                </el-form-item>
              </el-form>
              <ScheduleGrid
                v-model="motion.schedule.grid!"
                class="full"
                :disabled="!motion.schedule.enabled"
              />
            </el-tab-pane>

            <el-tab-pane label="联动" name="link">
              <el-form label-width="100px">
                <el-form-item label="联动录像">
                  <el-switch v-model="motion.link.record" />
                </el-form-item>
                <el-form-item label="联动抓图">
                  <el-switch v-model="motion.link.snapshot" />
                </el-form-item>
              </el-form>
            </el-tab-pane>
          </el-tabs>
          <div class="actions">
            <el-button type="primary" :loading="saving" @click="onSave">保存并应用</el-button>
          </div>
        </template>
      </div>

      <div class="split-right events-pane">
        <div class="events-head">
          <span>实时事件</span>
        </div>
        <el-table :data="pagedEvents" empty-text="暂无事件" stripe class="events-table" height="100%">
          <el-table-column label="时间" min-width="140">
            <template #default="{ row }">
              {{ new Date(row.ts * 1000).toLocaleString() }}
            </template>
          </el-table-column>
          <el-table-column label="面积" prop="square" width="72" />
          <el-table-column label="抓图" min-width="100">
            <template #default="{ row }">
              <a v-if="row.snapshot" href="#" @click.prevent="openPreview(row.snapshot)">
                <img class="thumb" :src="alarmSnapshotUrl(row.snapshot)" :alt="row.snapshot" />
              </a>
              <span v-else class="hint">—</span>
            </template>
          </el-table-column>
        </el-table>
        <div class="pager">
          <el-pagination
            v-model:current-page="page"
            v-model:page-size="pageSize"
            background
            small
            layout="total, prev, pager, next"
            :total="events.length"
            :page-sizes="[20]"
          />
        </div>
      </div>
    </div>

    <el-dialog v-model="previewOpen" title="告警抓图" width="640px" destroy-on-close>
      <img v-if="previewSrc" class="preview-img" :src="previewSrc" alt="snapshot" />
    </el-dialog>
  </PageShell>
</template>

<style scoped>
.alarm-split {
  height: calc(100vh - 200px);
  max-height: calc(100vh - 200px);
  min-height: 360px;
  overflow: hidden;
}
.runtime {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  margin-bottom: 0.5rem;
}
.muted { color: var(--ipc-muted); font-size: 0.85rem; }
.slider-row {
  display: flex;
  align-items: center;
  gap: 0.75rem;
}
.val {
  min-width: 1.5rem;
  color: var(--ipc-muted);
  font-variant-numeric: tabular-nums;
}
.unit {
  margin-left: 0.5rem;
  color: var(--ipc-muted);
  font-size: 0.8rem;
}
.region-btns {
  display: flex;
  flex-wrap: wrap;
  gap: 0.4rem;
  margin-bottom: 0.5rem;
}
.hint { color: var(--ipc-muted); font-size: 0.8rem; margin: 0.25rem 0 0.5rem; }
.full { width: 100%; overflow-x: auto; }
.actions { margin-top: 0.85rem; }
.events-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 0.5rem;
  font-weight: 600;
  flex: 0 0 auto;
}
.events-pane {
  display: flex;
  flex-direction: column;
  min-height: 0;
  height: 100%;
  overflow: hidden;
}
.events-table { flex: 1 1 auto; min-height: 0; }
.pager {
  flex: 0 0 auto;
  display: flex;
  justify-content: flex-end;
  margin-top: 0.5rem;
}
.thumb {
  max-width: 88px;
  max-height: 56px;
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
