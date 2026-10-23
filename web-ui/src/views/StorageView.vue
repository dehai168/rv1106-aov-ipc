<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import {
  formatStorage,
  getRecordConfig,
  getStorageRecords,
  getStorageStatus,
  setRecordConfig,
  type StorageRecord,
  type StorageStatus,
} from '../api'
import PageShell from '../components/PageShell.vue'

const loading = ref(true)
const formatting = ref(false)
const savingRecycle = ref(false)
const activeTab = ref('status')
const status = ref<StorageStatus | null>(null)
const records = ref<StorageRecord[]>([])
const total = ref(0)
const date = ref('')
const page = ref(1)
const pageSize = ref(20)
const playOpen = ref(false)
const playing = ref<StorageRecord | null>(null)
const recyclePct = ref(10)
const kindFilter = ref<'' | 'event' | 'normal'>('')

const stateLabel = computed(() => {
  switch (status.value?.state) {
    case 'mounted':
      return '已挂载'
    case 'unmounted':
      return '未挂载（已格式化）'
    case 'unformatted':
      return '未格式化'
    case 'nocard':
      return '未检测到 SD 卡'
    default:
      return status.value?.mounted ? '已挂载' : '未挂载'
  }
})

const stateTagType = computed(() => {
  switch (status.value?.state) {
    case 'mounted':
      return 'success'
    case 'unformatted':
      return 'warning'
    case 'nocard':
      return 'danger'
    default:
      return 'info'
  }
})

const stateHint = computed(() => {
  switch (status.value?.state) {
    case 'mounted':
      return ''
    case 'unformatted':
      return 'SD 卡没有可识别的文件系统，请执行「格式化 SD 卡」。'
    case 'unmounted':
      return 'SD 卡已格式化但未挂载，可重新插拔或格式化。'
    case 'nocard':
      return '请插入 SD 卡。'
    default:
      return ''
  }
})

function downloadUrl(relPath: string) {
  return `/api/v1/storage/download?path=${encodeURIComponent(relPath)}`
}

function formatDateQuery(v: string | Date | null | undefined) {
  if (!v) return ''
  if (typeof v === 'string') {
    return v.replace(/-/g, '').slice(0, 8)
  }
  const y = v.getFullYear()
  const m = String(v.getMonth() + 1).padStart(2, '0')
  const d = String(v.getDate()).padStart(2, '0')
  return `${y}${m}${d}`
}

function recordKindLabel(row: { kind?: string; event_src?: string }) {
  if (row.kind !== 'event') return '常规'
  if (row.event_src === 'face') return '事件·人脸'
  if (row.event_src === 'motion,face') return '事件·移动+人脸'
  return '事件·移动'
}

function formatSizeMb(size: number) {
  return `${((size || 0) / 1024 / 1024).toFixed(2)} MB`
}

async function refreshStatus() {
  const [st, rc] = await Promise.all([getStorageStatus(), getRecordConfig()])
  if (st.code !== 0) throw new Error(st.msg || '读取存储状态失败')
  status.value = st.data
  if (rc.code === 0) recyclePct.value = rc.data.recycle_free_percent
}

async function refreshRecords() {
  const offset = (page.value - 1) * pageSize.value
  const rs = await getStorageRecords({
    date: formatDateQuery(date.value) || undefined,
    limit: pageSize.value,
    offset,
    kind: kindFilter.value || undefined,
  })
  if (rs.code !== 0) throw new Error(rs.msg || '读取录像列表失败')
  records.value = rs.data.records
  total.value = rs.data.total
}

async function refresh() {
  loading.value = true
  try {
    await Promise.all([refreshStatus(), refreshRecords()])
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function onQuery() {
  page.value = 1
  loading.value = true
  try {
    await refreshRecords()
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function onPageChange() {
  loading.value = true
  try {
    await refreshRecords()
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function onSaveRecycle() {
  savingRecycle.value = true
  try {
    const r = await setRecordConfig({ recycle_free_percent: recyclePct.value, apply: true })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存失败')
      return
    }
    recyclePct.value = r.data.recycle_free_percent
    ElMessage.success('覆盖比例已保存')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    savingRecycle.value = false
  }
}

function onPreview(row: StorageRecord) {
  playing.value = row
  playOpen.value = true
}

function onDownload(row: StorageRecord) {
  const a = document.createElement('a')
  a.href = downloadUrl(row.path)
  a.download = row.name || 'record.mp4'
  a.rel = 'noopener'
  document.body.appendChild(a)
  a.click()
  a.remove()
}

async function onFormatCard() {
  try {
    await ElMessageBox.confirm(
      '确认格式化 SD 卡？所有录像、抓图、告警记录都会被永久删除，并重新创建 FAT32 文件系统。',
      '格式化 SD 卡',
      { type: 'warning', confirmButtonText: '格式化', cancelButtonText: '取消' },
    )
  } catch {
    return
  }
  formatting.value = true
  try {
    const r = await formatStorage({ mkfs: true })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '格式化失败')
      return
    }
    ElMessage.success('SD 卡已格式化并挂载')
    await refresh()
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    formatting.value = false
  }
}

async function onClearRecords() {
  try {
    await ElMessageBox.confirm(
      '确认清空 TF 卡上的录像、抓图与告警记录？（软清空，不重新分区）',
      '清空存储',
      { type: 'warning', confirmButtonText: '清空', cancelButtonText: '取消' },
    )
  } catch {
    return
  }
  formatting.value = true
  try {
    const r = await formatStorage({ mkfs: false })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '清空失败')
      return
    }
    ElMessage.success(`已清空，删除 ${r.data.deleted} 个文件`)
    await refresh()
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    formatting.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <PageShell title="存储与回放" subtitle="查看容量、覆盖策略，并检索回放录像。">
    <div v-loading="loading">
      <el-tabs v-model="activeTab">
        <el-tab-pane label="存储状态" name="status">
          <el-descriptions class="ipc-desc" :column="1" border>
            <el-descriptions-item label="SD 卡状态">
              <el-tag :type="stateTagType" size="small">{{ stateLabel }}</el-tag>
              <span v-if="stateHint" class="hint state-hint">{{ stateHint }}</span>
            </el-descriptions-item>
            <el-descriptions-item label="挂载路径">
              {{ status?.mount_path || '-' }}
            </el-descriptions-item>
            <el-descriptions-item label="设备节点">
              {{ status?.device || '-' }}
            </el-descriptions-item>
            <el-descriptions-item label="文件系统">
              {{ status?.fstype || '-' }}
            </el-descriptions-item>
            <el-descriptions-item label="总容量">
              {{ status ? `${Math.round(status.total_bytes / 1024 / 1024)} MB` : '-' }}
            </el-descriptions-item>
            <el-descriptions-item label="剩余空间">
              {{
                status
                  ? `${Math.round(status.free_bytes / 1024 / 1024)} MB（${status.free_percent}%）`
                  : '-'
              }}
            </el-descriptions-item>
            <el-descriptions-item label="覆盖比例">
              <el-space>
                <el-input-number v-model="recyclePct" :min="5" :max="90" />
                <span class="unit">%</span>
                <el-button :loading="savingRecycle" type="primary" @click="onSaveRecycle">保存</el-button>
              </el-space>
              <div class="hint">空闲低于该比例时删除最旧录像</div>
            </el-descriptions-item>
          </el-descriptions>
          <div class="mt">
            <el-button type="danger" :loading="formatting" @click="onFormatCard">
              格式化 SD 卡
            </el-button>
            <el-button type="danger" plain :loading="formatting" @click="onClearRecords">
              清空录像/抓图/告警
            </el-button>
            <p class="hint">
              格式化会执行 mkfs.vfat 并重新挂载（录像会短暂中断）；清空仅删除文件。
            </p>
          </div>
        </el-tab-pane>

        <el-tab-pane label="录像列表" name="records">
          <div class="filters">
            <span class="lab">日期</span>
            <el-date-picker
              v-model="date"
              type="date"
              value-format="YYYY-MM-DD"
              placeholder="yyyy-MM-dd"
              clearable
            />
            <span class="lab">类型</span>
            <el-select v-model="kindFilter" style="width: 130px" @change="onQuery">
              <el-option label="全部" value="" />
              <el-option label="常规录像" value="normal" />
              <el-option label="事件录像" value="event" />
            </el-select>
            <el-button type="primary" @click="onQuery">查询</el-button>
          </div>
          <el-table
            :data="records"
            stripe
            empty-text="没有找到 mp4 录像"
            max-height="560"
          >
            <el-table-column prop="name" label="文件名" min-width="200" show-overflow-tooltip />
            <el-table-column label="类型" width="130">
              <template #default="{ row }">
                <el-tag :type="row.kind === 'event' ? 'warning' : 'info'" size="small">
                  {{ recordKindLabel(row) }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="时间" min-width="150">
              <template #default="{ row }">
                {{ new Date((row.mtime ?? 0) * 1000).toLocaleString() }}
              </template>
            </el-table-column>
            <el-table-column label="大小" width="110">
              <template #default="{ row }">
                {{ formatSizeMb(row.size ?? 0) }}
              </template>
            </el-table-column>
            <el-table-column label="操作" width="140" fixed="right">
              <template #default="{ row }">
                <el-button link type="primary" @click="onPreview(row as StorageRecord)">预览</el-button>
                <el-button link type="primary" @click="onDownload(row as StorageRecord)">下载</el-button>
              </template>
            </el-table-column>
          </el-table>
          <div class="pager">
            <el-pagination
              v-model:current-page="page"
              v-model:page-size="pageSize"
              background
              layout="total, sizes, prev, pager, next"
              :total="total"
              :page-sizes="[10, 20, 50]"
              @current-change="onPageChange"
              @size-change="() => { page = 1; onPageChange() }"
            />
          </div>
        </el-tab-pane>
      </el-tabs>
    </div>

    <el-dialog
      v-model="playOpen"
      :title="playing?.name || '录像预览'"
      width="720px"
      destroy-on-close
      @closed="playing = null"
    >
      <video
        v-if="playing"
        :key="playing.path"
        class="player"
        :src="downloadUrl(playing.path)"
        controls
        autoplay
      />
    </el-dialog>
  </PageShell>
</template>

<style scoped>
.filters {
  display: flex;
  align-items: center;
  gap: 0.6rem;
  margin-bottom: 0.85rem;
  flex-wrap: wrap;
}
.lab { color: var(--ipc-muted); font-size: 0.9rem; }
.unit { color: var(--ipc-muted); }
.hint { color: var(--ipc-muted); font-size: 0.8rem; margin: 0.35rem 0 0; }
.state-hint { margin-left: 0.5rem; }
.mt { margin-top: 1rem; }
.pager {
  display: flex;
  justify-content: flex-end;
  margin-top: 0.85rem;
}
.player {
  width: 100%;
  max-height: 480px;
  background: #0b0f14;
  border-radius: 6px;
}
</style>
