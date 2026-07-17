<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import {
  formatStorage,
  getStorageRecords,
  getStorageStatus,
  type StorageRecord,
  type StorageStatus,
} from '../api'

const loading = ref(true)
const formatting = ref(false)
const status = ref<StorageStatus | null>(null)
const records = ref<StorageRecord[]>([])
const date = ref('')
const limit = ref(100)
const playing = ref<StorageRecord | null>(null)

function downloadUrl(relPath: string) {
  return `/api/v1/storage/download?path=${encodeURIComponent(relPath)}`
}

async function refresh() {
  loading.value = true
  try {
    const [st, rs] = await Promise.all([
      getStorageStatus(),
      getStorageRecords({ date: date.value || undefined, limit: limit.value }),
    ])
    if (st.code !== 0) throw new Error(st.msg || '读取存储状态失败')
    if (rs.code !== 0) throw new Error(rs.msg || '读取录像列表失败')
    status.value = st.data
    records.value = rs.data.records
    playing.value = records.value[0] ?? null
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function onFormat() {
  try {
    await ElMessageBox.confirm(
      '确认清空 TF 卡上的录像、抓图与告警记录？（软格式化，不重新分区；之后请重新打开预览以恢复侦测）',
      '清空存储',
      { type: 'warning', confirmButtonText: '清空', cancelButtonText: '取消' },
    )
  } catch {
    return
  }
  formatting.value = true
  try {
    const r = await formatStorage()
    if (r.code !== 0) {
      ElMessage.error(r.msg || '格式化失败')
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
  <section class="page">
    <div>
      <h2 class="page-title">存储与回放</h2>
      <p class="page-desc">查看容量、检索录像，并支持软清空 records/snapshots/alarms。</p>
    </div>

    <el-row v-loading="loading" :gutter="16">
      <el-col :xs="24" :md="12">
        <el-card shadow="never" class="panel-card mb">
          <template #header>状态</template>
          <p>
            挂载：<b>{{ status?.mounted ? '已挂载' : '未挂载' }}</b>
            · 文件系统：<b>{{ status?.fstype || '-' }}</b>
          </p>
          <p class="muted">
            空闲：<b>{{ status?.free_percent ?? 0 }}%</b> ·
            {{
              status
                ? `${Math.round(status.free_bytes / 1024 / 1024)}MB / ${Math.round(status.total_bytes / 1024 / 1024)}MB`
                : '-'
            }}
          </p>
          <el-button type="danger" plain :loading="formatting" @click="onFormat">清空录像/抓图/告警</el-button>
          <p class="tip">软格式化：删除文件，不执行 mkfs。</p>
        </el-card>
      </el-col>
      <el-col :xs="24" :md="12">
        <el-card shadow="never" class="panel-card mb">
          <template #header>检索</template>
          <el-form label-width="120px">
            <el-form-item label="日期 YYYYMMDD">
              <el-input v-model="date" placeholder="例如 20260716" clearable />
            </el-form-item>
            <el-form-item label="返回条数">
              <el-input-number v-model="limit" :min="1" :max="200" />
            </el-form-item>
            <el-form-item>
              <el-button type="primary" @click="refresh">刷新列表</el-button>
            </el-form-item>
          </el-form>
        </el-card>
      </el-col>

      <el-col :span="24">
        <el-card shadow="never" class="panel-card mb">
          <template #header>播放</template>
          <el-empty v-if="!playing" description="未选择录像" />
          <video
            v-else
            :key="playing.path"
            class="player"
            :src="downloadUrl(playing.path)"
            controls
          />
        </el-card>
      </el-col>

      <el-col :span="24">
        <el-card shadow="never" class="panel-card">
          <template #header>录像列表</template>
          <el-table
            :data="records"
            stripe
            highlight-current-row
            :current-row-key="playing?.path"
            row-key="path"
            empty-text="没有找到 mp4 录像"
            @current-change="(row: StorageRecord | undefined) => { if (row) playing = row }"
            @row-click="(row: StorageRecord) => { playing = row }"
          >
            <el-table-column prop="name" label="文件名" min-width="220" show-overflow-tooltip />
            <el-table-column label="时间" min-width="160">
              <template #default="{ row }">
                {{ new Date((row.mtime ?? 0) * 1000).toLocaleString() }}
              </template>
            </el-table-column>
            <el-table-column label="大小" width="110">
              <template #default="{ row }">
                {{ Math.round((row.size ?? 0) / 1024) }} KB
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
.muted { color: var(--ipc-muted); }
.tip { margin: 0.5rem 0 0; font-size: 0.8rem; color: var(--ipc-muted); }
.player {
  width: 100%;
  max-height: 480px;
  background: #0b0f14;
  border-radius: 6px;
}
</style>
