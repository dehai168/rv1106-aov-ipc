<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'
import { ElMessage } from 'element-plus'
import { getSystemLogTail, systemLogUrl } from '../../api'
import { Document, Download, Refresh } from '@element-plus/icons-vue'
import PageShell from '../../components/PageShell.vue'

const logUrl = systemLogUrl()
const loading = ref(false)
const auto = ref(false)
const lines = ref<string[]>([])
const text = ref('')
let timer: number | undefined

async function refresh() {
  loading.value = true
  try {
    const r = await getSystemLogTail(200)
    if (r.code !== 0) {
      ElMessage.error(r.msg || '读取日志失败')
      return
    }
    lines.value = r.data.lines || []
    text.value = lines.value.join('\n')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

function onAutoChange(v: string | number | boolean) {
  const on = !!v
  if (timer) {
    window.clearInterval(timer)
    timer = undefined
  }
  if (on) {
    timer = window.setInterval(refresh, 5000)
  }
}

onMounted(refresh)
onUnmounted(() => {
  if (timer) window.clearInterval(timer)
})
</script>

<template>
  <PageShell title="系统日志" subtitle="查看 ipc_app.log 尾部，或下载完整日志文件。">
    <div class="tools">
      <el-button type="primary" :icon="Refresh" :loading="loading" @click="refresh">刷新尾部</el-button>
      <el-switch v-model="auto" active-text="每 5s 自动刷新" @change="onAutoChange" />
      <a :href="logUrl" target="_blank" rel="noopener">
        <el-button :icon="Download">下载完整日志</el-button>
      </a>
    </div>
    <el-input
      v-model="text"
      type="textarea"
      :rows="32"
      readonly
      class="log"
      resize="vertical"
      :placeholder="loading ? '加载中…' : '暂无日志'"
    />
    <p class="meta">
      <el-icon><Document /></el-icon>
      最近 {{ lines.length }} 行
    </p>
  </PageShell>
</template>

<style scoped>
.tools {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.75rem;
  margin-bottom: 0.85rem;
}
.log {
  min-height: calc(100vh - 260px);
}
.log :deep(textarea) {
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 12px;
  line-height: 1.45;
  background: #0f141a;
  color: #d7dee7;
  min-height: calc(100vh - 260px);
}
.meta {
  display: flex;
  align-items: center;
  gap: 0.35rem;
  margin: 0.5rem 0 0;
  color: var(--ipc-muted);
  font-size: 0.85rem;
}
</style>
