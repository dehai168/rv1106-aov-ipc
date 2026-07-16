<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { getStorageRecords, getStorageStatus, type StorageRecord, type StorageStatus } from '../api'

const loading = ref(true)
const error = ref('')
const status = ref<StorageStatus | null>(null)
const records = ref<StorageRecord[]>([])

const date = ref<string>('') // YYYYMMDD
const limit = ref<number>(100)

const playing = ref<StorageRecord | null>(null)

function downloadUrl(relPath: string) {
  return `/api/v1/storage/download?path=${encodeURIComponent(relPath)}`
}

async function refresh() {
  loading.value = true
  error.value = ''
  try {
    const [st, rs] = await Promise.all([
      getStorageStatus(),
      getStorageRecords({ date: date.value || undefined, limit: limit.value }),
    ])

    if (st.code !== 0) throw new Error(st.msg || '读取存储状态失败')
    if (rs.code !== 0) throw new Error(rs.msg || '读取录像列表失败')

    status.value = st.data
    records.value = rs.data.records
    if (records.value.length > 0) {
      playing.value = records.value[0]
    }
  } catch (e) {
    error.value = String(e)
  } finally {
    loading.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <section class="storage">
    <h2>存储与回放</h2>
    <p v-if="loading" class="muted">加载中…</p>

    <div v-else class="grid">
      <div class="panel">
        <h3>状态</h3>
        <p class="muted">
          挂载：<b>{{ status?.mounted ? '已挂载' : '未挂载' }}</b>
          · 文件系统：<b>{{ status?.fstype || '-' }}</b>
        </p>
        <p class="muted">
          空闲：<b>{{ status?.free_percent ?? 0 }}%</b> ·
          {{ status ? `${Math.round(status.free_bytes / 1024 / 1024)}MB / ${Math.round(status.total_bytes / 1024 / 1024)}MB` : '-' }}
        </p>
      </div>

      <div class="panel">
        <h3>检索</h3>
        <label>
          日期（可选 YYYYMMDD）
          <input v-model="date" placeholder="例如 20260716" />
        </label>
        <label>
          返回条数
          <input v-model.number="limit" type="number" min="1" max="200" />
        </label>
        <div class="row">
          <button type="button" @click="refresh">刷新列表</button>
        </div>
      </div>

      <div class="panel video-panel">
        <h3>播放</h3>
        <p v-if="!playing" class="muted">未选择录像</p>
        <video v-else :key="playing.path" :src="downloadUrl(playing.path)" controls style="width: 100%; border: 1px solid var(--border)" />
      </div>

      <div class="panel list-panel">
        <h3>录像列表</h3>
        <p v-if="records.length === 0" class="muted">没有找到 mp4 录像</p>
        <div class="list">
          <button
            v-for="r in records"
            :key="r.path"
            type="button"
            class="item"
            :class="{ active: playing?.path === r.path }"
            @click="playing = r"
          >
            <div class="name">{{ r.name }}</div>
            <div class="meta">{{ new Date((r.mtime ?? 0) * 1000).toLocaleString() }}</div>
          </button>
        </div>
      </div>
    </div>

    <p v-if="error" class="error">{{ error }}</p>
  </section>
</template>

<style scoped>
.storage { max-width: 980px; display: grid; gap: 1rem; }
.grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; }
.panel { border: 1px solid var(--border); border-radius: 8px; padding: 0.85rem; display: grid; gap: 0.6rem; }
.video-panel { grid-column: 1 / -1; }
.list-panel { grid-column: 1 / -1; }
h3 { margin: 0; font-size: 1rem; color: var(--text); }
label { display: grid; gap: 0.35rem; color: var(--muted); font-size: 0.9rem; }
input { padding: 0.35rem 0.5rem; }
.row { display: flex; gap: 0.75rem; flex-wrap: wrap; align-items: center; }
.list { display: grid; gap: 0.5rem; grid-template-columns: 1fr 1fr; }
.item { text-align: left; background: transparent; border: 1px solid var(--border); border-radius: 8px; padding: 0.6rem; cursor: pointer; }
.item.active { border-color: rgba(61, 139, 253, 0.85); background: rgba(61, 139, 253, 0.12); }
.name { font-size: 0.85rem; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.meta { font-size: 0.8rem; color: var(--muted); }

@media (max-width: 860px) {
  .grid { grid-template-columns: 1fr; }
  .list { grid-template-columns: 1fr; }
}
</style>

