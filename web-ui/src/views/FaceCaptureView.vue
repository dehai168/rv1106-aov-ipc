<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import {
  alarmSnapshotUrl,
  deleteFaceEvents,
  faceImageUrl,
  getFaceConfig,
  getFaceEvents,
  previewSnapshot,
  setFaceConfig,
  type FaceConfig,
  type FaceEventItem,
  type FaceRegion,
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
let bgObjectUrl: string | undefined
let autoSnapDone = false
let refreshTimer: number | undefined

const face = ref<FaceConfig | null>(null)
const events = ref<FaceEventItem[]>([])
const eventsTotal = ref(0)
const eventsDate = ref('')
const eventsPage = ref(1)
const eventsPageSize = ref(20)
const eventsLoading = ref(false)
const selected = ref<string[]>([])
const previewOpen = ref(false)
const previewSrc = ref('')
const previewTitle = ref('人脸预览')

const intervalOptions = [
  { label: '1 秒', value: 1000 },
  { label: '3 秒', value: 3000 },
  { label: '5 秒', value: 5000 },
  { label: '10 秒', value: 10000 },
]

const sensitivityOptions = [
  { label: '高（更灵敏）', value: 'high' },
  { label: '标准', value: 'normal' },
  { label: '低误报', value: 'low' },
]

const eventsOffset = computed(() => (eventsPage.value - 1) * eventsPageSize.value)

function emptyRegions(): FaceRegion[] {
  return Array.from({ length: 4 }, () => ({ enabled: false, x: 0, y: 0, w: 0, h: 0 }))
}

function emptyGrid() {
  return '1'.repeat(168)
}

function todayDate8() {
  const d = new Date()
  const y = d.getFullYear()
  const m = String(d.getMonth() + 1).padStart(2, '0')
  const day = String(d.getDate()).padStart(2, '0')
  return `${y}${m}${day}`
}

function ensureDefaults(cfg: FaceConfig): FaceConfig {
  if (!cfg.regions || !Array.isArray(cfg.regions) || cfg.regions.length === 0) {
    cfg.regions = emptyRegions()
  }
  while (cfg.regions.length < 4) {
    cfg.regions.push({ enabled: false, x: 0, y: 0, w: 0, h: 0 })
  }
  cfg.regions = cfg.regions.slice(0, 4).map((r) => ({
    enabled: !!r.enabled,
    x: r.x || 0,
    y: r.y || 0,
    w: r.w || 0,
    h: r.h || 0,
  }))
  if (!cfg.schedule) {
    cfg.schedule = { enabled: false, grid: emptyGrid() }
  }
  if (!cfg.schedule.grid || cfg.schedule.grid.length !== 168) {
    cfg.schedule.grid = emptyGrid()
  }
  if (!cfg.link) {
    /* Older firmware only had save_scene / link_record / link_alarm. */
    cfg.link = {
      record: cfg.link_record ?? true,
      snapshot: cfg.save_scene ?? true,
      alarm: cfg.link_alarm ?? true,
    }
  }
  if (typeof cfg.link.snapshot !== 'boolean') cfg.link.snapshot = !!cfg.save_scene
  if (!cfg.sensitivity) cfg.sensitivity = 'normal'
  if (![1000, 3000, 5000, 10000].includes(cfg.interval_ms)) {
    const nearest = intervalOptions.reduce((best, cur) =>
      Math.abs(cur.value - cfg.interval_ms) < Math.abs(best.value - cfg.interval_ms) ? cur : best,
    )
    cfg.interval_ms = nearest.value
  }
  return cfg
}

async function refreshBg(silent = false) {
  bgLoading.value = true
  try {
    const r = await previewSnapshot()
    if (r.code !== 0) throw new Error(r.msg || '抓图失败（请先打开实时预览）')
    const url = alarmSnapshotUrl(r.data.file)
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

async function refreshConfig() {
  loading.value = true
  try {
    const r = await getFaceConfig()
    if (r.code !== 0) throw new Error(r.msg || '读取人脸配置失败')
    face.value = ensureDefaults(r.data)
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function refreshEvents() {
  eventsLoading.value = true
  try {
    const date = eventsDate.value || todayDate8()
    eventsDate.value = date
    const r = await getFaceEvents({
      date,
      limit: eventsPageSize.value,
      offset: eventsOffset.value,
    })
    if (r.code !== 0) throw new Error(r.msg || '读取抓拍记录失败')
    events.value = r.data.events || []
    eventsTotal.value = r.data.total || 0
    selected.value = []
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    eventsLoading.value = false
  }
}

async function onSave() {
  if (!face.value) return
  saving.value = true
  try {
    const r = await setFaceConfig({
      enabled: face.value.enabled,
      sensitivity: face.value.sensitivity,
      interval_ms: face.value.interval_ms,
      max_snap_per_frame: face.value.max_snap_per_frame,
      /* 联动抓图 is the old 保存全景 switch, kept under link.snapshot. */
      save_scene: face.value.link.snapshot,
      regions: face.value.regions,
      schedule: face.value.schedule,
      link: face.value.link,
      quota_percent: face.value.quota_percent,
      daily_limit: face.value.daily_limit,
      apply: true,
    })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存失败')
      if (r.data) face.value = ensureDefaults(r.data)
      return
    }
    ElMessage.success('已保存并应用')
    face.value = ensureDefaults(r.data)
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    saving.value = false
  }
}

function openFace(file: string) {
  previewTitle.value = '人脸预览'
  previewSrc.value = faceImageUrl(file)
  previewOpen.value = true
}

function downloadFace(file: string) {
  const a = document.createElement('a')
  a.href = faceImageUrl(file)
  a.download = file.split('/').pop() || 'face.jpg'
  a.target = '_blank'
  a.click()
}

async function onDeleteSelected() {
  if (selected.value.length === 0) return
  try {
    await ElMessageBox.confirm(`确认删除选中的 ${selected.value.length} 张人脸图？`, '删除', {
      type: 'warning',
    })
  } catch {
    return
  }
  const r = await deleteFaceEvents({ files: selected.value })
  if (r.code !== 0) {
    ElMessage.error(r.msg || '删除失败')
    return
  }
  ElMessage.success(`已删除 ${r.data.deleted}`)
  await refreshEvents()
}

async function onDeleteDay() {
  const date = eventsDate.value || todayDate8()
  try {
    await ElMessageBox.confirm(`确认删除 ${date} 当天全部人脸抓拍？`, '按日删除', {
      type: 'warning',
    })
  } catch {
    return
  }
  const r = await deleteFaceEvents({ date })
  if (r.code !== 0) {
    ElMessage.error(r.msg || '删除失败')
    return
  }
  ElMessage.success(`已删除 ${r.data.deleted}`)
  eventsPage.value = 1
  await refreshEvents()
}

function onSelectionChange(rows: FaceEventItem[]) {
  selected.value = rows.map((r) => r.face_file)
}

watch(activeTab, (tab) => {
  if (tab === 'region' && !autoSnapDone) {
    autoSnapDone = true
    refreshBg(true)
  }
  if (tab === 'records') refreshEvents()
})

watch(eventsPage, () => {
  if (activeTab.value === 'records') refreshEvents()
})

onMounted(async () => {
  eventsDate.value = todayDate8()
  await refreshConfig()
  refreshTimer = window.setInterval(() => {
    getFaceConfig()
      .then((r) => {
        if (r.code === 0 && face.value) {
          face.value.running = r.data.running
          face.value.snap_count = r.data.snap_count
          face.value.infer_count = r.data.infer_count
          face.value.last_event = r.data.last_event
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
  <PageShell title="人脸抓拍" subtitle="运动门控触发 YuNet 检测；布防区域/布防时间/联动可分别配置。">
    <div v-loading="loading" class="face-page">
      <template v-if="face">
        <div class="runtime">
          <el-tag :type="face.running ? 'success' : 'info'" size="small">
            {{ face.running ? '运行中' : '未运行' }}
          </el-tag>
          <span class="muted">抓拍 {{ face.snap_count }} · 推理 {{ face.infer_count }}</span>
        </div>

        <el-tabs v-model="activeTab">
          <el-tab-pane label="侦测" name="detect">
            <el-form label-width="120px" class="detect-form">
              <el-form-item label="启用人脸抓拍">
                <el-switch v-model="face.enabled" />
              </el-form-item>
              <el-form-item label="灵敏度">
                <el-select v-model="face.sensitivity" style="width: 220px">
                  <el-option
                    v-for="o in sensitivityOptions"
                    :key="o.value"
                    :label="o.label"
                    :value="o.value"
                  />
                </el-select>
              </el-form-item>
              <el-form-item label="抓拍间隔">
                <el-select v-model="face.interval_ms" style="width: 220px">
                  <el-option
                    v-for="o in intervalOptions"
                    :key="o.value"
                    :label="o.label"
                    :value="o.value"
                  />
                </el-select>
              </el-form-item>
              <el-form-item label="单帧最多抓拍">
                <el-input-number v-model="face.max_snap_per_frame" :min="1" :max="4" />
              </el-form-item>
              <el-alert
                type="info"
                :closable="false"
                show-icon
                title="有移动时才检测（motion_gate），不提供其它触发方式。"
                class="gate-hint"
              />
            </el-form>
            <div class="actions">
              <el-button type="primary" :loading="saving" @click="onSave">保存并应用</el-button>
            </div>
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
            <p class="hint">未画区域 = 全画面；区域按画面百分比匹配，与分辨率无关。</p>
            <RegionDrawCanvas
              v-model="face.regions"
              :image-url="bgUrl"
              :logic-w="640"
              :logic-h="360"
              :max-width="420"
              :active-index="regionIdx"
            />
            <div class="actions">
              <el-button type="primary" :loading="saving" @click="onSave">保存并应用</el-button>
            </div>
          </el-tab-pane>

          <el-tab-pane label="布防时间" name="schedule">
            <el-form label-width="100px">
              <el-form-item label="启用时间表">
                <el-switch v-model="face.schedule.enabled" active-text="关闭=全天" />
              </el-form-item>
            </el-form>
            <ScheduleGrid
              v-model="face.schedule.grid"
              class="full"
              :disabled="!face.schedule.enabled"
            />
            <div class="actions">
              <el-button type="primary" :loading="saving" @click="onSave">保存并应用</el-button>
            </div>
          </el-tab-pane>

          <el-tab-pane label="联动" name="link">
            <el-form label-width="120px" class="detect-form">
              <el-form-item label="联动抓图">
                <el-switch v-model="face.link.snapshot" />
                <span class="unit">抓拍时同时保存一张全景图</span>
              </el-form-item>
              <el-form-item label="联动录像">
                <el-switch v-model="face.link.record" />
                <span class="unit">抓拍时触发事件录像</span>
              </el-form-item>
              <el-form-item label="日抓拍上限">
                <el-input-number v-model="face.daily_limit" :min="10" :max="20000" :step="100" />
              </el-form-item>
              <el-form-item label="存储配额">
                <el-input-number v-model="face.quota_percent" :min="1" :max="50" />
                <span class="unit">% TF 容量（超限删最旧）</span>
              </el-form-item>
            </el-form>
            <div class="actions">
              <el-button type="primary" :loading="saving" @click="onSave">保存并应用</el-button>
            </div>
          </el-tab-pane>

          <el-tab-pane label="抓拍记录" name="records">
            <div class="records-toolbar">
              <el-input
                v-model="eventsDate"
                placeholder="YYYYMMDD"
                style="width: 140px"
                maxlength="8"
                @keyup.enter="eventsPage = 1; refreshEvents()"
              />
              <el-button @click="eventsPage = 1; refreshEvents()">查询</el-button>
              <el-button :disabled="!selected.length" @click="onDeleteSelected">删除选中</el-button>
              <el-button type="danger" plain @click="onDeleteDay">删除当日</el-button>
            </div>
            <el-table
              v-loading="eventsLoading"
              :data="events"
              empty-text="暂无抓拍"
              stripe
              @selection-change="onSelectionChange"
            >
              <el-table-column type="selection" width="42" />
              <el-table-column label="人脸" min-width="120">
                <template #default="{ row }">
                  <a href="#" @click.prevent="openFace(row.face_file)">
                    <img class="thumb" :src="faceImageUrl(row.face_file)" :alt="row.face_file" />
                  </a>
                </template>
              </el-table-column>
              <el-table-column label="文件" min-width="220" prop="face_file" show-overflow-tooltip />
              <el-table-column label="操作" width="140">
                <template #default="{ row }">
                  <el-button link type="primary" @click="openFace(row.face_file)">预览</el-button>
                  <el-button link type="primary" @click="downloadFace(row.face_file)">下载</el-button>
                </template>
              </el-table-column>
            </el-table>
            <div class="pager">
              <el-pagination
                v-model:current-page="eventsPage"
                v-model:page-size="eventsPageSize"
                background
                small
                layout="total, prev, pager, next"
                :total="eventsTotal"
              />
            </div>
          </el-tab-pane>
        </el-tabs>
      </template>
    </div>

    <el-dialog v-model="previewOpen" :title="previewTitle" width="520px" destroy-on-close>
      <img v-if="previewSrc" class="preview-img" :src="previewSrc" alt="face" />
    </el-dialog>
  </PageShell>
</template>

<style scoped>
.face-page { min-height: 360px; }
.runtime {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  margin-bottom: 0.5rem;
}
.muted { color: var(--ipc-muted); font-size: 0.85rem; }
.detect-form { max-width: 520px; }
.gate-hint { margin-top: 0.25rem; }
.actions { margin-top: 0.85rem; }
.region-btns {
  display: flex;
  flex-wrap: wrap;
  gap: 0.4rem;
  margin-bottom: 0.5rem;
}
.hint { color: var(--ipc-muted); font-size: 0.8rem; margin: 0.25rem 0 0.5rem; }
.unit {
  margin-left: 0.5rem;
  color: var(--ipc-muted);
  font-size: 0.8rem;
}
.sched-form { margin-top: 1rem; }
.full { width: 100%; overflow-x: auto; }
.records-toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
  margin-bottom: 0.75rem;
}
.pager {
  display: flex;
  justify-content: flex-end;
  margin-top: 0.5rem;
}
.thumb {
  max-width: 88px;
  max-height: 88px;
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
